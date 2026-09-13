#include "csapp.h"
#include <pthread.h>

#define MAX_CACHE_SIZE 10
#define MAX_OBJECT_SIZE (1024 * 1024)

typedef struct cache_obj {
    char url[MAXLINE];
    char *content;
    size_t content_len;
    struct cache_obj *prev;
    struct cache_obj *next;
} cache_obj;

static cache_obj *cache_head = NULL;
static cache_obj *cache_tail = NULL;
static int cache_count = 0;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

void *doit(void *arg);
void parse_uri(char *uri, char *host, char *port, char *path);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

void cache_init(void);
cache_obj *cache_find(char *url);
void cache_add(char *url, char *data, size_t len);
void cache_evict(void);
void cache_free_entry(cache_obj *entry);

int main(int argc, char **argv)
{
    
    struct sockaddr_storage clientaddr;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    Signal(SIGPIPE, SIG_IGN);                
    int listenfd;
    listenfd = Open_listenfd(argv[1]);

    /* 主循环，将每个连接请求新开一个线程接入doit处理，保留监听端口空闲 */
    while (1) {
        socklen_t clientlen;
        clientlen = sizeof(clientaddr);

        int connfd;
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);

        pthread_t tid;
        Pthread_create(&tid, NULL, doit, (void *)((long)connfd));
        Pthread_detach(tid);                  
    }
    return 0;
}

void *doit(void *arg)
{
    int fd = (int)((long)arg);
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    rio_t rio;

    Rio_readinitb(&rio, fd);
    if (Rio_readlineb(&rio, buf, MAXLINE) <= 0) {
        Close(fd);
        return NULL;
    }
    printf("Request line: %s", buf);
    sscanf(buf, "%s %s %s", method, uri, version);

    if (strcasecmp(method, "GET") != 0) {
        clienterror(fd, method, "501", "Not Implemented",
                    "Proxy only supports GET method");
        Close(fd);
        return NULL;
    }

    /* 丢弃http请求头 */
    char header[MAXLINE];
    do {
        if (Rio_readlineb(&rio, header, MAXLINE) <= 0) {
            Close(fd);
            return NULL;
        }
        printf("%s", header);
    } while (strcmp(header, "\r\n") != 0);

    char host[MAXLINE], port[MAXLINE], path[MAXLINE];
    parse_uri(uri, host, port, path);

    /* 在缓存中查找 */
    pthread_mutex_lock(&cache_mutex);
    cache_obj *cached = cache_find(uri);
    if (cached != NULL) {
        Rio_writen(fd, cached->content, cached->content_len);
        pthread_mutex_unlock(&cache_mutex);
        Close(fd);
        return NULL;
    }
    pthread_mutex_unlock(&cache_mutex);

    int serverfd;
    serverfd = open_clientfd(host, port);
    if (serverfd < 0) {
        clienterror(fd, host, "502", "Bad Gateway",
                    "Cannot connect to origin server");
        Close(fd);
        return NULL;
    }

    /* 构造HTTP/1.0请求 */
    char request[MAXBUF];
    sprintf(request, "GET %s HTTP/1.0\r\n", path);
    sprintf(request + strlen(request), "Host: %s", host);
    if (strcmp(port, "80") != 0) {
        sprintf(request + strlen(request), ":%s", port);
    }
    sprintf(request + strlen(request), "\r\n");
    sprintf(request + strlen(request),
            "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) "
            "Gecko/20120305 Firefox/10.0.3\r\n");
    sprintf(request + strlen(request), "Connection: close\r\n");
    sprintf(request + strlen(request), "Proxy-Connection: close\r\n");
    sprintf(request + strlen(request), "\r\n");

    /* 把构造好的HTTP请求发到目标网站 */
    if (rio_writen(serverfd, request, strlen(request)) < 0) {
        Close(serverfd);
        Close(fd);
        return NULL;
    }

    size_t total = 0, capacity = 4096;
    char *resp = malloc(capacity);
    if (resp == NULL) {
        Close(serverfd);
        Close(fd);
        return NULL;
    }

    /* 读取返回的HTTP响应 */
    ssize_t n;
    while ((n = rio_readn(serverfd, resp + total, capacity - total)) > 0) {
        total += n;
        if (total == capacity) {
            capacity *= 2;
            char *tmp = realloc(resp, capacity);
            if (tmp == NULL) {
                free(resp);
                Close(serverfd);
                Close(fd);
                return NULL;
            }
            resp = tmp;
        }
    }
    Close(serverfd);

    /* 如果响应小于MAX_OBJECT_SIZE，缓存 */
    if (total <= MAX_OBJECT_SIZE) {
        pthread_mutex_lock(&cache_mutex);
        cache_add(uri, resp, total);
        pthread_mutex_unlock(&cache_mutex);
    }

    Rio_writen(fd, resp, total);
    free(resp);
    Close(fd);
    return NULL;
}

/*
 * 把一个URI拆解成host、port和path
 */
void parse_uri(char *uri, char *host, char *port, char *path)
{
    char *p = strstr(uri, "://");
    if (!p)
        p = uri;
    else
        p += 3;

    char *q = strchr(p, '/');
    if (q) {
        strcpy(path, q);
        int len = q - p;
        char hostport[MAXLINE];
        strncpy(hostport, p, len);
        hostport[len] = '\0';

        char *colon = strchr(hostport, ':');
        if (colon) {
            *colon = '\0';
            strcpy(host, hostport);
            strcpy(port, colon + 1);
        } else {
            strcpy(host, hostport);
            strcpy(port, "80");
        }
    } else {
        strcpy(path, "/");
        char *colon = strchr(p, ':');
        if (colon) {
            *colon = '\0';
            strcpy(host, p);
            strcpy(port, colon + 1);
        } else {
            strcpy(host, p);
            strcpy(port, "80");
        }
    }
}

/*
 * 处理错误信息
 */
void clienterror(int fd, char *cause, char *errnum,
                 char *shortmsg, char *longmsg)
{
    char buf[MAXLINE];

    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    write(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n\r\n");
    write(fd, buf, strlen(buf));

    sprintf(buf, "<html><title>Proxy Error</title>");
    write(fd, buf, strlen(buf));
    sprintf(buf, "<body bgcolor=\"ffffff\">\r\n");
    write(fd, buf, strlen(buf));
    sprintf(buf, "%s: %s\r\n", errnum, shortmsg);
    write(fd, buf, strlen(buf));
    sprintf(buf, "<p>%s: %s\r\n", longmsg, cause);
    write(fd, buf, strlen(buf));
    sprintf(buf, "<hr><em>The Proxy server</em>\r\n");
    write(fd, buf, strlen(buf));
}

/*
 * 在缓存里按URL查找，找到了返回目标指针，否则返回NULL
 */
cache_obj *cache_find(char *url)
{
    cache_obj *cur = cache_head;
    while (cur != NULL) {
        /* 如果找到 */
        if (strcmp(cur->url, url) == 0) {
            /* 只有在头部的时候不用处理 */
            if (cur != cache_head) {
                /* 删除该节点、处理前后元素的链接 */
                if (cur->prev) cur->prev->next = cur->next;
                if (cur->next) cur->next->prev = cur->prev;
                if (cur == cache_tail) cache_tail = cur->prev;
                
                /* 把这个节点插入到头部 */
                cur->next = cache_head;
                cur->prev = NULL;
                if (cache_head) cache_head->prev = cur;
                cache_head = cur;
                if (cache_tail == NULL) cache_tail = cur;
            }
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

/*
 * 把一个URL和它对应的响应数据存进缓存
 */
void cache_add(char *url, char *data, size_t len)
{
    /* 如果已经存在了，就直接更新内容 */
    cache_obj *existing = cache_find(url);
    if (existing != NULL) {
        if (existing->content_len != len) {
            char *newdata = malloc(len);
            if (newdata == NULL) return;
            memcpy(newdata, data, len);
            free(existing->content);
            existing->content = newdata;
            existing->content_len = len;
        } else {
            memcpy(existing->content, data, len);
        }
        return;
    }

    /* 缓存已满，直接丢弃 */
    while (cache_count >= MAX_CACHE_SIZE) {
        cache_evict();
    }

    /* 创建新条目并丢弃 */
    cache_obj *new = malloc(sizeof(cache_obj));
    if (new == NULL) return;
    strcpy(new->url, url);
    new->content = malloc(len);
    if (new->content == NULL) {
        free(new);
        return;
    }
    memcpy(new->content, data, len);
    new->content_len = len;

    new->prev = NULL;
    new->next = cache_head;
    if (cache_head) cache_head->prev = new;
    cache_head = new;
    if (cache_tail == NULL) cache_tail = new;
    cache_count++;
}

/*
 * 淘汰在缓存中最近最少使用的条目
 */
void cache_evict(void)
{
    if (cache_tail == NULL) return;
    cache_obj *victim = cache_tail;
    cache_tail = victim->prev;
    if (cache_tail) cache_tail->next = NULL;
    else cache_head = NULL;
    cache_count--;
    free(victim->content);
    free(victim);
}

/*
 * 释放条目的内存
 */
void cache_free_entry(cache_obj *entry)
{
    if (entry) {
        free(entry->content);
        free(entry);
    }
}