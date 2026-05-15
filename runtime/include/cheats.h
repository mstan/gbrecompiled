/**
 * @file cheats.h
 * @brief Libretro-style .cht cheat loader + applier.
 *
 * Reads `cheats/<game_id>/*.cht` files on cart launch and exposes
 * the parsed entries to the UI. Two code types are supported:
 *
 *   - GameShark "01VVLLHH"  -> write byte $VV to RAM $HHLL each frame.
 *     Applied by gb_cheats_tick() from the runtime's per-frame poll.
 *
 *   - Game Genie "XXX-XXX-XXX" / "XXX-XXX"  -> ROM patch with
 *     optional compare-byte verifier. Applied at toggle-on, the
 *     original byte saved to a per-op backup for toggle-off
 *     restore. Bank-0 addresses ($0000-$3FFF) are fully supported;
 *     banked addresses ($4000-$7FFF) are best-effort -- v1 patches
 *     the bank-0 mirror only and logs a note for the user.
 *
 * Cheats are cart-agnostic: anything in the libretro database can
 * drop in. The module makes no assumptions about Pokemon-specific
 * data.
 */

#ifndef GB_CHEATS_H
#define GB_CHEATS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GB_CHEAT_DESC_MAX     128
#define GB_CHEAT_OPS_PER_CHEAT 32
#define GB_CHEAT_MAX_ENTRIES   2048

typedef enum {
    GB_CHEAT_OP_NONE        = 0,
    GB_CHEAT_OP_GAMESHARK   = 1,  /* RAM write each frame */
    GB_CHEAT_OP_GAMEGENIE   = 2,  /* ROM patch on enable, restore on disable */
} GBCheatOpType;

typedef struct {
    GBCheatOpType type;
    uint8_t  value;
    uint16_t address;
    int      compare;       /* -1 if none (6-char Genie code) */
} GBCheatOp;

typedef struct {
    char       description[GB_CHEAT_DESC_MAX];
    int        op_count;
    GBCheatOp  ops[GB_CHEAT_OPS_PER_CHEAT];
    bool       enabled;
    /* For toggle-off restore of GameGenie ROM patches. The patched
     * ROM offset is resolved at toggle-on (it may live in any bank
     * if the address is $4000-$7FFF and a compare byte matched);
     * we remember it here so unapply restores the right byte. */
    uint8_t    saved_byte[GB_CHEAT_OPS_PER_CHEAT];
    bool       saved_valid[GB_CHEAT_OPS_PER_CHEAT];
    size_t     applied_offset[GB_CHEAT_OPS_PER_CHEAT];
} GBCheat;

/* Scan `cheats/<game_id>/*.cht` and parse every cheat into the
 * static cheat list. Replaces any previously-loaded cheats. Returns
 * the number of cheats loaded (>=0) or -1 on directory-not-found. */
int gb_cheats_load(const char* game_id);

/* Per-cart count + accessor for the parsed cheats. */
int            gb_cheats_count(void);
const GBCheat* gb_cheats_get(int idx);

/* Flip the enabled state. For GameGenie cheats this immediately
 * applies / reverts the ROM patch; for GameShark cheats it just
 * flips the flag and the per-frame tick takes care of it. */
void gb_cheats_set_enabled(GBContext* ctx, int idx, bool on);

/* Edit the value byte of an individual op inside a cheat. Address
 * stays fixed -- only the value changes, which is how every
 * "Modifier" cheat in the libretro database is meant to be used
 * (the .cht file ships a placeholder value the user is expected to
 * customize). For an enabled GameGenie cheat the new value is
 * written to ROM immediately; for GameShark the per-frame tick
 * picks it up automatically. */
void gb_cheats_set_op_value(GBContext* ctx, int cheat_idx,
                            int op_idx, uint8_t value);

/* Bulk disable -- restores any GameGenie patches and clears the
 * enabled flag on everything. Useful for the "Disable All" button
 * and for cart shutdown. */
void gb_cheats_disable_all(GBContext* ctx);

/* Per-frame poll. Iterates enabled GameShark codes and writes each
 * value to its RAM address. No-op if nothing's enabled. */
void gb_cheats_tick(GBContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* GB_CHEATS_H */
