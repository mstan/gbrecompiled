#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct GBContext;
/* Host-only compositor. The hardware PPU and its save-state layout stay native. */
#define GB_CUSTOM_MAX_WIDTH 4096
#define GB_CUSTOM_FRAME_SIZE (GB_CUSTOM_MAX_WIDTH * 144)
typedef int (*GBCustomRender)(struct GBContext *, uint32_t *, int, const uint32_t *);
typedef void (*GBCustomSnapshot)(struct GBContext *);
typedef void (*GBCustomReadTap)(struct GBContext *, uint16_t);
typedef uint8_t (*GBCustomReadOverride)(struct GBContext *, uint16_t, uint8_t);
extern GBCustomRender gb_custom_render;
extern GBCustomSnapshot gb_custom_snapshot;
extern GBCustomSnapshot gb_custom_reset;
extern GBCustomReadTap gb_custom_read_tap;
extern GBCustomReadOverride gb_custom_read_override;
/* 0 = disabled; -1 = fit window; positive = fixed game-pixel width. */
extern int gb_custom_requested_width;
extern int gb_custom_width;
int gb_custom_resolve_width(int window_width, int window_height);
#ifdef __cplusplus
}
#endif
