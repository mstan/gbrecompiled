/* Auto-split default for one game hook. One strong symbol per translation
 * unit: a game's extras.c that overrides only SOME hooks links cleanly.
 * See game_extras.h and game_default_get_name.c. */
#include "game_extras.h"
#include "gbrt.h"

/* Default to Game Boy Color branding — the ecosystem's default, correct for
 * every color-capable and colorizable cart. DMG-only titles override this to
 * "gb" in their own extras.c. */
const char *game_get_platform(void) { return "gbc"; }
