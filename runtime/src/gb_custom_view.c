#include "gb_custom_view.h"
GBCustomRender gb_custom_render;
GBCustomSnapshot gb_custom_snapshot;
GBCustomSnapshot gb_custom_reset;
GBCustomReadTap gb_custom_read_tap;
GBCustomReadOverride gb_custom_read_override;
int gb_custom_requested_width;
int gb_custom_width = 160;
int gb_custom_resolve_width(int w, int h) {
    int64_t width = gb_custom_requested_width;
    if (!gb_custom_render || !width) return 160;
    if (width < 0) width = h > 0 ? ((int64_t)w * 144 + h / 2) / h : 256;
    if (width < 160) width = 160;
    if (width > GB_CUSTOM_MAX_WIDTH) width = GB_CUSTOM_MAX_WIDTH;
    return (int)width;
}
