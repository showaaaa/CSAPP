#include <stdio.h>
#include "csapp.h"
#include "sbuf.h"
#include "cache.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define NTHREADS 4
#define SBUFSIZE 16
#define LRU_MAGIC_NUMBER 9999

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *conn_hdr = "Connection: close\r\n";
static const char *prox_hdr = "Proxy-Connection: close\r\n";
static const char *host_hdr_format = "Host: %s\r\n";
static const char *requestlint_hdr_format = "GET %s HTTP/1.1\r\n";
static const char *endof_hdr = "\r\n";

static const char *connection_key = "Connection";
static const char *user_agent_key= "User-Agent";
static const char *proxy_connection_key = "Proxy-Connection";
static const char *host_key = "Host";

sbuf_t sbuf; /* Shared buffer of connected descriptors */

void doit(int fd);
int parse_uri(char *uri, char *hostname, char *path, int *port);
void build_http_header(char *http_header,char *hostname,char *path,int port,rio_t *client_rio);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void *thread(void *vargp);

/*cache function*/
void cache_init();
int cache_find(char *url);
int cache_eviction();
void cache_LRU(int index);
void cache_uri(char *uri,char *buf);

Cache cache;

int main(int argc, char **argv) 
{
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    /* Check command line args */
    if (argc != 2) {
	    fprintf(stderr, "usage: %s <port>\n", argv[0]);
	    exit(1);
    }

    signal(SIGPIPE, SIG_IGN);
    cache_init();
    listenfd = Open_listenfd(argv[1]);
    sbuf_init(&sbuf, SBUFSIZE);
    for(int i = 0; i < NTHREADS;i++){
        Pthread_create(&tid, NULL, thread, NULL);
    }
    while (1) {
	    clientlen = sizeof(clientaddr);
	    connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen); //line:netp:tiny:accept
        Getnameinfo((SA *) &clientaddr, clientlen, hostname, MAXLINE, 
                    port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
	    //doit(connfd);                                             //line:netp:tiny:doit
	    //Close(connfd);                                            //line:netp:tiny:close
        sbuf_insert(&sbuf, connfd);
    }
    sbuf_deinit(&sbuf);
}

/*
 * doit - handle one HTTP request/response transaction
 */
/* $begin doit */
void doit(int fd) 
{
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hostname[MAXLINE], path[MAXLINE];
    int port;
    char to_endserver_http_header[MAXLINE];
    rio_t rio, server_rio;

    /* Read request line and headers */
    Rio_readinitb(&rio, fd);
    if (!Rio_readlineb(&rio, buf, MAXLINE))  //line:netp:doit:readrequest
        //need to modify for crash
        return;
    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, uri, version);       //line:netp:doit:parserequest
    char url_store[100];
    strcpy(url_store,uri);  /*store the original url */
    
    if (strcasecmp(method, "GET")) {                     //line:netp:doit:beginrequesterr
        clienterror(fd, method, "501", "Not Implemented",
                    "Tiny does not implement this method");
        return;
    }
    
    /*the uri is cached ? */
    int cache_index;
    if((cache_index=cache_find(url_store))!=-1){/*in cache then return the cache content*/
         //readerPre(cache_index);

         Rio_writen(fd,cache.cacheobjs[cache_index].cache_obj,strlen(cache.cacheobjs[cache_index].cache_obj));
         //readerAfter(cache_index);
         pthread_rwlock_unlock(&cache.cacheobjs[cache_index].rwlock);
         if(!pthread_rwlock_trywrlock(&cache.cacheobjs[cache_index].rwlock)){
             cache.cacheobjs[cache_index].LRU = LRU_MAGIC_NUMBER;
             pthread_rwlock_unlock(&cache.cacheobjs[cache_index].rwlock);
         }
         return;
    }
                                                        //line:netp:doit:endrequesterr
    //parse uri to get hostname, file path, port
    parse_uri(uri, hostname, path, &port);

    /*build the http header which will send to the end server*/
    build_http_header(to_endserver_http_header,hostname,path,port,&rio);
    printf("new header:%s", to_endserver_http_header);
    //connect to end server
    char portStr[100];
    sprintf(portStr,"%d",port);
    printf("before open\n");
    int end_server_fd = Open_clientfd(hostname,portStr);
    printf("after open\n");
    if(end_server_fd < 0){
        printf("Connection failed\n");
        //clienterror(fd, method, "400", "Bad Request",
                    //"Connection failed");
        return;
    }
    printf("after return\n");
    Rio_readinitb(&server_rio,end_server_fd);
    /*write the http header to endserver*/
    Rio_writen(end_server_fd,to_endserver_http_header,strlen(to_endserver_http_header));
    printf("after writen\n");

    /*receive message from end server and send to the client*/
    char cachebuf[MAX_OBJECT_SIZE];
    int sizebuf = 0;
    int n;
    while((n=Rio_readlineb(&server_rio,buf,MAXLINE))!=0)
    {
        printf("proxy received %d bytes,then send\n",n);
        printf("Received: %s\n", buf);
        sizebuf+=n;
        if(sizebuf < MAX_OBJECT_SIZE)  strcat(cachebuf,buf);
        Rio_writen(fd,buf,n);
        //sleep(10);
    }
    printf("after readlineb\n");
    //sleep(10);
    Close(end_server_fd);
    
    /*store it*/
    if(sizebuf < MAX_OBJECT_SIZE){
        cache_uri(url_store,cachebuf);
    }
}
/* $end doit */

/*
 * parse_uri - parse URI into filename and CGI args
 *             return 0 if dynamic content, 1 if static
 */
/* $begin parse_uri */
int parse_uri(char *uri, char *hostname, char *path, int *port) 
{
    *port = 80;
    char *ptr = strstr(uri, "http://");
    if(ptr == NULL){
        ptr = uri;
    }else{
        ptr += strlen("http://");
    }
    char *ptr1 = strstr(ptr,":");
    if(ptr1 != NULL){
        *ptr1 = '\0';
        sscanf(ptr, "%s", hostname);
        sscanf(ptr1 + 1, "%d%s", port, path);
    }else{
        ptr1 = strstr(ptr, "/");
        if(ptr1 != NULL){
            *ptr1 = '\0';
            sscanf(ptr, "%s", hostname);
            *ptr1 = '/';
            sscanf(ptr1, "%s", path);
        }else{
            sscanf(ptr, "%s", hostname);
        }
    }
    return 0;

}
/* $end parse_uri */

void build_http_header(char *http_header,char *hostname,char *path,int port,rio_t *client_rio)
{
    char buf[MAXLINE],request_hdr[MAXLINE],other_hdr[MAXLINE],host_hdr[MAXLINE];
    /*request line*/
    sprintf(request_hdr,requestlint_hdr_format,path);
    /*get other request header for client rio and change it */
    while(Rio_readlineb(client_rio,buf,MAXLINE)>0)
    {
        if(strcmp(buf,endof_hdr)==0) break;/*EOF*/

        if(!strncasecmp(buf,host_key,strlen(host_key)))/*Host:*/
        {
            strcpy(host_hdr,buf);
            continue;
        }

        if(!strncasecmp(buf,connection_key,strlen(connection_key))
                &&!strncasecmp(buf,proxy_connection_key,strlen(proxy_connection_key))
                &&!strncasecmp(buf,user_agent_key,strlen(user_agent_key)))
        {
            strcat(other_hdr,buf);
        }
    }
    if(strlen(host_hdr)==0)
    {
        sprintf(host_hdr,host_hdr_format,hostname);
    }
    
    sprintf(http_header,"%s%s%s%s%s%s%s",
            request_hdr,
            host_hdr,
            conn_hdr,
            prox_hdr,
            user_agent_hdr,
            other_hdr,
            endof_hdr);

    return ;
}

/*
 * clienterror - returns an error message to the client
 */
/* $begin clienterror */
void clienterror(int fd, char *cause, char *errnum, 
		 char *shortmsg, char *longmsg) 
{
    char buf[MAXLINE];

    /* Print the HTTP response headers */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n\r\n");
    Rio_writen(fd, buf, strlen(buf));

    /* Print the HTTP response body */
    sprintf(buf, "<html><title>Tiny Error</title>");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<body bgcolor=""ffffff"">\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "%s: %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<p>%s: %s\r\n", longmsg, cause);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<hr><em>The Tiny Web server</em>\r\n");
    Rio_writen(fd, buf, strlen(buf));
}
/* $end clienterror */

void *thread(void *vargp){
    Pthread_detach(pthread_self());
    while(1){
        int connfd = sbuf_remove(&sbuf);
        doit(connfd);
	    Close(connfd); 
    }
}

void cache_init(){
    cache.cache_num = 0;
    int i;
    for(i=0;i<CACHE_OBJS_COUNT;i++){
        cache.cacheobjs[i].LRU = 0;
        cache.cacheobjs[i].isEmpty = 1;
        //Sem_init(&cache.cacheobjs[i].wmutex,0,1);
        //Sem_init(&cache.cacheobjs[i].rdcntmutex,0,1);
        //cache.cacheobjs[i].readCnt = 0;
        //初始化读写锁
        //pthread_rwlock_init(&myrwlock, NULL);
        pthread_rwlock_init(&cache.cacheobjs[i].rwlock, NULL);
    }
}
/*
void readerPre(int i){
    P(&cache.cacheobjs[i].rdcntmutex);
    cache.cacheobjs[i].readCnt++;
    if(cache.cacheobjs[i].readCnt==1) P(&cache.cacheobjs[i].wmutex);
    V(&cache.cacheobjs[i].rdcntmutex);
}

void readerAfter(int i){
    P(&cache.cacheobjs[i].rdcntmutex);
    cache.cacheobjs[i].readCnt--;
    if(cache.cacheobjs[i].readCnt==0) V(&cache.cacheobjs[i].wmutex);
    V(&cache.cacheobjs[i].rdcntmutex);

}

void writePre(int i){
    P(&cache.cacheobjs[i].wmutex);
}

void writeAfter(int i){
    V(&cache.cacheobjs[i].wmutex);
}
*/

/*find url is in the cache or not */
int cache_find(char *url){
    int i;
    for(i=0;i<CACHE_OBJS_COUNT;i++){
        //readerPre(i);
        //pthread_rwlock_rdlock(&cache[i].rwlock);
        pthread_rwlock_rdlock(&cache.cacheobjs[i].rwlock);
        if((cache.cacheobjs[i].isEmpty==0) && (strcmp(url,cache.cacheobjs[i].cache_url)==0)) break;
        pthread_rwlock_unlock(&cache.cacheobjs[i].rwlock);
    }
    if(i>=CACHE_OBJS_COUNT) return -1; /*can not find url in the cache*/
    return i;
}

/*find the empty cacheObj or which cacheObj should be evictioned*/
int cache_eviction(){
    int min = LRU_MAGIC_NUMBER;
    int minindex = 0;
    int i;
    for(i=0; i<CACHE_OBJS_COUNT; i++)
    {
        pthread_rwlock_rdlock(&cache.cacheobjs[i].rwlock);
        if(cache.cacheobjs[i].isEmpty == 1){/*choose if cache block empty */
            minindex = i;
            pthread_rwlock_unlock(&cache.cacheobjs[i].rwlock);
            break;
        }
        if(cache.cacheobjs[i].LRU< min){    /*if not empty choose the min LRU*/
            minindex = i;
            pthread_rwlock_unlock(&cache.cacheobjs[i].rwlock);
            continue;
        }
        pthread_rwlock_unlock(&cache.cacheobjs[i].rwlock);
    }

    return minindex;
}
/*update the LRU number except the new cache one*/
void cache_LRU(int index){
    int i;
    for(i = 0; i<index; i++)    {
        //writePre(i);
        pthread_rwlock_wrlock(&cache.cacheobjs[i].rwlock);
        if(cache.cacheobjs[i].isEmpty==0 && i!=index){
            cache.cacheobjs[i].LRU--;
        }
        //writeAfter(i);
        pthread_rwlock_unlock(&cache.cacheobjs[i].rwlock);
    }
    i++;
    for(; i<CACHE_OBJS_COUNT; i++)    {
        //writePre(i);
        pthread_rwlock_wrlock(&cache.cacheobjs[i].rwlock);
        if(cache.cacheobjs[i].isEmpty==0 && i!=index){
            cache.cacheobjs[i].LRU--;
        }
        //writeAfter(i);
        pthread_rwlock_unlock(&cache.cacheobjs[i].rwlock);
    }
}
/*cache the uri and content in cache*/
void cache_uri(char *uri,char *buf){


    int i = cache_eviction();

    //writePre(i);
    pthread_rwlock_wrlock(&cache.cacheobjs[i].rwlock);

    strcpy(cache.cacheobjs[i].cache_obj,buf);
    strcpy(cache.cacheobjs[i].cache_url,uri);
    cache.cacheobjs[i].isEmpty = 0;
    cache.cacheobjs[i].LRU = LRU_MAGIC_NUMBER;
    cache_LRU(i);

    //writeAfter(i);
    pthread_rwlock_unlock(&cache.cacheobjs[i].rwlock);
}
