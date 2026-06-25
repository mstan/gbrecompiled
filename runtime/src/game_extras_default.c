/*
 * game_extras_default.c -- Default (no-op) implementations of game hooks
 *
 * These weak stubs are used when no game-specific extras.c is linked.
 * Games override these by providing their own extras.c with strong definitions.
 *
 * On GCC/Clang: __attribute__((weak)) lets the linker prefer game's version.
 * On MSVC: /FORCE:MULTIPLE or the game's .obj takes precedence (linker picks first).
 */
#include "game_extras.h"
#include "gbrt.h"

/* Weak symbols don't work reliably with mingw-w64 static archives.
   For now, use plain definitions. Game-specific overrides can replace
   this file in the link. */
#define WEAK_FUNC

WEAK_FUNC void game_on_init(struct GBContext *ctx) { (void)ctx; }
WEAK_FUNC void game_on_frame(struct GBContext *ctx) { (void)ctx; }
WEAK_FUNC void game_post_frame(struct GBContext *ctx) { (void)ctx; }
WEAK_FUNC void game_fill_frame_record(GBFrameRecord *r) { (void)r; }
WEAK_FUNC int  game_handle_debug_cmd(const char *cmd, int id, const char *json) {
    (void)cmd; (void)id; (void)json; return 0;
}
WEAK_FUNC int  game_dispatch_override(struct GBContext *ctx, uint16_t addr) {
    (void)ctx; (void)addr; return 0;
}
WEAK_FUNC const char *game_get_name(void) { return "GB Recompiled"; }
WEAK_FUNC int  game_handle_arg(const char *arg, const char *next_arg) {
    (void)arg; (void)next_arg; return 0;
}
WEAK_FUNC uint32_t game_get_expected_crc32(void) { return 0; }
WEAK_FUNC const uint32_t *game_get_valid_crcs(int *out_count) {
    if (out_count) *out_count = 0;
    return 0;
}
