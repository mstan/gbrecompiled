/*
 * game_extras_crc_default.c -- Default (no-op) ROM identity hooks.
 *
 * Linked into gbrt only when the game project does NOT provide an
 * extras.c. When extras.c IS present, the project's strong definitions
 * win and this file is excluded by the generated CMakeLists.txt.
 *
 * Split from game_extras_default.c so games only need to override the
 * CRC hooks, not the lifecycle/debug ones.
 */
#include <stdint.h>
#include "game_extras.h"

uint32_t game_get_expected_crc32(void) { return 0; }
const uint32_t *game_get_valid_crcs(int *out_count) {
    if (out_count) *out_count = 0;
    return 0;
}
