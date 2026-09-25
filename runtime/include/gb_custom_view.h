#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct GBContext;
/* Host-only compositor. The hardware PPU and its save-state layout stay native.
 * Frame buffers are sized to the resolved view, so the limits cost nothing
 * until a view uses them. 8192 covers a whole 32 x 32 map of 256-pixel sectors. */
#define GB_CUSTOM_MAX_WIDTH 8192
#define GB_CUSTOM_MAX_HEIGHT 8192
typedef int (*GBCustomRender)(struct GBContext *, uint32_t *, int, const uint32_t *);
typedef void (*GBCustomSnapshot)(struct GBContext *);
typedef void (*GBCustomReadTap)(struct GBContext *, uint16_t);
/* Called from gb_read8 for banked ROM, external RAM, work RAM and HRAM, so a
 * game module can present a virtual value (e.g. a widened camera) to specific
 * consumers. Inert when no hook is installed. */
typedef uint8_t (*GBCustomReadOverride)(struct GBContext *, uint16_t, uint8_t);
extern GBCustomRender gb_custom_render;
extern GBCustomSnapshot gb_custom_snapshot;
/* Called when the PPU completes line 143, before VBlank code can change state
 * that drew the finished native frame (e.g. palettes). */
extern GBCustomSnapshot gb_custom_frame_end;
extern GBCustomSnapshot gb_custom_reset;
extern GBCustomReadTap gb_custom_read_tap;
extern GBCustomReadOverride gb_custom_read_override;
/* 0 = disabled; -1 = fill the window; positive = fixed game-pixel width. */
extern int gb_custom_requested_width;
/* The fixed height, or when filling the window the least height to show.
 * Either may change while running; the next presented frame resolves it. */
extern int gb_custom_requested_height;
/* The resolved size of the presented frame. Existing renderers default to 144
 * rows; custom render callbacks may use gb_custom_height for their row count.
 * A gb_custom_fit hook can make it smaller than the view below. */
extern int gb_custom_width;
extern int gb_custom_height;
/* The view resolved for the window before gb_custom_fit (the window's size and
 * borders follow it). */
extern int gb_custom_view_width;
extern int gb_custom_view_height;
/* Optional, called before each frame is rendered: *width x *height holds the
 * resolved view, which the game may shrink to the part of the world worth
 * showing (a room smaller than the view), and *scale, 0 on entry, may be set
 * to present it at that many window pixels per game pixel instead of by the
 * platform's scaling mode. whole_pixels is set for Pixel Perfect scaling. */
typedef void (*GBCustomFit)(struct GBContext *, int window_w, int window_h, int whole_pixels,
                            int *width, int *height, double *scale);
extern GBCustomFit gb_custom_fit;
/* Size of a view that fills a window_w x window_h window and is at least
 * `height` tall. With whole pixels it takes the largest whole scale that
 * keeps that height (a window shorter than it is filled at one game pixel per
 * window pixel); otherwise the height is exact and the width follows the
 * window's shape. Clamped to 160 x 144 .. the maximum. */
void gb_custom_fill_size(int window_w, int window_h, int height, int whole_pixels,
                         int *out_width, int *out_height);
/* The requested view for a window of this size: fixed, filled, or native. */
void gb_custom_resolve_size(int window_w, int window_h, int whole_pixels,
                            int *out_width, int *out_height);
/* How the platform shows the native picture on frames the game declines (its
 * render returns 0: menus, dialogue, rooms it keeps native). IN_VIEW centers
 * it in the view, one view pixel per game pixel; the others present the
 * 160 x 144 picture on its own, through the shader chain, scaled to the window
 * by the platform's scaling mode, by one of the modes 0-3 (Pixel Perfect,
 * Aspect Fit, Aspect Fill, Stretch), or at gb_custom_native_scale window
 * pixels per game pixel (WHOLE_SCALE, at most as many as fit). */
enum {
    GB_CUSTOM_NATIVE_IN_VIEW = -2,
    GB_CUSTOM_NATIVE_SCALING_MODE = -1,
    GB_CUSTOM_NATIVE_WHOLE_SCALE = 4,
};
extern int gb_custom_native_scaling;
extern int gb_custom_native_scale;
/* Render the view into out; 0, drawing nothing, when the game declines the
 * frame and the native picture is shown instead. */
int gb_custom_draw_frame(struct GBContext *, uint32_t *out, int width, const uint32_t *native);
/* The native picture centered in black, gb_custom_height rows. */
void gb_custom_center_native(uint32_t *out, int width, const uint32_t *native, int native_width);
/* Render, or center the native image when the game declines a scene. Shared
 * by display and debug captures so both use the same two-dimensional layout;
 * returns whether the view drew the frame. */
int gb_custom_compose_frame(struct GBContext *, uint32_t *out, int width,
                            const uint32_t *native, int native_width);
#ifdef __cplusplus
}
#endif
