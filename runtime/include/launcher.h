#pragma once

/**
 * launcher.h — ROM discovery, identity verification, and external-ROM loading.
 *
 * Identity verification has two layers. CRC validation is driven by
 * game_extras hooks (game_get_expected_crc32 / game_get_valid_crcs),
 * mirroring the NES recomp pattern; game projects provide an extras.c
 * implementing those, and they take precedence (multi-revision carts).
 * When no CRC hooks are declared, the launcher falls back to an exact
 * SHA-256 match against the digest the recompiler embedded at gen time
 * (see launcher_set_expected_sha256). Both layers absent → no validation.
 */

/**
 * Initialize launcher — find rom.cfg location next to the executable.
 * Call before launcher_get_rom_path().
 */
void launcher_init(void);

/**
 * Register the SHA-256 digest (lowercase 64-char hex) of the exact ROM this
 * binary was recompiled from. When set, launcher_get_rom_path() enforces an
 * exact-match identity check on the user-supplied ROM — UNLESS the game also
 * declares CRC hooks (game_get_valid_crcs / game_get_expected_crc32), which
 * take precedence to allow multi-revision carts. Pass NULL/"" to disable.
 * Call after launcher_init() and before launcher_get_rom_path().
 */
void launcher_set_expected_sha256(const char *hex);

/**
 * Register a BPS patch filename (looked up next to the executable) that
 * transforms a STOCK ROM into the exact ROM this binary expects. When the user
 * picks a ROM that doesn't match the expected SHA-256, the launcher tries to
 * apply this patch; if the result matches, it writes "<romstem>.extended.gbc"
 * next to the executable and uses that — so users supply only a stock ROM and
 * the enhanced ROM is produced automatically. No-op if the patch file is
 * absent. Call after launcher_init() and before launcher_get_rom_path().
 */
void launcher_set_patch_file(const char *filename);

/**
 * Get ROM file path. Checks in order:
 *   1. Cached path from rom.cfg (if file still exists and CRC matches)
 *   2. Windows file picker dialog (re-prompts on CRC mismatch)
 * Caches the result to rom.cfg for next run.
 * Returns NULL if no ROM was selected (user cancelled).
 *
 * CRC list is read from game_get_valid_crcs() (preferred) or
 * game_get_expected_crc32() (fallback). Both returning 0 / NULL skips
 * CRC validation.
 */
const char *launcher_get_rom_path(void);

/**
 * Load a ROM file into a malloc'd buffer.
 * Caller must free the returned buffer.
 * Sets *out_size to the file size.
 * Returns NULL on failure.
 */
unsigned char *launcher_load_rom(const char *path, unsigned int *out_size);
