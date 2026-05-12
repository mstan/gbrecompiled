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
 *   2. `.gbmon` — our own simple text format. Routes through the
 *      Gen 1 / Gen 2 builder so stats and moves auto-fill from
 *      the cart's own ROM tables based on species/level/shiny.
 *      Useful for hand-written events or the planned HTML builder.
 *
 *      # Example: shiny lvl-70 Mewtwo
 *      species  = MEWTWO     # or numeric dex#
 *      level    = 70
 *      shiny    = true
 *      nickname = TYRANT     # optional, overrides default
 *
 * Both kinds live side-by-side in the same injects/ folder; format
 * is detected by file extension + length.
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

/* Scan `injects/<game_id>/*.{gbmon,pk1,pk2}` and fill up to `max`
 * entries into `out`. Pass the live GBContext so the scanner can
 * resolve species names from the cart's own ROM tables to build a
 * friendlier display label than the bare filename. Returns the
 * count actually filled, or 0 if the folder doesn't exist or no
 * matching files are present. Idempotent and cheap — call on every
 * menu open. */
int gb_inject_file_scan(const GBContext* ctx, const char* game_id,
                        GBInjectFileEntry* out, int max);

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
