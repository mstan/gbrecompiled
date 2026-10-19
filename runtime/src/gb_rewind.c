#include "gb_snapshot.h"
#include "gbrt.h"
#include "retcomm_rbengine/snap_ring.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

typedef struct {
    RbeSnapRing* ring;
    uint32_t next_tick;
    GBRewindInfo info;
} GBRewind;

bool gb_rewind_enable(GBContext* ctx, uint32_t depth, size_t byte_limit) {
    if (!ctx || !depth || depth > 36000 || !byte_limit) return false;
    GBRewind* state = (GBRewind*)calloc(1, sizeof(*state));
    if (!state) return false;
    state->ring = rbe_snap_ring_create(depth);
    if (!state->ring) { free(state); return false; }
    state->info.depth = depth;
    state->info.byte_limit = byte_limit;
    gb_rewind_destroy(ctx);
    ctx->rewind = state;
    return true;
}

void gb_rewind_clear(GBContext* ctx) {
    GBRewind* state = ctx ? (GBRewind*)ctx->rewind : NULL;
    if (!state) return;
    rbe_snap_ring_clear(state->ring);
    state->next_tick = 0;
    state->info.count = 0;
    state->info.bytes = 0;
}

void gb_rewind_destroy(GBContext* ctx) {
    GBRewind* state = ctx ? (GBRewind*)ctx->rewind : NULL;
    if (!state) return;
    rbe_snap_ring_destroy(state->ring);
    free(state);
    ctx->rewind = NULL;
}

static void drop_oldest(GBRewind* state) {
    uint32_t tick = rbe_snap_ring_oldest_tick(state->ring);
    size_t size = 0;
    rbe_snap_ring_peek(state->ring, tick, &size);
    rbe_snap_ring_drop_tick(state->ring, tick);
    state->info.bytes -= size;
}

bool gb_rewind_capture(GBContext* ctx) {
    GBRewind* state = ctx ? (GBRewind*)ctx->rewind : NULL;
    if (!state) return false;
    uint8_t* data = NULL;
    size_t size = 0;
    if (!gb_snapshot_serialize(ctx, &data, &size) || size > state->info.byte_limit) {
        free(data);
        state->info.failures++;
        return false;
    }
    /* RBE orders uint32 ticks numerically. Reset before wrap instead of
     * misordering the oldest/newest records after a very long session. */
    if (state->next_tick == UINT32_MAX) gb_rewind_clear(ctx);
    while (rbe_snap_ring_count(state->ring) &&
           (rbe_snap_ring_count(state->ring) == state->info.depth ||
            size > state->info.byte_limit - state->info.bytes)) drop_oldest(state);
    if (!rbe_snap_ring_store(state->ring, state->next_tick++, data, size)) {
        free(data);
        state->info.failures++;
        return false;
    }
    state->info.bytes += size;
    state->info.count = rbe_snap_ring_count(state->ring);
    state->info.captures++;
    return true;
}

bool gb_rewind_step(GBContext* ctx, uint32_t snapshots) {
    GBRewind* state = ctx ? (GBRewind*)ctx->rewind : NULL;
    if (!state || !snapshots || snapshots >= rbe_snap_ring_count(state->ring)) return false;
    const uint32_t newest = rbe_snap_ring_newest_tick(state->ring);
    const uint32_t target = newest - snapshots;
    size_t size = 0;
    const uint8_t* data = rbe_snap_ring_peek(state->ring, target, &size);
    if (!data || !gb_snapshot_deserialize(ctx, data, size)) {
        state->info.failures++;
        return false;
    }
    /* Only discard the abandoned future after a successful restore. IDs in
     * a history branch are contiguous; restart at target+1 when play resumes. */
    for (uint32_t tick = target + 1; tick <= newest; ++tick) {
        size_t bytes = 0;
        rbe_snap_ring_peek(state->ring, tick, &bytes);
        state->info.bytes -= bytes;
    }
    rbe_snap_ring_drop_after(state->ring, target);
    state->next_tick = target + 1;
    state->info.count = rbe_snap_ring_count(state->ring);
    state->info.restores++;
    return true;
}

GBRewindInfo gb_rewind_info(const GBContext* ctx) {
    GBRewindInfo empty = {0};
    return ctx && ctx->rewind ? ((const GBRewind*)ctx->rewind)->info : empty;
}
