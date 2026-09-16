/* Auto-split default for one game hook. One strong symbol per translation
 * unit: a game's extras.c that overrides only SOME hooks links cleanly.
 * See game_extras.h and gb_body.h.
 *
 * Both multi-body hooks live in ONE TU: a game that ships several bodies must
 * provide the pair together (a body table with no selector, or a selector with
 * no table, is a bug). */
#include "game_extras.h"

const struct GBBody *const *game_get_bodies(int *out_count) {
    if (out_count) *out_count = 0;
    return 0;
}

int game_select_body(const struct GBBody *const *bodies, int count) {
    (void)bodies;
    (void)count;
    return 0;
}
