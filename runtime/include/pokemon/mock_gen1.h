/**
 * @file mock_gen1.h
 * @brief Runtime injectors for Gen 1 carts (Red / Blue / Yellow).
 *
 * Mirror of `mock_gen2.h` for the original mainline trilogy. Same
 * public-API shape (`gb_mock_gen1_detect`, `_party_count`,
 * `_species_name`, `_inject_builder`) so the Esc-menu UI can branch
 * on whichever generation the active cart belongs to.
 *
 * Gen 1 differs from Gen 2:
 *   - 151 species (vs 251)
 *   - 44-byte party_struct (vs 48) — no held item, no happiness,
 *     no Pokerus, single Special stat instead of SAtk/SDef split,
 *     Type1/Type2 cached inside the struct
 *   - No shiny mechanic in-cart, but the DV layout is identical
 *     so Gen-2-compatible shiny DVs survive a Time Capsule trade
 *   - BaseStats entry is 28 bytes (vs 32 in Gen 2)
 */

#ifndef MOCK_GEN1_H
#define MOCK_GEN1_H

#include <stdbool.h>
#include <stdint.h>
#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GB_MOCK_GEN1_NONE = 0,
    GB_MOCK_GEN1_RED,
    GB_MOCK_GEN1_BLUE,
    GB_MOCK_GEN1_YELLOW,
} GBGen1Game;

/* Which Gen 1 cart is loaded, or NONE for everything else. */
GBGen1Game gb_mock_gen1_detect(const GBContext* ctx);

/* Number of valid species (151 for Gen 1). */
#define GB_MOCK_GEN1_SPECIES_COUNT 151

/* Live wPartyCount in WRAM. 0xFF if not Gen 1. */
uint8_t gb_mock_gen1_party_count(const GBContext* ctx);

/* Decode species name (1..151) from cart's MonsterNames table to
 * ASCII NUL-terminated. out must be at least 11 bytes. */
bool gb_mock_gen1_species_name(const GBContext* ctx, int species,
                               char* out, size_t out_size);

/* Build + inject a Pokemon into the next party slot.
 *   species : 1..151
 *   level   : 2..100
 *   shiny   : sets DVs to the Gen-2-compatible shiny pattern
 *             (Atk=Def=Spd=Spc=10). Gen 1 doesn't display anything
 *             differently for these DVs, but the Time Capsule trade
 *             will make the mon shiny when imported into Gen 2.
 * Returns false on bad inputs or full party. */
bool gb_mock_gen1_inject_builder(GBContext* ctx,
                                 int species, int level, bool shiny);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_GEN1_H */
