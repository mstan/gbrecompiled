/**
 * @file rewind.h
 * @brief Rewind buffer: RetroArch's state manager (state_manager.c), ported
 *
 * A fixed-size ring of machine states. The newest state is kept whole; each
 * older one is stored as a patch that turns the state after it back into it,
 * holding only the 16-bit words that differ. When the ring fills, the oldest
 * patches are dropped. States go in with push_where/push_do and come back
 * newest first with pop.
 */

#ifndef GB_REWIND_H
#define GB_REWIND_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBRewind GBRewind;

/* A buffer for states of state_size bytes in buffer_size bytes of patches.
 * NULL when out of memory or when buffer_size cannot hold one patch. */
GBRewind* gb_rewind_new(size_t state_size, size_t buffer_size);
void gb_rewind_free(GBRewind* rewind);

/* Where the next state is to be written (state_size bytes). Bytes the
 * caller leaves unwritten keep their contents from an earlier state, so a
 * part never written stays out of the patches. */
void* gb_rewind_push_where(GBRewind* rewind);
/* Adds the state written at push_where. */
void gb_rewind_push_do(GBRewind* rewind);

/* The newest state, which is taken out; the next push first puts back the
 * last one taken, the state the machine was rewound to. At the oldest,
 * returns false and points *data at the oldest state again (NULL if no state
 * was ever pushed). *data stays valid until the next push or pop. */
bool gb_rewind_pop(GBRewind* rewind, const void** data);

/* Whether pop has a state to give: none before the first push. */
bool gb_rewind_has_state(const GBRewind* rewind);
unsigned gb_rewind_entries(const GBRewind* rewind);
/* Bytes of patches held, and the most there is room for. */
size_t gb_rewind_used(const GBRewind* rewind);
size_t gb_rewind_capacity(const GBRewind* rewind);

#ifdef __cplusplus
}
#endif

#endif /* GB_REWIND_H */
