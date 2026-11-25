/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Bounded raw-byte cache for HTTP streams which alternate between track ranges.
 * Caller serializes access. A block exposes only a contiguous initialized span;
 * unrelated partial writes must never make a hole readable. */
#ifndef MPV_CURL_SEEK_CACHE_H
#define MPV_CURL_SEEK_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct mp_curl_cache_block {
    uint64_t offset, age;
    size_t begin, end;
    unsigned char *data;
};

struct mp_curl_cache {
    struct mp_curl_cache_block *blocks;
    size_t count, block_size;
    uint64_t clock;
};

static inline void mp_curl_cache_init(struct mp_curl_cache *c, size_t budget)
{
    *c = (struct mp_curl_cache){0};
    if (!budget)
        return;
    c->block_size = budget < 256 * 1024 ? budget : 256 * 1024;
    c->count = budget / c->block_size;
    c->blocks = calloc(c->count, sizeof(*c->blocks));
    if (!c->blocks)
        c->count = 0;
}

static inline void mp_curl_cache_uninit(struct mp_curl_cache *c)
{
    for (size_t i = 0; i < c->count; i++)
        free(c->blocks[i].data);
    free(c->blocks);
    *c = (struct mp_curl_cache){0};
}

static inline struct mp_curl_cache_block *mp_curl_cache_find(
    struct mp_curl_cache *c, uint64_t offset)
{
    for (size_t i = 0; i < c->count; i++) {
        struct mp_curl_cache_block *b = &c->blocks[i];
        if (b->data && b->end > b->begin && b->offset == offset)
            return b;
    }
    return NULL;
}

static inline bool mp_curl_cache_has(struct mp_curl_cache *c, uint64_t pos)
{
    if (!c->count)
        return false;
    uint64_t offset = pos - pos % c->block_size;
    struct mp_curl_cache_block *b = mp_curl_cache_find(c, offset);
    size_t in = pos - offset;
    return b && in >= b->begin && in < b->end;
}

static inline size_t mp_curl_cache_get(struct mp_curl_cache *c, uint64_t pos,
                                     void *out, size_t size)
{
    if (!size || !mp_curl_cache_has(c, pos))
        return 0;
    uint64_t offset = pos - pos % c->block_size;
    struct mp_curl_cache_block *b = mp_curl_cache_find(c, offset);
    size_t in = pos - offset;
    size_t n = size < b->end - in ? size : b->end - in;
    memcpy(out, b->data + in, n);
    b->age = ++c->clock;
    return n;
}

static inline void mp_curl_cache_put(struct mp_curl_cache *c, uint64_t pos,
                                    const void *data, size_t size)
{
    while (c->count && size && pos < UINT64_MAX) {
        uint64_t offset = pos - pos % c->block_size;
        size_t in = pos - offset;
        size_t n = size < c->block_size - in ? size : c->block_size - in;
        if (n > UINT64_MAX - pos)
            n = UINT64_MAX - pos;
        struct mp_curl_cache_block *b = mp_curl_cache_find(c, offset);
        if (!b) {
            b = &c->blocks[0];
            for (size_t i = 0; i < c->count; i++) {
                if (!c->blocks[i].data) {
                    b = &c->blocks[i];
                    break;
                }
                if (c->blocks[i].age < b->age)
                    b = &c->blocks[i];
            }
            if (!b->data)
                b->data = malloc(c->block_size);
            if (!b->data)
                return; // Caching must not make playback fail on allocation.
            b->offset = offset;
            b->begin = b->end = 0;
        }
        memcpy(b->data + in, data, n);
        if (b->end == b->begin || in > b->end || in + n < b->begin) {
            b->begin = in;
            b->end = in + n;
        } else {
            if (in < b->begin)
                b->begin = in;
            if (in + n > b->end)
                b->end = in + n;
        }
        b->age = ++c->clock;
        pos += n;
        data = (const unsigned char *)data + n;
        size -= n;
    }
}

#endif
