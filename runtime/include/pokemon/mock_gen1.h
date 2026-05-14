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

/* Linear scan of MonsterNames for an ASCII species name match.
 * Case-insensitive. Returns 1-based dex# on hit, -1 on miss or
 * non-Gen-1 cart. */
int gb_mock_gen1_dex_for_name(const GBContext* ctx, const char* name);

/* Same as species_name but takes an internal hex ID (1..190) — what
 * pk1 files and the cart's MON_SPECIES field actually store.
 * MonsterNames is internal-indexed natively, so this is the more
 * direct lookup of the two. */
bool gb_mock_gen1_species_name_by_internal(const GBContext* ctx,
                                           int internal_id,
                                           char* out, size_t out_size);

/* Linear scan of MoveNames for an ASCII move name match.
 * Case-insensitive. Returns 1-based move ID (1..165) on hit, -1 on
 * miss or non-Gen-1 cart. */
int gb_mock_gen1_move_id_for_name(const GBContext* ctx, const char* name);

/* Reverse: look up move name by 1-based ID. out must be at least 16
 * bytes. Returns false on out-of-range or non-Gen-1 cart. */
bool gb_mock_gen1_move_name(const GBContext* ctx, int move_id,
                            char* out, size_t out_size);

/* Convert a dex# (1..151) to the cart's internal MON_SPECIES byte,
 * which is what party_struct[0] and wPartySpecies[] actually store.
 * Returns -1 on out-of-range or non-Gen-1 cart. */
int gb_mock_gen1_internal_id_for_dex(const GBContext* ctx, int dex);

/* Look up the ASCII item name by 1-based item ID. out >= 14 bytes.
 * Returns false on out-of-range, empty slot, or non-Gen-1 cart. */
bool gb_mock_gen1_item_name(const GBContext* ctx, int item_id,
                            char* out, size_t out_size);

/* Number of item slots the cart's ItemNames table has. Hardcoded to
 * 95 for Gen 1 -- the table is 96 entries with the last being the
 * "CANCEL" sentinel. Items 1..95 are catchable names. */
#define GB_MOCK_GEN1_ITEM_COUNT 95

/* Add `qty` of `item_id` to the cart's single bag pocket. If the
 * item is already present, quantity stacks (capped at 99). If not,
 * a new slot is appended provided the bag isn't full (20 slots
 * max). Returns false on out-of-range item, full bag (with new
 * item), or non-Gen-1 cart. */
bool gb_mock_gen1_give_item(GBContext* ctx, int item_id, int qty);

/* Resolve the flat ROM byte offset of the species's evos+moves
 * record. EvosMovesPointerTable is internal-id-indexed, so the input
 * is the cart's MON_SPECIES byte (1..190). The returned offset points
 * at the first evolution opcode byte. Returns 0 on out-of-range or
 * lookup failure. */
size_t gb_mock_gen1_evos_record_offset(const GBContext* ctx, int internal_id);

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

/* Pointer to wPartyMonNicks[slot] in the live wram buffer, or NULL
 * if not Gen 1 or slot out of range. */
uint8_t* gb_mock_gen1_nick_slot(GBContext* ctx, int slot);
uint8_t* gb_mock_gen1_party_mons_slot   (GBContext* ctx, int slot);
uint8_t* gb_mock_gen1_party_ots_slot    (GBContext* ctx, int slot);
uint8_t* gb_mock_gen1_party_species_slot(GBContext* ctx, int slot);
uint8_t  gb_mock_gen1_party_count_inc(GBContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_GEN1_H */
