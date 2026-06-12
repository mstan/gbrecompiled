// color_lut.c — see color_lut.h. Present-time only.
//
// Ported from JRickey/gba-recomp (crates/screen/src/{color,profile,lut}.rs)
// via the gbarecomp C++ port (src/runtime/color_lut.cpp), (c) Jrickey,
// MIT OR Apache-2.0, used with permission. C port is ours.

#include "color_lut.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ── CIE colorimetry (all f64; runs only at LUT-build time) ─────────
typedef struct { double x, y; } Xy;
typedef struct { Xy red, green, blue, white; } Primaries;
typedef struct { double m[3][3]; } Mat3;

static const Xy kD65 = {0.3127, 0.3290};
static const Primaries kSrgb = {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}, {0.3127, 0.3290}};
static const Primaries kDisplayP3 = {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.3127, 0.3290}};

// Measured reflective/frontlit gamut, and the near-sRGB backlit revision.
static const Primaries kPanelReflective = {
    {0.4925, 0.3100}, {0.3150, 0.4825}, {0.1625, 0.1925}, {0.3127, 0.3290}};
static const Primaries kPanelBacklit = {
    {0.6191, 0.3454}, {0.3269, 0.6003}, {0.1436, 0.0893}, {0.3127, 0.3290}};

struct ColorLut {
  uint8_t table[32768][3];  // table[bgr555] = {R,G,B}
  bool passthrough;
};

static bool xy_eq(Xy a, Xy b) { return a.x == b.x && a.y == b.y; }

static void mat_apply(const Mat3* a, const double v[3], double out[3]) {
  for (int i = 0; i < 3; ++i)
    out[i] = a->m[i][0] * v[0] + a->m[i][1] * v[1] + a->m[i][2] * v[2];
}

static Mat3 mat_mul(const Mat3* a, const Mat3* b) {
  Mat3 r;
  memset(&r, 0, sizeof(r));
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k) r.m[i][j] += a->m[i][k] * b->m[k][j];
  return r;
}

static Mat3 mat_inverse(const Mat3* a) {
  const double (*m)[3] = a->m;
  Mat3 out;
  memset(&out, 0, sizeof(out));
  double cof[3][3];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      int r1 = (r + 1) % 3, r2 = (r + 2) % 3;
      int c1 = (c + 1) % 3, c2 = (c + 2) % 3;
      cof[r][c] = m[r1][c1] * m[r2][c2] - m[r1][c2] * m[r2][c1];
    }
  }
  double det = m[0][0] * cof[0][0] + m[0][1] * cof[0][1] + m[0][2] * cof[0][2];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) out.m[i][j] = cof[j][i] / det;
  return out;
}

static void xy_to_xyz(Xy c, double out[3]) {
  out[0] = c.x / c.y;
  out[1] = 1.0;
  out[2] = (1.0 - c.x - c.y) / c.y;
}

// Linear RGB -> CIE XYZ for a set of primaries (white maps to Y=1).
static Mat3 rgb_to_xyz(const Primaries* p) {
  double r[3], g[3], b[3], w[3];
  xy_to_xyz(p->red, r);
  xy_to_xyz(p->green, g);
  xy_to_xyz(p->blue, b);
  xy_to_xyz(p->white, w);
  Mat3 m = {{{r[0], g[0], b[0]}, {r[1], g[1], b[1]}, {r[2], g[2], b[2]}}};
  Mat3 inv = mat_inverse(&m);
  double s[3];
  mat_apply(&inv, w, s);
  Mat3 out = m;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) out.m[i][j] *= s[j];
  return out;
}

static Mat3 bradford_adaptation(Xy from, Xy to) {
  static const Mat3 kBradford = {{{0.8951, 0.2664, -0.1614},
                                  {-0.7502, 1.7135, 0.0367},
                                  {0.0389, -0.0685, 1.0296}}};
  double f[3], t[3], src[3], dst[3];
  xy_to_xyz(from, f);
  xy_to_xyz(to, t);
  mat_apply(&kBradford, f, src);
  mat_apply(&kBradford, t, dst);
  Mat3 scale = {{{dst[0] / src[0], 0, 0},
                 {0, dst[1] / src[1], 0},
                 {0, 0, dst[2] / src[2]}}};
  Mat3 binv = mat_inverse(&kBradford);
  Mat3 a = mat_mul(&binv, &scale);
  return mat_mul(&a, &kBradford);
}

static Mat3 rgb_to_rgb(const Primaries* src, const Primaries* dst) {
  Mat3 to_xyz = rgb_to_xyz(src);
  Mat3 dst_xyz = rgb_to_xyz(dst);
  Mat3 from_xyz = mat_inverse(&dst_xyz);
  if (xy_eq(src->white, dst->white)) return mat_mul(&from_xyz, &to_xyz);
  Mat3 adapt = bradford_adaptation(src->white, dst->white);
  Mat3 a = mat_mul(&from_xyz, &adapt);
  return mat_mul(&a, &to_xyz);
}

static double srgb_oetf(double v) {
  return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

// ── Panel optical model ────────────────────────────────────────────
typedef struct { Primaries primaries; double gamma, luminance, black_floor; } PanelModel;

static double effective_gamma(double darken) {
  if (darken < 0.0) darken = 0.0;
  if (darken > 1.0) darken = 1.0;
  return 2.2 + 1.6 * darken;
}

static double default_darken(ScreenKind s) {
  switch (s) {
    case GBC_SCREEN_UNLIT:    return 0.7;
    case GBC_SCREEN_FRONTLIT: return 0.15;
    default:                  return 0.0;
  }
}

// Returns false for Raw/Classic (handled separately).
static bool panel_model(ScreenKind s, double darken, PanelModel* out) {
  switch (s) {
    case GBC_SCREEN_UNLIT:
      out->primaries = kPanelReflective; out->gamma = effective_gamma(darken);
      out->luminance = 0.91; out->black_floor = 0.055; return true;
    case GBC_SCREEN_FRONTLIT:
      out->primaries = kPanelReflective; out->gamma = effective_gamma(darken);
      out->luminance = 0.91; out->black_floor = 0.030; return true;
    case GBC_SCREEN_BACKLIT:
      out->primaries = kPanelBacklit; out->gamma = 2.2;
      out->luminance = 0.935; out->black_floor = 0.002; return true;
    default:
      return false;
  }
}

static uint8_t quantize(double v) {
  if (v < 0.0) v = 0.0;
  if (v > 1.0) v = 1.0;
  return (uint8_t)(v * 255.0 + 0.5);
}

// The community-canonical gamma-4.0 model, reproduced exactly as published.
static void classic_entry(uint16_t px, uint8_t out[3]) {
  double r = (px & 31) / 31.0;
  double g = ((px >> 5) & 31) / 31.0;
  double b = ((px >> 10) & 31) / 31.0;
  double lr = pow(r, 4.0), lg = pow(g, 4.0), lb = pow(b, 4.0);
  double vr = (0.0 * lb + 50.0 * lg + 255.0 * lr) / 255.0;
  double vg = (30.0 * lb + 230.0 * lg + 10.0 * lr) / 255.0;
  double vb = (220.0 * lb + 10.0 * lg + 50.0 * lr) / 255.0;
  out[0] = quantize(pow(vr, 1.0 / 2.2) * (255.0 / 280.0));
  out[1] = quantize(pow(vg, 1.0 / 2.2) * (255.0 / 280.0));
  out[2] = quantize(pow(vb, 1.0 / 2.2) * (255.0 / 280.0));
}

static const Primaries* target_primaries(DisplayTarget t) {
  return t == GBC_DISPLAY_P3 ? &kDisplayP3 : &kSrgb;
}

bool screen_kind_from_name(const char* name, ScreenKind* out) {
  if (!name) return false;
  if (strcmp(name, "raw") == 0)      { *out = GBC_SCREEN_RAW;      return true; }
  if (strcmp(name, "unlit") == 0)    { *out = GBC_SCREEN_UNLIT;    return true; }
  if (strcmp(name, "frontlit") == 0) { *out = GBC_SCREEN_FRONTLIT; return true; }
  if (strcmp(name, "backlit") == 0)  { *out = GBC_SCREEN_BACKLIT;  return true; }
  if (strcmp(name, "classic") == 0)  { *out = GBC_SCREEN_CLASSIC;  return true; }
  return false;
}

ColorLut* color_lut_create(const ColorSettings* settings) {
  ColorLut* lut = (ColorLut*)calloc(1, sizeof(ColorLut));
  if (!lut) return NULL;

  if (settings->screen == GBC_SCREEN_RAW) {
    lut->passthrough = true;
    for (int px = 0; px < 32768; ++px) {
      uint8_t r = px & 31, g = (px >> 5) & 31, b = (px >> 10) & 31;
      lut->table[px][0] = (uint8_t)(r << 3 | r >> 2);
      lut->table[px][1] = (uint8_t)(g << 3 | g >> 2);
      lut->table[px][2] = (uint8_t)(b << 3 | b >> 2);
    }
    return lut;
  }
  if (settings->screen == GBC_SCREEN_CLASSIC) {
    for (int px = 0; px < 32768; ++px) classic_entry((uint16_t)px, lut->table[px]);
    return lut;
  }

  double darken = settings->darken < 0.0 ? default_darken(settings->screen)
                                         : settings->darken;
  PanelModel model;
  memset(&model, 0, sizeof(model));
  panel_model(settings->screen, darken, &model);
  Mat3 to_display = rgb_to_rgb(&model.primaries, target_primaries(settings->target));

  for (int px = 0; px < 32768; ++px) {
    double c[3] = {(px & 31) / 31.0, ((px >> 5) & 31) / 31.0,
                   ((px >> 10) & 31) / 31.0};
    double lin[3];
    for (int i = 0; i < 3; ++i) {
      double v = pow(c[i], model.gamma) * model.luminance;
      lin[i] = v > 1.0 ? 1.0 : v;
    }
    double out[3];
    mat_apply(&to_display, lin, out);
    for (int i = 0; i < 3; ++i) {
      double v = out[i] < 0.0 ? 0.0 : (out[i] > 1.0 ? 1.0 : out[i]);
      double lifted = model.black_floor + (1.0 - model.black_floor) * v;
      out[i] = srgb_oetf(lifted);
    }
    lut->table[px][0] = quantize(out[0]);
    lut->table[px][1] = quantize(out[1]);
    lut->table[px][2] = quantize(out[2]);
  }
  return lut;
}

void color_lut_destroy(ColorLut* lut) { free(lut); }

bool color_lut_is_passthrough(const ColorLut* lut) {
  return lut && lut->passthrough;
}

void color_lut_map_argb8888(const ColorLut* lut, const uint32_t* src,
                            uint32_t* dst, int width, int height) {
  const int n = width * height;
  for (int i = 0; i < n; ++i) {
    uint32_t px = src[i];
    uint32_t a = px & 0xFF000000u;
    uint8_t r = (uint8_t)((px >> 16) & 0xFF);
    uint8_t g = (uint8_t)((px >> 8) & 0xFF);
    uint8_t b = (uint8_t)(px & 0xFF);
    // Recover the 5-bit hardware channels (top 5 bits) and re-pack as BGR555
    // (red is the low 5 bits, matching the LUT index).
    uint32_t idx = (uint32_t)(r >> 3) | ((uint32_t)(g >> 3) << 5) |
                   ((uint32_t)(b >> 3) << 10);
    const uint8_t* e = lut->table[idx];
    dst[i] = a | ((uint32_t)e[0] << 16) | ((uint32_t)e[1] << 8) | (uint32_t)e[2];
  }
}

ColorLut* color_lut_create_from_env(void) {
  const char* name = getenv("GBCRECOMP_SCREEN");
  ScreenKind kind;
  if (!name || !screen_kind_from_name(name, &kind) || kind == GBC_SCREEN_RAW) {
    return NULL;  // default OFF: caller leaves the frame untouched (passthrough)
  }
  ColorSettings s;
  s.screen = kind;
  s.darken = -1.0;
  s.target = GBC_DISPLAY_SRGB;
  const char* darken = getenv("GBCRECOMP_SCREEN_DARKEN");
  if (darken) s.darken = atof(darken);
  const char* target = getenv("GBCRECOMP_SCREEN_TARGET");
  if (target && strcmp(target, "p3") == 0) s.target = GBC_DISPLAY_P3;
  return color_lut_create(&s);
}
