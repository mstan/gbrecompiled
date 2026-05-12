/**
 * @file mock_gen2.h
 * @brief Runtime injectors for Gen 2 mainline carts (Gold/Silver/Crystal).
 *
 * Two flavors of functionality live here:
 *
 *  1. Generic builder + cart detection (`gb_mock_gen2_*`) — works
 *     on any of the three Gen 2 carts. Per-cart WRAM and ROM
 *     offsets are kept in a dispatch table inside the .c file so
 *     the public API doesn't need to know which cart is loaded.
 *
 *  2. Crystal-only Mobile-Adapter event injectors
 *     (`gb_mock_crystal_*`) — the GS Ball / Celebi chain and the
 *     Odd Egg distribution. These features have payload code in
 *     Crystal but not Gold/Silver, and they hit Crystal-specific
 *     SRAM/ROM addresses, so they live behind the Crystal-only
 *     `gb_mock_crystal_active()` gate.
 */

#ifndef MOCK_GEN2_H
#define MOCK_GEN2_H

#include <stdbool.h>
#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which Gen 2 mainline cart is loaded. Detected by cart-title byte
 * matching at 0x134-0x143 — same approach as gb_mock_ir_detect. */
typedef enum {
    GB_MOCK_GEN2_NONE = 0,
    GB_MOCK_GEN2_GOLD,
    GB_MOCK_GEN2_SILVER,
    GB_MOCK_GEN2_CRYSTAL,
} GBGen2Game;

GBGen2Game gb_mock_gen2_detect(const GBContext* ctx);

/* True iff the active cart is Pokemon Crystal (US 1.0 / 1.1).
 * Thin wrapper for the few features that are Crystal-only (GS Ball
 * event, Odd Egg distribution). The generic builder, party_count,
 * and species_name helpers below work on Gold/Silver/Crystal. */
bool gb_mock_crystal_active(const GBContext* ctx);

/* True iff the Pokedex shows Celebi (#251) as caught in the live
 * WRAM bank 1 state. Used to flip menu copy between "Trigger" and
 * "Re-arm" framing. Returns false if the cart isn't Crystal. */
bool gb_mock_crystal_celebi_caught(const GBContext* ctx);

/* Set sGSBallFlag + sGSBallFlagBackup in SRAM bank 1 to GS_BALL_AVAILABLE
 * (0x0B), the value the Goldenrod Pokemon Center NPC checks for.
 * Same effect as sceptios/pokecrystal's VC-style hook, but triggered
 * by the user via menu rather than by entering the Hall of Fame.
 * The runtime's battery-save persists the write on next save.
 * No-op + returns false if the cart isn't Crystal. */
bool gb_mock_crystal_apply_gs_ball(GBContext* ctx);

/* Read the raw sGSBallFlag byte from SRAM bank 1. 0xFF if the cart
 * isn't Crystal or SRAM isn't big enough. */
uint8_t gb_mock_crystal_gs_ball_flag(const GBContext* ctx);

/* Human-readable label for the current GS Ball flag state, suitable
 * for menu display. Returns a static string. */
const char* gb_mock_crystal_gs_ball_state_label(const GBContext* ctx);

/* Live wPartyCount from WRAM bank 1. 0xFF if cart isn't Crystal. */
uint8_t gb_mock_crystal_party_count(const GBContext* ctx);

/* Add a randomly-rolled Odd Egg to the player's party. Returns false
 * if the cart isn't Crystal or the party is already at 6 Pokemon.
 * Picks one of 7 baby species (Pichu/Cleffa/Igglybuff/Smoochum/Magby/
 * Tyrogue/Elekid) using the cart's own probability table (in vanilla
 * US Crystal ROM at bank 0x7E $7552). Uses 14% shiny rate per the
 * original Mobile event's design. */
bool gb_mock_crystal_apply_odd_egg(GBContext* ctx);

/* Number of valid species (251 for Gen 2). */
#define GB_MOCK_GEN2_SPECIES_COUNT 251

/* Live wPartyCount for the active Gen 2 cart. 0xFF if not Gen 2. */
uint8_t gb_mock_gen2_party_count(const GBContext* ctx);

/* Decode the Gen 2-charmapped name of a species (1..251) from the
 * active cart's PokemonNames table into out (ASCII, NUL-terminated).
 * Out must be at least 11 bytes. Returns false on out-of-range or
 * non-Gen-2 cart. */
bool gb_mock_gen2_species_name(const GBContext* ctx, int species,
                               char* out, size_t out_size);

/* Build and inject a Pokemon into the next party slot.
 *   species : 1..251 (1-based dex number)
 *   level   : 2..100
 *   shiny   : true → canonical Gen 2 shiny DVs (Atk=Def=Spd=Spc=10)
 * Other fields are derived: base stats read from the cart ROM,
 * stats computed via the Gen 2 formula, moves picked as the
 * last-4-learned at the chosen level from the cart's evos/attacks
 * table, OT = player's name+ID, no held item, default happiness.
 * Returns false if the cart isn't Gen 2, the party is full, or
 * the species/level is out of range. */
bool gb_mock_gen2_inject_builder(GBContext* ctx,
                                 int species, int level, bool shiny);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_GEN2_H */
