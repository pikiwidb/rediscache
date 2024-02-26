#ifndef __REDIS_DB_IF_H__
#define __REDIS_DB_IF_H__

#include <stdint.h>

#ifdef _cplusplus
extern "C" {
#endif

#include "commondef.h"
#include "filter_cuckoo.h"
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
void RcSetConfig(db_config* cfg);
redisCache RcCreateCacheHandle(void);
void RcDestroyCacheHandle(redisCache cache);
int RcFreeMemoryIfNeeded(redisCache cache);
int RcActiveExpireCycle(redisCache cache);
size_t RcGetUsedMemory(void);
void RcGetHitAndMissNum(long long *hits, long long *misses);
void RcResetHitAndMissNum(void);

/*-----------------------------------------------------------------------------
 * Normal Commands
 *----------------------------------------------------------------------------*/
int RcExpire(redisCache cache, robj *key, robj *expire);
int RcExpireat(redisCache cache, robj *key, robj *expire);
int RcTTL(redisCache cache, robj *key, int64_t *ttl);
int RcPersist(redisCache cache, robj *key);
int RcType(redisCache cache, robj *key, sds *val);
int RcDel(redisCache cache, robj *key);
int RcExists(redisCache cache, robj *key);
int RcCacheSize(redisCache cache, long long *dbsize);
int RcFlushCache(redisCache cache);
int RcRandomkey(redisCache cache, sds *key);

/*-----------------------------------------------------------------------------
 * String Commands
 *----------------------------------------------------------------------------*/
int RcSet(redisCache cache, robj *key, robj *val, robj *expire);
int RcSetnx(redisCache cache, robj *key, robj *val, robj *expire);
int RcSetxx(redisCache cache, robj *key, robj *val, robj *expire);
int RcGet(redisCache cache, robj *key, robj **val);
int RcIncr(redisCache cache, robj *key, long long *ret);
int RcDecr(redisCache cache, robj *key, long long *ret);
int RcIncrBy(redisCache cache, robj *key, long long incr, long long *ret);
int RcDecrBy(redisCache cache, robj *key, long long incr, long long *ret);
int RcIncrByFloat(redisCache cache, robj *key, long double incr, long double *ret);
int RcAppend(redisCache cache, robj *key, robj *val, unsigned long *ret);
int RcGetRange(redisCache cache, robj *key, long start, long end, sds *val);
int RcSetRange(redisCache cache, robj *key, long start, robj *val, unsigned long *ret);
int RcStrlen(redisCache cache, robj *key, int *val_len);

/*-----------------------------------------------------------------------------
 * Hash type commands
 *----------------------------------------------------------------------------*/
int RcHDel(redisCache cache, robj *key, robj *fields[], unsigned long fields_size, unsigned long *ret);
int RcHSet(redisCache cache, robj *key, robj *field, robj *val);
int RcHSetnx(redisCache cache, robj *key, robj *field, robj *val);
int RcHMSet(redisCache cache, robj *key, robj *items[], unsigned long items_size);
int RcHGet(redisCache cache, robj *key, robj *field, sds *val);
int RcHMGet(redisCache cache, robj *key, hitem *items, unsigned long items_size);
int RcHGetAll(redisCache cache, robj *key, hitem **items, unsigned long *items_size);
int RcHKeys(redisCache cache, robj *key, hitem **items, unsigned long *items_size);
int RcHVals(redisCache cache, robj *key, hitem **items, unsigned long *items_size);
int RcHExists(redisCache cache, robj *key, robj *field, int *is_exist);
int RcHIncrby(redisCache cache, robj *key, robj *field, long long val, long long *ret);
int RcHIncrbyfloat(redisCache cache, robj *key, robj *field, long double val, long double *ret);
int RcHlen(redisCache cache, robj *key, unsigned long *len);
int RcHStrlen(redisCache cache, robj *key, robj *field, unsigned long *len);

/*-----------------------------------------------------------------------------
 * List Commands
 *----------------------------------------------------------------------------*/
int RcLIndex(redisCache cache, robj *key, long index, sds *element);
int RcLInsert(redisCache cache, robj *key, int where, robj *pivot, robj *val);
int RcLLen(redisCache cache, robj *key, unsigned long *len);
int RcLPop(redisCache cache, robj *key, sds *element);
int RcLPush(redisCache cache, robj *key, robj *vals[], unsigned long vals_size);
int RcLPushx(redisCache cache, robj *key, robj *vals[], unsigned long vals_size);
int RcLRange(redisCache cache, robj *key, long start, long end, sds **vals, unsigned long *vals_size);
int RcLRem(redisCache cache, robj *key, long count, robj *val);
int RcLSet(redisCache cache, robj *key, long index, robj *val);
int RcLTrim(redisCache cache, robj *key, long start, long end);
int RcRPop(redisCache cache, robj *key, sds *element);
int RcRPush(redisCache cache, robj *key, robj *vals[], unsigned long vals_size);
int RcRPushx(redisCache cache, robj *key, robj *vals[], unsigned long vals_size);

/*-----------------------------------------------------------------------------
 * Set Commands
 *----------------------------------------------------------------------------*/
int RcSAdd(redisCache cache, robj *key, robj *members[], unsigned long members_size);
int RcSCard(redisCache cache, robj *key, unsigned long *len);
int RcSIsmember(redisCache cache, robj *key, robj *member, int *is_member);
int RcSMembers(redisCache cache, robj *key, sds **members, unsigned long *members_size);
int RcSRem(redisCache cache, robj *key, robj *members[], unsigned long members_size);
int RcSRandmember(redisCache cache, robj *key, long l, sds **members, unsigned long *members_size);

/*-----------------------------------------------------------------------------
 * Sorted set commands
 *----------------------------------------------------------------------------*/
int RcZAdd(redisCache cache, robj *key, robj *items[], unsigned long items_size);
int RcZCard(redisCache cache, robj *key, unsigned long *len);
int RcZCount(redisCache cache, robj *key, robj *min, robj *max, unsigned long *len);
int RcZIncrby(redisCache cache, robj *key, robj *items[], unsigned long items_size);
int RcZrange(redisCache cache, robj *key, long start, long end, zitem **items, unsigned long *items_size);
int RcZRangebyscore(redisCache cache, robj *key, robj *min, robj *max, zitem **items, unsigned long *items_size, long offset, long count);
int RcZRank(redisCache cache, robj *key, robj *member, long *rank);
int RcZRem(redisCache cache, robj *key, robj *members[], unsigned long members_size);
int RcZRemrangebyrank(redisCache cache, robj *key, robj *min, robj *max);
int RcZRemrangebyscore(redisCache cache, robj *key, robj *min, robj *max);
int RcZRevrange(redisCache cache, robj *key, long start, long end, zitem **items, unsigned long *items_size);
int RcZRevrangebyscore(redisCache cache, robj *key, robj *min, robj *max, zitem **items, unsigned long *items_size, long offset, long count);
int RcZRevrangebylex(redisCache cache, robj *key, robj *min, robj *max, sds **members, unsigned long *members_size);
int RcZRevrank(redisCache cache, robj *key, robj *member, long *rank);
int RcZScore(redisCache cache, robj *key, robj *member, double *score);
int RcZRangebylex(redisCache cache, robj *key, robj *min, robj *max, sds **members, unsigned long *members_size);
int RcZLexcount(redisCache cache, robj *key, robj *min, robj *max, unsigned long *len);
int RcZRemrangebylex(redisCache cache, robj *key, robj *min, robj *max);

/*-----------------------------------------------------------------------------
 * Bit Commands
 *----------------------------------------------------------------------------*/
int RcSetBit(redisCache cache, robj *key, size_t bitoffset, long on);
int RcGetBit(redisCache cache, robj *key, size_t bitoffset, long *val);
int RcBitCount(redisCache cache, robj *key, long start, long end, long *val, int have_offset);
int RcBitPos(redisCache cache, robj *key, long bit, long start, long end, long *val, int offset_status);

 /*-----------------------------------------------------------------------------
  * Filter Commands
  *----------------------------------------------------------------------------*/
CuckooFilter* CreateCuckooFilterHandle(filter_config* filter_config);
void DestroyCuckooFilterHandle(CuckooFilter* filter);
CuckooInsertStatus CuckooFilterInsertUnique(CuckooFilter* filter, CuckooHash hash);
CuckooInsertStatus CuckooFilterInsert(CuckooFilter* filter, CuckooHash hash);
int CuckooFilterDelete(CuckooFilter* filter, CuckooHash hash);
int CuckooFilterCheck(const CuckooFilter* filter, CuckooHash hash);
uint64_t CuckooFilterCount(const CuckooFilter* filter, CuckooHash);
void CuckooFilterCompact(CuckooFilter* filter, bool cont);
void CuckooFilterGetInfo(const CuckooFilter* cf, CuckooHash hash, CuckooKey* out);
/**
 * \brief  check whether the setting parameters meet requirements
 * \param cf a pointor point to CuckooFilter
 * \return 0 on success
 */
int CuckooFilterValidateIntegrity(const CuckooFilter *cf);

#ifdef _cplusplus
}
#endif

#endif

