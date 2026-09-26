#include "gb_custom_view.h"
#include <string.h>
GBCustomRender gb_custom_render;
GBCustomSnapshot gb_custom_snapshot;
GBCustomSnapshot gb_custom_frame_end;
GBCustomSnapshot gb_custom_reset;
GBCustomReadTap gb_custom_read_tap;
GBCustomReadOverride gb_custom_read_override;
int gb_custom_requested_width;
int gb_custom_width = 160;
int gb_custom_requested_height = 144;
int gb_custom_height = 144;
int gb_custom_view_width = 160;
int gb_custom_view_height = 144;
GBCustomFit gb_custom_fit;
static int clamp_size(int64_t value, int lo, int hi) {
    return value < lo ? lo : value > hi ? hi : (int)value;
}
void gb_custom_fill_size(int window_w, int window_h, int height, int whole_pixels,
                         int *out_width, int *out_height) {
    int64_t w = window_w, h = clamp_size(height, 144, GB_CUSTOM_MAX_HEIGHT);
    if (window_w <= 0 || window_h <= 0) {
        w = h * 16 / 9;   /* no window yet: a common screen shape */
    } else if (whole_pixels) {
        int64_t scale = window_h / h;
        if (scale < 1) scale = 1;
        w = window_w / scale;
        h = window_h / scale;
    } else {
        w = ((int64_t)window_w * h + window_h / 2) / window_h;
    }
    *out_width = clamp_size(w, 160, GB_CUSTOM_MAX_WIDTH);
    *out_height = clamp_size(h, 144, GB_CUSTOM_MAX_HEIGHT);
}
void gb_custom_resolve_size(int window_w, int window_h, int whole_pixels,
                            int *out_width, int *out_height) {
    if (!gb_custom_render || !gb_custom_requested_width) {
        *out_width = 160;
        *out_height = 144;
    } else if (gb_custom_requested_width < 0) {
        gb_custom_fill_size(window_w, window_h, gb_custom_requested_height, whole_pixels,
                            out_width, out_height);
    } else {
        *out_width = clamp_size(gb_custom_requested_width, 160, GB_CUSTOM_MAX_WIDTH);
        *out_height = clamp_size(gb_custom_requested_height, 144, GB_CUSTOM_MAX_HEIGHT);
    }
}
int gb_custom_native_scaling = GB_CUSTOM_NATIVE_IN_VIEW;
int gb_custom_native_scale = 1;
int gb_custom_draw_frame(struct GBContext *ctx, uint32_t *out, int width, const uint32_t *native) {
    return gb_custom_render && gb_custom_render(ctx, out, width, native);
}
void gb_custom_center_native(uint32_t *out, int width, const uint32_t *native, int native_width) {
    for (size_t i = 0; i < (size_t)width * gb_custom_height; ++i) out[i] = 0xff000000u;
    int copy_width = native_width < width ? native_width : width;
    for (int y = 0; y < 144; ++y)
        memcpy(out + (size_t)(y + (gb_custom_height - 144) / 2) * width + (width - copy_width) / 2,
               native + y * native_width + (native_width - copy_width) / 2,
               (size_t)copy_width * sizeof(uint32_t));
}
int gb_custom_compose_frame(struct GBContext *ctx, uint32_t *out, int width,
                            const uint32_t *native, int native_width) {
    if (gb_custom_draw_frame(ctx, out, width, native)) return 1;
    gb_custom_center_native(out, width, native, native_width);
    return 0;
}
