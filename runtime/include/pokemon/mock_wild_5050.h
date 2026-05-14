/**
 * @file mock_wild_5050.h
 * @brief Wild-encounter 50/50 toggle (cross-version exclusives).
 *
 * When the toggle is on, the runtime rolls a coin for every wild
 * encounter slot that differs between Red and Blue (extracted from
 * pret/pokered's wild map files). Heads -> write the Red species
 * byte into ROM; tails -> the Blue one. Slots that already match
 * across versions are untouched. The cart reads from the patched
 * bytes for the rest of the session, so each load gets a fresh
 * mix.
 *
 * Toggling off restores the original bytes from a per-session
 * backup. Phase 1 covers Red <-> Blue only; Yellow and Gen 2 land
 * later.
 */

#ifndef MOCK_WILD_5050_H
#define MOCK_WILD_5050_H

#include <stdbool.h>
#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

bool gb_wild_5050_is_supported(const GBContext* ctx);
bool gb_wild_5050_is_enabled(void);
void gb_wild_5050_set_enabled(GBContext* ctx, bool enable);

/* Number of diverging slots for the active cart (UI display). */
int gb_wild_5050_slot_count(const GBContext* ctx);

/* Per-frame poll. When the toggle is on, watches the cart's current
 * map ID byte(s); when the player walks into a new map, re-rolls
 * every diff slot so the next encounter in that map draws from a
 * fresh species mix. Safe to call every frame regardless of toggle
 * state -- no-op when disabled. */
void gb_wild_5050_tick(GBContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_WILD_5050_H */
