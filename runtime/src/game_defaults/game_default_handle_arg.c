/* Auto-split default for one game hook. One strong symbol per translation
 * unit: a game's extras.c that overrides only SOME hooks links cleanly —
 * the archive pulls only the per-hook objects the game does not provide,
 * so there is never a multiple-definition clash. mingw-safe (no reliance on
 * weak symbols in static archives). See game_extras.h. */
#include "game_extras.h"
#include "gbrt.h"

int game_handle_arg(const char *arg, const char *next_arg) { (void)arg; (void)next_arg; return 0; }
