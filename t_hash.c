/*
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

#include <string.h>
#include <assert.h>
#include <limits.h>

#include "redis.h"
#include "commondef.h"
#include "commonfunc.h"
#include "object.h"
#include "zmalloc.h"
#include "db.h"
#include "ziplist.h"
#include "listpack.h"
#include "util.h"
#include <math.h>

/*-----------------------------------------------------------------------------
 * Hash type API
 *----------------------------------------------------------------------------*/
extern dictType hashDictType;

typedef struct {
    robj *subject;
    int encoding;

    unsigned char *fptr, *vptr;

    dictIterator *di;
    dictEntry *de;
} hashTypeIterator;

hashTypeIterator *hashTypeInitIterator(robj *subject) {
    hashTypeIterator *hi = zmalloc(sizeof(hashTypeIterator));
    hi->subject = subject;
    hi->encoding = subject->encoding;

    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        hi->fptr = NULL;
        hi->vptr = NULL;
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        hi->di = dictGetIterator(subject->ptr);
    } else {
//        serverPanic("Unknown hash encoding");
    }
    return hi;
}

/* Return the number of elements in a hash. */
unsigned long hashTypeLength(const robj *o) {
    unsigned long length = ULONG_MAX;

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        length = lpLength(o->ptr) / 2;
    } else if (o->encoding == OBJ_ENCODING_HT) {
        length = dictSize((const dict*)o->ptr);
    } else {
//        serverPanic("Unknown hash encoding");
    }
    return length;
}


/* Move to the next entry in the hash. Return C_OK when the next entry
 * could be found and C_ERR when the iterator reaches the end. */
int hashTypeNext(hashTypeIterator *hi) {
    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl;
        unsigned char *fptr, *vptr;

        zl = hi->subject->ptr;
        fptr = hi->fptr;
        vptr = hi->vptr;

        if (fptr == NULL) {
            /* Initialize cursor */
            assert(vptr == NULL);
            fptr = lpFirst(zl);
        } else {
            /* Advance cursor */
            assert(vptr != NULL);
            fptr = lpNext(zl, vptr);
        }
        if (fptr == NULL) return C_ERR;

        /* Grab pointer to the value (fptr points to the field) */
        vptr = lpNext(zl, fptr);
        assert(vptr != NULL);

        /* fptr, vptr now point to the first or next pair */
        hi->fptr = fptr;
        hi->vptr = vptr;
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        if ((hi->de = dictNext(hi->di)) == NULL) return C_ERR;
    } else {
//        serverPanic("Unknown hash encoding");
    }
    return C_OK;
}

/* Get the field or value at iterator cursor, for an iterator on a hash value
 * encoded as a listpack. Prototype is similar to `hashTypeGetFromListpack`. */
void hashTypeCurrentFromListpack(hashTypeIterator *hi, int what,
                                 unsigned char **vstr,
                                 unsigned int *vlen,
                                 long long *vll)
{
    assert(hi->encoding == OBJ_ENCODING_LISTPACK);

    if (what & OBJ_HASH_KEY) {
        *vstr = lpGetValue(hi->fptr, vlen, vll);
    } else {
        *vstr = lpGetValue(hi->vptr, vlen, vll);
    }
}


/* Get the field or value at iterator cursor, for an iterator on a hash value
 * encoded as a hash table. Prototype is similar to
 * `hashTypeGetFromHashTable`. */
sds hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what) {
    assert(hi->encoding == OBJ_ENCODING_HT);

    if (what & OBJ_HASH_KEY) {
        return dictGetKey(hi->de);
    } else {
        return dictGetVal(hi->de);
    }
}


/* Higher level function of hashTypeCurrent*() that returns the hash value
 * at current iterator position.
 *
 * The returned element is returned by reference in either *vstr and *vlen if
 * it's returned in string form, or stored in *vll if it's returned as
 * a number.
 *
 * If *vll is populated *vstr is set to NULL, so the caller
 * can always check the function return by checking the return value
 * type checking if vstr == NULL. */
void hashTypeCurrentObject(hashTypeIterator *hi, int what, unsigned char **vstr, unsigned int *vlen, long long *vll) {
    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        *vstr = NULL;
        hashTypeCurrentFromListpack(hi, what, vstr, vlen, vll);
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        sds ele = hashTypeCurrentFromHashTable(hi, what);
        *vstr = (unsigned char*) ele;
        *vlen = sdslen(ele);
    } else {
//        serverPanic("Unknown hash encoding");
    }
}


/* Return the key or value at the current iterator position as a new
 * SDS string. */
sds hashTypeCurrentObjectNewSds(hashTypeIterator *hi, int what) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vll;

    hashTypeCurrentObject(hi,what,&vstr,&vlen,&vll);
    if (vstr) return sdsnewlen(vstr,vlen);
    return sdsfromlonglong(vll);
}


void hashTypeReleaseIterator(hashTypeIterator *hi) {
    if (hi->encoding == OBJ_ENCODING_HT)
        dictReleaseIterator(hi->di);
    zfree(hi);
}



void hashTypeConvertListpack(robj *o, int enc) {
    assert(o->encoding == OBJ_ENCODING_LISTPACK);

    if (enc == OBJ_ENCODING_LISTPACK) {
        /* Nothing to do... */

    } else if (enc == OBJ_ENCODING_HT) {
        hashTypeIterator *hi;
        dict *dict;
        int ret;

        hi = hashTypeInitIterator(o);
        dict = dictCreate(&hashDictType);

        /* Presize the dict to avoid rehashing */
        dictExpand(dict,hashTypeLength(o));

        while (hashTypeNext(hi) != C_ERR) {
            sds key, value;

            key = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_KEY);
            value = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_VALUE);
            ret = dictAdd(dict, key, value);
            if (ret != DICT_OK) {
                sdsfree(key); sdsfree(value); /* Needed for gcc ASAN */
                hashTypeReleaseIterator(hi);  /* Needed for gcc ASAN */
//                serverLogHexDump(LL_WARNING,"listpack with dup elements dump",
//                                 o->ptr,lpBytes(o->ptr));
//                serverPanic("Listpack corruption detected");
            }
        }
        hashTypeReleaseIterator(hi);
        zfree(o->ptr);
        o->encoding = OBJ_ENCODING_HT;
        o->ptr = dict;
    } else {
//        serverPanic("Unknown hash encoding");
    }
}



void hashTypeConvert(robj *o, int enc) {
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        hashTypeConvertListpack(o, enc);
    } else if (o->encoding == OBJ_ENCODING_HT) {
//        serverPanic("Not implemented");
    } else {
//        serverPanic("Unknown hash encoding");
    }
}


/* Check the length of a number of objects to see if we need to convert a
 * listpack to a real hash. Note that we only check string encoded objects
 * as their string length can be queried in constant time. */
void hashTypeTryConversion(robj *o, robj **argv, int start, int end) {
    int i;
    size_t sum = 0;

    if (o->encoding != OBJ_ENCODING_LISTPACK) return;

    /* We guess that most of the values in the input are unique, so
     * if there are enough arguments we create a pre-sized hash, which
     * might over allocate memory if there are duplicates. */
    size_t new_fields = (end - start + 1) / 2;
    if (new_fields > OBJ_HASH_MAX_LISTPACK_VALUE) {
        hashTypeConvert(o, OBJ_ENCODING_HT);
        dictExpand(o->ptr, new_fields);
        return;
    }

    for (i = start; i <= end; i++) {
        if (!sdsEncodedObject(argv[i]))
            continue;
        size_t len = sdslen(argv[i]->ptr);
        if (len > OBJ_HASH_MAX_LISTPACK_VALUE) {
            hashTypeConvert(o, OBJ_ENCODING_HT);
            return;
        }
        sum += len;
    }
    if (!lpSafeToAdd(o->ptr, sum))
        hashTypeConvert(o, OBJ_ENCODING_HT);
}

/* Get the value from a listpack encoded hash, identified by field.
 * Returns -1 when the field cannot be found. */
int hashTypeGetFromListpack(robj *o, sds field,
                            unsigned char **vstr,
                            unsigned int *vlen,
                            long long *vll)
{
    unsigned char *zl, *fptr = NULL, *vptr = NULL;

    assert(o->encoding == OBJ_ENCODING_LISTPACK);

    zl = o->ptr;
    fptr = lpFirst(zl);
    if (fptr != NULL) {
        fptr = lpFind(zl, fptr, (unsigned char*)field, sdslen(field), 1);
        if (fptr != NULL) {
            /* Grab pointer to the value (fptr points to the field) */
            vptr = lpNext(zl, fptr);
            assert(vptr != NULL);
        }
    }

    if (vptr != NULL) {
        *vstr = lpGetValue(vptr, vlen, vll);
        return 0;
    }

    return -1;
}

/* Get the value from a hash table encoded hash, identified by field.
 * Returns NULL when the field cannot be found, otherwise the SDS value
 * is returned. */
sds hashTypeGetFromHashTable(robj *o, sds field) {
    dictEntry *de;

    assert(o->encoding == OBJ_ENCODING_HT);

    de = dictFind(o->ptr, field);
    if (de == NULL) return NULL;
    return dictGetVal(de);
}

/* Higher level function of hashTypeGet*() that returns the hash value
 * associated with the specified field. If the field is found C_OK
 * is returned, otherwise C_ERR. The returned object is returned by
 * reference in either *vstr and *vlen if it's returned in string form,
 * or stored in *vll if it's returned as a number.
 *
 * If *vll is populated *vstr is set to NULL, so the caller
 * can always check the function return by checking the return value
 * for C_OK and checking if vll (or vstr) is NULL. */
int hashTypeGetValue(robj *o, sds field, unsigned char **vstr, unsigned int *vlen, long long *vll) {
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        *vstr = NULL;
        if (hashTypeGetFromListpack(o, field, vstr, vlen, vll) == 0)
            return C_OK;
    } else if (o->encoding == OBJ_ENCODING_HT) {
        sds value;
        if ((value = hashTypeGetFromHashTable(o, field)) != NULL) {
            *vstr = (unsigned char*) value;
            *vlen = sdslen(value);
            return C_OK;
        }
    } else {
//        serverPanic("Unknown hash encoding");
    }
    return C_ERR;
}

/* Like hashTypeGetValue() but returns a Redis object, which is useful for
 * interaction with the hash type outside t_hash.c.
 * The function returns NULL if the field is not found in the hash. Otherwise
 * a newly allocated string object with the value is returned. */
robj *hashTypeGetValueObject(robj *o, sds field) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vll;

    if (hashTypeGetValue(o,field,&vstr,&vlen,&vll) == C_ERR) return NULL;
    if (vstr) return createStringObject((char*)vstr,vlen);
    else return createStringObjectFromLongLong(vll);
}

/* Higher level function using hashTypeGet*() to return the length of the
 * object associated with the requested field, or 0 if the field does not
 * exist. */
size_t hashTypeGetValueLength(robj *o, sds field) {
    size_t len = 0;
    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    if (hashTypeGetValue(o, field, &vstr, &vlen, &vll) == C_OK)
        len = vstr ? vlen : sdigits10(vll);

    return len;
}

/* Test if the specified field exists in the given hash. Returns 1 if the field
 * exists, and 0 when it doesn't. */
int hashTypeExists(robj *o, sds field) {
    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    return hashTypeGetValue(o, field, &vstr, &vlen, &vll) == C_OK;
}

/* Add a new field, overwrite the old with the new value if it already exists.
 * Return 0 on insert and 1 on update.
 *
 * By default, the key and value SDS strings are copied if needed, so the
 * caller retains ownership of the strings passed. However this behavior
 * can be effected by passing appropriate flags (possibly bitwise OR-ed):
 *
 * HASH_SET_TAKE_FIELD -- The SDS field ownership passes to the function.
 * HASH_SET_TAKE_VALUE -- The SDS value ownership passes to the function.
 *
 * When the flags are used the caller does not need to release the passed
 * SDS string(s). It's up to the function to use the string to create a new
 * entry or to free the SDS string before returning to the caller.
 *
 * HASH_SET_COPY corresponds to no flags passed, and means the default
 * semantics of copying the values if needed.
 *
 */
#define HASH_SET_TAKE_FIELD (1<<0)
#define HASH_SET_TAKE_VALUE (1<<1)
#define HASH_SET_COPY 0
int hashTypeSet(robj *o, sds field, sds value, int flags) {
    int update = 0;

    /* Check if the field is too long for listpack, and convert before adding the item.
     * This is needed for HINCRBY* case since in other commands this is handled early by
     * hashTypeTryConversion, so this check will be a NOP. */
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        if (sdslen(field) > OBJ_HASH_MAX_LISTPACK_VALUE || sdslen(value) > OBJ_HASH_MAX_LISTPACK_VALUE)
            hashTypeConvert(o, OBJ_ENCODING_HT);
    }

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl, *fptr, *vptr;

        zl = o->ptr;
        fptr = lpFirst(zl);
        if (fptr != NULL) {
            fptr = lpFind(zl, fptr, (unsigned char*)field, sdslen(field), 1);
            if (fptr != NULL) {
                /* Grab pointer to the value (fptr points to the field) */
                vptr = lpNext(zl, fptr);
                assert(vptr != NULL);
                update = 1;

                /* Replace value */
                zl = lpReplace(zl, &vptr, (unsigned char*)value, sdslen(value));
            }
        }

        if (!update) {
            /* Push new field/value pair onto the tail of the listpack */
            zl = lpAppend(zl, (unsigned char*)field, sdslen(field));
            zl = lpAppend(zl, (unsigned char*)value, sdslen(value));
        }
        o->ptr = zl;

        /* Check if the listpack needs to be converted to a hash table */
        if (hashTypeLength(o) > OBJ_HASH_MAX_LISTPACK_VALUE)
            hashTypeConvert(o, OBJ_ENCODING_HT);
    } else if (o->encoding == OBJ_ENCODING_HT) {
        dict *ht = o->ptr;
        dictEntry *de, *existing;
        sds v;
        if (flags & HASH_SET_TAKE_VALUE) {
            v = value;
            value = NULL;
        } else {
            v = sdsdup(value);
        }
        de = dictAddRaw(ht, field, &existing);
        if (de) {
            dictSetVal(ht, de, v);
            if (flags & HASH_SET_TAKE_FIELD) {
                field = NULL;
            } else {
                dictSetKey(ht, de, sdsdup(field));
            }
        } else {
            sdsfree(dictGetVal(existing));
            dictSetVal(ht, existing, v);
            update = 1;
        }
    } else {
//        serverPanic("Unknown hash encoding");
    }

    /* Free SDS strings we did not referenced elsewhere if the flags
     * want this function to be responsible. */
    if (flags & HASH_SET_TAKE_FIELD && field) sdsfree(field);
    if (flags & HASH_SET_TAKE_VALUE && value) sdsfree(value);
    return update;
}

/* Delete an element from a hash.
 * Return 1 on deleted and 0 on not found. */
int hashTypeDelete(robj *o, sds field) {
    int deleted = 0;

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl, *fptr;

        zl = o->ptr;
        fptr = lpFirst(zl);
        if (fptr != NULL) {
            fptr = lpFind(zl, fptr, (unsigned char*)field, sdslen(field), 1);
            if (fptr != NULL) {
                /* Delete both of the key and the value. */
                zl = lpDeleteRangeWithEntry(zl,&fptr,2);
                o->ptr = zl;
                deleted = 1;
            }
        }
    } else if (o->encoding == OBJ_ENCODING_HT) {
        if (dictDelete((dict*)o->ptr, field) == C_OK) {
            deleted = 1;

            /* Always check if the dictionary needs a resize after a delete. */
            if (htNeedsResize(o->ptr)) dictResize(o->ptr);
        }

    } else {
//        serverPanic("Unknown hash encoding");
    }
    return deleted;
}




robj *hashTypeLookupWriteOrCreate(redisDb *redis_db, robj *key) {
    robj *o = lookupKeyWrite(redis_db,key);
    if (checkType(o,OBJ_HASH)) return NULL;

    if (o == NULL) {
        o = createHashObject();
        dbAdd(redis_db,key,o);
    }
    return o;
}



/* This is a helper function for the COPY command.
 * Duplicate a hash object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
robj *hashTypeDup(robj *o) {
    robj *hobj;
    hashTypeIterator *hi;

    assert(o->type == OBJ_HASH);

    if(o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = o->ptr;
        size_t sz = lpBytes(zl);
        unsigned char *new_zl = zmalloc(sz);
        memcpy(new_zl, zl, sz);
        hobj = createObject(OBJ_HASH, new_zl);
        hobj->encoding = OBJ_ENCODING_LISTPACK;
    } else if(o->encoding == OBJ_ENCODING_HT){
        dict *d = dictCreate(&hashDictType);
        dictExpand(d, dictSize((const dict*)o->ptr));

        hi = hashTypeInitIterator(o);
        while (hashTypeNext(hi) != C_ERR) {
            sds field, value;
            sds newfield, newvalue;
            /* Extract a field-value pair from an original hash object.*/
            field = hashTypeCurrentFromHashTable(hi, OBJ_HASH_KEY);
            value = hashTypeCurrentFromHashTable(hi, OBJ_HASH_VALUE);
            newfield = sdsdup(field);
            newvalue = sdsdup(value);

            /* Add a field-value pair to a new hash object. */
            dictAdd(d,newfield,newvalue);
        }
        hashTypeReleaseIterator(hi);

        hobj = createObject(OBJ_HASH, d);
        hobj->encoding = OBJ_ENCODING_HT;
    } else {
//        serverPanic("Unknown hash encoding");
    }
    return hobj;
}

/* Create a new sds string from the listpack entry. */
sds hashSdsFromListpackEntry(listpackEntry *e) {
    return e->sval ? sdsnewlen(e->sval, e->slen) : sdsfromlonglong(e->lval);
}

/* Reply with bulk string from the listpack entry. */
//void hashReplyFromListpackEntry(client *c, listpackEntry *e) {
//    if (e->sval)
//        addReplyBulkCBuffer(c, e->sval, e->slen);
//    else
//        addReplyBulkLongLong(c, e->lval);
//}

/* Return random element from a non empty hash.
 * 'key' and 'val' will be set to hold the element.
 * The memory in them is not to be freed or modified by the caller.
 * 'val' can be NULL in which case it's not extracted. */
void hashTypeRandomElement(robj *hashobj, unsigned long hashsize, listpackEntry *key, listpackEntry *val) {
    if (hashobj->encoding == OBJ_ENCODING_HT) {
        dictEntry *de = dictGetFairRandomKey(hashobj->ptr);
        sds s = dictGetKey(de);
        key->sval = (unsigned char*)s;
        key->slen = sdslen(s);
        if (val) {
            sds s = dictGetVal(de);
            val->sval = (unsigned char*)s;
            val->slen = sdslen(s);
        }
    } else if (hashobj->encoding == OBJ_ENCODING_LISTPACK) {
        lpRandomPair(hashobj->ptr, hashsize, key, val);
    } else {
//        serverPanic("Unknown hash encoding");
    }
}


/*-----------------------------------------------------------------------------
 * Hash type commands
 *----------------------------------------------------------------------------*/

static int HSet(redisDb *redis_db, robj *kobj, robj *fobj, robj *vobj)
{
    robj *o;
    if ((o = hashTypeLookupWriteOrCreate(redis_db,kobj)) == NULL) return C_ERR;

    robj *argv[2];
    argv[0] = fobj;
    argv[1] = vobj;
    hashTypeTryConversion(o,argv,0,1);
    hashTypeSet(o, argv[0]->ptr,argv[1]->ptr,HASH_SET_COPY);
    return C_OK;
}

static int HMSet(redisDb *redis_db, robj *kobj, robj *items[], unsigned long items_size)
{
    robj *o;
    if ((o = hashTypeLookupWriteOrCreate(redis_db,kobj)) == NULL) return C_ERR;

    hashTypeTryConversion(o,items,0,items_size-1);

    unsigned long i;
    for (i = 0; i < items_size; i += 2)
        hashTypeSet(o,items[i]->ptr,items[i+1]->ptr,HASH_SET_COPY);

    return C_OK;
}

static int HSetnx(redisDb *redis_db, robj *kobj, robj *fobj, robj *vobj)
{
    robj *o;
    if ((o = hashTypeLookupWriteOrCreate(redis_db,kobj)) == NULL) return C_ERR;

    robj *argv[2];
    argv[0] = fobj;
    argv[1] = vobj;
    hashTypeTryConversion(o,argv,0,1);
    if (!hashTypeExists(o,argv[0]->ptr)) {
        hashTypeSet(o,argv[0]->ptr,argv[1]->ptr,HASH_SET_COPY);
    }

    return C_OK;
}

static int GetHashFieldValue(robj *o, sds field, sds *val)
{
    if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        if (0 > hashTypeGetFromListpack(o, field, &vstr, &vlen, &vll)) {
            return REDIS_ITEM_NOT_EXIST;
        } else {
            if (vstr) {
                *val = sdsnewlen(vstr, vlen);
            } else {
                *val = sdsfromlonglong(vll);
            }
        }
    } else if (o->encoding == OBJ_ENCODING_HT) {
        sds value = hashTypeGetFromHashTable(o, field);
        if (value == NULL) {
            return REDIS_ITEM_NOT_EXIST;
        } else {
            *val = sdsdup(value);
        }
    } else {
        return C_ERR;
    }

    return C_OK;
}

static void addHashIteratorCursorToReply(hashTypeIterator *hi, int what, sds *out) {
    if (hi->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        hashTypeCurrentFromListpack(hi, what, &vstr, &vlen, &vll);
        if (vstr) {
            *out = sdsnewlen(vstr, vlen);
        } else {
            *out = sdsfromlonglong(vll);
        }
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        sds value = hashTypeCurrentFromHashTable(hi, what);
        *out = sdsdup(value);
    } else {
        // serverPanic("Unknown hash encoding");
    }
}

static int genericHgetall(redisDb *redis_db, robj *kobj, hitem **items, unsigned long *items_size, int flags)
{
    robj *o;
    hashTypeIterator *hi;

    if ((o = lookupKeyRead(redis_db,kobj)) == NULL
        || checkType(o,OBJ_HASH)) return REDIS_KEY_NOT_EXIST;

    *items_size = hashTypeLength(o);
    *items = (hitem*)zcallocate(sizeof(hitem) * (*items_size));

    hi = hashTypeInitIterator(o);
    unsigned long i = 0;
    while (hashTypeNext(hi) != C_ERR) {
        if (flags & OBJ_HASH_KEY) {
            addHashIteratorCursorToReply(hi, OBJ_HASH_KEY, &((*items+i)->field));
        }
        if (flags & OBJ_HASH_VALUE) {
            addHashIteratorCursorToReply(hi, OBJ_HASH_VALUE, &((*items+i)->value));
        }

        ++i;
        if (i >= *items_size) break;
    }

    hashTypeReleaseIterator(hi);
    return C_OK;
}

int RcHDel(redisCache db, robj *key, robj *fields[], unsigned long fields_size, unsigned long *ret)
{
    if (NULL == db || NULL == key || NULL == fields) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    robj *o;
    if ((o = lookupKeyRead(redis_db,key)) == NULL || checkType(o,OBJ_HASH)) {
        return REDIS_KEY_NOT_EXIST;
    }

    unsigned long i, deleted = 0;
    for (i = 0; i < fields_size; i++) {
        if (hashTypeDelete(o,fields[i]->ptr)) {
            deleted++;
            if (hashTypeLength(o) == 0) {
                dbDelete(redis_db,key);
                break;
            }
        }
    }
    *ret = deleted;

    return C_OK;
}

int RcHSet(redisCache db, robj *key, robj *field, robj *val)
{
    if (NULL == db || NULL == key || NULL == field || NULL == val) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    return HSet(redis_db, key, field, val);
}

int RcHSetnx(redisCache db, robj *key, robj *field, robj *val)
{
    if (NULL == db || NULL == key || NULL == field || NULL == val) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    return HSetnx(redis_db, key, field, val);
}

int RcHMSet(redisCache db, robj *key, robj *items[], unsigned long items_size)
{
    if (NULL == db || NULL == key || NULL == items) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    return HMSet(redis_db, key, items, items_size);
}

int RcHGet(redisCache db, robj *key, robj *field, sds *val)
{
    if (NULL == db || NULL == key || NULL == field) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    robj *o;
    if ((o = lookupKeyRead(redis_db,key)) == NULL || checkType(o,OBJ_HASH)) {
        return REDIS_KEY_NOT_EXIST;
    }

    return GetHashFieldValue(o, field->ptr, val);
}

int RcHMGet(redisCache db, robj *key, hitem *items, unsigned long items_size)
{
    if (NULL == db || NULL == key || NULL == items) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    robj *o;
    if ((o = lookupKeyRead(redis_db,key)) == NULL || checkType(o,OBJ_HASH)) {
        return REDIS_KEY_NOT_EXIST;
    }

    unsigned long i;
    for (i = 0; i < items_size; ++i) {
        items[i].status = GetHashFieldValue(o, items[i].field, &(items[i].value));
    }

    return C_OK;
}

int RcHGetAll(redisCache db, robj *key, hitem **items, unsigned long *items_size)
{
    if (NULL == db || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    return genericHgetall(redis_db, key, items, items_size, OBJ_HASH_KEY|OBJ_HASH_VALUE);
}

int RcHKeys(redisCache db, robj *key, hitem **items, unsigned long *items_size)
{
    if (NULL == db || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    return genericHgetall(redis_db, key, items, items_size, OBJ_HASH_KEY);
}

int RcHVals(redisCache db, robj *key, hitem **items, unsigned long *items_size)
{
    if (NULL == db || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    return genericHgetall(redis_db, key, items, items_size, OBJ_HASH_VALUE);
}

int RcHExists(redisCache db, robj *key, robj *field, int *is_exist)
{
    if (NULL == db || NULL == key || NULL == field) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    robj *o;
    if ((o = lookupKeyRead(redis_db,key)) == NULL || checkType(o,OBJ_HASH)) {
        return REDIS_KEY_NOT_EXIST;
    }

    *is_exist = hashTypeExists(o,field->ptr);

    return C_OK;
}

int RcHIncrby(redisCache db, robj *key, robj *field, long long val, long long *ret)
{
    if (NULL == db || NULL == key || NULL == field) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    long long value, oldvalue;
    robj *o;
    sds new;
    unsigned char *vstr;
    unsigned int vlen;

    if ((o = hashTypeLookupWriteOrCreate(redis_db,key)) == NULL) {
        return C_ERR;
    }

    if (hashTypeGetValue(o,field->ptr,&vstr,&vlen,&value) == C_OK) {
        if (vstr) {
            if (string2ll((char*)vstr,vlen,&value) == 0) {
                return C_ERR;
            }
        } /* Else hashTypeGetValue() already stored it into &value */
    } else {
        value = 0;
    }

    oldvalue = value;
    if ((val < 0 && oldvalue < 0 && val < (LLONG_MIN-oldvalue)) ||
        (val > 0 && oldvalue > 0 && val > (LLONG_MAX-oldvalue))) {
        return REDIS_OVERFLOW;
    }
    value += val;
    *ret = value;

    new = sdsfromlonglong(value);
    hashTypeSet(o,field->ptr,new,HASH_SET_TAKE_VALUE);

    return C_OK;
}

int RcHIncrbyfloat(redisCache db, robj *key, robj *field, long double val, long double *ret)
{
    if (NULL == db || NULL == key || NULL == field) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    long double value;
    long long ll;
    robj *o;
    sds new;
    unsigned char *vstr;
    unsigned int vlen;

    if ((o = hashTypeLookupWriteOrCreate(redis_db,key)) == NULL) {
        return C_ERR;
    }

    if (hashTypeGetValue(o,field->ptr,&vstr,&vlen,&ll) == C_OK) {
        if (vstr) {
            if (string2ld((char*)vstr,vlen,&value) == 0) {
                return C_ERR;
            }
        } else {
            value = (long double)ll;
        }
    } else {
        value = 0;
    }
    value += val;
    *ret = value;

    char buf[MAX_LONG_DOUBLE_CHARS];
    int len = ld2string(buf,sizeof(buf),value,1);
    new = sdsnewlen(buf,len);
    hashTypeSet(o,field->ptr,new,HASH_SET_TAKE_VALUE);

    return C_OK;
}

int RcHlen(redisCache db, robj *key, unsigned long *len)
{
    if (NULL == db || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    robj *o;
    if ((o = lookupKeyRead(redis_db,key)) == NULL || checkType(o,OBJ_HASH)) {
        return REDIS_KEY_NOT_EXIST;
    }

    *len = hashTypeLength(o);

    return C_OK;
}

int RcHStrlen(redisCache db, robj *key, robj *field, unsigned long *len)
{
    if (NULL == db || NULL == key || NULL == field) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    robj *o;
    if ((o = lookupKeyRead(redis_db,key)) == NULL || checkType(o,OBJ_HASH)) {
        return REDIS_KEY_NOT_EXIST;
    }

    *len = hashTypeGetValueLength(o,field->ptr);

    return C_OK;
}
