/* Bit operations.
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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include "redis.h"
#include "commondef.h"
#include "commonfunc.h"
#include "zmalloc.h"
#include "object.h"
#include "sds.h"
#include "db.h"
#include "util.h"


/* -----------------------------------------------------------------------------
 * Helpers and low level bit functions.
 * -------------------------------------------------------------------------- */

/* Count number of bits set in the binary array pointed by 's' and long
 * 'count' bytes. The implementation of this function is required to
 * work with an input string length up to 512 MB or more (server.proto_max_bulk_len) */
long long redisPopcount(void *s, long count) {
    long long bits = 0;
    unsigned char *p = s;
    uint32_t *p4;
    static const unsigned char bitsinbyte[256] = {0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4,1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,4,5,5,6,5,6,6,7,5,6,6,7,6,7,7,8};

    /* Count initial bytes not aligned to 32 bit. */
    while((unsigned long)p & 3 && count) {
        bits += bitsinbyte[*p++];
        count--;
    }

    /* Count bits 28 bytes at a time */
    p4 = (uint32_t*)p;
    while(count>=28) {
        uint32_t aux1, aux2, aux3, aux4, aux5, aux6, aux7;

        aux1 = *p4++;
        aux2 = *p4++;
        aux3 = *p4++;
        aux4 = *p4++;
        aux5 = *p4++;
        aux6 = *p4++;
        aux7 = *p4++;
        count -= 28;

        aux1 = aux1 - ((aux1 >> 1) & 0x55555555);
        aux1 = (aux1 & 0x33333333) + ((aux1 >> 2) & 0x33333333);
        aux2 = aux2 - ((aux2 >> 1) & 0x55555555);
        aux2 = (aux2 & 0x33333333) + ((aux2 >> 2) & 0x33333333);
        aux3 = aux3 - ((aux3 >> 1) & 0x55555555);
        aux3 = (aux3 & 0x33333333) + ((aux3 >> 2) & 0x33333333);
        aux4 = aux4 - ((aux4 >> 1) & 0x55555555);
        aux4 = (aux4 & 0x33333333) + ((aux4 >> 2) & 0x33333333);
        aux5 = aux5 - ((aux5 >> 1) & 0x55555555);
        aux5 = (aux5 & 0x33333333) + ((aux5 >> 2) & 0x33333333);
        aux6 = aux6 - ((aux6 >> 1) & 0x55555555);
        aux6 = (aux6 & 0x33333333) + ((aux6 >> 2) & 0x33333333);
        aux7 = aux7 - ((aux7 >> 1) & 0x55555555);
        aux7 = (aux7 & 0x33333333) + ((aux7 >> 2) & 0x33333333);
        bits += ((((aux1 + (aux1 >> 4)) & 0x0F0F0F0F) +
                    ((aux2 + (aux2 >> 4)) & 0x0F0F0F0F) +
                    ((aux3 + (aux3 >> 4)) & 0x0F0F0F0F) +
                    ((aux4 + (aux4 >> 4)) & 0x0F0F0F0F) +
                    ((aux5 + (aux5 >> 4)) & 0x0F0F0F0F) +
                    ((aux6 + (aux6 >> 4)) & 0x0F0F0F0F) +
                    ((aux7 + (aux7 >> 4)) & 0x0F0F0F0F))* 0x01010101) >> 24;
    }
    /* Count the remaining bytes. */
    p = (unsigned char*)p4;
    while(count--) bits += bitsinbyte[*p++];
    return bits;
}

/* Return the position of the first bit set to one (if 'bit' is 1) or
 * zero (if 'bit' is 0) in the bitmap starting at 's' and long 'count' bytes.
 *
 * The function is guaranteed to return a value >= 0 if 'bit' is 0 since if
 * no zero bit is found, it returns count*8 assuming the string is zero
 * padded on the right. However if 'bit' is 1 it is possible that there is
 * not a single set bit in the bitmap. In this special case -1 is returned. */
long long redisBitpos(void *s, unsigned long count, int bit) {
    unsigned long *l;
    unsigned char *c;
    unsigned long skipval, word = 0, one;
    long long pos = 0; /* Position of bit, to return to the caller. */
    unsigned long j;
    int found;

    /* Process whole words first, seeking for first word that is not
     * all ones or all zeros respectively if we are looking for zeros
     * or ones. This is much faster with large strings having contiguous
     * blocks of 1 or 0 bits compared to the vanilla bit per bit processing.
     *
     * Note that if we start from an address that is not aligned
     * to sizeof(unsigned long) we consume it byte by byte until it is
     * aligned. */

    /* Skip initial bits not aligned to sizeof(unsigned long) byte by byte. */
    skipval = bit ? 0 : UCHAR_MAX;
    c = (unsigned char*) s;
    found = 0;
    while((unsigned long)c & (sizeof(*l)-1) && count) {
        if (*c != skipval) {
            found = 1;
            break;
        }
        c++;
        count--;
        pos += 8;
    }

    /* Skip bits with full word step. */
    l = (unsigned long*) c;
    if (!found) {
        skipval = bit ? 0 : ULONG_MAX;
        while (count >= sizeof(*l)) {
            if (*l != skipval) break;
            l++;
            count -= sizeof(*l);
            pos += sizeof(*l)*8;
        }
    }

    /* Load bytes into "word" considering the first byte as the most significant
     * (we basically consider it as written in big endian, since we consider the
     * string as a set of bits from left to right, with the first bit at position
     * zero.
     *
     * Note that the loading is designed to work even when the bytes left
     * (count) are less than a full word. We pad it with zero on the right. */
    c = (unsigned char*)l;
    for (j = 0; j < sizeof(*l); j++) {
        word <<= 8;
        if (count) {
            word |= *c;
            c++;
            count--;
        }
    }

    /* Special case:
     * If bits in the string are all zero and we are looking for one,
     * return -1 to signal that there is not a single "1" in the whole
     * string. This can't happen when we are looking for "0" as we assume
     * that the right of the string is zero padded. */
    if (bit == 1 && word == 0) return -1;

    /* Last word left, scan bit by bit. The first thing we need is to
     * have a single "1" set in the most significant position in an
     * unsigned long. We don't know the size of the long so we use a
     * simple trick. */
    one = ULONG_MAX; /* All bits set to 1.*/
    one >>= 1;       /* All bits set to 1 but the MSB. */
    one = ~one;      /* All bits set to 0 but the MSB. */

    while(one) {
        if (((one & word) != 0) == bit) return pos;
        pos++;
        one >>= 1;
    }

    /* If we reached this point, there is a bug in the algorithm, since
     * the case of no match is handled as a special case before. */
    //serverPanic("End of redisBitpos() reached.");
    return 0; /* Just to avoid warnings. */
}




/* This is a helper function for commands implementations that need to write
 * bits to a string object. The command creates or pad with zeroes the string
 * so that the 'maxbit' bit can be addressed. The object is finally
 * returned. Otherwise if the key holds a wrong type NULL is returned and
 * an error is sent to the client. */
robj *lookupStringForBitCommand(redisDb *redis_db, robj *kobj, uint64_t maxbit, int *dirty) {
    size_t byte = maxbit >> 3;
    robj *o = lookupKeyWrite(redis_db, kobj);
    if (checkType(o,OBJ_STRING)) return NULL;
    if (dirty) *dirty = 0;

    if (o == NULL) {
        o = createObject(OBJ_STRING,sdsnewlen(NULL, byte+1));
        dbAdd(redis_db,kobj,o);
        if (dirty) *dirty = 1;
    } else {
        o = dbUnshareStringValue(redis_db,kobj,o);
        size_t oldlen = sdslen(o->ptr);
        o->ptr = sdsgrowzero(o->ptr,byte+1);
        if (dirty && oldlen != sdslen(o->ptr)) *dirty = 1;
    }
    return o;
}

/* Return a pointer to the string object content, and stores its length
 * in 'len'. The user is required to pass (likely stack allocated) buffer
 * 'llbuf' of at least LONG_STR_SIZE bytes. Such a buffer is used in the case
 * the object is integer encoded in order to provide the representation
 * without using heap allocation.
 *
 * The function returns the pointer to the object array of bytes representing
 * the string it contains, that may be a pointer to 'llbuf' or to the
 * internal object representation. As a side effect 'len' is filled with
 * the length of such buffer.
 *
 * If the source object is NULL the function is guaranteed to return NULL
 * and set 'len' to 0. */
unsigned char *getObjectReadOnlyString(robj *o, long *len, char *llbuf) {
    assert(!o || o->type == OBJ_STRING);
    unsigned char *p = NULL;

    /* Set the 'p' pointer to the string, that can be just a stack allocated
     * array if our string was integer encoded. */
    if (o && o->encoding == OBJ_ENCODING_INT) {
        p = (unsigned char*) llbuf;
        if (len) *len = ll2string(llbuf,LONG_STR_SIZE,(long)o->ptr);
    } else if (o) {
        p = (unsigned char*) o->ptr;
        if (len) *len = sdslen(o->ptr);
    } else {
        if (len) *len = 0;
    }
    return p;
}

/* SETBIT key offset bitvalue */
int RcSetBit(redisCache db, robj *key, size_t bitoffset, long on) {
    if (NULL == db || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*)db;

    /* Bits can only be set or cleared... */
    if (on & ~1) {
        return C_ERR;
    }

    int dirty;
    robj *o;
    if ((o = lookupStringForBitCommand(redis_db, key, bitoffset, &dirty)) == NULL) return C_ERR;

    ssize_t byte, bit;
    int byteval, bitval;

    /* Get current values */
    byte = bitoffset >> 3;
    byteval = ((uint8_t*)o->ptr)[byte];
    bit = 7 - (bitoffset & 0x7);
    bitval = byteval & (1 << bit);

    /* Either it is newly created, changed length, or the bit changes before and after.
     * Note that the bitval here is actually a decimal number.
     * So we need to use `!!` to convert it to 0 or 1 for comparison. */
    if (dirty || (!!bitval != on)) {
        /* Update byte with new bit value. */
        byteval &= ~(1 << bit);
        byteval |= ((on & 0x1) << bit);
        ((uint8_t*)o->ptr)[byte] = byteval;
    }
    return C_OK;
}

/* GETBIT key offset */
int RcGetBit(redisCache db, robj *key, size_t bitoffset, long *val) {
    if (NULL == db || NULL == key) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*) db;

    robj *o;
    if ((o = lookupKeyRead(redis_db, key)) == NULL ||
        checkType(o,OBJ_STRING)) return REDIS_KEY_NOT_EXIST;

    char llbuf[32];
    size_t byte, bit;
    size_t bitval = 0;

    byte = bitoffset >> 3;
    bit = 7 - (bitoffset & 0x7);
    if (sdsEncodedObject(o)) {
        if (byte < sdslen(o->ptr))
            bitval = ((uint8_t*)o->ptr)[byte] & (1 << bit);
    } else {
        if (byte < (size_t)ll2string(llbuf,sizeof(llbuf),(long)o->ptr))
            bitval = llbuf[byte] & (1 << bit);
    }

    *val = bitval ? 1 : 0;

    return C_OK;
}


/* BITCOUNT key [start end [BIT|BYTE]] */
// add isbit, if isbit is 1, get BIT, or get BYTE
int RcBitCount(redisCache db, robj *key, long start, long end, int isbit, long *val, int have_offset) {
    if (NULL == db || NULL == key || isbit > 1) {
        return REDIS_INVALID_ARG;
    }
    redisDb *redis_db = (redisDb*) db;

    unsigned char first_byte_neg_mask = 0, last_byte_neg_mask = 0;

    robj *o;
    /* Lookup, check for type, and return 0 for non existing keys. */
    if ((o = lookupKeyRead(redis_db, key)) == NULL ||
        checkType(o,OBJ_STRING)) return REDIS_KEY_NOT_EXIST;


    long strlen;
    char llbuf[LONG_STR_SIZE];
    unsigned char *p;
    p = getObjectReadOnlyString(o,&strlen,llbuf);

    /* Parse start/end range if any. */
    if (have_offset) {
        long long totlen = strlen;
        /* Convert negative indexes */
        if (start < 0 && end < 0 && start > end) {
            return C_ERR;
        }

        if (isbit) totlen <<= 3;
        if (start < 0) start = totlen+start;
        if (end < 0) end = totlen+end;
        if (start < 0) start = 0;
        if (end < 0) end = 0;
        if (end >= totlen) end = totlen-1;
        if (isbit && start <= end) {
            /* Before converting bit offset to byte offset, create negative masks
             * for the edges. */
            first_byte_neg_mask = ~((1<<(8-(start&7)))-1) & 0xFF;
            last_byte_neg_mask = (1<<(7-(end&7)))-1;
            start >>= 3;
            end >>= 3;
        }
    } else  {
        /* The whole string. */
        start = 0;
        end = strlen-1;
    }

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * zero can be returned is: start > end. */
    if (start > end) {
        *val = 0;
    } else {
        long bytes = (long)(end-start+1);
        long long count = redisPopcount(p+start,bytes);
        if (first_byte_neg_mask != 0 || last_byte_neg_mask != 0) {
            unsigned char firstlast[2] = {0, 0};
            /* We may count bits of first byte and last byte which are out of
            * range. So we need to subtract them. Here we use a trick. We set
            * bits in the range to zero. So these bit will not be excluded. */
            if (first_byte_neg_mask != 0) firstlast[0] = p[start] & first_byte_neg_mask;
            if (last_byte_neg_mask != 0) firstlast[1] = p[end] & last_byte_neg_mask;
            count -= redisPopcount(firstlast,2);
        }
        *val = count;
    }

    return C_OK;
}

/* BITPOS key bit [start [end [BIT|BYTE]]] */
int RcBitPos(redisCache db, robj *key, long bit, long start, long end, int isbit, long *val, int offset_status) {
    if (NULL == db || NULL == key || isbit > 1) {
        return REDIS_INVALID_ARG;
    }

    redisDb *redis_db = (redisDb*)db;

    if (bit != 0 && bit != 1) {
        return C_ERR;
    }

    robj *o;
    /* If the key does not exist, from our point of view it is an infinite
     * array of 0 bits. If the user is looking for the first clear bit return 0,
     * If the user is looking for the first set bit, return -1. */
    if ((o = lookupKeyRead(redis_db, key)) == NULL || checkType(o,OBJ_STRING)) {
        return REDIS_KEY_NOT_EXIST;
    }

    long strlen;
    unsigned char *p;
    char llbuf[LONG_STR_SIZE];
    int end_given = 0;
    unsigned char first_byte_neg_mask = 0, last_byte_neg_mask = 0;
    p = getObjectReadOnlyString(o,&strlen,llbuf);

    /* Parse start/end range if any. */
    if (BIT_POS_START_OFFSET == offset_status || BIT_POS_START_END_OFFSET == offset_status) {
        long long totlen = strlen;
        assert(totlen <= LLONG_MAX >> 3);
        if (BIT_POS_START_END_OFFSET == offset_status) {
            end_given = 1;
        } else {
            if (isbit) end = (totlen<<3) + 7;
            else end = totlen-1;
        }
        if (isbit) totlen <<= 3;
        /* Convert negative indexes */
        if (start < 0) start = totlen+start;
        if (end < 0) end = totlen+end;
        if (start < 0) start = 0;
        if (end < 0) end = 0;
        if (end >= totlen) end = totlen-1;
        if (isbit && start <= end) {
            /* Before converting bit offset to byte offset, create negative masks
             * for the edges. */
            first_byte_neg_mask = ~((1<<(8-(start&7)))-1) & 0xFF;
            last_byte_neg_mask = (1<<(7-(end&7)))-1;
            start >>= 3;
            end >>= 3;
        }
    } else if (BIT_POS_NO_OFFSET == offset_status) {
        /* The whole string. */
        start = 0;
        end = strlen-1;
    } else {
        /* Syntax error. */
        return C_ERR;
    }

    /* For empty ranges (start > end) we return -1 as an empty range does
     * not contain a 0 nor a 1. */
    if (start > end) {
        *val = -1;
    } else {
        long bytes = end-start+1;
        long long pos;
        unsigned char tmpchar;
        if (first_byte_neg_mask) {
            if (bit) tmpchar = p[start] & ~first_byte_neg_mask;
            else tmpchar = p[start] | first_byte_neg_mask;
            /* Special case, there is only one byte */
            if (last_byte_neg_mask && bytes == 1) {
                if (bit) tmpchar = tmpchar & ~last_byte_neg_mask;
                else tmpchar = tmpchar | last_byte_neg_mask;
            }
            pos = redisBitpos(&tmpchar,1,bit);
            /* If there are no more bytes or we get valid pos, we can exit early */
            if (bytes == 1 || (pos != -1 && pos != 8)) goto result;
            start++;
            bytes--;
        }
        /* If the last byte has not bits in the range, we should exclude it */
        long curbytes = bytes - (last_byte_neg_mask ? 1 : 0);
        if (curbytes > 0) {
            pos = redisBitpos(p+start,curbytes,bit);
            /* If there is no more bytes or we get valid pos, we can exit early */
            if (bytes == curbytes || (pos != -1 && pos != (long long)curbytes<<3)) goto result;
            start += curbytes;
            bytes -= curbytes;
        }
        if (bit) tmpchar = p[end] & ~last_byte_neg_mask;
        else tmpchar = p[end] | last_byte_neg_mask;
        pos = redisBitpos(&tmpchar,1,bit);
        *val = pos;

    result:
        /* If we are looking for clear bits, and the user specified an exact
         * range with start-end, we can't consider the right of the range as
         * zero padded (as we do when no explicit end is given).
         *
         * So if redisBitpos() returns the first bit outside the range,
         * we return -1 to the caller, to mean, in the specified range there
         * is not a single "0" bit. */
        if (end_given && bit == 0 && pos == (long long)bytes<<3) {
            return C_ERR;
        }
        if (pos != -1) pos += (long long)start<<3; /* Adjust for the bytes we skipped. */
        *val = pos;
    }

    return C_OK;
}
