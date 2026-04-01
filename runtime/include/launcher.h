#pragma once

/**
 * Initialize launcher — find rom.cfg location next to the executable.
 * Call before launcher_get_rom_path().
 */
void launcher_init(void);

/**
 * Get ROM file path. Checks in order:
 *   1. Cached path from rom.cfg (if file still exists)
 *   2. Windows file picker dialog
 * Caches the result to rom.cfg for next run.
 * Returns NULL if no ROM was selected (user cancelled).
 */
const char *launcher_get_rom_path(void);

/**
 * Set expected CRC32 for ROM verification.
 * If non-zero, launcher_load_rom will verify the CRC and prompt
 * the user to pick again on mismatch. Set to 0 to skip verification.
 */
void launcher_set_expected_crc32(unsigned int crc32);

/**
 * Load a ROM file into a malloc'd buffer.
 * Caller must free the returned buffer.
 * Sets *out_size to the file size.
 * Returns NULL on failure.
 */
unsigned char *launcher_load_rom(const char *path, unsigned int *out_size);
