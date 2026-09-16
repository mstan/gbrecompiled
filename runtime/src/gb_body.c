/* gb_body.c -- multi-body resolution. See gb_body.h. */
#include "gb_body.h"
#include "game_extras.h"

#include <stdio.h>
#include <string.h>

static const GBBody *s_active;
static int s_resolved;

const GBBody *gb_body_resolve(const GBBody *fallback) {
    if (s_resolved) return s_active;
    s_resolved = 1;
    s_active = fallback;

    int count = 0;
    const GBBody *const *bodies = game_get_bodies(&count);
    if (!bodies || count <= 0) return s_active;

    int index = game_select_body(bodies, count);
    if (index < 0 || index >= count || !bodies[index]) {
        fprintf(stderr,
                "[body] game_select_body() returned %d for %d body/bodies; "
                "falling back to %s\n",
                index, count,
                s_active && s_active->id ? s_active->id : "(none)");
        return s_active;
    }

    s_active = bodies[index];
    /* Route the runtime's own dispatch entry at this body. Bodies other than
     * the primary one are compiled with namespaced symbols, so the plain
     * gb_dispatch the runtime would otherwise call belongs to the primary. */
    if (s_active->dispatch) {
        gb_set_dispatch(s_active->dispatch, s_active->dispatch_call);
    }
    return s_active;
}

const GBBody *gb_body_active(void) { return s_active; }

const char *gb_body_active_id(void) {
    return s_active ? s_active->id : NULL;
}
