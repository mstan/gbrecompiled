/* Auto-split default for one game hook. One strong symbol per translation
 * unit: a game's extras.c that overrides only SOME hooks links cleanly —
 * the archive pulls only the per-hook objects the game does not provide,
 * so there is never a multiple-definition clash. mingw-safe (no reliance on
 * weak symbols in static archives). See game_extras.h. */
#include "game_extras.h"
#include "gbrt.h"

/* Both extended-view hooks live in ONE TU: a game that opts in must provide
 * the pair together (a capability without an init, or vice versa, is a bug). */
uint16_t game_max_view_width(void) { return 160; }

void game_extended_view_init(struct GBContext *ctx,
                             uint32_t extra_left, uint32_t extra_right) {
    (void)ctx; (void)extra_left; (void)extra_right;
}

void game_extended_view_update(struct GBContext *ctx) { (void)ctx; }

int game_extended_view_bg_tile(struct GBContext *ctx, int screen_x,
                               uint8_t scanline, uint8_t *tile,
                               uint8_t *attr) {
    (void)ctx; (void)screen_x; (void)scanline; (void)tile; (void)attr;
    return 0;
}
