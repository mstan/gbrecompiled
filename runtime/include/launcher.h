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
 * Apply a BPS patch to an in-memory ROM image. Nothing is written to disk and
 * the caller's buffer is left untouched.
 *
 * `patch_filename` is looked up next to the executable (launcher_init() must
 * have run). On success returns 1 and hands back a malloc'd patched image in
 * *out / *out_len (caller frees). When `expect_sha256` is non-NULL/non-empty
 * the patched image must hash to it, otherwise the call fails.
 *
 * This is the multi-body path (gb_body.h): a body recompiled from a romhack
 * derives its image from the user's stock ROM at boot, in RAM, so the user
 * only ever supplies — and the CRC gate only ever accepts — the stock cart.
 * Returns 0 on any failure, with a human-readable reason in
 * launcher_last_error().
 */
int launcher_apply_patch_in_memory(const char *patch_filename,
                                   const unsigned char *rom, unsigned int rom_len,
                                   unsigned char **out, unsigned int *out_len,
                                   const char *expect_sha256);

/**
 * Human-readable reason for the most recent launcher failure ("" if none).
 * The string is owned by the launcher and valid until the next failure.
 */
const char *launcher_last_error(void);

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
