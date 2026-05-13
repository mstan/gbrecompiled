/**
 * @file mock_inject_file.h
 * @brief File-based Pokemon injectors.
 *
 * Drop a Pokemon file into `injects/<game_id>/` next to the launcher
 * binary; the Esc-menu Pokemon Builder section lists it as an
 * "Event" and one click injects the mon into the next free party
 * slot. Two formats are accepted:
 *
 *   1. `.pk1` / `.pk2` — the binary format PKHeX / EventsGallery
 *      use. Body layout matches the cart's party_struct exactly
 *      (44 bytes Gen 1, 48 bytes Gen 2), wrapped in a list header
 *      that gives OT name + nickname. Sizes:
 *
 *        69 bytes — PK1 international (11-byte OT/nick)
 *        59 bytes — PK1 Japanese     (6-byte OT/nick)
 *        73 bytes — PK2 international
 *        63 bytes — PK2 Japanese
 *
 *      Loaded as-is — stats / DVs / moves are whatever the file
 *      specifies, so e.g. a level-50 Mewtwo .pk2 lands as a level-50
 *      Mewtwo with the exact stats the file recorded.
 *
 *   2. `.pkm` — our own simple text format, generation-agnostic.
 *      The same file applies on either Gen 1 or Gen 2 carts; the
 *      runtime fills in any unspecified field from the cart's own
 *      tables, and silently ignores keys that don't apply to the
 *      active gen (e.g. `held_item` on a Gen 1 cart). Routes
 *      through the Gen 1 / Gen 2 builder for the stat math, then
 *      post-patches any explicit overrides from the file.
 *
 *      All recognized keys (every field optional except `species`
 *      and `level`):
 *
 *        species     = MEWTWO        # name or numeric dex#
 *        level       = 70            # 2..100
 *        shiny       = true          # canonical Gen-2 shiny DVs
 *        nickname    = TYRANT
 *        ot_name     = NINTNDO       # max 10 chars
 *        ot_id       = 12345         # 0..65535
 *        moves       = 94,105,86,87  # 1-4 numeric move IDs, comma-sep
 *        dvs         = 15,15,15,15   # atk,def,spd,spc each 0..15 (overrides shiny)
 *        held_item   = 1             # gen 2 only (silently ignored on gen 1)
 *        happiness   = 250           # gen 2 only, 0..255
 *        pokerus     = 0             # gen 2 only, raw byte
 *        catch_rate  = 45            # gen 1 only, 1..255
 *
 *      Comments start with `#`. Unknown keys are silently skipped
 *      so future runtimes can add fields without breaking old files.
 *
 * Both `.pkm` and `.pk1`/`.pk2` files live side-by-side in the same
 * injects/ folder; format is detected by file extension + length.
 */

#ifndef MOCK_INJECT_FILE_H
#define MOCK_INJECT_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One scanned inject file. The display label is what the menu
 * dropdown shows (e.g. "Mew lvl5 — EUROPE/01693"); full_path is
 * the actual file path on disk. */
typedef struct {
    char filename[64];
    char display[96];
    char full_path[512];
} GBInjectFileEntry;

#define GB_INJECT_FILE_MAX 64

/* Scan `injects/<game_id>/*.{pkm,pk1,pk2}` and fill up to `max`
 * entries into `out`. Pass the live GBContext so the scanner can
 * resolve species names from the cart's own ROM tables to build a
 * friendlier display label than the bare filename. Returns the
 * count actually filled, or 0 if the folder doesn't exist or no
 * matching files are present. Idempotent and cheap — call on every
 * menu open. */
int gb_inject_file_scan(const GBContext* ctx, const char* game_id,
                        GBInjectFileEntry* out, int max);

/* Build a human-readable, multi-line description of an entry's
 * contents — species, level, moves, OT, DVs, etc. Output is plain
 * ASCII suitable for ImGui::TextWrapped or stderr. Returns false if
 * the file can't be read; on failure `out` receives a one-line
 * error. Safe to call after a successful scan. */
bool gb_inject_file_describe(const GBContext* ctx,
                             const GBInjectFileEntry* entry,
                             char* out, size_t out_size);

/* Parse and apply the file at `entry->full_path`. Routes through
 * the active cart's inject_builder; only species/level/shiny
 * affect stat math, anything else is post-patched. Returns false
 * on parse error, party full, or unknown cart/species. The exact
 * failure reason is written to `out_err` (caller-provided ~128 byte
 * buffer) for the menu to display. */
bool gb_inject_file_apply(GBContext* ctx, const GBInjectFileEntry* entry,
                          char* out_err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_INJECT_FILE_H */
