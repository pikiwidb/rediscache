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
#include <stdlib.h>

#include "redis.h"
#include "commondef.h"
#include "commonfunc.h"
#include "object.h"
#include "zmalloc.h"
#include "db.h"
#include "util.h"
#include "intset.h"  /* Compact integer set structure */
#include "listpack.h"
#include "dict.h"


#define SRANDMEMBER_SUB_STRATEGY_MUL 3


/* Structure to hold set iteration abstraction. */
typedef struct {
    robj *subject;
    int encoding;
    int ii; /* intset iterator */
    dictIterator *di;
    unsigned char *lpi; /* listpack iterator */
} setTypeIterator;


/*-----------------------------------------------------------------------------
 * Set Commands
 *----------------------------------------------------------------------------*/

/* Factory method to return a set that *can* hold "value". When the object has
 * an integer-encodable value, an intset will be returned. Otherwise a listpack
 * or a regular hash table.
 *
 * The size hint indicates approximately how many items will be added which is
 * used to determine the initial representation. */
robj *setTypeCreate(sds value) {
    if (isSdsRepresentableAsLongLong(value, NULL) == C_OK)
        return createIntsetObject();

    /* We may oversize the set by using the hint if the hint is not accurate,
     * but we will assume this is acceptable to maximize performance. */
    robj *o = createSetObject();
    return o;
}

/* Return the maximum number of entries to store in an intset. */
static size_t intsetMaxEntries(void) {
    size_t max_entries = SET_MAX_INTSET_ENTRIES;
    /* limit to 1G entries due to intset internals. */
    if (max_entries >= 1 << 30) max_entries = 1 << 30;
    return max_entries;
}

void setTypeConvert(robj *setobj, int enc);

/* Converts intset to HT if it contains too many entries. */
static void maybeConvertIntset(robj *subject) {
    assert(subject->encoding == OBJ_ENCODING_INTSET);
    if (intsetLen(subject->ptr) > intsetMaxEntries())
        setTypeConvert(subject, OBJ_ENCODING_HT);
}

setTypeIterator *setTypeInitIterator(robj *subject) {
    setTypeIterator *si = zmalloc(sizeof(setTypeIterator));
    si->subject = subject;
    si->encoding = subject->encoding;
    if (si->encoding == OBJ_ENCODING_HT) {
        si->di = dictGetIterator(subject->ptr);
    } else if (si->encoding == OBJ_ENCODING_INTSET) {
        si->ii = 0;
    } else if (si->encoding == OBJ_ENCODING_LISTPACK) {
        si->lpi = NULL;
    } else {
//        serverPanic("Unknown set encoding");
    }
    return si;
}

unsigned long setTypeSize(const robj *subject) {
    if (subject->encoding == OBJ_ENCODING_HT) {
        return dictSize((const dict *) subject->ptr);
    } else if (subject->encoding == OBJ_ENCODING_INTSET) {
        return intsetLen((const intset *) subject->ptr);
    } else if (subject->encoding == OBJ_ENCODING_LISTPACK) {
        return lpLength((unsigned char *) subject->ptr);
    } else {
//        serverPanic("Unknown set encoding");
    }
}

/* Move to the next entry in the set. Returns the object at the current
 * position, as a string or as an integer.
 *
 * Since set elements can be internally be stored as SDS strings, char buffers or
 * simple arrays of integers, setTypeNext returns the encoding of the
 * set object you are iterating, and will populate the appropriate pointers
 * (str and len) or (llele) depending on whether the value is stored as a string
 * or as an integer internally.
 *
 * If OBJ_ENCODING_HT is returned, then str points to an sds string and can be
 * used as such. If OBJ_ENCODING_INTSET, then llele is populated and str is
 * pointed to NULL. If OBJ_ENCODING_LISTPACK is returned, the value can be
 * either a string or an integer. If *str is not NULL, then str and len are
 * populated with the string content and length. Otherwise, llele populated with
 * an integer value.
 *
 * Note that str, len and llele pointers should all be passed and cannot
 * be NULL since the function will try to defensively populate the non
 * used field with values which are easy to trap if misused.
 *
 * When there are no more elements -1 is returned. */
int setTypeNext(setTypeIterator *si, char **str, size_t *len, int64_t *llele) {
    if (si->encoding == OBJ_ENCODING_HT) {
        dictEntry *de = dictNext(si->di);
        if (de == NULL) return -1;
        *str = dictGetKey(de);
        *len = sdslen(*str);
        *llele = -123456789; /* Not needed. Defensive. */
    } else if (si->encoding == OBJ_ENCODING_INTSET) {
        if (!intsetGet(si->subject->ptr, si->ii++, llele))
            return -1;
        *str = NULL;
    } else if (si->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = si->subject->ptr;
        unsigned char *lpi = si->lpi;
        if (lpi == NULL) {
            lpi = lpFirst(lp);
        } else {
            lpi = lpNext(lp, lpi);
        }
        if (lpi == NULL) return -1;
        si->lpi = lpi;
        unsigned int l;
        *str = (char *) lpGetValue(lpi, &l, (long long *) llele);
        *len = (size_t) l;
    } else {
//        serverPanic("Wrong set encoding in setTypeNext");
    }
    return si->encoding;
}


void setTypeReleaseIterator(setTypeIterator *si) {
    if (si->encoding == OBJ_ENCODING_HT)
        dictReleaseIterator(si->di);
    zfree(si);
}

/* When you know all set elements are integers, call this to convert the set to
 * an intset. No conversion happens if the set contains too many entries for an
 * intset. */
static void maybeConvertToIntset(robj *set) {
    if (set->encoding == OBJ_ENCODING_INTSET) return; /* already intset */
    if (setTypeSize(set) > intsetMaxEntries()) return; /* can't use intset */
    intset *is = intsetNew();
    char *str;
    size_t len;
    int64_t llval;
    setTypeIterator *si = setTypeInitIterator(set);
    while (setTypeNext(si, &str, &len, &llval) != -1) {
        if (str) {
            /* If the element is returned as a string, we may be able to convert
             * it to integer. This happens for OBJ_ENCODING_HT. */
            assert(string2ll(str, len, (long long *) &llval));
        }
        uint8_t success = 0;
        is = intsetAdd(is, llval, &success);
        assert(success);
    }
    setTypeReleaseIterator(si);
    freeSetObject(set); /* frees the internals but not robj itself */
    set->ptr = is;
    set->encoding = OBJ_ENCODING_INTSET;
}


/* The not copy on write friendly version but easy to use version
 * of setTypeNext() is setTypeNextObject(), returning new SDS
 * strings. So if you don't retain a pointer to this object you should call
 * sdsfree() against it.
 *
 * This function is the way to go for write operations where COW is not
 * an issue. */
sds setTypeNextObject(setTypeIterator *si) {
    int64_t intele;
    char *str;
    size_t len;

    if (setTypeNext(si, &str, &len, &intele) == -1) return NULL;
    if (str != NULL) return sdsnewlen(str, len);
    return sdsfromlonglong(intele);
}


/* Converts a set to the specified encoding, pre-sizing it for 'cap' elements.
 * The 'panic' argument controls whether to panic on OOM (panic=1) or return
 * C_ERR on OOM (panic=0). If panic=1 is given, this function always returns
 * C_OK. */
int setTypeConvertAndExpand(robj *setobj, int enc, unsigned long cap, int panic) {
    setTypeIterator *si;
    assert(setobj->type == OBJ_SET && setobj->encoding != enc);

    if (enc == OBJ_ENCODING_HT) {
        dict *d = dictCreate(&setDictType);
        sds element;

        /* Presize the dict to avoid rehashing */
        if (panic) {
            dictExpand(d, cap);
        } else if (dictTryExpand(d, cap) != DICT_OK) {
            dictRelease(d);
            return C_ERR;
        }

        /* To add the elements we extract integers and create redis objects */
        si = setTypeInitIterator(setobj);
        while ((element = setTypeNextObject(si)) != NULL) {
            assert(dictAdd(d, element, NULL) == DICT_OK);
        }
        setTypeReleaseIterator(si);

        freeSetObject(setobj); /* frees the internals but not setobj itself */
        setobj->encoding = OBJ_ENCODING_HT;
        setobj->ptr = d;
    } else if (enc == OBJ_ENCODING_LISTPACK) {
        /* Preallocate the minimum two bytes per element (enc/value + backlen) */
        size_t estcap = cap * 2;
        if (setobj->encoding == OBJ_ENCODING_INTSET && setTypeSize(setobj) > 0) {
            /* If we're converting from intset, we have a better estimate. */
            size_t s1 = lpEstimateBytesRepeatedInteger(intsetMin(setobj->ptr), cap);
            size_t s2 = lpEstimateBytesRepeatedInteger(intsetMax(setobj->ptr), cap);
            estcap = redis_max(s1, s2);
        }
        unsigned char *lp = lpNew(estcap);
        char *str;
        size_t len;
        int64_t llele;
        si = setTypeInitIterator(setobj);
        while (setTypeNext(si, &str, &len, &llele) != -1) {
            if (str != NULL)
                lp = lpAppend(lp, (unsigned char *) str, len);
            else
                lp = lpAppendInteger(lp, llele);
        }
        setTypeReleaseIterator(si);

        freeSetObject(setobj); /* frees the internals but not setobj itself */
        setobj->encoding = OBJ_ENCODING_LISTPACK;
        setobj->ptr = lp;
    } else {
//        serverPanic("Unsupported set conversion");
    }
    return C_OK;
}


/* Add member. This function is optimized for the different encodings. The
 * value can be provided as an sds string (indicated by passing str_is_sds =
 * 1), as string and length (str_is_sds = 0) or as an integer in which case str
 * is set to NULL and llval is provided instead.
 *
 * Returns 1 if the value was added and 0 if it was already a member. */
int setTypeAddAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {
        if (set->encoding == OBJ_ENCODING_INTSET) {
            uint8_t success = 0;
            set->ptr = intsetAdd(set->ptr, llval, &success);
            if (success) maybeConvertIntset(set);
            return success;
        }
        /* Convert int to string. */
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    assert(str);
    if (set->encoding == OBJ_ENCODING_HT) {
        /* Avoid duping the string if it is an sds string. */
        sds sdsval = str_is_sds ? (sds) str : sdsnewlen(str, len);
        dict *ht = set->ptr;
        void *position = dictFindPositionForInsert(ht, sdsval, NULL);
        if (position) {
            /* Key doesn't already exist in the set. Add it but dup the key. */
            if (sdsval == str) sdsval = sdsdup(sdsval);
            dictInsertAtPosition(ht, sdsval, position);
        } else if (sdsval != str) {
            /* String is already a member. Free our temporary sds copy. */
            sdsfree(sdsval);
        }
        return (position != NULL);
    } else if (set->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = set->ptr;
        unsigned char *p = lpFirst(lp);
        if (p != NULL)
            p = lpFind(lp, p, (unsigned char *) str, len, 0);
        if (p == NULL) {
            /* Not found.  */
            if (lpLength(lp) < SET_MAX_LISTPACK_ENTRIES &&
                len <= SET_MAX_LISTPACK_VALUE &&
                lpSafeToAdd(lp, len)) {
                if (str == tmpbuf) {
                    /* This came in as integer so we can avoid parsing it again.
                     * TODO: Create and use lpFindInteger; don't go via string. */
                    lp = lpAppendInteger(lp, llval);
                } else {
                    lp = lpAppend(lp, (unsigned char *) str, len);
                }
                set->ptr = lp;
            } else {
                /* Size limit is reached. Convert to hashtable and add. */
                setTypeConvertAndExpand(set, OBJ_ENCODING_HT, lpLength(lp) + 1, 1);
                assert(dictAdd(set->ptr, sdsnewlen(str, len), NULL) == DICT_OK);
            }
            return 1;
        }
    } else if (set->encoding == OBJ_ENCODING_INTSET) {
        long long value;
        if (string2ll(str, len, &value)) {
            uint8_t success = 0;
            set->ptr = intsetAdd(set->ptr, value, &success);
            if (success) {
                maybeConvertIntset(set);
                return 1;
            }
        } else {
            /* Check if listpack encoding is safe not to cross any threshold. */
            size_t maxelelen = 0, totsize = 0;
            unsigned long n = intsetLen(set->ptr);
            if (n != 0) {
                size_t elelen1 = sdigits10(intsetMax(set->ptr));
                size_t elelen2 = sdigits10(intsetMin(set->ptr));
                maxelelen = redis_max(elelen1, elelen2);
                size_t s1 = lpEstimateBytesRepeatedInteger(intsetMax(set->ptr), n);
                size_t s2 = lpEstimateBytesRepeatedInteger(intsetMin(set->ptr), n);
                totsize = redis_max(s1, s2);
            }
            if (intsetLen((const intset *) set->ptr) < SET_MAX_LISTPACK_ENTRIES &&
                len <= SET_MAX_LISTPACK_VALUE &&
                maxelelen <= SET_MAX_LISTPACK_VALUE &&
                lpSafeToAdd(NULL, totsize + len)) {
                /* In the "safe to add" check above we assumed all elements in
                 * the intset are of size maxelelen. This is an upper bound. */
                setTypeConvertAndExpand(set, OBJ_ENCODING_LISTPACK,
                                        intsetLen(set->ptr) + 1, 1);
                unsigned char *lp = set->ptr;
                lp = lpAppend(lp, (unsigned char *) str, len);
                lp = lpShrinkToFit(lp);
                set->ptr = lp;
                return 1;
            } else {
                setTypeConvertAndExpand(set, OBJ_ENCODING_HT,
                                        intsetLen(set->ptr) + 1, 1);
                /* The set *was* an intset and this value is not integer
                 * encodable, so dictAdd should always work. */
                assert(dictAdd(set->ptr, sdsnewlen(str, len), NULL) == DICT_OK);
                return 1;
            }
        }
    } else {
//        serverPanic("Unknown set encoding");
    }
    return 0;
}

/* Add the specified sds value into a set.
 *
 * If the value was already member of the set, nothing is done and 0 is
 * returned, otherwise the new element is added and 1 is returned. */
int setTypeAdd(robj *subject, sds value) {
    return setTypeAddAux(subject, value, sdslen(value), 0, 1);
}

/* Remove a member. This function is optimized for the different encodings. The
 * value can be provided as an sds string (indicated by passing str_is_sds =
 * 1), as string and length (str_is_sds = 0) or as an integer in which case str
 * is set to NULL and llval is provided instead.
 *
 * Returns 1 if the value was deleted and 0 if it was not a member of the set. */
int setTypeRemoveAux(robj *setobj, char *str, size_t len, int64_t llval, int str_is_sds) {
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {
        if (setobj->encoding == OBJ_ENCODING_INTSET) {
            int success;
            setobj->ptr = intsetRemove(setobj->ptr, llval, &success);
            return success;
        }
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    if (setobj->encoding == OBJ_ENCODING_HT) {
        sds sdsval = str_is_sds ? (sds) str : sdsnewlen(str, len);
        int deleted = (dictDelete(setobj->ptr, sdsval) == DICT_OK);
        if (deleted && htNeedsResize(setobj->ptr)) dictResize(setobj->ptr);
        if (sdsval != str) sdsfree(sdsval); /* free temp copy */
        return deleted;
    } else if (setobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = setobj->ptr;
        unsigned char *p = lpFirst(lp);
        if (p == NULL) return 0;
        p = lpFind(lp, p, (unsigned char *) str, len, 0);
        if (p != NULL) {
            lp = lpDelete(lp, p, NULL);
            setobj->ptr = lp;
            return 1;
        }
    } else if (setobj->encoding == OBJ_ENCODING_INTSET) {
        long long llval;
        if (string2ll(str, len, &llval)) {
            int success;
            setobj->ptr = intsetRemove(setobj->ptr, llval, &success);
            if (success) return 1;
        }
    } else {
//        serverPanic("Unknown set encoding");
    }
    return 0;
}

/* Deletes a value provided as an sds string from the set. Returns 1 if the
 * value was deleted and 0 if it was not a member of the set. */
int setTypeRemove(robj *setobj, sds value) {
    return setTypeRemoveAux(setobj, value, sdslen(value), 0, 1);
}


/* Membership checking optimized for the different encodings. The value can be
 * provided as an sds string (indicated by passing str_is_sds = 1), as string
 * and length (str_is_sds = 0) or as an integer in which case str is set to NULL
 * and llval is provided instead.
 *
 * Returns 1 if the value is a member of the set and 0 if it isn't. */
int setTypeIsMemberAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {
        if (set->encoding == OBJ_ENCODING_INTSET)
            return intsetFind(set->ptr, llval);
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    if (set->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = set->ptr;
        unsigned char *p = lpFirst(lp);
        return p && lpFind(lp, p, (unsigned char *) str, len, 0);
    } else if (set->encoding == OBJ_ENCODING_INTSET) {
        long long llval;
        return string2ll(str, len, &llval) && intsetFind(set->ptr, llval);
    } else if (set->encoding == OBJ_ENCODING_HT && str_is_sds) {
        return dictFind(set->ptr, (sds) str) != NULL;
    } else if (set->encoding == OBJ_ENCODING_HT) {
        sds sdsval = sdsnewlen(str, len);
        int result = dictFind(set->ptr, sdsval) != NULL;
        sdsfree(sdsval);
        return result;
    } else {
//        serverPanic("Unknown set encoding");
    }
}


/* Check if an sds string is a member of the set. Returns 1 if the value is a
 * member of the set and 0 if it isn't. */
int setTypeIsMember(robj *subject, sds value) {
    return setTypeIsMemberAux(subject, value, sdslen(value), 0, 1);
}


/* Return random element from a non empty set.
 * The returned element can be an int64_t value if the set is encoded
 * as an "intset" blob of integers, or an string.
 *
 * The caller provides three pointers to be populated with the right
 * object. The return value of the function is the object->encoding
 * field of the object and can be used by the caller to check if the
 * int64_t pointer or the str and len pointers were populated, as for
 * setTypeNext. If OBJ_ENCODING_HT is returned, str is pointed to a
 * string which is actually an sds string and it can be used as such.
 *
 * Note that both the str, len and llele pointers should be passed and cannot
 * be NULL. If str is set to NULL, the value is an integer stored in llele. */
int setTypeRandomElement(robj *setobj, char **str, size_t *len, int64_t *llele) {
    if (setobj->encoding == OBJ_ENCODING_HT) {
        dictEntry *de = dictGetFairRandomKey(setobj->ptr);
        *str = dictGetKey(de);
        *len = sdslen(*str);
        *llele = -123456789; /* Not needed. Defensive. */
    } else if (setobj->encoding == OBJ_ENCODING_INTSET) {
        *llele = intsetRandom(setobj->ptr);
        *str = NULL; /* Not needed. Defensive. */
    } else if (setobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = setobj->ptr;
        int r = rand() % lpLength(lp);
        unsigned char *p = lpSeek(lp, r);
        unsigned int l;
        *str = (char *) lpGetValue(p, &l, (long long *) llele);
        *len = (size_t) l;
    } else {
//        serverPanic("Unknown set encoding");
        return -1;
    }
    return setobj->encoding;
}

/* Pops a random element and returns it as an object. */
robj *setTypePopRandom(robj *set) {
    robj *obj;
    if (set->encoding == OBJ_ENCODING_LISTPACK) {
        /* Find random and delete it without re-seeking the listpack. */
        unsigned int i = 0;
        unsigned char *p = lpNextRandom(set->ptr, lpFirst(set->ptr), &i, 1, 0);
        unsigned int len = 0; /* initialize to silence warning */
        long long llele = 0; /* initialize to silence warning */
        char *str = (char *) lpGetValue(p, &len, &llele);
        if (str)
            obj = createStringObject(str, len);
        else
            obj = createStringObjectFromLongLong(llele);
        set->ptr = lpDelete(set->ptr, p, NULL);
    } else {
        char *str;
        size_t len = 0;
        int64_t llele = 0;
        int encoding = setTypeRandomElement(set, &str, &len, &llele);
        if (str)
            obj = createStringObject(str, len);
        else
            obj = createStringObjectFromLongLong(llele);
        setTypeRemoveAux(set, str, len, llele, encoding == OBJ_ENCODING_HT);
    }
    return obj;
}


/* Convert the set to specified encoding. The resulting dict (when converting
 * to a hash table) is presized to hold the number of elements in the original
 * set. */
void setTypeConvert(robj *setobj, int enc) {
    setTypeConvertAndExpand(setobj, enc, setTypeSize(setobj), 1);
}

/* This is a helper function for the COPY command.
 * Duplicate a set object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
robj *setTypeDup(robj *o) {
    robj *set;
    setTypeIterator *si;

    assert(o->type == OBJ_SET);

    /* Create a new set object that have the same encoding as the original object's encoding */
    if (o->encoding == OBJ_ENCODING_INTSET) {
        intset *is = o->ptr;
        size_t size = intsetBlobLen(is);
        intset *newis = zmalloc(size);
        memcpy(newis, is, size);
        set = createObject(OBJ_SET, newis);
        set->encoding = OBJ_ENCODING_INTSET;
    } else if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = o->ptr;
        size_t sz = lpBytes(lp);
        unsigned char *new_lp = zmalloc(sz);
        memcpy(new_lp, lp, sz);
        set = createObject(OBJ_SET, new_lp);
        set->encoding = OBJ_ENCODING_LISTPACK;
    } else if (o->encoding == OBJ_ENCODING_HT) {
        set = createSetObject();
        dict *d = o->ptr;
        dictExpand(set->ptr, dictSize(d));
        si = setTypeInitIterator(o);
        char *str;
        size_t len;
        int64_t intobj;
        while (setTypeNext(si, &str, &len, &intobj) != -1) {
            setTypeAdd(set, (sds) str);
        }
        setTypeReleaseIterator(si);
    } else {
//        serverPanic("Unknown set encoding");
    }
    return set;
}

int RcSAdd(redisCache db, robj *key, robj *members[], unsigned long members_size) {
    if (NULL == db || NULL == key || NULL == members) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb *) db;

    robj *set = lookupKeyWrite(redis_db, key);
    if (checkType(set, OBJ_SET)) return C_ERR;

    if (set == NULL) {
        set = setTypeCreate(members[0]->ptr);
        dbAdd(redis_db, key, set);
    } else {
//        setTypeMaybeConvert(set, c->argc - 2);
    }

    unsigned int j;
    for (j = 0; j < members_size; j++) {
        setTypeAdd(set, members[j]->ptr);
    }

    return C_OK;
}

int RcSRem(redisCache db, robj *key, robj *members[], unsigned long members_size) {
    if (NULL == db || NULL == key || NULL == members) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    robj *set;
    if ((set = lookupKeyWrite(redis_db,key)) == NULL || checkType(set,OBJ_SET)) {
        return REDIS_KEY_NOT_EXIST;
    }

    unsigned long j;
    for (j = 0; j < members_size; j++) {
        if (setTypeRemove(set, members[j]->ptr)) {

            if (setTypeSize(set) == 0) {
                dbDelete(redis_db, key);
                break;
            }
        }
    }

    return C_OK;
}

int RcSIsmember(redisCache db, robj *key, robj *member, int *is_member) {
    if (NULL == db || NULL == key || NULL == member) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    robj *set;
    if ((set = lookupKeyRead(redis_db,key)) == NULL || checkType(set,OBJ_SET)) {
        return REDIS_KEY_NOT_EXIST;
    }

    *is_member = setTypeIsMember(set,member->ptr);

    return C_OK;
}

int RcSCard(redisCache db, robj *key, unsigned long *len) {
    if (NULL == db || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    robj *o;
    if ((o = lookupKeyRead(redis_db,key)) == NULL || checkType(o,OBJ_SET)) {
        return REDIS_KEY_NOT_EXIST;
    }

    *len = setTypeSize(o);

    return C_OK;
}



/* handle the "SRANDMEMBER key <count>" variant. The normal version of the
 * command is handled by the srandmemberCommand() function itself. */

/* How many times bigger should be the set compared to the requested size
 * for us to don't use the "remove elements" strategy? Read later in the
 * implementation for more info. */
#define SRANDMEMBER_SUB_STRATEGY_MUL 3

/* If client is trying to ask for a very large number of random elements,
 * queuing may consume an unlimited amount of memory, so we want to limit
 * the number of randoms per time. */
#define SRANDFIELD_RANDOM_SAMPLE_LIMIT 1000

int RcSRandmember(redisCache db, robj *key, long l, sds **members, unsigned long *members_size) {
    if (NULL == db || NULL == key || NULL == members) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    robj *subject;
    if ((subject = lookupKeyRead(redis_db,key)) == NULL || checkType(subject,OBJ_SET)) {
        return REDIS_KEY_NOT_EXIST;
    }

    unsigned long count, size;
    int uniq = 1;
    robj *set;
    char *str;
    size_t len;
    int64_t llele;

    dict *d;

    if (l >= 0) {
        count = (unsigned long) l;
    } else {
        /* A negative count means: return the same elements multiple times
         * (i.e. don't remove the extracted element after every extraction). */
        count = -l;
        uniq = 0;
    }

    size = setTypeSize(set);

    /* If count is zero, serve it ASAP to avoid special cases later. */
    if (count == 0) {
        return C_OK;
    }

    /* CASE 1: The count was negative, so the extraction method is just:
     * "return N random elements" sampling the whole set every time.
     * This case is trivial and can be served without auxiliary data
     * structures. This case is the only one that also needs to return the
     * elements in random order. */
    if (!uniq || count == 1) {
        *members_size = count;
        *members = (sds *)zcallocate(sizeof(sds) * count);
        sds *arrays = *members;
        // TODO pika后续适配
//        if (set->encoding == OBJ_ENCODING_LISTPACK && count > 1) {
//            /* Specialized case for listpack, traversing it only once. */
//            unsigned long limit, sample_count;
//            limit = count > SRANDFIELD_RANDOM_SAMPLE_LIMIT ? SRANDFIELD_RANDOM_SAMPLE_LIMIT : count;
//            listpackEntry *entries = zmalloc(limit * sizeof(listpackEntry));
//            while (count) {
//                sample_count = count > limit ? limit : count;
//                count -= sample_count;
//                lpRandomEntries(set->ptr, sample_count, entries);
//                for (unsigned long i = 0; i < sample_count; i++) {
//                    if (entries[i].sval)
//                        addReplyBulkCBuffer(c, entries[i].sval, entries[i].slen);
//                    else
//                        addReplyBulkLongLong(c, entries[i].lval);
//                }
//                if (c->flags & CLIENT_CLOSE_ASAP)
//                    break;
//            }
//            zfree(entries);
//            return C_OK;
//        }
        int i = 0;
        while (count--) {
            setTypeRandomElement(set, &str, &len, &llele);
            if (str == NULL) {
                arrays[i] = sdsfromlonglong(llele);
            } else {
                arrays[i] = sdsdup(str);
            }
            ++i;
        }
        return C_OK;
    }

    /* CASE 2:
     * The number of requested elements is greater than the number of
     * elements inside the set: simply return the whole set. */
    if (count >= size) {
        setTypeIterator *si;
        sds *arrays = *members;
        si = setTypeInitIterator(set);
        int encoding;
        unsigned int i = 0;
        while ((encoding = setTypeNext(si, &str, &len, &llele)) != -1) {
            if (encoding == OBJ_ENCODING_HT) {
                arrays[i] = sdsdup(str);
            } else {
                arrays[i] = sdsfromlonglong(llele);
            }

            ++i;
            if (i >= *members_size) break;
        }
        setTypeReleaseIterator(si);
        assert(size == 0);
        return C_OK;
    }

    /* CASE 2.5 listpack only. Sampling unique elements, in non-random order.
     * Listpack encoded sets are meant to be relatively small, so
     * SRANDMEMBER_SUB_STRATEGY_MUL isn't necessary and we rather not make
     * copies of the entries. Instead, we emit them directly to the output
     * buffer.
     *
     * And it is inefficient to repeatedly pick one random element from a
     * listpack in CASE 4. So we use this instead. */
//    if (set->encoding == OBJ_ENCODING_LISTPACK) {
//        unsigned char *lp = set->ptr;
//        unsigned char *p = lpFirst(lp);
//        unsigned int i = 0;
//
//        while (count) {
//            p = lpNextRandom(lp, p, &i, count--, 0);
//            unsigned int len;
//            str = (char *) lpGetValue(p, &len, (long long *) &llele);
//            if (str == NULL) {
//                addReplyBulkLongLong(c, llele);
//            } else {
//                addReplyBulkCBuffer(c, str, len);
//            }
//            p = lpNext(lp, p);
//            i++;
//        }
//        return;
//    }

    /* For CASE 3 and CASE 4 we need an auxiliary dictionary. */
    d = dictCreate(&objectKeyPointerValueDictType);

    /* CASE 3:
     * The number of elements inside the set is not greater than
     * SRANDMEMBER_SUB_STRATEGY_MUL times the number of requested elements.
     * In this case we create a set from scratch with all the elements, and
     * subtract random elements to reach the requested number of elements.
     *
     * This is done because if the number of requested elements is just
     * a bit less than the number of elements in the set, the natural approach
     * used into CASE 4 is highly inefficient. */
    if (count * SRANDMEMBER_SUB_STRATEGY_MUL > size) {
        setTypeIterator *si;

        /* Add all the elements into the temporary dictionary. */
        si = setTypeInitIterator(set);
        dictExpand(d, size);
        while (setTypeNext(si, &str, &len, &llele) != -1) {
            int retval = DICT_ERR;

            if (str == NULL) {
                retval = dictAdd(d, sdsfromlonglong(llele), NULL);
            } else {
                retval = dictAdd(d, sdsnewlen(str, len), NULL);
            }
            assert(retval == DICT_OK);
        }
        setTypeReleaseIterator(si);
        assert(dictSize(d) == size);

        /* Remove random elements to reach the right count. */
        while (size > count) {
            dictEntry *de;
            de = dictGetFairRandomKey(d);
            dictUnlink(d, dictGetKey(de));
            sdsfree(dictGetKey(de));
            dictFreeUnlinkedEntry(d, de);
            size--;
        }
    }

        /* CASE 4: We have a big set compared to the requested number of elements.
         * In this case we can simply get random elements from the set and add
         * to the temporary set, trying to eventually get enough unique elements
         * to reach the specified count. */
    else {
        unsigned long added = 0;
        sds sdsele;

        dictExpand(d, count);
        while (added < count) {
            setTypeRandomElement(set, &str, &len, &llele);
            if (str == NULL) {
                sdsele = sdsfromlonglong(llele);
            } else {
                sdsele = sdsnewlen(str, len);
            }
            /* Try to add the object to the dictionary. If it already exists
             * free it, otherwise increment the number of objects we have
             * in the result dictionary. */
            if (dictAdd(d, sdsele, NULL) == DICT_OK)
                added++;
            else
                sdsfree(sdsele);
        }
    }

    /* CASE 3 & 4: send the result to the user. */
    {
        unsigned long i = 0;
        dictIterator *di;
        dictEntry *de;
        *members_size = count;
        *members = (sds *)zcallocate(sizeof(sds) * count);
        sds *arrays = *members;

        di = dictGetIterator(d);
        while((de = dictNext(di)) != NULL) {
            robj *o = dictGetKey(de);
            if (sdsEncodedObject(o)) {
                arrays[i] = sdsdup(o->ptr);
            } else if (o->encoding == OBJ_ENCODING_INT) {
                arrays[i] = sdsfromlonglong((long)o->ptr);
            }

            ++i;
            if (i >= count) break;
        }
        dictReleaseIterator(di);
        dictRelease(d);
    }
}

static void SMembers(robj *subject,
                     sds **members,
                     unsigned long *members_size) {

    *members_size = setTypeSize(subject);
    *members = (sds *)zcallocate(sizeof(sds) * (*members_size));
    sds *arrays = *members;

    unsigned long i = 0;
    sds elesds;
    int64_t intobj;
    int encoding;
    size_t len;
    setTypeIterator *si = setTypeInitIterator(subject);
    while((encoding = setTypeNext(si,&elesds, &len, &intobj)) != -1) {
        if (encoding == OBJ_ENCODING_HT) {
            arrays[i] = sdsdup(elesds);
        } else {
            arrays[i] = sdsfromlonglong(intobj);
        }

        ++i;
        if (i >= *members_size) break;
    }
    setTypeReleaseIterator(si);
}

int RcSMembers(redisCache db, robj *key, sds **members, unsigned long *members_size)
{
    if (NULL == db || NULL == key || NULL == members) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    robj *subject;
    if ((subject = lookupKeyRead(redis_db,key)) == NULL || checkType(subject,OBJ_SET)) {
        return REDIS_KEY_NOT_EXIST;
    }

    SMembers(subject, members, members_size);

    return C_OK;
}
