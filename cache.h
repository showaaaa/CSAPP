#include "csapp.h"
/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define CACHE_BLOCK_NUM 10
#define CACHE_OBJS_COUNT 10

typedef struct {
    char cache_obj[MAX_OBJECT_SIZE];
    char cache_url[MAXLINE];
    int LRU;
    int isEmpty;

    pthread_rwlock_t rwlock;
    //int readCnt;            /*count of readers*/
    //sem_t wmutex;           /*protects accesses to cache*/
    //sem_t rdcntmutex;       /*protects accesses to readcnt*/

}cache_block;

typedef struct {
    cache_block cacheobjs[CACHE_OBJS_COUNT];  /*ten cache blocks*/
    int cache_num;
}Cache;
