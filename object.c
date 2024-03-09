/* Redis Object implementation.
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */



#include "intset.h"  /* Compact integer set structure */

#include <string.h>
#include <limits.h>
#include <assert.h>
#include "object.h"
#include "commondef.h"
#include "zmalloc.h"
#include "util.h"
#include "dict.h"
#include "quicklist.h"
#include "zset.h"
#include "evict.h"
#include "listpack.h"
#include "stream.h"

#ifdef __CYGWIN__
#define strtold(a,b) ((long double)strtod((a),(b)))
#endif

extern db_config g_db_config;
extern db_status g_db_status;


/* Set dictionary type. Keys are SDS strings, values are ot used. */
dictType setDictType = {
        dictSdsHash,               /* hash function */
        NULL,                      /* key dup */
        NULL,                      /* val dup */
        dictSdsKeyCompare,         /* key compare */
        dictSdsDestructor,         /* key destructor */
        NULL                       /* val destructor */
};

/* Sorted sets hash (note: a skiplist is used in addition to the hash table) */
dictType zsetDictType = {
        dictSdsHash,               /* hash function */
        NULL,                      /* key dup */
        NULL,                      /* val dup */
        dictSdsKeyCompare,         /* key compare */
        NULL,                      /* Note: SDS string shared & freed by skiplist */
        NULL                       /* val destructor */
};

/* ===================== Creation and parsing of objects ==================== */

robj *createObject(int type, void *ptr) {
    robj *o = zmalloc(sizeof(*o));
    o->type = type;
    o->encoding = OBJ_ENCODING_RAW;
    o->ptr = ptr;
    o->refcount = 1;
    o->lru = 0;
    return o;
}

void initObjectLRUOrLFU(robj *o) {
    if (o->refcount == OBJ_SHARED_REFCOUNT)
        return;
    /* Set the LRU to the current lruclock (minutes resolution), or
     * alternatively the LFU counter. */
    if (g_db_config.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        o->lru = (LFUGetTimeInMinutes() << 8) | LFU_INIT_VAL;
    } else {
        o->lru = LRU_CLOCK();
    }
    return;
}

/* Set a special refcount in the object to make it "shared":
 * incrRefCount and decrRefCount() will test for this special refcount
 * and will not touch the object. This way it is free to access shared
 * objects such as small integers from different threads without any
 * mutex.
 *
 * A common pattern to create shared objects:
 *
 * robj *myobject = makeObjectShared(createObject(...));
 *
 */
robj *makeObjectShared(robj *o) {
    assert(o->refcount == 1);
    o->refcount = OBJ_SHARED_REFCOUNT;
    return o;
}

/* Create a string object with encoding OBJ_ENCODING_RAW, that is a plain
 * string object where o->ptr points to a proper sds string. */
robj *createRawStringObject(const char *ptr, size_t len) {
    return createObject(OBJ_STRING, sdsnewlen(ptr, len));
}

/* Create a string object with encoding OBJ_ENCODING_EMBSTR, that is
 * an object where the sds string is actually an unmodifiable string
 * allocated in the same chunk as the object itself. */
robj *createEmbeddedStringObject(const char *ptr, size_t len) {
    robj *o = zmalloc(sizeof(robj) + sizeof(struct sdshdr8) + len + 1);
    struct sdshdr8 *sh = (void *) (o + 1);

    o->type = OBJ_STRING;
    o->encoding = OBJ_ENCODING_EMBSTR;
    o->ptr = sh + 1;
    o->refcount = 1;
    o->lru = 0;

    sh->len = len;
    sh->alloc = len;
    sh->flags = SDS_TYPE_8;
    if (ptr == SDS_NOINIT)
        sh->buf[len] = '\0';
    else if (ptr) {
        memcpy(sh->buf, ptr, len);
        sh->buf[len] = '\0';
    } else {
        memset(sh->buf, 0, len + 1);
    }
    return o;
}

/* Create a string object with EMBSTR encoding if it is smaller than
 * OBJ_ENCODING_EMBSTR_SIZE_LIMIT, otherwise the RAW encoding is
 * used.
 *
 * The current limit of 44 is chosen so that the biggest string object
 * we allocate as EMBSTR will still fit into the 64 byte arena of jemalloc. */
#define OBJ_ENCODING_EMBSTR_SIZE_LIMIT 44

robj *createStringObject(const char *ptr, size_t len) {
    if (len <= OBJ_ENCODING_EMBSTR_SIZE_LIMIT)
        return createEmbeddedStringObject(ptr, len);
    else
        return createRawStringObject(ptr, len);
}

/* Same as CreateRawStringObject, can return NULL if allocation fails */
robj *tryCreateRawStringObject(const char *ptr, size_t len) {
    sds str = sdstrynewlen(ptr, len);
    if (!str) return NULL;
    return createObject(OBJ_STRING, str);
}

/* Same as createStringObject, can return NULL if allocation fails */
robj *tryCreateStringObject(const char *ptr, size_t len) {
    if (len <= OBJ_ENCODING_EMBSTR_SIZE_LIMIT)
        return createEmbeddedStringObject(ptr, len);
    else
        return tryCreateRawStringObject(ptr, len);
}

/* Create a string object from a long long value according to the specified flag. */
#define LL2STROBJ_AUTO 0       /* automatically create the optimal string object */
#define LL2STROBJ_NO_SHARED 1  /* disallow shared objects */
#define LL2STROBJ_NO_INT_ENC 2 /* disallow integer encoded objects. */

robj *createStringObjectFromLongLongWithOptions(long long value, int flag) {
    robj *o;
    if ((value >= LONG_MIN && value <= LONG_MAX) && flag != LL2STROBJ_NO_INT_ENC) {
        o = createObject(OBJ_STRING, NULL);
        o->encoding = OBJ_ENCODING_INT;
        o->ptr = (void *) ((long) value);
    } else {
        char buf[LONG_STR_SIZE];
        int len = ll2string(buf, sizeof(buf), value);
        o = createStringObject(buf, len);
    }

    return o;
}

/* Wrapper for createStringObjectFromLongLongWithOptions() always demanding
 * to create a shared object if possible. */
robj *createStringObjectFromLongLong(long long value) {
    return createStringObjectFromLongLongWithOptions(value, LL2STROBJ_AUTO);
}

/* The function avoids returning a shared integer when LFU/LRU info
 * are needed, that is, when the object is used as a value in the key
 * space(for instance when the INCR command is used), and Redis is
 * configured to evict based on LFU/LRU, so we want LFU/LRU values
 * specific for each key. */
robj *createStringObjectFromLongLongForValue(long long value) {
    if (g_db_config.maxmemory == 0 || !(g_db_config.maxmemory_policy & MAXMEMORY_FLAG_NO_SHARED_INTEGERS)) {
        /* If the maxmemory policy permits, we can still return shared integers */
        return createStringObjectFromLongLongWithOptions(value, LL2STROBJ_AUTO);
    } else {
        return createStringObjectFromLongLongWithOptions(value, LL2STROBJ_NO_SHARED);
    }
}

/* Create a string object that contains an sds inside it. That means it can't be
 * integer encoded (OBJ_ENCODING_INT), and it'll always be an EMBSTR type. */
robj *createStringObjectFromLongLongWithSds(long long value) {
    return createStringObjectFromLongLongWithOptions(value, LL2STROBJ_NO_INT_ENC);
}

/* Create a string object from a long double. If humanfriendly is non-zero
 * it does not use exponential format and trims trailing zeroes at the end,
 * however this results in loss of precision. Otherwise exp format is used
 * and the output of snprintf() is not modified.
 *
 * The 'humanfriendly' option is used for INCRBYFLOAT and HINCRBYFLOAT. */
robj *createStringObjectFromLongDouble(long double value, int humanfriendly) {
    char buf[MAX_LONG_DOUBLE_CHARS];
    int len = ld2string(buf, sizeof(buf), value, humanfriendly ? LD_STR_HUMAN : LD_STR_AUTO);
    return createStringObject(buf, len);
}

/* Duplicate a string object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * This function also guarantees that duplicating a small integer object
 * (or a string object that contains a representation of a small integer)
 * will always result in a fresh object that is unshared (refcount == 1).
 *
 * The resulting object always has refcount set to 1. */
robj *dupStringObject(const robj *o) {
    robj *d;

    assert(o->type == OBJ_STRING);

    switch (o->encoding) {
        case OBJ_ENCODING_RAW:
            return createRawStringObject(o->ptr, sdslen(o->ptr));
        case OBJ_ENCODING_EMBSTR:
            return createEmbeddedStringObject(o->ptr, sdslen(o->ptr));
        case OBJ_ENCODING_INT:
            d = createObject(OBJ_STRING, NULL);
            d->encoding = OBJ_ENCODING_INT;
            d->ptr = o->ptr;
            return d;
        default:
            return NULL;
    }
}

robj *createQuicklistObject(void) {
    quicklist *l = quicklistCreate();
    robj *o = createObject(OBJ_LIST, l);
    o->encoding = OBJ_ENCODING_QUICKLIST;
    return o;
}

robj *createListListpackObject(void) {
    unsigned char *lp = lpNew(0);
    robj *o = createObject(OBJ_LIST, lp);
    o->encoding = OBJ_ENCODING_LISTPACK;
    return o;
}

robj *createSetObject(void) {
    dict *d = dictCreate(&setDictType);
    robj *o = createObject(OBJ_SET, d);
    o->encoding = OBJ_ENCODING_HT;
    return o;
}

robj *createIntsetObject(void) {
    intset *is = intsetNew();
    robj *o = createObject(OBJ_SET, is);
    o->encoding = OBJ_ENCODING_INTSET;
    return o;
}

robj *createSetListpackObject(void) {
    unsigned char *lp = lpNew(0);
    robj *o = createObject(OBJ_SET, lp);
    o->encoding = OBJ_ENCODING_LISTPACK;
    return o;
}

robj *createHashObject(void) {
    unsigned char *zl = lpNew(0);
    robj *o = createObject(OBJ_HASH, zl);
    o->encoding = OBJ_ENCODING_LISTPACK;
    return o;
}

robj *createZsetObject(void) {
    zset *zs = zmalloc(sizeof(*zs));
    robj *o;

    zs->dict = dictCreate(&zsetDictType);
    zs->zsl = zslCreate();
    o = createObject(OBJ_ZSET, zs);
    o->encoding = OBJ_ENCODING_SKIPLIST;
    return o;
}

robj *createZsetListpackObject(void) {
    unsigned char *lp = lpNew(0);
    robj *o = createObject(OBJ_ZSET, lp);
    o->encoding = OBJ_ENCODING_LISTPACK;
    return o;
}

robj *createStreamObject(void) {
    stream *s = streamNew();
    robj *o = createObject(OBJ_STREAM, s);
    o->encoding = OBJ_ENCODING_STREAM;
    return o;
}

//robj *createModuleObject(moduleType *mt, void *value) {
//    moduleValue *mv = zmalloc(sizeof(*mv));
//    mv->type = mt;
//    mv->value = value;
//    return createObject(OBJ_MODULE, mv);
//}

void freeStringObject(robj *o) {
    if (o->encoding == OBJ_ENCODING_RAW) {
        sdsfree(o->ptr);
    }
}

void freeListObject(robj *o) {
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistRelease(o->ptr);
    } else if (o->encoding == OBJ_ENCODING_LISTPACK) {
        lpFree(o->ptr);
    } else {
//        serverPanic("Unknown list encoding type");
    }
}

void freeSetObject(robj *o) {
    switch (o->encoding) {
        case OBJ_ENCODING_HT:
            dictRelease((dict *) o->ptr);
            break;
        case OBJ_ENCODING_INTSET:
        case OBJ_ENCODING_LISTPACK:
            zfree(o->ptr);
            break;
        default:
//            serverPanic("Unknown set encoding type");
        break;
    }
}

void freeZsetObject(robj *o) {
    zset *zs;
    switch (o->encoding) {
        case OBJ_ENCODING_SKIPLIST:
            zs = o->ptr;
            dictRelease(zs->dict);
            zslFree(zs->zsl);
            zfree(zs);
            break;
        case OBJ_ENCODING_LISTPACK:
            zfree(o->ptr);
            break;
        default:
//            serverPanic("Unknown sorted set encoding");
            break;
    }
}

void freeHashObject(robj *o) {
    switch (o->encoding) {
        case OBJ_ENCODING_HT:
            dictRelease((dict *) o->ptr);
            break;
        case OBJ_ENCODING_LISTPACK:
            lpFree(o->ptr);
            break;
        default:
//            serverPanic("Unknown hash encoding type");
            break;
    }
}

//void freeModuleObject(robj *o) {
//    moduleValue *mv = o->ptr;
//    mv->type->free(mv->value);
//    zfree(mv);
//}

void freeStreamObject(robj *o) {
    freeStream(o->ptr);
}

void incrRefCount(robj *o) {
    if (o->refcount < OBJ_FIRST_SPECIAL_REFCOUNT) {
        o->refcount++;
    } else {
        if (o->refcount == OBJ_SHARED_REFCOUNT) {
            /* Nothing to do: this refcount is immutable. */
        } else if (o->refcount == OBJ_STATIC_REFCOUNT) {
//            serverPanic("You tried to retain an object allocated in the stack");
        }
    }
}

void decrRefCount(robj *o) {
    if (o->refcount == 1) {
        switch (o->type) {
            case OBJ_STRING:
                freeStringObject(o);
                break;
            case OBJ_LIST:
                freeListObject(o);
                break;
            case OBJ_SET:
                freeSetObject(o);
                break;
            case OBJ_ZSET:
                freeZsetObject(o);
                break;
            case OBJ_HASH:
                freeHashObject(o);
                break;
//            case OBJ_MODULE:
//                freeModuleObject(o);
//                break;
            case OBJ_STREAM:
                freeStreamObject(o);
                break;
            default:
//                serverPanic("Unknown object type");
                break;
        }
        zfree(o);
    } else {
        if (o->refcount <= 0) return; //serverPanic("decrRefCount against refcount <= 0");
        if (o->refcount != OBJ_SHARED_REFCOUNT) o->refcount--;
    }
}


/* This variant of decrRefCount() gets its argument as void, and is useful
 * as free method in data structures that expect a 'void free_object(void*)'
 * prototype for the free method. */
void decrRefCountVoid(void *o) {
    decrRefCount(o);
}

int checkType(robj *o, int type) {
    /* A NULL is considered an empty key */
    if (o && o->type != type) {
        return 1;
    }
    return 0;
}

int isSdsRepresentableAsLongLong(sds s, long long *llval) {
    return string2ll(s, sdslen(s), llval) ? C_OK : C_ERR;
}

int isObjectRepresentableAsLongLong(robj *o, long long *llval) {
    assert(o->type == OBJ_STRING);
    if (o->encoding == OBJ_ENCODING_INT) {
        if (llval) *llval = (long) o->ptr;
        return C_OK;
    } else {
        return isSdsRepresentableAsLongLong(o->ptr, llval);
    }
}

/* Optimize the SDS string inside the string object to require little space,
 * in case there is more than 10% of free space at the end of the SDS. */
//void trimStringObjectIfNeeded(robj *o, int trim_small_values) {
//    if (o->encoding != OBJ_ENCODING_RAW) return;
//    /* A string may have free space in the following cases:
//     * 1. When an arg len is greater than PROTO_MBULK_BIG_ARG the query buffer may be used directly as the SDS string.
//     * 2. When utilizing the argument caching mechanism in Lua.
//     * 3. When calling from RM_TrimStringAllocation (trim_small_values is true). */
//    size_t len = sdslen(o->ptr);
//    if (len >= PROTO_MBULK_BIG_ARG ||
//        trim_small_values ||
//        (server.executing_client && server.executing_client->flags & CLIENT_SCRIPT && len < LUA_CMD_OBJCACHE_MAX_LEN)) {
//        if (sdsavail(o->ptr) > len / 10) {
//            o->ptr = sdsRemoveFreeSpace(o->ptr, 0);
//        }
//    }
//}
//
///* Try to encode a string object in order to save space */
//robj *tryObjectEncodingEx(robj *o, int try_trim) {
//    long value;
//    sds s = o->ptr;
//    size_t len;
//
//    /* Make sure this is a string object, the only type we encode
//     * in this function. Other types use encoded memory efficient
//     * representations but are handled by the commands implementing
//     * the type. */
//    serverAssertWithInfo(NULL, o, o->type == OBJ_STRING);
//
//    /* We try some specialized encoding only for objects that are
//     * RAW or EMBSTR encoded, in other words objects that are still
//     * in represented by an actually array of chars. */
//    if (!sdsEncodedObject(o)) return o;
//
//    /* It's not safe to encode shared objects: shared objects can be shared
//     * everywhere in the "object space" of Redis and may end in places where
//     * they are not handled. We handle them only as values in the keyspace. */
//    if (o->refcount > 1) return o;
//
//    /* Check if we can represent this string as a long integer.
//     * Note that we are sure that a string larger than 20 chars is not
//     * representable as a 32 nor 64 bit integer. */
//    len = sdslen(s);
//    if (len <= 20 && string2l(s, len, &value)) {
//        /* This object is encodable as a long. Try to use a shared object.
//         * Note that we avoid using shared integers when maxmemory is used
//         * because every object needs to have a private LRU field for the LRU
//         * algorithm to work well. */
//        if ((server.maxmemory == 0 ||
//             !(server.maxmemory_policy & MAXMEMORY_FLAG_NO_SHARED_INTEGERS)) &&
//            value >= 0 &&
//            value < OBJ_SHARED_INTEGERS) {
//            decrRefCount(o);
//            return shared.integers[value];
//        } else {
//            if (o->encoding == OBJ_ENCODING_RAW) {
//                sdsfree(o->ptr);
//                o->encoding = OBJ_ENCODING_INT;
//                o->ptr = (void *) value;
//                return o;
//            } else if (o->encoding == OBJ_ENCODING_EMBSTR) {
//                decrRefCount(o);
//                return createStringObjectFromLongLongForValue(value);
//            }
//        }
//    }
//
//    /* If the string is small and is still RAW encoded,
//     * try the EMBSTR encoding which is more efficient.
//     * In this representation the object and the SDS string are allocated
//     * in the same chunk of memory to save space and cache misses. */
//    if (len <= OBJ_ENCODING_EMBSTR_SIZE_LIMIT) {
//        robj *emb;
//
//        if (o->encoding == OBJ_ENCODING_EMBSTR) return o;
//        emb = createEmbeddedStringObject(s, sdslen(s));
//        decrRefCount(o);
//        return emb;
//    }
//
//    /* We can't encode the object...
//     * Do the last try, and at least optimize the SDS string inside */
//    if (try_trim)
//        trimStringObjectIfNeeded(o, 0);
//
//    /* Return the original object. */
//    return o;
//}

//robj *tryObjectEncoding(robj *o) {
//    return tryObjectEncodingEx(o, 1);
//}

/* Get a decoded version of an encoded object (returned as a new object).
 * If the object is already raw-encoded just increment the ref count. */
robj *getDecodedObject(robj *o) {
    robj *dec;

    if (sdsEncodedObject(o)) {
        incrRefCount(o);
        return o;
    }
    if (o->type == OBJ_STRING && o->encoding == OBJ_ENCODING_INT) {
        char buf[32];

        ll2string(buf, 32, (long) o->ptr);
        dec = createStringObject(buf, strlen(buf));
        return dec;
    } else {
//        serverPanic("Unknown encoding type");
        return NULL;
    }
}

/* Compare two string objects via strcmp() or strcoll() depending on flags.
 * Note that the objects may be integer-encoded. In such a case we
 * use ll2string() to get a string representation of the numbers on the stack
 * and compare the strings, it's much faster than calling getDecodedObject().
 *
 * Important note: when REDIS_COMPARE_BINARY is used a binary-safe comparison
 * is used. */

#define REDIS_COMPARE_BINARY (1<<0)
#define REDIS_COMPARE_COLL (1<<1)

int compareStringObjectsWithFlags(const robj *a, const robj *b, int flags) {
    assert(a->type == OBJ_STRING && b->type == OBJ_STRING);
    char bufa[128], bufb[128], *astr, *bstr;
    size_t alen, blen, minlen;

    if (a == b) return 0;
    if (sdsEncodedObject(a)) {
        astr = a->ptr;
        alen = sdslen(astr);
    } else {
        alen = ll2string(bufa, sizeof(bufa), (long) a->ptr);
        astr = bufa;
    }
    if (sdsEncodedObject(b)) {
        bstr = b->ptr;
        blen = sdslen(bstr);
    } else {
        blen = ll2string(bufb, sizeof(bufb), (long) b->ptr);
        bstr = bufb;
    }
    if (flags & REDIS_COMPARE_COLL) {
        return strcoll(astr, bstr);
    } else {
        int cmp;

        minlen = (alen < blen) ? alen : blen;
        cmp = memcmp(astr, bstr, minlen);
        if (cmp == 0) return alen - blen;
        return cmp;
    }
}

/* Wrapper for compareStringObjectsWithFlags() using binary comparison. */
int compareStringObjects(const robj *a, const robj *b) {
    return compareStringObjectsWithFlags(a, b, REDIS_COMPARE_BINARY);
}

/* Wrapper for compareStringObjectsWithFlags() using collation. */
int collateStringObjects(const robj *a, const robj *b) {
    return compareStringObjectsWithFlags(a, b, REDIS_COMPARE_COLL);
}

/* Equal string objects return 1 if the two objects are the same from the
 * point of view of a string comparison, otherwise 0 is returned. Note that
 * this function is faster then checking for (compareStringObject(a,b) == 0)
 * because it can perform some more optimization. */
int equalStringObjects(robj *a, robj *b) {
    if (a->encoding == OBJ_ENCODING_INT &&
        b->encoding == OBJ_ENCODING_INT) {
        /* If both strings are integer encoded just check if the stored
         * long is the same. */
        return a->ptr == b->ptr;
    } else {
        return compareStringObjects(a, b) == 0;
    }
}

size_t stringObjectLen(robj *o) {
    assert(o->type == OBJ_STRING);
    if (sdsEncodedObject(o)) {
        return sdslen(o->ptr);
    } else {
        return sdigits10((long) o->ptr);
    }
}

int getDoubleFromObject(const robj *o, double *target) {
    double value;

    if (o == NULL) {
        value = 0;
    } else {
        assert(o->type == OBJ_STRING);
        if (sdsEncodedObject(o)) {
            if (!string2d(o->ptr, sdslen(o->ptr), &value))
                return C_ERR;
        } else if (o->encoding == OBJ_ENCODING_INT) {
            value = (long) o->ptr;
        } else {
//            serverPanic("Unknown string encoding");
        }
    }
    *target = value;
    return C_OK;
}

//int getDoubleFromObjectOrReply(client *c, robj *o, double *target, const char *msg) {
//    double value;
//    if (getDoubleFromObject(o, &value) != C_OK) {
//        if (msg != NULL) {
//            addReplyError(c, (char *) msg);
//        } else {
//            addReplyError(c, "value is not a valid float");
//        }
//        return C_ERR;
//    }
//    *target = value;
//    return C_OK;
//}

int getLongDoubleFromObject(robj *o, long double *target) {
    long double value;

    if (o == NULL) {
        value = 0;
    } else {
        assert(o->type == OBJ_STRING);
        if (sdsEncodedObject(o)) {
            if (!string2ld(o->ptr, sdslen(o->ptr), &value))
                return C_ERR;
        } else if (o->encoding == OBJ_ENCODING_INT) {
            value = (long) o->ptr;
        } else {
//            serverPanic("Unknown string encoding");
        }
    }
    *target = value;
    return C_OK;
}

//int getLongDoubleFromObjectOrReply(client *c, robj *o, long double *target, const char *msg) {
//    long double value;
//    if (getLongDoubleFromObject(o, &value) != C_OK) {
//        if (msg != NULL) {
//            addReplyError(c, (char *) msg);
//        } else {
//            addReplyError(c, "value is not a valid float");
//        }
//        return C_ERR;
//    }
//    *target = value;
//    return C_OK;
//}

int getLongLongFromObject(robj *o, long long *target) {
    long long value;

    if (o == NULL) {
        value = 0;
    } else {
        assert(o->type == OBJ_STRING);
        if (sdsEncodedObject(o)) {
            if (string2ll(o->ptr, sdslen(o->ptr), &value) == 0) return C_ERR;
        } else if (o->encoding == OBJ_ENCODING_INT) {
            value = (long) o->ptr;
        } else {
//            serverPanic("Unknown string encoding");
        }
    }
    if (target) *target = value;
    return C_OK;
}

//int getLongLongFromObjectOrReply(client *c, robj *o, long long *target, const char *msg) {
//    long long value;
//    if (getLongLongFromObject(o, &value) != C_OK) {
//        if (msg != NULL) {
//            addReplyError(c, (char *) msg);
//        } else {
//            addReplyError(c, "value is not an integer or out of range");
//        }
//        return C_ERR;
//    }
//    *target = value;
//    return C_OK;
//}

//int getLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg) {
//    long long value;
//
//    if (getLongLongFromObjectOrReply(c, o, &value, msg) != C_OK) return C_ERR;
//    if (value < LONG_MIN || value > LONG_MAX) {
//        if (msg != NULL) {
//            addReplyError(c, (char *) msg);
//        } else {
//            addReplyError(c, "value is out of range");
//        }
//        return C_ERR;
//    }
//    *target = value;
//    return C_OK;
//}
//
//int getRangeLongFromObjectOrReply(client *c, robj *o, long min, long max, long *target, const char *msg) {
//    if (getLongFromObjectOrReply(c, o, target, msg) != C_OK) return C_ERR;
//    if (*target < min || *target > max) {
//        if (msg != NULL) {
//            addReplyError(c, (char *) msg);
//        } else {
//            addReplyErrorFormat(c, "value is out of range, value must between %ld and %ld", min, max);
//        }
//        return C_ERR;
//    }
//    return C_OK;
//}
//
//int getPositiveLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg) {
//    if (msg) {
//        return getRangeLongFromObjectOrReply(c, o, 0, LONG_MAX, target, msg);
//    } else {
//        return getRangeLongFromObjectOrReply(c, o, 0, LONG_MAX, target, "value is out of range, must be positive");
//    }
//}
//
//int getIntFromObjectOrReply(client *c, robj *o, int *target, const char *msg) {
//    long value;
//
//    if (getRangeLongFromObjectOrReply(c, o, INT_MIN, INT_MAX, &value, msg) != C_OK)
//        return C_ERR;
//
//    *target = value;
//    return C_OK;
//}

char *strEncoding(int encoding) {
    switch (encoding) {
        case OBJ_ENCODING_RAW:
            return "raw";
        case OBJ_ENCODING_INT:
            return "int";
        case OBJ_ENCODING_HT:
            return "hashtable";
        case OBJ_ENCODING_QUICKLIST:
            return "quicklist";
        case OBJ_ENCODING_LISTPACK:
            return "listpack";
        case OBJ_ENCODING_INTSET:
            return "intset";
        case OBJ_ENCODING_SKIPLIST:
            return "skiplist";
        case OBJ_ENCODING_EMBSTR:
            return "embstr";
        case OBJ_ENCODING_STREAM:
            return "stream";
        default:
            return "unknown";
    }
}

int convertObjectToSds(robj *obj, sds *val)
{
    if (sdsEncodedObject(obj)) {
        *val = sdsdup(obj->ptr);
    } else if (obj->encoding == OBJ_ENCODING_INT) {
        *val = sdsfromlonglong((long)obj->ptr);
    } else {
        return C_ERR;
    }

    return C_OK;
}
