#ifndef __REDIS_DB_IF_H__
#define __REDIS_DB_IF_H__

#include <stdint.h>

#ifdef _cplusplus
extern "C" {
#endif

#include "commondef.h"
#include "object.h"
#include "zmalloc.h"

// redis db handle
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
redisCache RsCreateDbHandle(void);
void RsDestroyDbHandle(redisCache db);
int RsFreeMemoryIfNeeded(redisCache db);
int RsActiveExpireCycle(redisCache db);
size_t RsGetUsedMemory(void);
void RsGetHitAndMissNum(long long *hits, long long *misses);
void RsResetHitAndMissNum(void);

/*-----------------------------------------------------------------------------
 * Normal Commands
 *----------------------------------------------------------------------------*/
int RsExpire(redisCache db, robj *key, robj *expire);
int RsExpireat(redisCache db, robj *key, robj *expire);
int RsTTL(redisCache db, robj *key, int64_t *ttl);
int RsPersist(redisCache db, robj *key);
int RsType(redisCache db, robj *key, sds *val);
int RsDel(redisCache db, robj *key);
int RsExists(redisCache db, robj *key);
int RsDbSize(redisCache db, long long *dbsize);
int RsFlushDb(redisCache db);
int RsRandomkey(redisCache db, sds *key);

/*-----------------------------------------------------------------------------
 * String Commands
 *----------------------------------------------------------------------------*/
int RsSet(redisCache db, robj *key, robj *val, robj *expire);
int RsSetnx(redisCache db, robj *key, robj *val, robj *expire);
int RsSetxx(redisCache db, robj *key, robj *val, robj *expire);
int RsGet(redisCache db, robj *key, robj **val);
int RsIncr(redisCache db, robj *key, long long *ret);
int RsDecr(redisCache db, robj *key, long long *ret);
int RsIncrBy(redisCache db, robj *key, long long incr, long long *ret);
int RsDecrBy(redisCache db, robj *key, long long incr, long long *ret);
int RsIncrByFloat(redisCache db, robj *key, long double incr, long double *ret);
int RsAppend(redisCache db, robj *key, robj *val, unsigned long *ret);
int RsGetRange(redisCache db, robj *key, long start, long end, sds *val);
int RsSetRange(redisCache db, robj *key, long start, robj *val, unsigned long *ret);
int RsStrlen(redisCache db, robj *key, int *val_len);

/*-----------------------------------------------------------------------------
 * Hash type commands
 *----------------------------------------------------------------------------*/
int RsHDel(redisCache db, robj *key, robj *fields[], unsigned long fields_size, unsigned long *ret);
int RsHSet(redisCache db, robj *key, robj *field, robj *val);
int RsHSetnx(redisCache db, robj *key, robj *field, robj *val);
int RsHMSet(redisCache db, robj *key, robj *items[], unsigned long items_size);
int RsHGet(redisCache db, robj *key, robj *field, sds *val);
int RsHMGet(redisCache db, robj *key, hitem *items, unsigned long items_size);
int RsHGetAll(redisCache db, robj *key, hitem **items, unsigned long *items_size);
int RsHKeys(redisCache db, robj *key, hitem **items, unsigned long *items_size);
int RsHVals(redisCache db, robj *key, hitem **items, unsigned long *items_size);
int RsHExists(redisCache db, robj *key, robj *field, int *is_exist);
int RsHIncrby(redisCache db, robj *key, robj *field, long long val, long long *ret);
int RsHIncrbyfloat(redisCache db, robj *key, robj *field, long double val, long double *ret);
int RsHlen(redisCache db, robj *key, unsigned long *len);
int RsHStrlen(redisCache db, robj *key, robj *field, unsigned long *len);

/*-----------------------------------------------------------------------------
 * List Commands
 *----------------------------------------------------------------------------*/
int RsLIndex(redisCache db, robj *key, long index, sds *element);
int RsLInsert(redisCache db, robj *key, int where, robj *pivot, robj *val);
int RsLLen(redisCache db, robj *key, unsigned long *len);
int RsLPop(redisCache db, robj *key, sds *element);
int RsLPush(redisCache db, robj *key, robj *vals[], unsigned long vals_size);
int RsLPushx(redisCache db, robj *key, robj *vals[], unsigned long vals_size);
int RsLRange(redisCache db, robj *key, long start, long end, sds **vals, unsigned long *vals_size);
int RsLRem(redisCache db, robj *key, long count, robj *val);
int RsLSet(redisCache db, robj *key, long index, robj *val);
int RsLTrim(redisCache db, robj *key, long start, long end);
int RsRPop(redisCache db, robj *key, sds *element);
int RsRPush(redisCache db, robj *key, robj *vals[], unsigned long vals_size);
int RsRPushx(redisCache db, robj *key, robj *vals[], unsigned long vals_size);

/*-----------------------------------------------------------------------------
 * Set Commands
 *----------------------------------------------------------------------------*/
int RsSAdd(redisCache db, robj *key, robj *members[], unsigned long members_size);
int RsSCard(redisCache db, robj *key, unsigned long *len);
int RsSIsmember(redisCache db, robj *key, robj *member, int *is_member);
int RsSMembers(redisCache db, robj *key, sds **members, unsigned long *members_size);
int RsSRem(redisCache db, robj *key, robj *members[], unsigned long members_size);
int RsSRandmember(redisCache db, robj *key, long l, sds **members, unsigned long *members_size);

/*-----------------------------------------------------------------------------
 * Sorted set commands
 *----------------------------------------------------------------------------*/
int RsZAdd(redisCache db, robj *key, robj *items[], unsigned long items_size);
int RsZCard(redisCache db, robj *key, unsigned long *len);
int RsZCount(redisCache db, robj *key, robj *min, robj *max, unsigned long *len);
int RsZIncrby(redisCache db, robj *key, robj *items[], unsigned long items_size);
int RsZrange(redisCache db, robj *key, long start, long end, zitem **items, unsigned long *items_size);
int RsZRangebyscore(redisCache db, robj *key, robj *min, robj *max, zitem **items, unsigned long *items_size, long offset, long count);
int RsZRank(redisCache db, robj *key, robj *member, long *rank);
int RsZRem(redisCache db, robj *key, robj *members[], unsigned long members_size);
int RsZRemrangebyrank(redisCache db, robj *key, robj *min, robj *max);
int RsZRemrangebyscore(redisCache db, robj *key, robj *min, robj *max);
int RsZRevrange(redisCache db, robj *key, long start, long end, zitem **items, unsigned long *items_size);
int RsZRevrangebyscore(redisCache db, robj *key, robj *min, robj *max, zitem **items, unsigned long *items_size, long offset, long count);
int RsZRevrangebylex(redisCache db, robj *key, robj *min, robj *max, sds **members, unsigned long *members_size);
int RsZRevrank(redisCache db, robj *key, robj *member, long *rank);
int RsZScore(redisCache db, robj *key, robj *member, double *score);
int RsZRangebylex(redisCache db, robj *key, robj *min, robj *max, sds **members, unsigned long *members_size);
int RsZLexcount(redisCache db, robj *key, robj *min, robj *max, unsigned long *len);
int RsZRemrangebylex(redisCache db, robj *key, robj *min, robj *max);

/*-----------------------------------------------------------------------------
 * Bit Commands
 *----------------------------------------------------------------------------*/
int RsSetBit(redisCache db, robj *key, size_t bitoffset, long on);
int RsGetBit(redisCache db, robj *key, size_t bitoffset, long *val);
int RsBitCount(redisCache db, robj *key, long start, long end, long *val, int have_offset);
int RsBitPos(redisCache db, robj *key, long bit, long start, long end, long *val, int offset_status);

#ifdef _cplusplus
}
#endif

#endif
