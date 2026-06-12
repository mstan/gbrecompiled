// psg_shadow.h — verified-enhancement shadow for the Game Boy 4-channel PSG.
//
// A higher-fidelity FLOAT re-render of the APU's per-channel output that runs
// ALONGSIDE the canon integer mixer in audio.c, is continuously diffed against
// it by the engine-agnostic ShadowVerifier, and substitutes for the canon
// stream ONLY after a proven window — reverting loudly (logs DEGRADED) the
// instant correlation breaks. Default OFF (GBCRECOMP_PSG_SHADOW unset/0) =>
// output is byte-identical to the canon mixer; the canon path always runs and
// remains the verify oracle. See PRINCIPLES.md "Verified-Enhancement HLE Is
// Allowed; Load-Bearing HLE Is Not".
//
// WHAT IT FIXES (vs the canon path in audio.c gb_audio_step):
//   - the canon mixer sums tiny integer per-channel values (each channel is
//     volume*±1 etc.) then applies an integer master-volume scale and a Q15
//     DC-blocker, requantizing twice; the shadow keeps the per-channel
//     contributions in float through the master mix, so the DAC step
//     quantization error is removed. Same notes, same envelopes, same timing —
//     just without the integer crush. The verifier proves this is structurally
//     identical before any substitution.
//
// The canon mixer's per-channel state and DAC decisions are reused verbatim;
// the shadow ONLY changes the numeric domain of the final mix, so it cannot
// invent behavior the APU did not produce.
//
// Verifier: audio_shadow.{h,c} (engine-agnostic, ported verbatim).

#ifndef GBCRECOMP_PSG_SHADOW_H
#define GBCRECOMP_PSG_SHADOW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Read the GBCRECOMP_PSG_SHADOW env var once. Default OFF.
bool psg_shadow_enabled(void);

// Per-sample contributions handed in by the canon mixer (audio.c). Each *_mix
// is the channel's signed pre-master contribution exactly as the canon path
// computed it; *_pan_l/r are the NR51 routing bits for that channel; vol_l/r
// are the NR50 master volume codes (0..7). These are enough to reproduce the
// canon mix in float without re-deriving any APU state.
typedef struct {
  float ch1_mix, ch2_mix, ch3_mix, ch4_mix;
  bool  ch1_l, ch1_r, ch2_l, ch2_r, ch3_l, ch3_r, ch4_l, ch4_r;
  int   vol_l, vol_r;  // NR50 left/right volume codes (0..7)
} PsgShadowSample;

// Feed one 44100 Hz grid sample. `canon_l/r` are the FINAL canon int16 output
// (post master-scale + DC block) for this sample. `s` carries the per-channel
// float contributions so the shadow can build its cleaner mix. On return,
// *out_l/*out_r hold the sample the FRONTEND should emit: the canon value when
// the shadow is not (yet) proven, or the shadow's float mix (quantized) once
// proven. When the shadow reverts, a DEGRADED line is logged once.
//
// With the shadow disabled this is a no-op that returns the canon samples
// unchanged (byte-identical).
void psg_shadow_process(const PsgShadowSample* s, int16_t canon_l,
                        int16_t canon_r, int16_t* out_l, int16_t* out_r);

// Reset verifier + DC-blocker state (call on APU reset).
void psg_shadow_reset(void);

#ifdef __cplusplus
}
#endif

#endif  // GBCRECOMP_PSG_SHADOW_H
