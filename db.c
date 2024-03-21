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

#include <ctype.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "atomicvar.h"

#include "db.h"
#include "object.h"
#include "atomicvar.h"
#include "commondef.h"
#include "commonfunc.h"
#include "zmalloc.h"

#include <signal.h>
#include <ctype.h>

extern db_config g_db_config;
extern db_status g_db_status;


/* Db->dict, keys are sds strings, vals are Redis objects. */
dictType dbDictType = {
        dictSdsHash,                /* hash function */
        NULL,                       /* key dup */
        NULL,                       /* val dup */
        dictSdsKeyCompare,          /* key compare */
        dictSdsDestructor,          /* key destructor */
        dictObjectDestructor   /* val destructor */
};

/* Db->expires */
dictType keyptrDictType = {
        dictSdsHash,                /* hash function */
        NULL,                       /* key dup */
        NULL,                       /* val dup */
        dictSdsKeyCompare,          /* key compare */
        NULL,                       /* key destructor */
        NULL                        /* val destructor */
};

/* Hash type hash table (note that small hashes are represented with ziplists) */
dictType hashDictType = {
        dictSdsHash,                /* hash function */
        NULL,                       /* key dup */
        NULL,                       /* val dup */
        dictSdsKeyCompare,          /* key compare */
        dictSdsDestructor,          /* key destructor */
        dictSdsDestructor           /* val destructor */
};

/* Generic hash table type where keys are Redis Objects, Values
 * dummy pointers. */
dictType objectKeyPointerValueDictType = {
        dictEncObjHash,            /* hash function */
        NULL,                      /* key dup */
        NULL,                      /* val dup */
        dictEncObjKeyCompare,      /* key compare */
        dictObjectDestructor,      /* key destructor */
        NULL                       /* val destructor */
};

redisDb* createRedisDb(void)
{
    redisDb *db = zcallocate(sizeof(*db));
    if (NULL == db) return NULL;

    db->dict = dictCreate(&dbDictType);
    db->expires = dictCreate(&keyptrDictType);
    db->eviction_pool = evictionPoolAlloc();
    return db;
}

void closeRedisDb(redisDb *db)
{
    if (db) {
        dictRelease(db->dict);
        dictRelease(db->expires);
        evictionPoolDestroy(db->eviction_pool);
        zfree(db->eviction_pool);
        zfree(db);
    }
}


/*-----------------------------------------------------------------------------
 * C-level DB API
 *----------------------------------------------------------------------------*/

/* Flags for expireIfNeeded */
#define EXPIRE_FORCE_DELETE_EXPIRED 1
#define EXPIRE_AVOID_DELETE_EXPIRED 2

static void dbSetValue(redisDb *db, robj *key, robj *val, int overwrite, dictEntry *de);

/* Update LFU when an object is accessed.
 * Firstly, decrement the counter if the decrement time is reached.
 * Then logarithmically increment the counter, and update the access time. */
void updateLFU(robj *val) {
    unsigned long counter = LFUDecrAndReturn(val);
    counter = LFULogIncr(counter);
    val->lru = (LFUGetTimeInMinutes()<<8) | counter;
}

/* Lookup a key for read or write operations, or return NULL if the key is not
 * found in the specified DB. This function implements the functionality of
 * lookupKeyRead(), lookupKeyWrite() and their ...WithFlags() variants.
 *
 * Side-effects of calling this function:
 *
 * 1. A key gets expired if it reached it's TTL.
 * 2. The key's last access time is updated.
 * 3. The global keys hits/misses stats are updated (reported in INFO).
 * 4. If keyspace notifications are enabled, a "keymiss" notification is fired.
 *
 * Flags change the behavior of this command:
 *
 *  LOOKUP_NONE (or zero): No special flags are passed.
 *  LOOKUP_NOTOUCH: Don't alter the last access time of the key.
 *  LOOKUP_NONOTIFY: Don't trigger keyspace event on key miss.
 *  LOOKUP_NOSTATS: Don't increment key hits/misses counters.
 *  LOOKUP_WRITE: Prepare the key for writing (delete expired keys even on
 *                replicas, use separate keyspace stats and events (TODO)).
 *  LOOKUP_NOEXPIRE: Perform expiration check, but avoid deleting the key,
 *                   so that we don't have to propagate the deletion.
 *
 * Note: this function also returns NULL if the key is logically expired but
 * still existing, in case this is a replica and the LOOKUP_WRITE is not set.
 * Even if the key expiry is master-driven, we can correctly report a key is
 * expired on replicas even if the master is lagging expiring our key via DELs
 * in the replication link. */
robj *lookupKey(redisDb *db, robj *key, int flags) {
    dictEntry *de = dictFind(db->dict,key->ptr);
    robj *val = NULL;
    if (de) {
        val = dictGetVal(de);
        /* Forcing deletion of expired keys on a replica makes the replica
         * inconsistent with the master. We forbid it on readonly replicas, but
         * we have to allow it on writable replicas to make write commands
         * behave consistently.
         *
         * It's possible that the WRITE flag is set even during a readonly
         * command, since the command may trigger events that cause modules to
         * perform additional writes. */
//        int is_ro_replica = server.masterhost && server.repl_slave_ro;
        int expire_flags = 0;
//        if (flags & LOOKUP_WRITE && !is_ro_replica)
        if (flags & LOOKUP_WRITE)
            expire_flags |= EXPIRE_FORCE_DELETE_EXPIRED;
        if (flags & LOOKUP_NOEXPIRE)
            expire_flags |= EXPIRE_AVOID_DELETE_EXPIRED;
        if (expireIfNeeded(db, key, expire_flags)) {
            /* The key is no longer valid. */
            val = NULL;
        }
    }

    if (val) {
        int maxmemory_policy;
        atomicGet(g_db_config.maxmemory_policy, maxmemory_policy);
        /* Update the access time for the ageing algorithm.
         * Don't do it if we have a saving child, as this will trigger
         * a copy on write madness. */
//        if (server.current_client && server.current_client->flags & CLIENT_NO_TOUCH &&
//            server.current_client->cmd->proc != touchCommand)
//            flags |= LOOKUP_NOTOUCH;
        if (!(flags & LOOKUP_NOTOUCH)){
            if (maxmemory_policy & MAXMEMORY_FLAG_LFU) {
                updateLFU(val);
            } else {
                val->lru = LRU_CLOCK();
            }
        }



        if (!(flags & (LOOKUP_NOSTATS | LOOKUP_WRITE)))
            atomicIncr(g_db_status.stat_keyspace_hits, 1);
        /* TODO: Use separate hits stats for WRITE */
    } else {
//        if (!(flags & (LOOKUP_NONOTIFY | LOOKUP_WRITE)))
//            notifyKeyspaceEvent(NOTIFY_KEY_MISS, "keymiss", key, db->id);
        if (!(flags & (LOOKUP_NOSTATS | LOOKUP_WRITE)))
//            server.stat_keyspace_misses++;
            atomicIncr(g_db_status.stat_keyspace_misses, 1);
        return NULL;
        /* TODO: Use separate misses stats and notify event for WRITE */
    }

    return val;
}

/* Lookup a key for read operations, or return NULL if the key is not found
 * in the specified DB.
 *
 * This API should not be used when we write to the key after obtaining
 * the object linked to the key, but only for read only operations.
 *
 * This function is equivalent to lookupKey(). The point of using this function
 * rather than lookupKey() directly is to indicate that the purpose is to read
 * the key. */
robj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags) {
    assert(!(flags & LOOKUP_WRITE));
    return lookupKey(db, key, flags);
}

/* Like lookupKeyReadWithFlags(), but does not use any flag, which is the
 * common case. */
robj *lookupKeyRead(redisDb *db, robj *key) {
    return lookupKeyReadWithFlags(db,key,LOOKUP_NONE);
}

/* Lookup a key for write operations, and as a side effect, if needed, expires
 * the key if its TTL is reached. It's equivalent to lookupKey() with the
 * LOOKUP_WRITE flag added.
 *
 * Returns the linked value object if the key exists or NULL if the key
 * does not exist in the specified DB. */
robj *lookupKeyWriteWithFlags(redisDb *db, robj *key, int flags) {
    return lookupKey(db, key, flags | LOOKUP_WRITE);
}

robj *lookupKeyWrite(redisDb *db, robj *key) {
    return lookupKeyWriteWithFlags(db, key, LOOKUP_NONE);
}


/* Add the key to the DB. It's up to the caller to increment the reference
 * counter of the value if needed.
 *
 * If the update_if_existing argument is false, the the program is aborted
 * if the key already exists, otherwise, it can fall back to dbOverwite. */
static void dbAddInternal(redisDb *db, robj *key, robj *val, int update_if_existing) {
    dictEntry *existing;
    dictEntry *de = dictAddRaw(db->dict, key->ptr, &existing);
    if (update_if_existing && existing) {
        dbSetValue(db, key, val, 1, existing);
        return;
    }
//    serverAssertWithInfo(NULL, key, de != NULL);
    dictSetKey(db->dict, de, sdsdup(key->ptr));
//    initObjectLRUOrLFU(val);
    dictSetVal(db->dict, de, val);
//    signalKeyAsReady(db, key, val->type);
//    if (server.cluster_enabled) slotToKeyAddEntry(de, db);
//    notifyKeyspaceEvent(NOTIFY_NEW,"new",key,db->id);
}

void dbAdd(redisDb *db, robj *key, robj *val) {
    dbAddInternal(db, key, val, 0);
}

/* This is a special version of dbAdd() that is used only when loading
 * keys from the RDB file: the key is passed as an SDS string that is
 * retained by the function (and not freed by the caller).
 *
 * Moreover this function will not abort if the key is already busy, to
 * give more control to the caller, nor will signal the key as ready
 * since it is not useful in this context.
 *
 * The function returns 1 if the key was added to the database, taking
 * ownership of the SDS string, otherwise 0 is returned, and is up to the
 * caller to free the SDS string. */
//int dbAddRDBLoad(redisDb *db, sds key, robj *val) {
//    dictEntry *de = dictAddRaw(db->dict, key, NULL);
//    if (de == NULL) return 0;
//    initObjectLRUOrLFU(val);
//    dictSetVal(db->dict, de, val);
//    if (server.cluster_enabled) slotToKeyAddEntry(de, db);
//    return 1;
//}

/* Overwrite an existing key with a new value. Incrementing the reference
 * count of the new value is up to the caller.
 * This function does not modify the expire time of the existing key.
 *
 * The 'overwrite' flag is an indication whether this is done as part of a
 * complete replacement of their key, which can be thought as a deletion and
 * replacement (in which case we need to emit deletion signals), or just an
 * update of a value of an existing key (when false).
 *
 * The dictEntry input is optional, can be used if we already have one.
 *
 * The program is aborted if the key was not already present. */
static void dbSetValue(redisDb *db, robj *key, robj *val, int overwrite, dictEntry *de) {
    if (!de) de = dictFind(db->dict,key->ptr);

    robj *old = dictGetVal(de);

    val->lru = old->lru;

    if (overwrite) {
        /* RM_StringDMA may call dbUnshareStringValue which may free val, so we
         * need to incr to retain old */
//        incrRefCount(old);
//        /* Although the key is not really deleted from the database, we regard
//         * overwrite as two steps of unlink+add, so we still need to call the unlink
//         * callback of the module. */
//        moduleNotifyKeyUnlink(key,old,db->id,DB_FLAG_KEY_OVERWRITE);
//        /* We want to try to unblock any module clients or clients using a blocking XREADGROUP */
//        signalDeletedKeyAsReady(db,key,old->type);
//        decrRefCount(old);
        /* Because of RM_StringDMA, old may be changed, so we need get old again */
        old = dictGetVal(de);
    }
    dictSetVal(db->dict, de, val);

    // TODO 不确定这里要怎么处理
//    if (server.lazyfree_lazy_server_del) {
//        freeObjAsync(key,old,db->id);
//    } else {

        /* This is just decrRefCount(old); */
    db->dict->type->valDestructor(db->dict, old);
//    }
}

/* Replace an existing key with a new value, we just replace value and don't
 * emit any events */
void dbReplaceValue(redisDb *db, robj *key, robj *val) {
    dbSetValue(db, key, val, 0, NULL);
}

/* High level Set operation. This function can be used in order to set
 * a key, whatever it was existing or not, to a new object.
 *
 * 1) The ref count of the value object is incremented.
 * 2) clients WATCHing for the destination key notified.
 * 3) The expire time of the key is reset (the key is made persistent),
 *    unless 'SETKEY_KEEPTTL' is enabled in flags.
 * 4) The key lookup can take place outside this interface outcome will be
 *    delivered with 'SETKEY_ALREADY_EXIST' or 'SETKEY_DOESNT_EXIST'
 *
 * All the new keys in the database should be created via this interface.
 * The client 'c' argument may be set to NULL if the operation is performed
 * in a context where there is no clear client performing the operation. */
void setKey(redisDb *db, robj *key, robj *val, int flags) {
    int keyfound = 0;

    if (flags & SETKEY_ALREADY_EXIST)
        keyfound = 1;
    else if (flags & SETKEY_ADD_OR_UPDATE)
        keyfound = -1;
    else if (!(flags & SETKEY_DOESNT_EXIST))
        keyfound = (lookupKeyWrite(db,key) != NULL);

    if (!keyfound) {
        dbAdd(db,key,val);
    } else if (keyfound<0) {
        dbAddInternal(db,key,val,1);
    } else {
        dbSetValue(db,key,val,1,NULL);
    }
    incrRefCount(val);
    if (!(flags & SETKEY_KEEPTTL)) removeExpire(db,key);
//    if (!(flags & SETKEY_NO_SIGNAL)) signalModifiedKey(c,db,key);
}

/* Return a random key, in form of a Redis object.
 * If there are no keys, NULL is returned.
 *
 * The function makes sure to return keys not already expired. */
robj *dbRandomKey(redisDb *db) {
    dictEntry *de;
//    int maxtries = 100;
//    int allvolatile = dictSize(db->dict) == dictSize(db->expires);

    while(1) {
        sds key;
        robj *keyobj;

        de = dictGetFairRandomKey(db->dict);
        if (de == NULL) return NULL;

        key = dictGetKey(de);
        keyobj = createStringObject(key,sdslen(key));
        if (dictFind(db->expires,key)) {
//            if (allvolatile && server.masterhost && --maxtries == 0) {
//                /* If the DB is composed only of keys with an expire set,
//                 * it could happen that all the keys are already logically
//                 * expired in the slave, so the function cannot stop because
//                 * expireIfNeeded() is false, nor it can stop because
//                 * dictGetFairRandomKey() returns NULL (there are keys to return).
//                 * To prevent the infinite loop we do some tries, but if there
//                 * are the conditions for an infinite loop, eventually we
//                 * return a key name that may be already expired. */
//                return keyobj;
//            }
            if (expireIfNeeded(db,keyobj,0)) {
                decrRefCount(keyobj);
                continue; /* search for another key. This expired. */
            }
        }
        return keyobj;
    }
}

/* Helper for sync and async delete. */
int dbGenericDelete(redisDb *db, robj *key, int async, int flags) {
    dictEntry **plink;
    int table;
    dictEntry *de = dictTwoPhaseUnlinkFind(db->dict,key->ptr,&plink,&table);
    if (de) {
        robj *val = dictGetVal(de);
        /* RM_StringDMA may call dbUnshareStringValue which may free val, so we
         * need to incr to retain val */
        incrRefCount(val);
        /* Tells the module that the key has been unlinked from the database. */
//        moduleNotifyKeyUnlink(key,val,db->id,flags);
        /* We want to try to unblock any module clients or clients using a blocking XREADGROUP */
//        signalDeletedKeyAsReady(db,key,val->type);
        /* We should call decr before freeObjAsync. If not, the refcount may be
         * greater than 1, so freeObjAsync doesn't work */
        decrRefCount(val);
        if (async) {
            /* Because of dbUnshareStringValue, the val in de may change. */
//            freeObjAsync(key, dictGetVal(de), db->id);
            dictSetVal(db->dict, de, NULL);
        }
//        if (server.cluster_enabled) slotToKeyDelEntry(de, db);

        /* Deleting an entry from the expires dict will not free the sds of
        * the key, because it is shared with the main dictionary. */
        if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);
        dictTwoPhaseUnlinkFree(db->dict,de,plink,table);
        return 1;
    } else {
        return 0;
    }
}

/* Delete a key, value, and associated expiration entry if any, from the DB */
int dbSyncDelete(redisDb *db, robj *key) {
    return dbGenericDelete(db, key, 0, DB_FLAG_KEY_DELETED);
}

/* Delete a key, value, and associated expiration entry if any, from the DB. If
 * the value consists of many allocations, it may be freed asynchronously. */
int dbAsyncDelete(redisDb *db, robj *key) {
    return dbGenericDelete(db, key, 1, DB_FLAG_KEY_DELETED);
}

/* This is a wrapper whose behavior depends on the Redis lazy free
 * configuration. Deletes the key synchronously or asynchronously. */
int dbDelete(redisDb *db, robj *key) {
    return dbGenericDelete(db, key, 1, DB_FLAG_KEY_DELETED);
}

/* Remove all keys from all the databases in a Redis server.
 * If callback is given the function is called from time to time to
 * signal that work is in progress.
 *
 * The dbnum can be -1 if all teh DBs should be flushed, or the specified
 * DB number if we want to flush only a single Redis database number.
 *
 * Flags are be EMPTYDB_NO_FLAGS if no special flags are specified or
 * EMPTYDB_ASYNC if we want the memory to be freed in a different thread
 * and the function to return ASAP.
 *
 * On success the fuction returns the number of keys removed from the
 * database(s). Otherwise -1 is returned in the specific case the
 * DB number is out of range, and errno is set to EINVAL. */
long long emptyDb(redisDb *db, void(callback)(dict*)) {
    long long removed = 0;

    removed += dictSize(db->dict);
    dictEmpty(db->dict,callback);
    dictEmpty(db->expires,callback);
    atomicSet(g_db_status.stat_keyspace_hits, 0);
    atomicSet(g_db_status.stat_keyspace_misses, 0);

    return removed;
}

int removeExpire(redisDb *db, robj *key) {
    return dictDelete(db->expires,key->ptr) == DICT_OK;
}

/* Set an expire to the specified key. If the expire is set in the context
 * of an user calling a command 'c' is the client, otherwise 'c' is set
 * to NULL. The 'when' parameter is the absolute unix time in milliseconds
 * after which the key will no longer be considered valid. */
void setExpire(redisDb *db, robj *key, long long when) {
    dictEntry *kde, *de;

    /* Reuse the sds from the main dict in the expire dict */
    if (NULL != (kde = dictFind(db->dict,key->ptr))) {
        de = dictAddOrFind(db->expires,dictGetKey(kde));
        dictSetSignedIntegerVal(de,when);
    }
}

/* Check if the key is expired. */
int keyIsExpired(redisDb *db, robj *key) {

    mstime_t when = getExpire(db,key);
    mstime_t now;

    if (when < 0) return 0; /* No expire for this key */

    now = mstime();

    /* The key expired if the current (virtual or real) time is greater
     * than the expire time of the key. */
    return now > when;
}

/* This function is called when we are going to perform some operation
 * in a given key, but such key may be already logically expired even if
 * it still exists in the database. The main way this function is called
 * is via lookupKey*() family of functions.
 *
 * The behavior of the function depends on the replication role of the
 * instance, because slave instances do not expire keys, they wait
 * for DELs from the master for consistency matters. However even
 * slaves will try to have a coherent return value for the function,
 * so that read commands executed in the slave side will be able to
 * behave like if the key is expired even if still present (because the
 * master has yet to propagate the DEL).
 *
 * In masters as a side effect of finding a key which is expired, such
 * key will be evicted from the database. Also this may trigger the
 * propagation of a DEL/UNLINK command in AOF / replication stream.
 *
 * The return value of the function is 0 if the key is still valid,
 * otherwise the function returns 1 if the key is expired. */
int expireIfNeeded(redisDb *db, robj *key, int flags) {
//    if (server.lazy_expire_disabled) return 0;
    if (!keyIsExpired(db,key)) return 0;

    /* If we are running in the context of a replica, instead of
     * evicting the expired key from the database, we return ASAP:
     * the replica key expiration is controlled by the master that will
     * send us synthesized DEL operations for expired keys. The
     * exception is when write operations are performed on writable
     * replicas.
     *
     * Still we try to return the right information to the caller,
     * that is, 0 if we think the key should be still valid, 1 if
     * we think the key is expired at this time.
     *
     * When replicating commands from the master, keys are never considered
     * expired. */
//    if (server.masterhost != NULL) {
//        if (server.current_client && (server.current_client->flags & CLIENT_MASTER)) return 0;
//        if (!(flags & EXPIRE_FORCE_DELETE_EXPIRED)) return 1;
//    }

    /* In some cases we're explicitly instructed to return an indication of a
     * missing key without actually deleting it, even on masters. */
    if (flags & EXPIRE_AVOID_DELETE_EXPIRED)
        return 1;

    /* If 'expire' action is paused, for whatever reason, then don't expire any key.
     * Typically, at the end of the pause we will properly expire the key OR we
     * will have failed over and the new primary will send us the expire. */
//    if (isPausedActionsWithUpdate(PAUSE_ACTION_EXPIRE)) return 1;

    /* The key needs to be converted from static to heap before deleted */
    int static_key = key->refcount == OBJ_STATIC_REFCOUNT;
    if (static_key) {
        key = createStringObject(key->ptr, sdslen(key->ptr));
    }
    /* Delete the key */
    dbDelete(db,key);
    if (static_key) {
        decrRefCount(key);
    }
    return 1;
}

/* Return the expire time of the specified key, or -1 if no expire
 * is associated with this key (i.e. the key is non volatile) */
long long getExpire(redisDb *db, robj *key) {
    dictEntry *de;

    /* No expire? return ASAP */
    if (dictSize(db->expires) == 0 ||
        (de = dictFind(db->expires,key->ptr)) == NULL) return -1;

    return dictGetSignedIntegerVal(de);
}

/* ----------------------------------------------------------------------------
 * The external API for eviction: freeMemroyIfNeeded() is called by the
 * server when there is data to add in order to make space if needed.
 * --------------------------------------------------------------------------*/
int freeMemoryIfNeeded(redisDb *db) {
    size_t mem_used, mem_tofree, mem_freed;
    long long delta;
    unsigned long long maxmemory;
    int maxmemory_policy;

    /* Check if we are over the memory usage limit. If we are not, no need
     * to subtract the slaves output buffers. We can just return ASAP. */
    atomicGet(g_db_config.maxmemory, maxmemory);
    mem_used = zmalloc_used_memory();
    if (mem_used <= maxmemory) return C_OK;

    /* Compute how much memory we need to free. */
    mem_tofree = mem_used - maxmemory;
    mem_freed = 0;

    atomicGet(g_db_config.maxmemory_policy, maxmemory_policy);
    if (maxmemory_policy == MAXMEMORY_NO_EVICTION) return C_ERR;

    while (mem_freed < mem_tofree) {
        int k, keys_freed = 0;
        sds bestkey = NULL;
        dict *dict;
        dictEntry *de;

        if (maxmemory_policy & (MAXMEMORY_FLAG_LRU|MAXMEMORY_FLAG_LFU) ||
            maxmemory_policy == MAXMEMORY_VOLATILE_TTL)
        {
            struct evictionPoolEntry *pool = db->eviction_pool;

            while(bestkey == NULL) {
                unsigned long keys = 0;

                /* We don't want to make local-db choices when expiring keys,
                 * so to start populate the eviction pool sampling keys from
                 * every DB. */
                dict = (maxmemory_policy & MAXMEMORY_FLAG_ALLKEYS) ?
                       db->dict : db->expires;
                if ((keys = dictSize(dict)) != 0) {
                    evictionPoolPopulate(dict, db->dict, pool);
                }
                if (!keys) break; /* No keys to evict. */

                /* Go backward from best to worst element to evict. */
                for (k = EVPOOL_SIZE-1; k >= 0; k--) {
                    if (pool[k].key == NULL) continue;

                    if (maxmemory_policy & MAXMEMORY_FLAG_ALLKEYS) {
                        de = dictFind(db->dict, pool[k].key);
                    } else {
                        de = dictFind(db->expires, pool[k].key);
                    }

                    /* Remove the entry from the pool. */
                    if (pool[k].key != pool[k].cached)
                        sdsfree(pool[k].key);
                    pool[k].key = NULL;
                    pool[k].idle = 0;

                    /* If the key exists, is our pick. Otherwise it is
                     * a ghost and we need to try the next element. */
                    if (de) {
                        bestkey = dictGetKey(de);
                        break;
                    } else {
                        /* Ghost... Iterate again. */
                    }
                }
            }
        }

            /* volatile-random and allkeys-random policy */
        else if (maxmemory_policy == MAXMEMORY_ALLKEYS_RANDOM ||
                 maxmemory_policy == MAXMEMORY_VOLATILE_RANDOM)
        {
            /* When evicting a random key, we try to evict a key for
             * each DB, so we use the static 'next_db' variable to
             * incrementally visit all DBs. */
            dict = (maxmemory_policy == MAXMEMORY_ALLKEYS_RANDOM) ?
                   db->dict : db->expires;
            if (dictSize(dict) != 0) {
                de = dictGetRandomKey(dict);
                bestkey = dictGetKey(de);
            }
        }

        /* Finally remove the selected key. */
        if (bestkey) {
            robj *keyobj = createStringObject(bestkey,sdslen(bestkey));
            delta = (long long) zmalloc_used_memory();
            dbDelete(db,keyobj);
            delta -= (long long) zmalloc_used_memory();
            mem_freed += delta;

            g_db_status.stat_evictedkeys++;
            decrRefCount(keyobj);
            keys_freed++;
        }

        if (!keys_freed) return C_ERR;
    }

    return C_OK;
}

/* Helper function for the activeExpireCycle() function.
 * This function will try to expire the key that is stored in the hash table
 * entry 'de' of the 'expires' hash table of a Redis database.
 *
 * activeExpireCycle() 函数使用的检查键是否过期的辅佐函数。
 *
 * If the key is found to be expired, it is removed from the database and
 * 1 is returned. Otherwise no operation is performed and 0 is returned.
 *
 * 如果 de 中的键已经过期，那么移除它，并返回 1 ，否则不做动作，并返回 0 。
 *
 * When a key is expired, server.stat_expiredkeys is incremented.
 *
 * The parameter 'now' is the current time in milliseconds as is passed
 * to the function to avoid too many gettimeofday() syscalls.
 *
 * 参数 now 是毫秒格式的当前时间
 */
static int activeExpireCycleTryExpire(redisDb *db, dictEntry *de, long long now) {
    // 获取键的过期时间
    long long t = dictGetSignedIntegerVal(de);
    if (now > t) {

        // 键已过期
        sds key = dictGetKey(de);
        robj *keyobj = createStringObject(key,sdslen(key));

        // 从数据库中删除该键
        dbDelete(db,keyobj);
        decrRefCount(keyobj);
        // 更新计数器
        atomicIncr(g_db_status.stat_expiredkeys, 1);
        return 1;
    } else {
        return 0;
    }
}

int activeExpireCycle(redisDb *db)
{
    static int type = ACTIVE_EXPIRE_CYCLE_SLOW;

    unsigned long num, slots;

    // If there is nothing to expire
    if (0 == (num = dictSize(db->expires))) return 0;

    /* When there are less than 1% filled slots getting random
     * keys is expensive, so stop here waiting for better times...
     * The dictionary will be resized asap. */
    slots = dictSlots(db->expires);
    if (num && slots > DICT_HT_INITIAL_SIZE && (num*100/slots < 1)) return 0;

    if (ACTIVE_EXPIRE_CYCLE_SLOW == type) {
        num = (num > ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP) ? ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP : num;
    }
    else {
        num = (num > ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP * 2) ? ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP * 2 : num;
    }

    int expired = 0;
    long long now = mstime();
    while (num--) {
        dictEntry *de;

        // 从 expires 中随机取出一个带过期时间的键
        if ((de = dictGetRandomKey(db->expires)) == NULL) break;

        // 如果键已经过期，那么删除它，并将 expired 计数器增一
        if (activeExpireCycleTryExpire(db,de,now)) expired++;
    }

    // 如果已删除的过期键占当前总数据库带过期时间的键数量的 25 %
    // 則下次執行快速淘汰
    if (expired > ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP/4) {
        type = ACTIVE_EXPIRE_CYCLE_FAST;
    }
    else {
        type = ACTIVE_EXPIRE_CYCLE_SLOW;
    }

    return expired;
}

/* Prepare the string object stored at 'key' to be modified destructively
 * to implement commands like SETBIT or APPEND.
 *
 * An object is usually ready to be modified unless one of the two conditions
 * are true:
 *
 * 1) The object 'o' is shared (refcount > 1), we don't want to affect
 *    other users.
 * 2) The object encoding is not "RAW".
 *
 * If the object is found in one of the above conditions (or both) by the
 * function, an unshared / not-encoded copy of the string object is stored
 * at 'key' in the specified 'db'. Otherwise the object 'o' itself is
 * returned.
 *
 * USAGE:
 *
 * The object 'o' is what the caller already obtained by looking up 'key'
 * in 'db', the usage pattern looks like this:
 *
 * o = lookupKeyWrite(db,key);
 * if (checkType(c,o,OBJ_STRING)) return;
 * o = dbUnshareStringValue(db,key,o);
 *
 * At this point the caller is ready to modify the object, for example
 * using an sdscat() call to append some data, or anything else.
 */
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o) {
    assert(o->type == OBJ_STRING);
    if (o->refcount != 1 || o->encoding != OBJ_ENCODING_RAW) {
        robj *decoded = getDecodedObject(o);
        o = createRawStringObject(decoded->ptr, sdslen(decoded->ptr));
        decrRefCount(decoded);
        dbReplaceValue(db,key,o);
    }
    return o;
}
