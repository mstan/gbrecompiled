/*
 * gb_widescreen.h -- Opt-in extended horizontal view (widescreen)
 *
 * Ported convention: gbarecomp (MegaManZero) capability gate + margins,
 * nesrecomp (SuperMarioBros) OAM X16 sidecar. See docs/WIDESCREEN.md.
 *
 * Everything here is host presentation state: default OFF, never serialized,
 * byte-identical output when off. A game opts in by overriding
 * game_max_view_width() / game_extended_view_init() in its extras.c; the
 * width request comes from GBCRECOMP_VIEW_WIDTH / --view-width. Arming only
 * happens in the SDL platform path, never in differential/cosim runs.
 */
#ifndef GB_WIDESCREEN_H
#define GB_WIDESCREEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GBContext;

/* Hard engine ceiling: 48px per side -> 256 total (16:9 at 144, and exactly
 * the 256px BG map width; wider would alias with the map wrap). */
#define GB_WS_MAX_EXTRA_X 48
#define GB_WS_MAX_VIEW_WIDTH (160 + 2 * GB_WS_MAX_EXTRA_X)

/* ---- Resolved geometry (0/0 when off) ---- */
extern int g_gbws_extra_left;
extern int g_gbws_extra_right;
extern int g_gbws_active;              /* non-native view armed */

/* ---- Fail-closed presentation gating (game module policy) ----
 * While set, the corresponding margin columns render black instead of BG.
 * Default ON (fail closed) when the view is armed; the game module clears
 * them per frame once it has proven the scene supports the wide view. */
extern int g_gbws_pillarbox;           /* both margins */
extern int g_gbws_pillarbox_left;
extern int g_gbws_pillarbox_right;

/* ---- OAM X16 sidecar (nesrecomp model, GB flavor) ----
 * The game keeps writing normal 8-bit OAM; the runtime keeps a parallel
 * signed 16-bit X (raw OAM domain: screen_x + 8) per slot, unwrapped at
 * shadow-OAM write time from the published draw-object context, copied to
 * the render side at OAM DMA. Renderer consumes g_gbws_oam_x16[slot]-8 when
 * the sidecar is armed. Not serialized; repopulates on the next DMA. */
extern int     g_gbws_oam_sidecar;     /* master enable (game module sets) */
extern int16_t g_gbws_oam_x16[40];     /* render side, paired with ctx->oam */
extern int16_t g_gbws_shadow_x16[40];  /* shadow-OAM page side */

/* Current draw-object context (published by the game module from inside the
 * game's own screen-X computation; raw OAM domain = true screen X + 8). */
extern int16_t g_gbws_obj_true_raw16;
extern uint8_t g_gbws_obj_rel8;
extern uint8_t g_gbws_obj_ctx_valid;

/* Shadow OAM page (addr>>8) the sidecar tracks. The game module may set it
 * explicitly; otherwise it auto-latches from the first OAM DMA source. */
extern uint16_t g_gbws_shadow_oam_page;

/* ---- API ---- */

/* Publish/clear the draw-object context. true_screen_x is the object's true
 * signed screen X (unbiased); rel8_biased is the 8-bit value the game's own
 * math produced for the same quantity (raw OAM domain, i.e. +8). */
void gb_ws_publish_obj_context(int true_screen_x, uint8_t rel8_biased);
void gb_ws_invalidate_obj_context(void);

/* CLI request (parsed in generated main, before arming). 0 = none. */
void gb_ws_set_cli_request(int width);

/* Resolve request (env > CLI) against the game capability and arm the view:
 * sizes the PPU margins and calls game_extended_view_init() exactly once.
 * Called from gb_platform_register_context(). No-op at native width. */
void gb_ws_arm(struct GBContext* ctx);

/* Re-apply margins to the PPU after whole-struct restores (savestate load)
 * and clear sidecar state. No-op when not armed. */
void gb_ws_reapply(struct GBContext* ctx);

/* Sidecar internals (called from gbrt.c write/DMA paths). */
void gb_ws_sidecar_track(int slot, uint8_t val);
int  gb_ws_sidecar_dma_match(uint8_t src_page);
void gb_ws_sidecar_direct_oam(int slot, uint8_t val);

/* WRAM write tap (gb_write8 calls this only while the view is armed): sidecar
 * tracking + the game module's watch-range hook, invoked AFTER the store. */
void gb_ws_wram_write_tap(struct GBContext* ctx, uint16_t addr, uint8_t val);
extern void (*g_gbws_wram_write_hook)(struct GBContext* ctx, uint16_t addr, uint8_t val);
extern uint16_t g_gbws_watch_lo;    /* inclusive; empty when lo > hi */
extern uint16_t g_gbws_watch_hi;

/* Current render width (native 160 when off). */
int gb_ws_render_width(void);

#ifdef __cplusplus
}
#endif

#endif /* GB_WIDESCREEN_H */
