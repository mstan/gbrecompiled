#ifndef GB_SNAPSHOT_H
#define GB_SNAPSHOT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct GBContext;

/* A game enhancement that changes future guest behavior must snapshot its
 * own state too. Register once at startup. Buffers must contain no pointers;
 * validation is read-only and restore must not allocate or fail. */
typedef struct {
    const char* id;
    uint32_t version;
    size_t (*size)(struct GBContext*);
    bool (*save)(struct GBContext*, void*, size_t);
    bool (*validate)(struct GBContext*, const void*, size_t);
    void (*restore)(struct GBContext*, const void*, size_t);
} GBSnapshotExtension;
bool gb_snapshot_set_extension(const GBSnapshotExtension* extension);

/* One compressed, versioned codec for files and rb-engine. Caller frees data.
 * Load validates the entire blob before committing; host pointers, transports
 * and process-lifetime coverage counters are never restored. */
bool gb_snapshot_serialize(struct GBContext* ctx, uint8_t** data, size_t* size);
bool gb_snapshot_deserialize(struct GBContext* ctx, const uint8_t* data, size_t size);

/* History is owned by the context. Capture/load only between complete run
 * slices, after generated guest functions have returned to the host loop. */
bool gb_rewind_enable(struct GBContext* ctx, uint32_t depth, size_t byte_limit);
void gb_rewind_clear(struct GBContext* ctx);
void gb_rewind_destroy(struct GBContext* ctx);
bool gb_rewind_capture(struct GBContext* ctx);
bool gb_rewind_step(struct GBContext* ctx, uint32_t snapshots);
typedef struct {
    uint32_t count, depth;
    size_t bytes, byte_limit;
    uint64_t captures, restores, failures;
} GBRewindInfo;
GBRewindInfo gb_rewind_info(const struct GBContext* ctx);
#ifdef __cplusplus
}
#endif
#endif
