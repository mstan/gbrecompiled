// color_lut.h — present-time screen-color simulation (BGR555 -> ARGB8888 LUT).
//
// PRESENT-TIME ONLY. This never touches the emulation, the framebuffer the PPU
// produces (ppu->rgb_framebuffer / ppu->color_framebuffer), or the
// differential-verify path — diff_frame / oracle comparisons in
// differential.c stay defined on the raw RGBA the PPU renders. The LUT is a
// 32768-entry table applied to a COPY of the frame at SDL-upload time, and it
// defaults to Raw (exact passthrough), so default behavior and every
// hashed/verified frame are byte-identical unless a screen model is opted in
// via GBCRECOMP_SCREEN={raw,unlit,frontlit,backlit,classic}.
//
// The math is first-principles CIE colorimetry (xyY->XYZ, primaries->matrix,
// Bradford adaptation, sRGB OETF) over published colorimeter measurements; it
// is not a ported shader. The Game Boy Color screen is the same BGR555 15-bit
// domain as the GBA, so the same LUT model applies directly.
//
// -- Attribution ----------------------------------------------------
// Ported from JRickey/gba-recomp (https://github.com/JRickey/gba-recomp),
// crates/screen/src/{color,profile,lut}.rs, via the gbarecomp C++ port
// (src/runtime/color_lut.*), (c) Jrickey, MIT OR Apache-2.0, used with
// permission. The C port + the ARGB8888-input present path are ours.

#ifndef GBCRECOMP_COLOR_LUT_H
#define GBCRECOMP_COLOR_LUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Which physical screen revision to simulate.
typedef enum {
  GBC_SCREEN_RAW = 0,  // 5->8 bit replication, untouched (default; passthrough)
  GBC_SCREEN_UNLIT,    // original reflective panel, dark viewing
  GBC_SCREEN_FRONTLIT, // reflective panel, lit
  GBC_SCREEN_BACKLIT,  // late near-sRGB panel, clean blacks
  GBC_SCREEN_CLASSIC,  // community-canonical gamma-4.0 model
} ScreenKind;

// Display colorspace the emitted bytes are interpreted in.
typedef enum {
  GBC_DISPLAY_SRGB = 0,
  GBC_DISPLAY_P3,
} DisplayTarget;

// Parse a config/env token; returns false if unrecognized.
bool screen_kind_from_name(const char* name, ScreenKind* out);

typedef struct {
  ScreenKind    screen;  // default GBC_SCREEN_RAW
  double        darken;  // <0 = per-screen default
  DisplayTarget target;  // default GBC_DISPLAY_SRGB
} ColorSettings;

// A baked BGR555 -> RGB888 table (one {R,G,B} per 15-bit hardware index).
// Build once per settings change; apply per frame as one indexed lookup per
// pixel. Opaque to callers; allocate with color_lut_create.
typedef struct ColorLut ColorLut;

// Build a LUT for the given settings. Returns NULL on allocation failure.
ColorLut* color_lut_create(const ColorSettings* settings);
void color_lut_destroy(ColorLut* lut);

// True when the LUT is exact 5->8 passthrough (Raw): callers may skip the map.
bool color_lut_is_passthrough(const ColorLut* lut);

// Transform a width*height ARGB8888 frame (0xAARRGGBB host-endian, as the GBC
// PPU's rgb_framebuffer and the SDL ARGB8888 streaming texture use) from `src`
// into `dst`. The 5-bit hardware index is recovered from the top 5 bits of
// each channel (GBC color is BGR555; rgb_framebuffer is its 5->8 expansion).
// Alpha is preserved. `src` and `dst` each hold width*height uint32 pixels and
// may alias (in-place is safe).
void color_lut_map_argb8888(const ColorLut* lut, const uint32_t* src,
                            uint32_t* dst, int width, int height);

// Convenience: build a LUT from the GBCRECOMP_SCREEN / GBCRECOMP_SCREEN_DARKEN
// / GBCRECOMP_SCREEN_TARGET environment variables. Returns NULL if the env is
// unset/raw/unrecognized (caller then leaves the frame untouched).
ColorLut* color_lut_create_from_env(void);

#ifdef __cplusplus
}
#endif

#endif  // GBCRECOMP_COLOR_LUT_H
