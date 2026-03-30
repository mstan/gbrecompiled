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

/* ---- Info ---- */

/* Return the game name for the window title. */
const char *game_get_name(void);

/* Handle a CLI argument. Return 1 if consumed, 0 to ignore. */
int game_handle_arg(const char *arg, const char *next_arg);

#ifdef __cplusplus
}
#endif
