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
#include "util.h"
#include "quicklist.h"
#include "listpack.h"


/* Structure to hold list iteration abstraction. */
typedef struct {
    robj *subject;
    unsigned char encoding;
    unsigned char direction; /* Iteration direction */

    unsigned char *lpi; /* listpack iterator */
    quicklistIter *iter;
} listTypeIterator;

/* Structure for an entry while iterating over a list. */
typedef struct {
    listTypeIterator *li;
    unsigned char *lpe; /* Entry in listpack */
    quicklistEntry entry; /* Entry in quicklist */
} listTypeEntry;

/*-----------------------------------------------------------------------------
 * List API
 *----------------------------------------------------------------------------*/

/* Check the length and size of a number of objects that will be added to list to see
 * if we need to convert a listpack to a quicklist. Note that we only check string
 * encoded objects as their string length can be queried in constant time.
 *
 * If callback is given the function is called in order for caller to do some work
 * before the list conversion. */
static void listTypeTryConvertListpack(robj *o, robj **argv, int start, int end,
                                       beforeConvertCB fn, void *data) {
    assert(o->encoding == OBJ_ENCODING_LISTPACK);

    size_t add_bytes = 0;
    size_t add_length = 0;

    if (argv) {
        for (int i = start; i <= end; i++) {
            if (!sdsEncodedObject(argv[i]))
                continue;
            add_bytes += sdslen(argv[i]->ptr);
        }
        add_length = end - start + 1;
    }

    if (quicklistNodeExceedsLimit(OBJ_HASH_MAX_LISTPACK_VALUE,
                                  lpBytes(o->ptr) + add_bytes, lpLength(o->ptr) + add_length)) {
        /* Invoke callback before conversion. */
        if (fn) fn(data);

        quicklist *ql = quicklistCreate();
        quicklistSetOptions(ql, OBJ_HASH_MAX_LISTPACK_VALUE, OBJ_LIST_COMPRESS_DEPTH);

        /* Append listpack to quicklist if it's not empty, otherwise release it. */
        if (lpLength(o->ptr))
            quicklistAppendListpack(ql, o->ptr);
        else
            lpFree(o->ptr);
        o->ptr = ql;
        o->encoding = OBJ_ENCODING_QUICKLIST;
    }
}

/* Check the length and size of a quicklist to see if we need to convert it to listpack.
 *
 * 'shrinking' is 1 means that the conversion is due to a list shrinking, to avoid
 * frequent conversions of quicklist and listpack due to frequent insertion and
 * deletion, we don't convert quicklist to listpack until its length or size is
 * below half of the limit.
 *
 * If callback is given the function is called in order for caller to do some work
 * before the list conversion. */
static void listTypeTryConvertQuicklist(robj *o, int shrinking, beforeConvertCB fn, void *data) {
    assert(o->encoding == OBJ_ENCODING_QUICKLIST);

    size_t sz_limit;
    unsigned int count_limit;
    quicklist *ql = o->ptr;

    /* A quicklist can be converted to listpack only if it has only one packed node. */
    if (ql->len != 1 || ql->head->container != QUICKLIST_NODE_CONTAINER_PACKED)
        return;

    /* Check the length or size of the quicklist is below the limit. */
    quicklistNodeLimit(OBJ_HASH_MAX_LISTPACK_VALUE, &sz_limit, &count_limit);
    if (shrinking) {
        sz_limit /= 2;
        count_limit /= 2;
    }
    if (ql->head->sz > sz_limit || ql->count > count_limit) return;

    /* Invoke callback before conversion. */
    if (fn) fn(data);

    /* Extract the listpack from the unique quicklist node,
     * then reset it and release the quicklist. */
    o->ptr = ql->head->entry;
    ql->head->entry = NULL;
    quicklistRelease(ql);
    o->encoding = OBJ_ENCODING_LISTPACK;
}

/* Check if the list needs to be converted to appropriate encoding due to
 * growing, shrinking or other cases.
 *
 * 'lct' can be one of the following values:
 * LIST_CONV_AUTO      - Used after we built a new list, and we want to let the
 *                       function decide on the best encoding for that list.
 * LIST_CONV_GROWING   - Used before or right after adding elements to the list,
 *                       in which case we are likely to only consider converting
 *                       from listpack to quicklist.
 *                       'argv' is only used in this case to calculate the size
 *                       of a number of objects that will be added to list.
 * LIST_CONV_SHRINKING - Used after removing an element from the list, in which case we
 *                       wanna consider converting from quicklist to listpack. When we
 *                       know we're shrinking, we use a lower (more strict) threshold in
 *                       order to avoid repeated conversions on every list change. */
static void listTypeTryConversionRaw(robj *o, list_conv_type lct,
                                     robj **argv, int start, int end,
                                     beforeConvertCB fn, void *data) {
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        if (lct == LIST_CONV_GROWING) return; /* Growing has nothing to do with quicklist */
        listTypeTryConvertQuicklist(o, lct == LIST_CONV_SHRINKING, fn, data);
    } else if (o->encoding == OBJ_ENCODING_LISTPACK) {
        if (lct == LIST_CONV_SHRINKING) return; /* Shrinking has nothing to do with listpack */
        listTypeTryConvertListpack(o, argv, start, end, fn, data);
    } else {
        // serverPanic("Unknown list encoding");
    }
}

/* This is just a wrapper for listTypeTryConversionRaw() that is
 * able to try conversion without passing 'argv'. */
void listTypeTryConversion(robj *o, list_conv_type lct, beforeConvertCB fn, void *data) {
    listTypeTryConversionRaw(o, lct, NULL, 0, 0, fn, data);
}

/* This is just a wrapper for listTypeTryConversionRaw() that is
 * able to try conversion before adding elements to the list. */
void listTypeTryConversionAppend(robj *o, robj **argv, int start, int end,
                                 beforeConvertCB fn, void *data) {
    listTypeTryConversionRaw(o, LIST_CONV_GROWING, argv, start, end, fn, data);
}

/* The function pushes an element to the specified list object 'subject',
 * at head or tail position as specified by 'where'.
 *
 * There is no need for the caller to increment the refcount of 'value' as
 * the function takes care of it if needed. */
void listTypePush(robj *subject, robj *value, int where) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        int pos = (where == REDIS_LIST_HEAD) ? QUICKLIST_HEAD : QUICKLIST_TAIL;
        if (value->encoding == OBJ_ENCODING_INT) {
            char buf[32];
            ll2string(buf, 32, (long) value->ptr);
            quicklistPush(subject->ptr, buf, strlen(buf), pos);
        } else {
            quicklistPush(subject->ptr, value->ptr, sdslen(value->ptr), pos);
        }
    } else if (subject->encoding == OBJ_ENCODING_LISTPACK) {
        if (value->encoding == OBJ_ENCODING_INT) {
            subject->ptr = (where == REDIS_LIST_HEAD) ?
                           lpPrependInteger(subject->ptr, (long) value->ptr) :
                           lpAppendInteger(subject->ptr, (long) value->ptr);
        } else {
            subject->ptr = (where == REDIS_LIST_HEAD) ?
                           lpPrepend(subject->ptr, value->ptr, sdslen(value->ptr)) :
                           lpAppend(subject->ptr, value->ptr, sdslen(value->ptr));
        }
    } else {
//        // serverPanic("Unknown list encoding");
    }
}

void *listPopSaver(unsigned char *data, size_t sz) {
    return createStringObject((char *) data, sz);
}

robj *listTypePop(robj *subject, int where) {
    robj *value = NULL;

    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        long long vlong;
        int ql_where = where == REDIS_LIST_HEAD ? QUICKLIST_HEAD : QUICKLIST_TAIL;
        if (quicklistPopCustom(subject->ptr, ql_where, (unsigned char **) &value,
                               NULL, &vlong, listPopSaver)) {
            if (!value)
                value = createStringObjectFromLongLong(vlong);
        }
    } else if (subject->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *p;
        unsigned char *vstr;
        int64_t vlen;
        unsigned char intbuf[LP_INTBUF_SIZE];

        p = (where == REDIS_LIST_HEAD) ? lpFirst(subject->ptr) : lpLast(subject->ptr);
        if (p) {
            vstr = lpGet(p, &vlen, intbuf);
            value = createStringObject((char *) vstr, vlen);
            subject->ptr = lpDelete(subject->ptr, p, NULL);
        }
    } else {
        // serverPanic("Unknown list encoding");
    }
    return value;
}

unsigned long listTypeLength(const robj *subject) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        return quicklistCount(subject->ptr);
    } else if (subject->encoding == OBJ_ENCODING_LISTPACK) {
        return lpLength(subject->ptr);
    } else {
        // serverPanic("Unknown list encoding");
    }
}

/* Initialize an iterator at the specified index. */
listTypeIterator *listTypeInitIterator(robj *subject, long index,
                                       unsigned char direction) {
    listTypeIterator *li = zmalloc(sizeof(listTypeIterator));
    li->subject = subject;
    li->encoding = subject->encoding;
    li->direction = direction;
    li->iter = NULL;
    /* REDIS_LIST_HEAD means start at TAIL and move *towards* head.
     * REDIS_LIST_TAIL means start at HEAD and move *towards* tail. */
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        int iter_direction = direction == REDIS_LIST_HEAD ? AL_START_TAIL : AL_START_HEAD;
        li->iter = quicklistGetIteratorAtIdx(li->subject->ptr,
                                             iter_direction, index);
    } else if (li->encoding == OBJ_ENCODING_LISTPACK) {
        li->lpi = lpSeek(subject->ptr, index);
    } else {
        // serverPanic("Unknown list encoding");
    }
    return li;
}

/* Sets the direction of an iterator. */
void listTypeSetIteratorDirection(listTypeIterator *li, listTypeEntry *entry, unsigned char direction) {
    if (li->direction == direction) return;

    li->direction = direction;
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        int dir = direction == REDIS_LIST_HEAD ? AL_START_TAIL : AL_START_HEAD;
        quicklistSetDirection(li->iter, dir);
    } else if (li->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = li->subject->ptr;
        /* Note that the iterator for listpack always points to the next of the current entry,
         * so we need to update position of the iterator depending on the direction. */
        li->lpi = (direction == REDIS_LIST_HEAD) ? lpNext(lp, entry->lpe) : lpPrev(lp, entry->lpe);
    } else {
        // serverPanic("Unknown list encoding");
    }
}

/* Clean up the iterator. */
void listTypeReleaseIterator(listTypeIterator *li) {
    if (li->encoding == OBJ_ENCODING_QUICKLIST)
        quicklistReleaseIterator(li->iter);
    zfree(li);
}

/* Stores pointer to current the entry in the provided entry structure
 * and advances the position of the iterator. Returns 1 when the current
 * entry is in fact an entry, 0 otherwise. */
int listTypeNext(listTypeIterator *li, listTypeEntry *entry) {
    /* Protect from converting when iterating */
    assert(li->subject->encoding == li->encoding);

    entry->li = li;
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        return quicklistNext(li->iter, &entry->entry);
    } else if (li->encoding == OBJ_ENCODING_LISTPACK) {
        entry->lpe = li->lpi;
        if (entry->lpe != NULL) {
            li->lpi = (li->direction == REDIS_LIST_TAIL) ?
                      lpNext(li->subject->ptr, li->lpi) : lpPrev(li->subject->ptr, li->lpi);
            return 1;
        }
    } else {
        // serverPanic("Unknown list encoding");
    }
    return 0;
}

/* Get entry value at the current position of the iterator.
 * When the function returns NULL, it populates the integer value by
 * reference in 'lval'. Otherwise a pointer to the string is returned,
 * and 'vlen' is set to the length of the string. */
unsigned char *listTypeGetValue(listTypeEntry *entry, size_t *vlen, long long *lval) {
    unsigned char *vstr = NULL;
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        if (entry->entry.value) {
            vstr = entry->entry.value;
            *vlen = entry->entry.sz;
        } else {
            *lval = entry->entry.longval;
        }
    } else if (entry->li->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned int slen;
        vstr = lpGetValue(entry->lpe, &slen, lval);
        *vlen = slen;
    } else {
        // serverPanic("Unknown list encoding");
    }
    return vstr;
}

/* Return entry or NULL at the current position of the iterator. */
robj *listTypeGet(listTypeEntry *entry) {
    unsigned char *vstr;
    size_t vlen;
    long long lval;

    vstr = listTypeGetValue(entry, &vlen, &lval);
    if (vstr)
        return createStringObject((char *) vstr, vlen);
    else
        return createStringObjectFromLongLong(lval);
}

void listTypeInsert(listTypeEntry *entry, robj *value, int where) {
    robj *subject = entry->li->subject;
    value = getDecodedObject(value);
    sds str = value->ptr;
    size_t len = sdslen(str);

    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        if (where == REDIS_LIST_TAIL) {
            quicklistInsertAfter(entry->li->iter, &entry->entry, str, len);
        } else if (where == REDIS_LIST_HEAD) {
            quicklistInsertBefore(entry->li->iter, &entry->entry, str, len);
        }
    } else if (entry->li->encoding == OBJ_ENCODING_LISTPACK) {
        int lpw = (where == REDIS_LIST_TAIL) ? LP_AFTER : LP_BEFORE;
        subject->ptr = lpInsertString(subject->ptr, (unsigned char *) str,
                                      len, entry->lpe, lpw, &entry->lpe);
    } else {
        // serverPanic("Unknown list encoding");
    }
    decrRefCount(value);
}

/* Replaces entry at the current position of the iterator. */
void listTypeReplace(listTypeEntry *entry, robj *value) {
    robj *subject = entry->li->subject;
    value = getDecodedObject(value);
    sds str = value->ptr;
    size_t len = sdslen(str);

    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistReplaceEntry(entry->li->iter, &entry->entry, str, len);
    } else if (entry->li->encoding == OBJ_ENCODING_LISTPACK) {
        subject->ptr = lpReplace(subject->ptr, &entry->lpe, (unsigned char *) str, len);
    } else {
        // serverPanic("Unknown list encoding");
    }

    decrRefCount(value);
}

/* Replace entry at offset 'index' by 'value'.
 *
 * Returns 1 if replace happened.
 * Returns 0 if replace failed and no changes happened. */
int listTypeReplaceAtIndex(robj *o, int index, robj *value) {
    value = getDecodedObject(value);
    sds vstr = value->ptr;
    size_t vlen = sdslen(vstr);
    int replaced = 0;

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklist *ql = o->ptr;
        replaced = quicklistReplaceAtIndex(ql, index, vstr, vlen);
    } else if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *p = lpSeek(o->ptr, index);
        if (p) {
            o->ptr = lpReplace(o->ptr, &p, (unsigned char *) vstr, vlen);
            replaced = 1;
        }
    } else {
        // serverPanic("Unknown list encoding");
    }

    decrRefCount(value);
    return replaced;
}

/* Compare the given object with the entry at the current position. */
int listTypeEqual(listTypeEntry *entry, robj *o) {
    assert(sdsEncodedObject(o));
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        return quicklistCompare(&entry->entry, o->ptr, sdslen(o->ptr));
    } else if (entry->li->encoding == OBJ_ENCODING_LISTPACK) {
        return lpCompare(entry->lpe, o->ptr, sdslen(o->ptr));
    } else {
        // serverPanic("Unknown list encoding");
    }
}

/* Delete the element pointed to. */
void listTypeDelete(listTypeIterator *iter, listTypeEntry *entry) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistDelEntry(iter->iter, &entry->entry);
    } else if (entry->li->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *p = entry->lpe;
        iter->subject->ptr = lpDelete(iter->subject->ptr, p, &p);

        /* Update position of the iterator depending on the direction */
        if (iter->direction == REDIS_LIST_TAIL)
            iter->lpi = p;
        else {
            if (p) {
                iter->lpi = lpPrev(iter->subject->ptr, p);
            } else {
                /* We deleted the last element, so we need to set the
                 * iterator to the last element. */
                iter->lpi = lpLast(iter->subject->ptr);
            }
        }
    } else {
        // serverPanic("Unknown list encoding");
    }
}

/* This is a helper function for the COPY command.
 * Duplicate a list object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
robj *listTypeDup(robj *o) {
    robj *lobj;

    assert(o->type == OBJ_LIST);

    switch (o->encoding) {
        case OBJ_ENCODING_LISTPACK:
            lobj = createObject(OBJ_LIST, lpDup(o->ptr));
            break;
        case OBJ_ENCODING_QUICKLIST:
            lobj = createObject(OBJ_LIST, quicklistDup(o->ptr));
            break;
        default:
            // serverPanic("Unknown list encoding");
            break;
    }
    lobj->encoding = o->encoding;
    return lobj;
}

/* Delete a range of elements from the list. */
void listTypeDelRange(robj *subject, long start, long count) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistDelRange(subject->ptr, start, count);
    } else if (subject->encoding == OBJ_ENCODING_LISTPACK) {
        subject->ptr = lpDeleteRange(subject->ptr, start, count);
    } else {
        // serverPanic("Unknown list encoding");
    }
}

/*-----------------------------------------------------------------------------
 * List Commands
 *----------------------------------------------------------------------------*/

/* Implements LPUSH/RPUSH/LPUSHX/RPUSHX. 
 * 'xx': push if key exists. */
static int pushGenericCommand(redisDb *redis_db, robj *kobj, robj *vals[], unsigned long vals_size, int where) {
    robj *lobj = lookupKeyWrite(redis_db, kobj);
    if (checkType(lobj, OBJ_LIST)) return C_ERR;
    if (!lobj) {
        lobj = createListListpackObject();
        dbAdd(redis_db, kobj, lobj);
    }

    listTypeTryConversionAppend(lobj, vals, 0, 1, NULL, NULL);

    unsigned long i;
    for (i = 0; i < vals_size; i++) {
        listTypePush(lobj, vals[i], where);
    }

    return C_OK;
}

/* LPUSH <key> <element> [<element> ...] */
int RcLPush(redisCache db, robj *key, robj *vals[], unsigned long vals_size) {
    if (NULL == db || NULL == key || NULL == vals) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb *) db;

    return pushGenericCommand(redis_db, key, vals, vals_size, REDIS_LIST_HEAD);
}


/* RPUSH <key> <element> [<element> ...] */
int RcRPush(redisCache db, robj *key, robj *vals[], unsigned long vals_size) {
    if (NULL == db || NULL == key || NULL == vals) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb *) db;

    return pushGenericCommand(redis_db, key, vals, vals_size, REDIS_LIST_TAIL);
}

/* LPUSHX <key> <element> [<element> ...] */
int RcLPushx(redisCache db, robj *key, robj *vals[], unsigned long vals_size) {
    if (NULL == db || NULL == key || NULL == vals) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb *) db;

    return pushGenericCommand(redis_db, key, vals, vals_size, REDIS_LIST_HEAD);
}

/* RPUSHX <key> <element> [<element> ...] */
int RcRPushx(redisCache db, robj *key, robj *vals[], unsigned long vals_size) {
    if (NULL == db || NULL == key || NULL == vals) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb *) db;

    return pushGenericCommand(redis_db, key, vals, vals_size, REDIS_LIST_TAIL);
}

/* LINSERT <key> (BEFORE|AFTER) <pivot> <element> */
int RcLInsert(redisCache db, robj *key, int where, robj *pivot, robj *val) {
    if (NULL == db || NULL == key || NULL == pivot || NULL == val) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb *) db;

    robj *subject;
    if ((subject = lookupKeyWrite(redis_db, key)) == NULL || checkType(subject, OBJ_LIST)) {
        return REDIS_KEY_NOT_EXIST;
    }

    robj *argv[4];
    argv[0] = key;
    argv[1] = where == REDIS_LIST_TAIL ? createObject(OBJ_STRING, sdsnewlen("after", sizeof("after"))) : createObject(
            OBJ_STRING, sdsnewlen("before", sizeof("before")));
    argv[2] = pivot;
    argv[3] = val;


    listTypeTryConversionAppend(subject, argv, 4, 4, NULL, NULL);

    /* Seek pivot from head to tail */
    listTypeEntry entry;
    listTypeIterator *iter = listTypeInitIterator(subject, 0, REDIS_LIST_TAIL);
    while (listTypeNext(iter, &entry)) {
        if (listTypeEqual(&entry, pivot)) {
            listTypeInsert(&entry, val, where);
            break;
        }
    }
    listTypeReleaseIterator(iter);

    return C_OK;
}

/* LLEN <key> */
int RcLLen(redisCache db, robj *key, unsigned long *len) {
    if (NULL == db || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb *) db;

    robj *o;
    if ((o = lookupKeyRead(redis_db, key)) == NULL || checkType(o, OBJ_LIST)) {
        return REDIS_KEY_NOT_EXIST;
    }

    *len = listTypeLength(o);

    return C_OK;
}

/* LINDEX <key> <index> */
int RcLIndex(redisCache db, robj *key, long index, sds *element) {
    if (NULL == db || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb *) db;
    robj *o;
    if ((o = lookupKeyRead(redis_db, key)) == NULL || checkType(o, OBJ_LIST)) {
        return REDIS_KEY_NOT_EXIST;
    }

    listTypeIterator *iter = listTypeInitIterator(o, index, REDIS_LIST_TAIL);
    listTypeEntry entry;
    unsigned char *vstr;
    size_t vlen;
    long long lval;

    if (listTypeNext(iter, &entry)) {
        *element = listTypeGetValue(&entry, &vlen, &lval);
    } else {
        return REDIS_ITEM_NOT_EXIST;
    }

    listTypeReleaseIterator(iter);

    return C_OK;
}

/* LSET <key> <index> <element> */
int RcLSet(redisCache db, robj *key, long index, robj *val) {
    if (NULL == db || NULL == key || NULL == val) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb *) db;

    robj *o;
    o = lookupKeyWrite(redis_db, key);
    if (o == NULL || checkType(o, OBJ_LIST)) return C_ERR;

    robj *argv[3];
    argv[0] = key;
    argv[1] = createStringObjectFromLongLong(index);
    argv[2] = val;


    listTypeTryConversionAppend(o, argv, 3, 3, NULL, NULL);
    if (listTypeReplaceAtIndex(o, index, val)) {
        /* We might replace a big item with a small one or vice versa, but we've
         * already handled the growing case in listTypeTryConversionAppend()
         * above, so here we just need to try the conversion for shrinking. */
        listTypeTryConversion(o, LIST_CONV_SHRINKING, NULL, NULL);
    } else {
        return C_ERR;
    }

    return C_OK;
}

/* A housekeeping helper for list elements popping tasks.
 *
 * If 'signal' is 0, skip calling signalModifiedKey().
 *
 * 'deleted' is an optional output argument to get an indication
 * if the key got deleted by this function. */
void listElementsRemoved(redisDb *redis_db, robj *key, int where, robj *o, long count, int *deleted) {
    char *event = (where == REDIS_LIST_HEAD) ? "lpop" : "rpop";

    if (listTypeLength(o) == 0) {
        if (deleted) *deleted = 1;
        dbDelete(redis_db, key);
    } else {
        listTypeTryConversion(o, LIST_CONV_SHRINKING, NULL, NULL);
        if (deleted) *deleted = 0;
    }
}



/* Implements the generic list pop operation for LPOP/RPOP.
 * The where argument specifies which end of the list is operated on. An
 * optional count may be provided as the third argument of the client's
 * command. */
// count default is 1.
static int popGenericCommand(redisDb *redis_db, robj *kobj, sds *element, int count, int where) {
    if (count <= 0) {
        return C_ERR;
    }

    robj *value;

    robj *o = lookupKeyWrite(redis_db,kobj);
    if (o == NULL || checkType(o,OBJ_LIST)) {
        return REDIS_KEY_NOT_EXIST;
    }


    if (!count || count == 1) {
        /* Pop a single element. This is POP's original behavior that replies
         * with a bulk string. */
        value = listTypePop(o, where);
        assert(value != NULL);
        convertObjectToSds(value, element);
        decrRefCount(value);
        listElementsRemoved(redis_db, kobj, where, o, 1, NULL);
    } else {
        /* Pop a range of elements. An addition to the original POP command,
         *  which replies with a multi-bulk. */
        long llen = listTypeLength(o);
        long rangelen = (count > llen) ? llen : count;
        long rangestart = (where == REDIS_LIST_HEAD) ? 0 : -rangelen;
        long rangeend = (where == REDIS_LIST_HEAD) ? rangelen - 1 : -1;
        int reverse = (where == REDIS_LIST_HEAD) ? 0 : 1;

        listTypeDelRange(o, rangestart, rangelen);
        listElementsRemoved(redis_db, kobj, where, o, rangelen, NULL);
    }

    return C_OK;
}

/* LPOP <key> [count] */
int RcLPop(redisCache db, robj *key, sds *element) {
    popGenericCommand(db, key, element, 1, REDIS_LIST_HEAD);
}

/* RPOP <key> [count] */
int RcRop(redisCache db, robj *key, sds *element) {
    if (NULL == db || NULL == key) {
        return REDIS_INVALID_ARG;
    }

    redisDb *redis_db = (redisDb*)db;
    return popGenericCommand(redis_db, key, element, 1, REDIS_LIST_TAIL);
}

/* Extracted from `addListRangeReply()` to reply with a quicklist list.
 * Note that the purpose is to make the methods small so that the
 * code in the loop can be inlined better to improve performance. */
void addListQuicklistRange(redisCache db, robj *o, int from, int rangelen, int reverse) {
    int direction = reverse ? AL_START_TAIL : AL_START_HEAD;
    quicklistIter *iter = quicklistGetIteratorAtIdx(o->ptr, direction, from);
    quicklistReleaseIterator(iter);
}

/* Extracted from `addListRangeReply()` to reply with a listpack list.
 * Note that the purpose is to make the methods small so that the
 * code in the loop can be inlined better to improve performance. */
void addListListpackRange(redisCache db, robj *o, int from, int rangelen, int reverse) {
    unsigned char *p = lpSeek(o->ptr, from);
    unsigned char *vstr;
    unsigned int vlen;
    long long lval;


    while(rangelen--) {
        assert(p); /* fail on corrupt data */
        vstr = lpGetValue(p, &vlen, &lval);
        p = reverse ? lpPrev(o->ptr,p) : lpNext(o->ptr,p);
    }
}

/* A helper for replying with a list's range between the inclusive start and end
 * indexes as multi-bulk, with support for negative indexes. Note that start
 * must be less than end or an empty array is returned. When the reverse
 * argument is set to a non-zero value, the reply is reversed so that elements
 * are returned from end to start. */
int addListRange(redisCache db, robj *o, long start, long end, int reverse, sds **vals, unsigned long *vals_size) {
    long rangelen, llen = listTypeLength(o);

    /* Convert negative indexes. */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        *vals_size = 0;
        return C_ERR;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;
    *vals_size = rangelen;

    int from = reverse ? end : start;
    if (o->encoding == OBJ_ENCODING_QUICKLIST)
        addListQuicklistRange(db, o, from, rangelen, reverse);
    else if (o->encoding == OBJ_ENCODING_LISTPACK)
        addListListpackRange(db, o, from, rangelen, reverse);
    else
//        serverPanic("Unknown list encoding");
    return C_OK;
}

/* LRANGE <key> <start> <stop> */
int RcLRange(redisCache db, robj *key, long start, long end, sds **vals, unsigned long *vals_size) {
    if (NULL == db || NULL == key || NULL == vals) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    robj *o;
    if ((o = lookupKeyRead(redis_db,key)) == NULL || checkType(o,OBJ_LIST)) {
        return REDIS_KEY_NOT_EXIST;
    }


    addListRange(redis_db, o, start, end, 0, vals, vals_size);
}

/* LTRIM <key> <start> <stop> */
int RcLTrim(redisCache db, robj *key, long start, long end) {
    if (NULL == db || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    robj *o;
    if ((o = lookupKeyWrite(redis_db,key)) == NULL || checkType(o,OBJ_LIST)) {
        return REDIS_KEY_NOT_EXIST;
    }


    if ((o = lookupKeyWrite(redis_db, key)) == NULL ||
        checkType(o, OBJ_LIST))
        return REDIS_KEY_NOT_EXIST;
    long llen = listTypeLength(o);

    /* convert negative indexes */
    if (start < 0) start = llen + start;
    if (end < 0) end = llen + end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    long ltrim, rtrim;
    if (start > end || start >= llen) {
        /* Out of range start or start > end result in empty list */
        ltrim = llen;
        rtrim = 0;
    } else {
        if (end >= llen) end = llen - 1;
        ltrim = start;
        rtrim = llen - end - 1;
    }

    /* Remove list elements to perform the trim */
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistDelRange(o->ptr, 0, ltrim);
        quicklistDelRange(o->ptr, -rtrim, rtrim);
    } else if (o->encoding == OBJ_ENCODING_LISTPACK) {
        o->ptr = lpDeleteRange(o->ptr, 0, ltrim);
        o->ptr = lpDeleteRange(o->ptr, -rtrim, rtrim);
    } else {
        // serverPanic("Unknown list encoding");
    }

    if (listTypeLength(o) == 0) {
        dbDelete(redis_db, key);
    } else {
        listTypeTryConversion(o, LIST_CONV_SHRINKING, NULL, NULL);
    }
    return C_OK;
}

/* LREM <key> <count> <element> */
int RcLRem(redisCache db, robj *key, long count, robj *val) {
    robj *subject, *obj;
    long toremove;
    long removed = 0;

    if (NULL == db || NULL == key || NULL == val) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;


    subject = lookupKeyWrite(redis_db, key);
    if (subject == NULL || checkType(subject, OBJ_LIST)) return REDIS_KEY_NOT_EXIST;

    listTypeIterator *li;
    if (toremove < 0) {
        toremove = -toremove;
        li = listTypeInitIterator(subject, -1, REDIS_LIST_HEAD);
    } else {
        li = listTypeInitIterator(subject, 0, REDIS_LIST_TAIL);
    }

    listTypeEntry entry;
    while (listTypeNext(li, &entry)) {
        if (listTypeEqual(&entry, obj)) {
            listTypeDelete(li, &entry);
            removed++;
            if (toremove && removed == toremove) break;
        }
    }
    listTypeReleaseIterator(li);


    if (listTypeLength(subject) == 0) {
        dbDelete(redis_db, key);
    } else if (removed) {
        listTypeTryConversion(subject, LIST_CONV_SHRINKING, NULL, NULL);
    }

    return C_OK;
}

int RcRPop(redisCache db, robj *key, sds *element)
{
    if (NULL == db || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    return popGenericCommand(redis_db, key, element, 1, REDIS_LIST_TAIL);
}

