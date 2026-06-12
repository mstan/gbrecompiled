// psg_shadow.c — see psg_shadow.h.
//
// Verified-enhancement shadow for the GBC PSG. The canon integer mixer in
// audio.c stays authoritative and is the verify oracle; this float re-render
// only substitutes after the ShadowVerifier proves a window, and reverts loud.

#include "psg_shadow.h"

#include "audio_shadow.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Matches the canon mixer's int16 normalization domain (~±32767).
static const float kNorm = 1.0f / 32768.0f;

static bool g_enabled = false;
static bool g_enabled_resolved = false;
static bool g_was_proven = false;

static ShadowVerifier g_verifier;
static bool g_verifier_init = false;

// Float DC-blocker mirroring audio.c's Q15 one-pole (R ≈ 32640/32768 ≈ 0.99609).
static const float kHpR = 32640.0f / 32768.0f;
static float g_hp_prev_in_l, g_hp_prev_in_r;
static float g_hp_prev_out_l, g_hp_prev_out_r;

bool psg_shadow_enabled(void) {
  if (!g_enabled_resolved) {
    const char* v = getenv("GBCRECOMP_PSG_SHADOW");
    g_enabled = (v && v[0] && strcmp(v, "0") != 0);
    g_enabled_resolved = true;
  }
  return g_enabled;
}

void psg_shadow_reset(void) {
  shadow_verifier_init(&g_verifier);
  g_verifier_init = true;
  g_was_proven = false;
  g_hp_prev_in_l = g_hp_prev_in_r = 0.0f;
  g_hp_prev_out_l = g_hp_prev_out_r = 0.0f;
}

static float hp_block(float in, float* prev_in, float* prev_out) {
  float out = in - *prev_in + kHpR * (*prev_out);
  *prev_in = in;
  *prev_out = out;
  return out;
}

void psg_shadow_process(const PsgShadowSample* s, int16_t canon_l,
                        int16_t canon_r, int16_t* out_l, int16_t* out_r) {
  // Default OFF: byte-identical passthrough of the canon samples.
  if (!psg_shadow_enabled()) {
    *out_l = canon_l;
    *out_r = canon_r;
    return;
  }
  if (!g_verifier_init) psg_shadow_reset();

  // Build the shadow mix in float, mirroring audio.c's routing + master scale
  // but never crushing the per-channel sum to int before the master stage.
  float left = 0.0f, right = 0.0f;
  if (s->ch1_r) right += s->ch1_mix;
  if (s->ch1_l) left  += s->ch1_mix;
  if (s->ch2_r) right += s->ch2_mix;
  if (s->ch2_l) left  += s->ch2_mix;
  if (s->ch3_r) right += s->ch3_mix;
  if (s->ch3_l) left  += s->ch3_mix;
  if (s->ch4_r) right += s->ch4_mix;
  if (s->ch4_l) left  += s->ch4_mix;

  float mixed_l = left * (float)(s->vol_l + 1) * 64.0f;
  float mixed_r = right * (float)(s->vol_r + 1) * 64.0f;
  float sh_l = hp_block(mixed_l, &g_hp_prev_in_l, &g_hp_prev_out_l);
  float sh_r = hp_block(mixed_r, &g_hp_prev_in_r, &g_hp_prev_out_r);

  // Normalize both streams into the verifier's domain.
  float cn_l = (float)canon_l * kNorm;
  float cn_r = (float)canon_r * kNorm;
  float chk_l = sh_l * kNorm;
  float chk_r = sh_r * kNorm;

  ShadowJudgement j = shadow_verifier_judge(&g_verifier, cn_l, cn_r, chk_l, chk_r);
  (void)j;

  // Loud revert: log DEGRADED once per pause.
  if (g_verifier.reverted[0]) {
    fprintf(stderr, "[PSG-SHADOW][DEGRADED] reverting to canon APU mix: %s\n",
            g_verifier.reverted);
    g_verifier.reverted[0] = '\0';
    g_was_proven = false;
  }

  bool proven = shadow_verifier_proven(&g_verifier);
  if (proven && !g_was_proven) {
    fprintf(stderr, "[PSG-SHADOW] proven; substituting float PSG render "
                    "(gain %.3f)\n", (double)shadow_verifier_gain(&g_verifier));
  }
  g_was_proven = proven;

  if (proven) {
    float g = shadow_verifier_gain(&g_verifier);
    float ql = sh_l * g;
    float qr = sh_r * g;
    if (ql > 32767.0f) ql = 32767.0f; else if (ql < -32768.0f) ql = -32768.0f;
    if (qr > 32767.0f) qr = 32767.0f; else if (qr < -32768.0f) qr = -32768.0f;
    *out_l = (int16_t)lrintf(ql);
    *out_r = (int16_t)lrintf(qr);
  } else {
    // Not proven: emit the authoritative canon output.
    *out_l = canon_l;
    *out_r = canon_r;
  }
}
