#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>

#include "redis.h"
#include "commondef.h"
#include "commonfunc.h"
#include "zmalloc.h"
#include "object.h"
#include "sds.h"
#include "db.h"
#include "solarisfixes.h"
#include "util.h"

/*-----------------------------------------------------------------------------
 * String Commands
 *----------------------------------------------------------------------------*/

static int checkStringLength(long long size)
{
    return (size > 512*1024*1024) ? C_ERR : C_OK;
}

/* The setGenericCommand() function implements the SET operation with different
 * options and variants. This function is called in order to implement the
 * following commands: SET, SETEX, PSETEX, SETNX.
 *
 * 'flags' changes the behavior of the command (NX or XX, see belove).
 *
 * 'expire' represents an expire to set in form of a Redis object as passed
 * by the user. It is interpreted according to the specified 'unit'.
 *
 * 'ok_reply' and 'abort_reply' is what the function will reply to the client
 * if the operation is performed, or when it is not because of NX or
 * XX flags.
 *
 * If ok_reply is NULL "+OK" is used.
 * If abort_reply is NULL, "$-1" is used. */

#define OBJ_SET_NO_FLAGS 0
#define OBJ_SET_NX (1<<0)     /* Set if key not exists. */
#define OBJ_SET_XX (1<<1)     /* Set if key exists. */
#define OBJ_SET_EX (1<<2)     /* Set if time in seconds is given */
#define OBJ_SET_PX (1<<3)     /* Set if time in ms in given */

static int setGenericCommand(redisCache *redis_cache, robj *kobj, robj *vobj, robj *expire, int unit, int flags) {
    long long milliseconds = 0; /* initialized to avoid any harmness warning */

    if (expire) {
        if (getLongLongFromObject(expire, &milliseconds) != C_OK)
            return REDIS_INVALID_ARG;
        if (milliseconds <= 0) {
            return REDIS_INVALID_ARG;
        }
        if (unit == UNIT_SECONDS) milliseconds *= 1000;
    }

    if ((flags & OBJ_SET_NX && lookupKeyWrite(redis_cache,kobj) != NULL) ||
        (flags & OBJ_SET_XX && lookupKeyWrite(redis_cache,kobj) == NULL)) {
        return C_ERR;
    }
    setKey(redis_cache, kobj, vobj);

    if (expire) setExpire(redis_cache, kobj, mstime()+milliseconds);

    return C_OK;
}

static int incrDecrCommand(redisCache *redis_cache, robj *kobj, long long incr, long long *ret) {
    long long value, oldvalue;
    robj *o, *new;

    o = lookupKeyWrite(redis_cache,kobj);
    if (o != NULL && checkType(o,OBJ_STRING)) return REDIS_INVALID_TYPE;
    if (getLongLongFromObject(o,&value) != C_OK) return REDIS_INVALID_TYPE;

    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {
        return REDIS_OVERFLOW;
    }
    value += incr;
    *ret = value;

    if (o && o->refcount == 1 && o->encoding == OBJ_ENCODING_INT &&
        (value < 0 || value >= OBJ_SHARED_INTEGERS) &&
        value >= LONG_MIN && value <= LONG_MAX)
    {
        new = o;
        o->ptr = (void*)((long)value);
    } else {
        new = createStringObjectFromLongLong(value);
        if (o) {
            dbOverwrite(redis_cache,kobj,new);
        } else {
            dbAdd(redis_cache,kobj,new);
        }
    }

    return C_OK;
}

static int incrbyfloatCommand(redisCache *redis_cache, robj *kobj, long double incr, long double *ret)
{
    long double value;
    robj *o, *new;

    o = lookupKeyWrite(redis_cache,kobj);
    if (o != NULL && checkType(o,OBJ_STRING)) return REDIS_INVALID_TYPE;
    if (getLongDoubleFromObject(o,&value) != C_OK) return REDIS_INVALID_TYPE;

    value += incr;
    *ret = value;
    if (isnan(value) || isinf(value)) {
        return REDIS_OVERFLOW;
    }
    new = createStringObjectFromLongDouble(value, 1);

    if (o)
        dbOverwrite(redis_cache,kobj,new);
    else
        dbAdd(redis_cache,kobj,new);

    return C_OK;
}

static int appendCommand(redisCache *redis_cache, robj *kobj, robj *vobj, unsigned long *ret)
{
    size_t totlen;
    robj *o, *append;

    o = lookupKeyWrite(redis_cache,kobj);
    if (o == NULL) {
        /* Create the key */
        // c->argv[2] = tryObjectEncoding(c->argv[2]);
        dbAdd(redis_cache,kobj,vobj);
        incrRefCount(vobj);
        totlen = stringObjectLen(vobj);
    } else {
        /* Key exists, check type */
        if (checkType(o,OBJ_STRING))
            return REDIS_INVALID_TYPE;

        /* "append" is an argument, so always an sds */
        append = vobj;
        totlen = stringObjectLen(o)+sdslen(append->ptr);
        if (checkStringLength(totlen) != C_OK)
            return REDIS_OVERFLOW;

        /* Append the value */
        o = dbUnshareStringValue(redis_cache,kobj,o);
        o->ptr = sdscatlen(o->ptr,append->ptr,sdslen(append->ptr));
        totlen = sdslen(o->ptr);
    }

    *ret = totlen;

    return C_OK;
}

static int getrangeCommand(redisCache *redis_cache,
                           robj *kobj,
                           long start,
                           long end,
                           sds *val)
{
    robj *o;
    char *str, llbuf[32];
    size_t strlen;

    if ((o = lookupKeyRead(redis_cache, kobj)) == NULL) return REDIS_KEY_NOT_EXIST;
    if (checkType(o,OBJ_STRING)) return REDIS_INVALID_TYPE;

    if (o->encoding == OBJ_ENCODING_INT) {
        str = llbuf;
        strlen = ll2string(llbuf,sizeof(llbuf),(long)o->ptr);
    } else {
        str = o->ptr;
        strlen = sdslen(str);
    }

    /* Convert negative indexes */
    if (start < 0 && end < 0 && start > end) {
        *val = sdsempty();
        return C_OK;
    }

    if (start < 0) start = strlen+start;
    if (end < 0) end = strlen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if ((unsigned)end >= strlen) end = strlen-1;

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * nothing can be returned is: start > end. */
    if (start > end || strlen == 0) {
        *val = sdsempty();
    } else {
        *val = sdsnewlen((char*)str+start, end-start+1);
    }

    return C_OK;
}

static int setrangeCommand(redisCache *redis_cache, robj *kobj, long offset, robj *vobj, unsigned long *ret)
{
    robj *o;
    sds value = vobj->ptr;

    if (offset < 0) return REDIS_INVALID_ARG;

    o = lookupKeyWrite(redis_cache,kobj);
    if (o == NULL) {
        /* Return 0 when setting nothing on a non-existing string */
        if (sdslen(value) == 0) {
            *ret = 0;
            return C_OK;
        }

        /* Return when the resulting string exceeds allowed size */
        if (checkStringLength(offset+sdslen(value)) != C_OK) return REDIS_OVERFLOW;

        o = createObject(OBJ_STRING,sdsempty());
        dbAdd(redis_cache,kobj,o);
    } else {

        /* Key exists, check type */
        if (checkType(o,OBJ_STRING)) return REDIS_INVALID_TYPE;

        /* Return existing string length when setting nothing */
        if (sdslen(value) == 0) {
            *ret = stringObjectLen(o);
            return C_OK;
        }

        /* Return when the resulting string exceeds allowed size */
        if (checkStringLength(offset+sdslen(value)) != C_OK) return REDIS_OVERFLOW;

        /* Create a copy when the object is shared or encoded. */
        o = dbUnshareStringValue(redis_cache,kobj,o);
    }

    if (sdslen(value) > 0) {
        o->ptr = sdsgrowzero(o->ptr,offset+sdslen(value));
        memcpy((char*)o->ptr+offset,value,sdslen(value));
    }
    *ret = sdslen(o->ptr);

    return C_OK;
}

int RcSet(redisCache cache, robj *key, robj *val, robj *expire)
{
    if (NULL == cache || NULL == key || NULL == val) {
        return REDIS_INVALID_ARG;
    }
    redisCache *redis_cache = (redisCache*)cache;

    return setGenericCommand(redis_cache, key, val, expire, UNIT_SECONDS, OBJ_SET_NO_FLAGS);
}

int RcSetnx(redisCache cache, robj *key, robj *val, robj *expire)
{
    if (NULL == cache || NULL == key || NULL == val) {
        return REDIS_INVALID_ARG;
    }
    redisCache *redis_cache = (redisCache*)cache;

    return setGenericCommand(redis_cache, key, val, expire, UNIT_SECONDS, OBJ_SET_NX);;
}

int RcSetxx(redisCache cache, robj *key, robj *val, robj *expire)
{
    if (NULL == cache || NULL == key || NULL == val) {
        return REDIS_INVALID_ARG;
    }
    redisCache *redis_cache = (redisCache*)cache;

    return setGenericCommand(redis_cache, key, val, expire, UNIT_SECONDS, OBJ_SET_XX);;
}

int RcGet(redisCache cache, robj *key, robj **val)
{
    if (NULL == cache || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisCache *redis_cache = (redisCache*)cache;

    robj *vobj = lookupKeyRead(redis_cache, key);
    if (NULL == vobj || OBJ_STRING != vobj->type) {
        return REDIS_KEY_NOT_EXIST;
    }
    *val = vobj;

    return C_OK;
}

int RcIncr(redisCache cache, robj *key, long long *ret)
{
    if (NULL == cache || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisCache *redis_cache = (redisCache*)cache;

    return incrDecrCommand(redis_cache, key, 1, ret);
}

int RcDecr(redisCache cache, robj *key, long long *ret)
{
    if (NULL == cache || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisCache *redis_cache = (redisCache*)cache;

    return incrDecrCommand(redis_cache, key, -1, ret);
}

int RcIncrBy(redisCache cache, robj *key, long long incr, long long *ret)
{
    if (NULL == cache || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisCache *redis_cache = (redisCache*)cache;

    return incrDecrCommand(redis_cache, key, incr, ret);
}

int RcDecrBy(redisCache cache, robj *key, long long incr, long long *ret)
{
    if (NULL == cache || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisCache *redis_cache = (redisCache*)cache;

    return incrDecrCommand(redis_cache, key, incr * (-1), ret);
}

int RcIncrByFloat(redisCache cache, robj *key, long double incr, long double *ret)
{
    if (NULL == cache || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisCache *redis_cache = (redisCache*)cache;

    return incrbyfloatCommand(redis_cache, key, incr, ret);
}

int RcAppend(redisCache cache, robj *key, robj *val, unsigned long *ret)
{
    if (NULL == cache || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisCache *redis_cache = (redisCache*)cache;

    return appendCommand(redis_cache, key, val, ret);
}

int RcGetRange(redisCache cache, robj *key, long start, long end, sds *val)
{
    if (NULL == cache || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisCache *redis_cache = (redisCache*)cache;

    return getrangeCommand(redis_cache, key, start, end, val);
}

int RcSetRange(redisCache cache, robj *key, long start, robj *val, unsigned long *ret)
{
    if (NULL == cache || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisCache *redis_cache = (redisCache*)cache;

    return setrangeCommand(redis_cache, key, start, val, ret);
}

int RcStrlen(redisCache cache, robj *key, int *val_len)
{
    if (NULL == cache || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisCache *redis_cache = (redisCache*)cache;

    robj *vobj = lookupKeyRead(redis_cache, key);
    if (NULL == vobj || OBJ_STRING != vobj->type) {
        return REDIS_KEY_NOT_EXIST;
    }
    *val_len = stringObjectLen(vobj);

    return C_OK;
}
