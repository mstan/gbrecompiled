/**
 * @file mock_evolve_patch.h
 * @brief Trade-evolution -> level-evolution ROM patch toggle.
 *
 * When the toggle is on, the runtime overwrites each trade-only
 * evolution row in the cart's evos table with an EVOLVE_LEVEL row
 * at a sensible threshold. The opcodes are the same length, so the
 * write is byte-equal in place -- no pointer-table shifting needed.
 *
 * Once patched, the cart's own evolution flow runs normally on the
 * next level-up event (battle XP, Rare Candy): stats are recomputed
 * with stat-exp, the learnset is re-checked, the Pokedex bits get
 * set, the animation plays. We just nudge the data; the cart does
 * the work.
 *
 * Toggling off restores the original bytes from a per-cart backup.
 * Mons already past the new threshold do NOT auto-evolve - they
 * evolve on the next level-up like any cart-native level-evolution.
 */

#ifndef MOCK_EVOLVE_PATCH_H
#define MOCK_EVOLVE_PATCH_H

#include <stdbool.h>
#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int from_dex;
    int to_dex;
    int level;
    const char* label;
} GBEvolvePatchEntry;

/* True iff the active cart is a Gen 1 or Gen 2 Pokemon cart and the
 * patch can be applied. */
bool gb_evolve_patch_is_supported(const GBContext* ctx);

/* Current toggle state. */
bool gb_evolve_patch_is_enabled(void);

/* Flip the toggle. On->off restores the original ROM bytes. */
void gb_evolve_patch_set_enabled(GBContext* ctx, bool enable);

/* Returns the per-gen entry table for UI display. Count is the
 * number of entries; sets *out_entries to point at a static array. */
int gb_evolve_patch_list(const GBContext* ctx,
                         const GBEvolvePatchEntry** out_entries);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_EVOLVE_PATCH_H */
