/**
 * @file gb_asset_loader.h
 * @brief Asset-bundle loader for recompiled GB binaries.
 *
 * Recompiled binaries don't ship the cart ROM bytes baked in. Instead,
 * the user supplies a clean ROM next to the executable on first run;
 * the loader SHA-1-verifies it against an expected hash, fans the bytes
 * out into per-section files under <cwd>/assets/<game_id>/<path>.bin
 * (defined by a per-game manifest), and copies the same bytes into the
 * target's BSS rom_data buffer. On subsequent runs the .gb is no
 * longer required — the loader just reads each .bin back into rom_data.
 *
 * Designed for the multi-ROM launcher case: each game has its own
 * rom_data buffer + manifest, and a GBGameAssets struct bundles them.
 */
#ifndef GB_ASSET_LOADER_H
#define GB_ASSET_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GB_ASSET_ENTRY_DEFINED
#define GB_ASSET_ENTRY_DEFINED
typedef struct {
    uint32_t rom_offset;
    uint32_t size;
    const char* path;
} GBAssetEntry;
#endif

typedef struct {
    const char* game_id;          /* directory name under assets/, also save_id */
    const char* rom_filename;     /* user-supplied .gb (e.g. "pokered.gb") */
    uint8_t* rom_data;            /* BSS buffer to populate (size == rom_size) */
    uint32_t rom_size;
    const uint8_t expected_sha1[20];
    const GBAssetEntry* manifest;
    uint32_t manifest_count;
} GBGameAssets;

/* chdir to the executable's directory. Idempotent across calls.
 * Linux-only today (uses /proc/self/exe); other platforms return false. */
bool gb_chdir_to_exe_dir(void);

/* Populate game->rom_data either from assets/<game_id>/ (if complete) or
 * by extracting from <rom_filename>. Returns false on any failure
 * (missing ROM, SHA-1 mismatch, missing assets, I/O error). */
bool gb_load_assets(const GBGameAssets* game);

#ifdef __cplusplus
}
#endif

#endif
