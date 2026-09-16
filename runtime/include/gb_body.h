/*
 * gb_body.h -- Multi-body seam: one executable, several recompiled ROM bodies.
 *
 * A "body" is one complete recompilation of one ROM image: its own dispatch
 * table, its own rom_data[], its own init/run/default_config. Normally a
 * generated project has exactly one, and nothing here is used at all.
 *
 * A project that ships more than one body (e.g. a faithful build plus a
 * romhack build of the same cart) recompiles each ROM into its own tree.
 * Every body except the primary one is generated with [options] body_only =
 * true, which namespaces every emitted global behind a symbol prefix so the
 * trees link into a single executable. The recompiler emits one
 * `const GBBody <prefix>_body` descriptor per body ([options] multi_body or
 * body_only); the game collects them in its extras.c:
 *
 *     extern const GBBody Foo_body, Foo_Hack_body;
 *     static const GBBody *const k_bodies[] = { &Foo_body, &Foo_Hack_body };
 *     const GBBody *const *game_get_bodies(int *n) { *n = 2; return k_bodies; }
 *     int game_select_body(const GBBody *const *b, int n) { ... mod toggle ... }
 *
 * The generated <prefix>_main.c of a multi_body project then boots whichever
 * body game_select_body() returned: it takes the GBConfig from that body, uses
 * the body's id as the save id (so .sav/.state files never cross between
 * bodies), routes the runtime's dispatch entry to that body, and calls the
 * body's init/run.
 *
 * Everything here is inert for a single-body project: game_get_bodies()
 * defaults to NULL/0, gb_body_active() stays NULL, and the runtime's dispatch
 * entry falls back to the plain gb_dispatch the generated code defines.
 */
#ifndef GB_BODY_H
#define GB_BODY_H

#include <stddef.h>
#include <stdint.h>

#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBBody {
    /* Stable identity. Also the save id, so two bodies of the same cart keep
     * separate .sav / .state files. Emitted as the body's output_prefix. */
    const char *id;

    /* Human-readable name / launcher console identity ("gb" or "gbc"). Either
     * may be NULL, in which case the game's game_get_name() /
     * game_get_platform() hooks answer instead. */
    const char *display_name;
    const char *platform;

    /* Optional BPS patch, looked up next to the executable, applied IN MEMORY
     * to the user-supplied ROM at init to derive the exact image this body was
     * recompiled from. NULL/"" = this body runs the user's ROM as-is. Nothing
     * is ever written to disk. */
    const char *patch_file;

    /* SHA-256 (lowercase hex) of the image this body was recompiled from.
     * Verified after patch_file is applied. */
    const char *expected_sha256;

    /* Cart header facts of this body's image (byte 0x143). */
    int cartridge_supports_cgb;
    int cartridge_requires_cgb;

    /* Generated entry points. */
    const GBConfig *(*default_config)(void);
    void (*init)(GBContext *ctx);
    void (*run)(GBContext *ctx);
    void (*dispatch)(GBContext *ctx, uint16_t addr);
    void (*dispatch_call)(GBContext *ctx, uint16_t addr);

    /* This body's rom_data[] / rom_size globals, so a game module can read the
     * active body's ROM image without knowing which body it is. */
    uint8_t **rom_data;
    size_t *rom_size;
} GBBody;

/* Pick the body to boot. Consults the game's game_get_bodies() /
 * game_select_body() hooks; falls back to `fallback` (the generated project's
 * own body) when the game declares none, and to NULL when there is no fallback
 * either. Latches the result: gb_body_active() returns it from here on, and a
 * second call is a no-op that returns the same body. */
const GBBody *gb_body_resolve(const GBBody *fallback);

/* The body that gb_body_resolve() latched, or NULL in a single-body build that
 * never called it. A game module may use this to pick per-body data tables. */
const GBBody *gb_body_active(void);

/* Body id (= save id) of the active body, or NULL. */
const char *gb_body_active_id(void);

#ifdef __cplusplus
}
#endif

#endif /* GB_BODY_H */
