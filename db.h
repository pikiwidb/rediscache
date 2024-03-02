#ifndef __DB_H__
#define __DB_H__

#include "object.h"
#include "dict.h"
#include "evict.h"

#ifdef _cplusplus
extern "C" {
#endif

#define LOOKUP_NONE 0
#define LOOKUP_NOTOUCH (1<<0)  /* Don't update LRU. */
#define LOOKUP_NONOTIFY (1<<1) /* Don't trigger keyspace event on key misses. */
#define LOOKUP_NOSTATS (1<<2)  /* Don't update keyspace hits/misses counters. */
#define LOOKUP_WRITE (1<<3)    /* Delete expired keys even in replicas. */
#define LOOKUP_NOEXPIRE (1<<4) /* Avoid deleting lazy expired keys. */
#define LOOKUP_NOEFFECTS (LOOKUP_NONOTIFY | LOOKUP_NOSTATS | LOOKUP_NOTOUCH | LOOKUP_NOEXPIRE) /* Avoid any effects from fetching the key */


// RedisDb 
typedef struct redisDb {
    dict *dict;                                 /* The keyspace for this DB */
    dict *expires;                              /* Timeout of keys with a timeout set */
    struct evictionPoolEntry *eviction_pool;    /* Eviction pool of keys */
} redisDb;

#define SETKEY_KEEPTTL 1
#define SETKEY_NO_SIGNAL 2
#define SETKEY_ALREADY_EXIST 4
#define SETKEY_DOESNT_EXIST 8
#define SETKEY_ADD_OR_UPDATE 16 /* Key most likely doesn't exists */

/* Key flags for how key is removed */
#define DB_FLAG_KEY_NONE 0
#define DB_FLAG_KEY_DELETED (1ULL<<0)
#define DB_FLAG_KEY_EXPIRED (1ULL<<1)
#define DB_FLAG_KEY_EVICTED (1ULL<<2)
#define DB_FLAG_KEY_OVERWRITE (1ULL<<3)

redisDb* createRedisDb(void);
void closeRedisDb(redisDb *db);
robj *lookupKey(redisDb *db, robj *key, int flags);
robj *lookupKeyRead(redisDb *db, robj *key);
robj *lookupKeyWrite(redisDb *db, robj *key);
void dbAdd(redisDb *db, robj *key, robj *val);
void dbOverwrite(redisDb *db, robj *key, robj *val);
void setKey(redisDb *db, robj *key, robj *val, int flags);
int dbExists(redisDb *db, robj *key);
robj *dbRandomKey(redisDb *db);
int dbDelete(redisDb *db, robj *key);
long long emptyDb(redisDb *db, void(callback)(dict*));
int removeExpire(redisDb *db, robj *key);
void setExpire(redisDb *db, robj *key, long long when);
long long getExpire(redisDb *db, robj *key);
int expireIfNeeded(redisDb *db, robj *key, int flags);
int freeMemoryIfNeeded(redisDb *db);
int activeExpireCycle(redisDb *db);
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o);



#ifdef _cplusplus
}
#endif

#endif