#ifndef __REDIS_DB_IF_H__
#define __REDIS_DB_IF_H__

#include <stdint.h>

#ifdef _cplusplus
extern "C" {
#endif

#include "commondef.h"
#include "object.h"
#include "zmalloc.h"

// redis cache handle
typedef void* redisCache;

// hash value
typedef struct _hitem {
    sds field;
    sds value;
    int status;
} hitem;

// zset member
typedef struct _zitem {
    double score;
    sds member;
} zitem;

/*-----------------------------------------------------------------------------
 * Server APIS
 *----------------------------------------------------------------------------*/
void RsSetConfig(db_config* cfg);
redisCache RsCreateCacheHandle(void);
void RsDestroyCacheHandle(redisCache cache);
int RsFreeMemoryIfNeeded(redisCache cache);
int RsActiveExpireCycle(redisCache cache);
size_t RsGetUsedMemory(void);
void RsGetHitAndMissNum(long long *hits, long long *misses);
void RsResetHitAndMissNum(void);

/*-----------------------------------------------------------------------------
 * Normal Commands
 *----------------------------------------------------------------------------*/
int RsExpire(redisCache cache, robj *key, robj *expire);
int RsExpireat(redisCache cache, robj *key, robj *expire);
int RsTTL(redisCache cache, robj *key, int64_t *ttl);
int RsPersist(redisCache cache, robj *key);
int RsType(redisCache cache, robj *key, sds *val);
int RsDel(redisCache cache, robj *key);
int RsExists(redisCache cache, robj *key);
int RsCacheSize(redisCache cache, long long *dbsize);
int RsFlushCache(redisCache cache);
int RsRandomkey(redisCache cache, sds *key);

/*-----------------------------------------------------------------------------
 * String Commands
 *----------------------------------------------------------------------------*/
int RsSet(redisCache cache, robj *key, robj *val, robj *expire);
int RsSetnx(redisCache cache, robj *key, robj *val, robj *expire);
int RsSetxx(redisCache cache, robj *key, robj *val, robj *expire);
int RsGet(redisCache cache, robj *key, robj **val);
int RsIncr(redisCache cache, robj *key, long long *ret);
int RsDecr(redisCache cache, robj *key, long long *ret);
int RsIncrBy(redisCache cache, robj *key, long long incr, long long *ret);
int RsDecrBy(redisCache cache, robj *key, long long incr, long long *ret);
int RsIncrByFloat(redisCache cache, robj *key, long double incr, long double *ret);
int RsAppend(redisCache cache, robj *key, robj *val, unsigned long *ret);
int RsGetRange(redisCache cache, robj *key, long start, long end, sds *val);
int RsSetRange(redisCache cache, robj *key, long start, robj *val, unsigned long *ret);
int RsStrlen(redisCache cache, robj *key, int *val_len);

/*-----------------------------------------------------------------------------
 * Hash type commands
 *----------------------------------------------------------------------------*/
int RsHDel(redisCache cache, robj *key, robj *fields[], unsigned long fields_size, unsigned long *ret);
int RsHSet(redisCache cache, robj *key, robj *field, robj *val);
int RsHSetnx(redisCache cache, robj *key, robj *field, robj *val);
int RsHMSet(redisCache cache, robj *key, robj *items[], unsigned long items_size);
int RsHGet(redisCache cache, robj *key, robj *field, sds *val);
int RsHMGet(redisCache cache, robj *key, hitem *items, unsigned long items_size);
int RsHGetAll(redisCache cache, robj *key, hitem **items, unsigned long *items_size);
int RsHKeys(redisCache cache, robj *key, hitem **items, unsigned long *items_size);
int RsHVals(redisCache cache, robj *key, hitem **items, unsigned long *items_size);
int RsHExists(redisCache cache, robj *key, robj *field, int *is_exist);
int RsHIncrby(redisCache cache, robj *key, robj *field, long long val, long long *ret);
int RsHIncrbyfloat(redisCache cache, robj *key, robj *field, long double val, long double *ret);
int RsHlen(redisCache cache, robj *key, unsigned long *len);
int RsHStrlen(redisCache cache, robj *key, robj *field, unsigned long *len);

/*-----------------------------------------------------------------------------
 * List Commands
 *----------------------------------------------------------------------------*/
int RsLIndex(redisCache cache, robj *key, long index, sds *element);
int RsLInsert(redisCache cache, robj *key, int where, robj *pivot, robj *val);
int RsLLen(redisCache cache, robj *key, unsigned long *len);
int RsLPop(redisCache cache, robj *key, sds *element);
int RsLPush(redisCache cache, robj *key, robj *vals[], unsigned long vals_size);
int RsLPushx(redisCache cache, robj *key, robj *vals[], unsigned long vals_size);
int RsLRange(redisCache cache, robj *key, long start, long end, sds **vals, unsigned long *vals_size);
int RsLRem(redisCache cache, robj *key, long count, robj *val);
int RsLSet(redisCache cache, robj *key, long index, robj *val);
int RsLTrim(redisCache cache, robj *key, long start, long end);
int RsRPop(redisCache cache, robj *key, sds *element);
int RsRPush(redisCache cache, robj *key, robj *vals[], unsigned long vals_size);
int RsRPushx(redisCache cache, robj *key, robj *vals[], unsigned long vals_size);

/*-----------------------------------------------------------------------------
 * Set Commands
 *----------------------------------------------------------------------------*/
int RsSAdd(redisCache cache, robj *key, robj *members[], unsigned long members_size);
int RsSCard(redisCache cache, robj *key, unsigned long *len);
int RsSIsmember(redisCache cache, robj *key, robj *member, int *is_member);
int RsSMembers(redisCache cache, robj *key, sds **members, unsigned long *members_size);
int RsSRem(redisCache cache, robj *key, robj *members[], unsigned long members_size);
int RsSRandmember(redisCache cache, robj *key, long l, sds **members, unsigned long *members_size);

/*-----------------------------------------------------------------------------
 * Sorted set commands
 *----------------------------------------------------------------------------*/
int RsZAdd(redisCache cache, robj *key, robj *items[], unsigned long items_size);
int RsZCard(redisCache cache, robj *key, unsigned long *len);
int RsZCount(redisCache cache, robj *key, robj *min, robj *max, unsigned long *len);
int RsZIncrby(redisCache cache, robj *key, robj *items[], unsigned long items_size);
int RsZrange(redisCache cache, robj *key, long start, long end, zitem **items, unsigned long *items_size);
int RsZRangebyscore(redisCache cache, robj *key, robj *min, robj *max, zitem **items, unsigned long *items_size, long offset, long count);
int RsZRank(redisCache cache, robj *key, robj *member, long *rank);
int RsZRem(redisCache cache, robj *key, robj *members[], unsigned long members_size);
int RsZRemrangebyrank(redisCache cache, robj *key, robj *min, robj *max);
int RsZRemrangebyscore(redisCache cache, robj *key, robj *min, robj *max);
int RsZRevrange(redisCache cache, robj *key, long start, long end, zitem **items, unsigned long *items_size);
int RsZRevrangebyscore(redisCache cache, robj *key, robj *min, robj *max, zitem **items, unsigned long *items_size, long offset, long count);
int RsZRevrangebylex(redisCache cache, robj *key, robj *min, robj *max, sds **members, unsigned long *members_size);
int RsZRevrank(redisCache cache, robj *key, robj *member, long *rank);
int RsZScore(redisCache cache, robj *key, robj *member, double *score);
int RsZRangebylex(redisCache cache, robj *key, robj *min, robj *max, sds **members, unsigned long *members_size);
int RsZLexcount(redisCache cache, robj *key, robj *min, robj *max, unsigned long *len);
int RsZRemrangebylex(redisCache cache, robj *key, robj *min, robj *max);

/*-----------------------------------------------------------------------------
 * Bit Commands
 *----------------------------------------------------------------------------*/
int RsSetBit(redisCache cache, robj *key, size_t bitoffset, long on);
int RsGetBit(redisCache cache, robj *key, size_t bitoffset, long *val);
int RsBitCount(redisCache cache, robj *key, long start, long end, long *val, int have_offset);
int RsBitPos(redisCache cache, robj *key, long bit, long start, long end, long *val, int offset_status);

#ifdef _cplusplus
}
#endif

#endif
