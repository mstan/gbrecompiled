#pragma once

/**
 * launcher.h — ROM discovery, CRC32 verification, and external-ROM loading.
 *
 * CRC validation is driven by game_extras hooks (game_get_expected_crc32 /
 * game_get_valid_crcs), mirroring the NES recomp pattern. Game projects
 * provide an extras.c implementing those; the runtime library has weak
 * defaults that return 0 (no validation).
 */

/**
 * Initialize launcher — find rom.cfg location next to the executable.
 * Call before launcher_get_rom_path().
 */
void launcher_init(void);

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
