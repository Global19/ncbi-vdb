/*===========================================================================
*
*                            PUBLIC DOMAIN NOTICE
*               National Center for Biotechnology Information
*
*  This software/database is a "United States Government Work" under the
*  terms of the United States Copyright Act.  It was written as part of
*  the author's official duties as a United States Government employee and
*  thus cannot be copyrighted.  This software/database is freely available
*  to the public for use. The National Library of Medicine and the U.S.
*  Government have not placed any restriction on its use or reproduction.
*
*  Although all reasonable efforts have been taken to ensure the accuracy
*  and reliability of the software and data, the NLM and the U.S.
*  Government do not and cannot warrant the performance or results that
*  may be obtained by using this software or data. The NLM and the U.S.
*  Government disclaim all warranties, express or implied, including
*  warranties of performance, merchantability or fitness for any particular
*  purpose.
*
*  Please cite the author in any work or product based on this material.
*
* ===========================================================================
*
*/

#include <kdb/extern.h>

#include "windex-priv.h"

#include <kdb/index.h>
#include <kfs/directory.h>
#include <kfs/file.h>
#include <kfs/md5.h>
#include <kfs/mmap.h>
#include <klib/hashtable.h>
#include <klib/text.h>
#include <klib/pack.h>
#include <klib/rc.h>
#include <os-native.h>
#include <sysalloc.h>

#include <byteswap.h>

#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#define KEY_BUFFER_SIZE ((size_t)(16ull * 1024ull *1024ull))

/** @brief: add up all the key lengths
 **/
static size_t totalKeySize(KHTIndex_v5 const *const self)
{
    size_t rslt = 0;
    
    BufferListEntry const *current = self->current;
    while (current) {
        rslt += current->used;
        current = current->prv;
    }
    
    return rslt;
}

/** @brief: make a new buffer to hold more keys
 **/
static BufferListEntry *makeBufferListEntry(BufferListEntry *prv, rc_t *rc)
{
    BufferListEntry *rslt = malloc(sizeof(*rslt));

    if (rslt) {
        rslt->prv = prv;
        rslt->used = 0;
        rslt->buffer = malloc(KEY_BUFFER_SIZE); /* pointers must stay valid so no realloc */
        if (rslt->buffer != NULL)
            return rslt;        
        free(rslt);
    }
    *rc = RC(rcDB, rcIndex, rcAllocating, rcMemory, rcExhausted);
    return NULL;
}

/** @brief: setup in preparation of filling the index with new entries
 **/
static bool initializeWrite(KHTIndex_v5 *const self, rc_t *rc)
{
    self->entries = malloc((self->maxKeys = INITIAL_ENTRY_COUNT) * sizeof(self->entries[0]));
    if (self->entries == NULL) {
        *rc = RC(rcDB, rcIndex, rcAllocating, rcMemory, rcExhausted);
        return false;
    }

    *rc = KHashTableMake(&self->hashtable, sizeof(char *), sizeof(size_t), INITIAL_ENTRY_COUNT, LOADING_FACTOR, KHT_key_type_cstr);
    if (*rc) return false;

    self->current = makeBufferListEntry(NULL, rc);
    if (self->current == NULL) return false;

    return true;
}

/** @brief: copy a nul-terminated string
 **/
static char *cstring_copy(size_t const n, char *const dst, char const *const src)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
    assert(src[n] == '\0');
    dst[n] = '\0';
    return dst;
}

/** @brief: checks that current key buffer is big enough, or creates a new one
 **/
static bool growKeys(KHTIndex_v5 *const self, size_t const keylen, rc_t *rc)
{
    if (self->current->used + keylen + 1 < KEY_BUFFER_SIZE)
        return true;
    self->current = makeBufferListEntry(self->current, rc);
    return self->current != NULL;
}

/** @brief: checks that array is big enough, or realloc's it
 **/
static bool growEntries(KHTIndex_v5 *const self, rc_t *rc)
{
    if (self->keyCount < self->maxKeys)
        return true;
    else {
        void *tmp = realloc(self->entries, (self->maxKeys * 2) * sizeof(self->entries[0]));
        if (tmp) {
            self->maxKeys *= 2;
            self->entries = tmp;
            return true;
        }
    }
    *rc = RC(rcDB, rcIndex, rcInserting, rcMemory, rcExhausted);
    return false;
}

/** @brief: adds a new entry to id-to-key index AND adds key to hashtable
 **/
static bool insertNewEntry( KHTIndex_v5 *const self
                          , size_t const keylen
                          , char const *const key
                          , int64_t const id
                          , rc_t *rc)
{
    size_t const i = self->keyCount;

    if (growEntries(self, rc) && growKeys(self, keylen, rc)) {
        IdMapEntry *const entry = self->entries + i;
        char const *const copy = self->current->buffer + self->current->used;

        entry->name = cstring_copy(keylen, copy, key);
        entry->firstId = entry->lastId = id;

        self->keyCount = i + 1;
        self->current->used += keylen + 1;

        return addEntryToHashTable(self, i, rc);
    }
    return false;
}

/** @brief: extends id-span of an existing entry, fails if not contiguous
 **/
static bool updateEntry(KHTIndex_v5 *const self, uint64_t const i, int64_t const id, rc_t *rc)
{
    IdMapEntry *const entry = self->entries + i;

    if (entry->lastId + 1 == id) {
        entry->lastId = id;
        return true;
    }
    *rc = RC(rcDB, rcIndex, rcInserting, rcConstraint, rcViolated);
    return false;
}

/** @brief: inserts a new entry into the index, or updates an existing one
 **/
rc_t KHTIndexInsert( KHTIndex_v5 *const self
                   , char const *const key
                   , int64_t const id )
{
    rc_t rc = 0;
    size_t keylen;

    assert(self != NULL)
    assert(key != NULL);

    keylen = cstring_byte_count(key);
    assert(keylen != 0); /* is this true? */
    
    if (self->keyCount > 0) {
        /* expected path */

        uint64_t fnd = 0;

        if (id <= self->prvId)
            return RC(rcDB, rcIndex, rcInserting, rcConstraint, rcViolated);
        
        /* check if key exists */
        if (!KHashTableFind(self->hashtable, key, keylen, KHash(keylen, key), &fnd)) {
            if (!insertNewEntry(self, keylen, key, id, &rc))
                return rc;
        }
        else {
            assert(fnd < self->keyCount);
            if (!updateEntry(self, fnd, id, &rc))
                return rc;
        }        
        self->prvId = id;
        self->numId += 1;
    }
    else {
        /* first time path */
        if (!initializeWrite(self, &rc))
            return rc;
        if (!insertNewEntry(self, key, id, &rc))
            return rc;
        self->prvId = self->minId = id;
        self->numId = 1;
    }
    return 0;
}

rc_t KHTIndexDelete( KHTIndex_v5 *const self,
                   , const char *const key
                   )
{
    return RC(rcDB, rcIndex, rcDeleting, rcFunction, rcNotImplemented);
}

typedef struct KHTIndexHdr IndexHeader;
struct KHTIndexHdr {
    KIndexFileHeader_v3 dad; /* index-cmn.h:65 */
    uint64_t entries;
    int64_t numIds, firstId, lastId;
    uint64_t cmp_idsize, cmp_spansize;
    uint64_t keysize, cmp_keysize;
};

static size_t serialize( KHTIndex_v3 const *const self
                       , void *const buffer
                       , size_t const maxsize
                       , size_t const keySize
                       )
{
    IndexHeader *const hdr = buffer;
    uint64_t *const id = (void *)(hdr + 1);
    uint32_t *span = (void *)(id + self->entries);
    char *key = (void *)(span + self->entries);
    uint32_t minspan = 0, maxspan = 0;
    int64_t next = self->minId;
    bool gap = false;
    size_t i;
    
    assert(((void const *)(key + keySize)) <= ((void const *)(((uint8_t const *)buffer) + maxsize)));

    memset(hdr, 0, sizeof(*hdr));
    KDBHdrInit(&hdr->dad.h, KDBINDEXVERS);
    hdr->dad.index_type = kitText | kitProj;

    hdr->entries = self->keyCount;
    hdr->numIds = self->numId;
    hdr->firstId = self->minId;
    hdr->lastId = self->prvId;
    hdr->keysize = keySize;
    
    for (i = 0; i < self->keyCount; ++i) {
        IdMapEntry const *const entry = self->entries + i;
        int64_t const cur = entry->firstId;
        uint32_t const Span = entry->lastId - cur;

        assert(cur + Span == entry->lastId);
        
        id[i] = cur - self->minId;

        gap |= cur != next;
        next = cur + Span + 1;
        
        minspan = (minspan > Span || i == 0) ? Span : minspan;
        maxspan = (maxspan < Span || i == 0) ? Span : maxspan;
    }
    if (gap == false && minspan == 0 && maxspan == 0) {
        key = (void *)id; /* no need to store IDs or spans */
    }
    else {
        /* optimization potential if minspan == maxspan */
        for (i = 0; i < self->keyCount; ++i) {
            IdMapEntry const *const entry = self->entries + i;
            
            span[i] = entry->lastId - entry->firstId;
        }
        hdr->cmp_idsize = sizeof(int64_t) * self->keyCount;
        hdr->cmp_spansize = sizeof(uint32_t) * self->keyCount;
    }
    {
        char *cur = key;
        for (i = 0; i < self->keyCount; ++i) {
            size_t j = 0;
            char const *entry = self->entries[i].name;
            while ((cur[j] = entry[j]) != '\0')
                ++j;
        }
        hdr->cmp_keysize = hdr->keysize;
    }
    return (key + keySize) - (uint8_t const *)buffer;
}

static bool persistFile(KHTIndex_v3 const *const self, KFile *const file, rc_t *const rc)
{
    size_t const keySize = totalKeySize(self);
    size_t const maxsize = sizeof(KHTIndexHdr) + sizeof(int64_t) * self->entries + sizeof(uint32_t) * self->entries + totalKeySize;
    KMMap *map = NULL;
    void *mem = NULL;
    size_t finalSize;

    *rc = KFileSetSize(file, maxsize);
    if (*rc) return false;
    
    *rc = KMMapMakeMaxUpdate(&map, kfile);
    if (*rc) return false;

    *rc = KMMapAddrUpdate(map, &mem);
    if (*rc) return false;
    
    finalSize = serialize(self, mem, maxsize, keysize);
    KMMapRelease(map);
    KFileSetSize(file, finalSize);
    
    return true;
}

rc_t KHTIndexPersist( KHTIndex_v3 *const self
                    , bool const proj
                    , struct KDirectory *const dir
                    , const char *const path
                    , bool const use_md5
                    )
{
    rc_t rc = 0;
    KFile *file = NULL;
    
    persistFile(self, file, &rc);
    return rc;
}