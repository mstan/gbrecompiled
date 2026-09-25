/**
 * @file rewind.c
 * @brief Rewind buffer, ported from RetroArch's state manager
 *
 * Follows libretro/RetroArch state_manager.c (GPLv3): the same patch format,
 * ring layout and push/pop bookkeeping, rewritten here rather than copied.
 *
 * A patch is a run of 16-bit words:
 *   changed != 0:  changed, skip, then `changed` words of the older state,
 *                  which go after `skip` unchanged words
 *   changed == 0:  a longer skip as two words (low, high); a zero skip ends
 *                  the patch
 *
 * In the ring, a record is an offset, a patch and another offset. The first
 * offset holds where the next record starts, so the tail can drop the oldest;
 * the last holds where this record starts, so the head can step back to the
 * newest. When a worst-case record might not fit before the end of the ring,
 * that last offset goes at the very start and the next record follows it.
 */

#include "rewind.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

struct GBRewind {
    uint8_t* data;          /* the ring of records */
    size_t capacity;
    uint8_t* head;          /* where the next record starts */
    uint8_t* tail;          /* the oldest record */
    uint8_t* thisblock;     /* the newest state, whole */
    uint8_t* nextblock;     /* where the next state is written */
    size_t blocksize;       /* the state size in whole words */
    size_t maxcompsize;     /* the largest record */
    unsigned entries;
    bool thisblock_valid;   /* thisblock is a state still in the buffer */
    bool thisblock_state;   /* thisblock holds a state: one popped, if not valid */
};

static void write_offset(uint8_t* at, size_t value) {
    memcpy(at, &value, sizeof(value));
}

static size_t read_offset(const uint8_t* at) {
    size_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

/* A state's block: its words, zero words that end find_same's scans, a guard
 * word that differs between the two blocks (uniq) and ends find_change's,
 * and room for find_change's 16-byte reads past the guard. Neither scan has
 * to check the length. */
static uint8_t* block_alloc(size_t size, uint16_t uniq) {
    const size_t words = (size + 1) / sizeof(uint16_t);
    uint16_t* block = (uint16_t*)calloc(words + 4 + 8, sizeof(uint16_t));
    if (block) block[words + 3] = uniq;
    return (uint8_t*)block;
}

/* Words before the first that differs. */
static size_t find_change(const uint16_t* a, const uint16_t* b) {
#if defined(__SSE2__)
    const uint8_t* start = (const uint8_t*)a;
    const __m128i* va = (const __m128i*)a;
    const __m128i* vb = (const __m128i*)b;
    for (;; va++, vb++) {
        const unsigned equal = (unsigned)_mm_movemask_epi8(
            _mm_cmpeq_epi32(_mm_loadu_si128(va), _mm_loadu_si128(vb)));
        if (equal != 0xFFFFu) {
            /* The first unequal 32-bit lane, then which of its words. */
            const size_t word = ((size_t)((const uint8_t*)va - start) +
                                 (size_t)__builtin_ctz(~equal)) / sizeof(uint16_t);
            return word + (a[word] == b[word]);
        }
    }
#else
    const uint16_t* from = a;
    while (*a == *b) {
        a++;
        b++;
    }
    return (size_t)(a - from);
#endif
}

/* Words before the first two equal ones. The scan compares 32-bit pairs, so a
 * lone equal word stays inside the changed run, where it costs less than the
 * header a new run would need. */
static size_t find_same(const uint16_t* a, const uint16_t* b) {
    const uint16_t* from = a;
    for (;;) {
        uint32_t x, y;
        memcpy(&x, a, sizeof(x));
        memcpy(&y, b, sizeof(y));
        if (x == y) break;
        a += 2;
        b += 2;
    }
    if (a != from && a[-1] == b[-1]) a--;
    return (size_t)(a - from);
}

/* The largest patch for a state of `size` bytes. A skip after the first is
 * at least two words (find_same stops at two equal words), no fewer than its
 * header takes; left over are the first header, one per split of a run at
 * 65535 words, and the three-word end. */
static size_t max_patch_size(size_t size) {
    const size_t words = (size + 1) / sizeof(uint16_t);
    const size_t headers = words / UINT16_MAX + 1;
    return (words + headers * 2 + 3) * sizeof(uint16_t);
}

/* Writes the patch that turns `newer` back into `older` and returns its size
 * in bytes. */
static size_t compress(const uint8_t* older, const uint8_t* newer, size_t size, uint8_t* patch) {
    const uint16_t* old16 = (const uint16_t*)older;
    const uint16_t* new16 = (const uint16_t*)newer;
    uint16_t* out = (uint16_t*)patch;
    size_t words = size / sizeof(uint16_t);

    while (words) {
        size_t skip = find_change(old16, new16);
        if (skip >= words) break;
        old16 += skip;
        new16 += skip;
        words -= skip;
        if (skip > UINT16_MAX) {
            *out++ = 0;
            *out++ = (uint16_t)skip;
            *out++ = (uint16_t)(skip >> 16);
            continue;
        }

        size_t changed = find_same(old16, new16);
        if (changed > UINT16_MAX) changed = UINT16_MAX;
        *out++ = (uint16_t)changed;
        *out++ = (uint16_t)skip;
        for (size_t i = 0; i < changed; i++) out[i] = old16[i];
        old16 += changed;
        new16 += changed;
        words -= changed;
        out += changed;
    }
    out[0] = out[1] = out[2] = 0;
    return (size_t)((uint8_t*)(out + 3) - patch);
}

static void decompress(const uint8_t* patch, uint8_t* state) {
    const uint16_t* in = (const uint16_t*)patch;
    uint16_t* out = (uint16_t*)state;
    for (;;) {
        const uint16_t changed = *in++;
        if (changed) {
            out += *in++;
            /* Runs are short; a plain loop beats memcpy's call overhead. */
            for (uint16_t i = 0; i < changed; i++) out[i] = in[i];
            in += changed;
            out += changed;
        } else {
            const uint32_t skip = (uint32_t)in[0] | ((uint32_t)in[1] << 16);
            if (!skip) break;
            in += 2;
            out += skip;
        }
    }
}

GBRewind* gb_rewind_new(size_t state_size, size_t buffer_size) {
    GBRewind* rewind = (GBRewind*)calloc(1, sizeof(*rewind));
    if (!rewind) return NULL;
    rewind->blocksize = (state_size + 1) & ~(size_t)1;
    rewind->maxcompsize = max_patch_size(state_size) + 2 * sizeof(size_t);
    rewind->capacity = buffer_size;
    /* Twice a record, so a record written at the start of an empty ring never
     * has to wrap. */
    if (state_size == 0 || buffer_size < 2 * rewind->maxcompsize) {
        free(rewind);
        return NULL;
    }
    rewind->data = (uint8_t*)malloc(buffer_size);
    rewind->thisblock = block_alloc(state_size, 0);
    rewind->nextblock = block_alloc(state_size, 1);
    if (!rewind->data || !rewind->thisblock || !rewind->nextblock) {
        gb_rewind_free(rewind);
        return NULL;
    }
    rewind->head = rewind->tail = rewind->data + sizeof(size_t);
    return rewind;
}

void gb_rewind_free(GBRewind* rewind) {
    if (!rewind) return;
    free(rewind->data);
    free(rewind->thisblock);
    free(rewind->nextblock);
    free(rewind);
}

bool gb_rewind_pop(GBRewind* rewind, const void** data) {
    *data = rewind->thisblock_state ? rewind->thisblock : NULL;
    if (rewind->thisblock_valid) {
        rewind->thisblock_valid = false;
        rewind->entries--;
        return true;
    }
    if (rewind->head == rewind->tail) return false;
    rewind->head = rewind->data + read_offset(rewind->head - sizeof(size_t));
    decompress(rewind->head + sizeof(size_t), rewind->thisblock);
    rewind->entries--;
    return true;
}

void* gb_rewind_push_where(GBRewind* rewind) {
    /* The next patch is taken against thisblock, so it must hold a state in
     * the buffer. After a pop it holds the state taken, which the newest patch
     * still leads down from: put it back. RetroArch pops once more here
     * instead, dropping the state rewound to, so rewinding again after running
     * on from it would skip a frame. */
    if (!rewind->thisblock_valid && rewind->thisblock_state) {
        rewind->thisblock_valid = true;
        rewind->entries++;
    }
    return rewind->nextblock;
}

void gb_rewind_push_do(GBRewind* rewind) {
    if (rewind->thisblock_valid) {
        /* Drop the oldest records until a worst-case one fits before the tail. */
        for (;;) {
            const size_t headpos = (size_t)(rewind->head - rewind->data);
            const size_t tailpos = (size_t)(rewind->tail - rewind->data);
            const size_t remaining = (tailpos + rewind->capacity - sizeof(size_t) - headpos - 1) %
                                     rewind->capacity + 1;
            if (remaining > rewind->maxcompsize) break;
            rewind->tail = rewind->data + read_offset(rewind->tail);
            rewind->entries--;
        }

        uint8_t* end = rewind->head + sizeof(size_t);
        end += compress(rewind->thisblock, rewind->nextblock, rewind->blocksize, end);
        if ((size_t)(end - rewind->data) + rewind->maxcompsize > rewind->capacity) {
            /* The next record would not fit before the end: close this one at
             * the start. The next head is then the start of the first record,
             * so a tail there moves on, or the ring would read as empty. */
            end = rewind->data;
            if (rewind->tail == rewind->data + sizeof(size_t)) {
                rewind->tail = rewind->data + read_offset(rewind->tail);
                rewind->entries--;
            }
        }
        write_offset(end, (size_t)(rewind->head - rewind->data));
        end += sizeof(size_t);
        write_offset(rewind->head, (size_t)(end - rewind->data));
        rewind->head = end;
    } else {
        rewind->thisblock_valid = true;
        rewind->thisblock_state = true;
    }

    uint8_t* swap = rewind->thisblock;
    rewind->thisblock = rewind->nextblock;
    rewind->nextblock = swap;
    rewind->entries++;
}

bool gb_rewind_has_state(const GBRewind* rewind) {
    return rewind->thisblock_state;
}

unsigned gb_rewind_entries(const GBRewind* rewind) {
    return rewind->entries;
}

size_t gb_rewind_used(const GBRewind* rewind) {
    return (size_t)(rewind->head - rewind->tail + (ptrdiff_t)rewind->capacity) % rewind->capacity;
}

size_t gb_rewind_capacity(const GBRewind* rewind) {
    return rewind->capacity;
}
