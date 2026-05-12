/**
 * @file mock_crystal.h
 * @brief Crystal-only runtime event injectors.
 *
 * Pokemon Crystal had several special events that were originally
 * distributed through the Japan-only Mobile Adapter GB (a peripheral
 * the cart talks to via the link port, separate from the IR receiver
 * the Mystery Gift mock in mock_ir.c handles). The US/EU localizations
 * shipped with most of that code intact, just gated behind flags the
 * adapter never gets a chance to set on non-Japanese hardware.
 *
 * This module offers the same end result without emulating the
 * adapter: a one-call write of the cart-side flag that's all the
 * normal in-game scripts ever cared about.
 *
 * Currently exposes the GS Ball / Celebi event. Other future entries
 * (Egg Ticket, etc.) would go here.
 */

#ifndef MOCK_CRYSTAL_H
#define MOCK_CRYSTAL_H

#include <stdbool.h>
#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True iff the active cart is Pokemon Crystal (US 1.0 / 1.1).
 * Matched by cart title bytes — same approach as gb_mock_ir_detect. */
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

#ifdef __cplusplus
}
#endif

#endif
