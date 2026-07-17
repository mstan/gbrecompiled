/*
 * game_extras.h -- Game-specific hook interface for GB recomp projects
 *
 * Each recompiled game implements these hooks in its own extras.c.
 * The runtime calls them at well-defined points in the frame loop.
 *
 * Modeled after nesrecomp/runner/game_extras.h and psxrecomp/runner/game_extras.h.
 */
#pragma once
#include <stdint.h>
#include "debug_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
struct GBContext;

/* ---- Lifecycle hooks ---- */

/* Called once at startup, after runtime init.
 * Good place to call gb_debug_server_init(). */
void game_on_init(struct GBContext *ctx);

/* Called once per frame, before game logic runs.
 * Good place to call gb_debug_server_poll() and
 * gb_debug_server_wait_if_paused(). */
void game_on_frame(struct GBContext *ctx);

/* Called after VBlank / frame completion.
 * Good place to call gb_debug_server_record_frame()
 * and gb_debug_server_check_watchpoints(). */
void game_post_frame(struct GBContext *ctx);

/* ---- UI hook ---- */

/* Called once per frame from inside the runtime's ImGui settings window
 * (after the built-in sections, inside the scroll area). The game may emit
 * its own ImGui controls here — e.g. a "Pokemon Options" CollapsingHeader.
 * Implementations linked into a C++ TU can call ImGui:: directly (the
 * runtime and game share the one ImGui context). Default: no-op.
 * Keeps the agnostic core free of any game-specific UI. */
void game_draw_overlay(struct GBContext *ctx);

/* ---- Debug hooks ---- */

/* Fill game-specific data into the frame record.
 * Write up to 16 bytes into r->game_data[]. */
void game_fill_frame_record(GBFrameRecord *r);

/* Handle a game-specific debug command.
 * Return 1 if handled, 0 to fall through to "unknown command". */
int game_handle_debug_cmd(const char *cmd, int id, const char *json);

/* ---- Dispatch hooks ---- */

/* Called when gb_dispatch() can't find a compiled function.
 * Return 1 if the game handled it (e.g., SRAM code), 0 to fall through. */
int game_dispatch_override(struct GBContext *ctx, uint16_t addr);

/* ---- Extended view (widescreen) capability ---- */

/* Maximum view width this game supports (opt-in widescreen). Default 160 =
 * not opted in; a game that supports the extended view returns up to
 * GB_WS_MAX_VIEW_WIDTH (256). See gb_widescreen.h / docs/WIDESCREEN.md. */
uint16_t game_max_view_width(void);

/* Called exactly once, only after a non-native view width is authorized.
 * The game installs its widescreen hooks here (sidecar arming, bound
 * overrides, pillarbox policy). Default: no-op. */
void game_extended_view_init(struct GBContext *ctx,
                             uint32_t extra_left, uint32_t extra_right);

/* Called once at the start of each armed frame's margin render. A game may
 * refresh presentation-only backing data and must fail closed if its scene or
 * validation contract is not satisfied. Part of the opt-in hook group. */
void game_extended_view_update(struct GBContext *ctx);

/* Optional margin BG source. Return 1 with tile/attr filled to replace the
 * wrapping hardware-map cell, 0 to use normal VRAM, or -1 for finite-map
 * void (black). Called only for margin pixels after the game has opened its
 * pillarbox gate. Part of the opt-in hook group. */
int game_extended_view_bg_tile(struct GBContext *ctx, int screen_x,
                               uint8_t scanline, uint8_t *tile,
                               uint8_t *attr);

/* ---- Info ---- */

/* Return the game name for the window title. */
const char *game_get_name(void);

/* Return the launcher console identity for this game: "gb" (original Game Boy /
 * DMG branding) or "gbc" (Game Boy Color branding). Consumed by the recomp-ui
 * pre-boot launcher (launcher_ui_seam.c) to pick the platform label, theme, and
 * controller/logo art. Default: "gbc" (the ecosystem is Game Boy Color; every
 * color-capable and colorizable cart uses it). DMG-only titles (Tetris, Super
 * Mario Land) override this to "gb" in their extras.c. */
const char *game_get_platform(void);

/* Handle a CLI argument. Return 1 if consumed, 0 to ignore. */
int game_handle_arg(const char *arg, const char *next_arg);

/* ---- ROM identity hooks (NES-style) ---- */

/* Return the game's expected ROM CRC32. Return 0 to skip CRC validation.
 * Used when there's a single canonical ROM version. */
uint32_t game_get_expected_crc32(void);

/* Return a static array of valid ROM CRC32s for multi-version games
 * (e.g. Pokemon Red + Blue share code; only data differs).
 * Set *out_count to the array length. Return NULL to defer to
 * game_get_expected_crc32(). */
const uint32_t *game_get_valid_crcs(int *out_count);

#ifdef __cplusplus
}
#endif
