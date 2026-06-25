/* Auto-split default for one game hook. One strong symbol per translation
 * unit: a game's extras.c that overrides only SOME hooks links cleanly —
 * the archive pulls only the per-hook objects the game does not provide,
 * so there is never a multiple-definition clash. mingw-safe (no reliance on
 * weak symbols in static archives). See game_extras.h. */
#include "game_extras.h"
#include "gbrt.h"

const uint32_t *game_get_valid_crcs(int *out_count) { if (out_count) *out_count = 0; return 0; }
