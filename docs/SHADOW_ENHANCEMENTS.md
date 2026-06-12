# Shadow Audio + Screen Enhancements (Game Boy Color backport)

Backport of the gbarecomp "verified-enhancement" QoL layer to the Game Boy /
Game Boy Color recompiler core (`gb-recompiled`). All work lives on the
`feat/shadow-enhancements` branch in the isolated `_shadow_gbcrecomp` worktree
(sibling of `gb-recompiled`); it does not touch `master`, `main`, the in-flight
`feature/*`, `pr/*`, or `merge/*` branches, or any per-game repo.

## Governing principle (the carve-out)

Faithfulness is the product; these are an opt-in layer on top. The one
permitted form of HLE here is a **verified-enhancement shadow**, allowed only
when ALL hold (see `recomp-template/PRINCIPLES.md`, "Verified-Enhancement HLE
Is Allowed; Load-Bearing HLE Is Not"):

1. The emulated (canon) path keeps running and stays both the authoritative
   output and the verify oracle. The shadow is never ground truth.
2. The shadow is continuously, differentially checked against the canon stream
   and substitutes only after a proven window.
3. It reverts loudly (logs `DEGRADED`) the instant it stops matching.
4. It is opt-in and present-time, off by default; with it off the output is
   byte-identical (frame hashes / `differential.c` comparisons stay on the raw
   canon framebuffer and the canon audio samples).

Worst-case failure is "the user hears/sees the authentic hardware output," and
it cannot mask a recompiler bug because the canon path it shadows is still the
thing being diffed.

## What ports verbatim vs what is GBC-specific

| Piece | Status | Notes |
|---|---|---|
| **`ShadowVerifier`** (envelope-correlation self-check, auto-gain, prove/strike/pause) | **DONE** — `runtime/{include,src}/audio_shadow.{h,c}`, C, compiles clean (`-Wall -Wextra -Wpedantic`, 0 warnings) | Engine-agnostic; ported VERBATIM from the snesrecomp C port / gbarecomp C++ port. Only the include guard / attribution renamed. |
| Color-science core (xyY→XYZ, primaries→matrix, Bradford adaptation, sRGB OETF) | **DONE** — `runtime/{include,src}/color_lut.{h,c}` | Lifts verbatim from gbarecomp `src/runtime/color_lut.cpp`; re-expressed in C (core is C). |
| Present-path color LUT | **DONE** — wired into `platform_sdl.cpp` upload path | GBC screen is BGR555 15-bit, **same domain as GBA**, so the GBA LCD-panel model applies directly. Adapted input/output from RGB888 to the GBC framebuffer's **ARGB8888** (`uint32_t`) pixel. |
| **PSG shadow render** | **DONE** — `runtime/{include,src}/psg_shadow.{h,c}`, wired into `audio.c` mixer | GBC-specific: the "engine" is the *hardware* 4-channel PSG/APU (2 pulse, 1 wave-RAM, 1 noise), not a software driver. The enhancement is **float headroom** — the canon mixer crushes per-channel ints, applies an int master scale and a Q15 DC-blocker (double requantization); the shadow keeps the per-channel contributions in float through the master mix + a float DC-blocker. Same notes/envelopes/timing; the verifier proves structural identity before substituting. |

### Why the PSG shadow is safe (not load-bearing HLE)

The shadow does **not** re-derive any APU state. It receives the canon path's
own per-channel signed contributions (`ch1_mix..ch4_mix`), the NR51 routing
bits, and the NR50 master-volume codes, and only changes the *numeric domain*
of the final mix (float instead of int, with a matched float DC-blocker). It
therefore cannot produce a note, envelope, or routing the APU did not. If the
float mix ever diverges structurally from the canon int mix, the verifier
strikes and reverts to canon — logging `DEGRADED`.

## Integration points (file:line, on `feat/shadow-enhancements` off `master`)

### Audio (canon = `runtime/src/audio.c`)
- **Per-sample mix loop:** `gb_audio_step()` runs the 44100 Hz fixed-point
  sample loop at **`audio.c:846`**. Per-channel signed contributions
  `ch1_mix..ch4_mix` are computed at **`audio.c:854`–`903`**; NR51 routing is
  applied inline there. Master scale + DC-block at **`audio.c:911`–`917`**.
- **Canon DAC emit:** the final stereo `int16` is stored to
  `apu->last_output_left/right` and pushed via `gb_audio_callback(ctx,left,right)`
  at **`audio.c:989`** → `gbrt.c:3633` → `ctx->callbacks.on_audio_sample`.
- **Shadow wiring (added):** right after the canon `left/right` are computed
  (post-DC-block, **`audio.c:916`**), the per-channel contributions + routing +
  vol codes are packed into a `PsgShadowSample` and run through
  `psg_shadow_process()`, which returns the sample the frontend should emit
  (canon until proven, float-shadow once proven). `psg_shadow_reset()` is
  called from `gb_audio_reset()` (**`audio.c` reset**). Default OFF
  (`GBCRECOMP_PSG_SHADOW` unset) ⇒ `psg_shadow_process` returns the canon
  samples unchanged.
- **Per-channel render reference (for any future per-voice shadow):**
  `audio_channel{1,2,3,4}_pcm()` at **`audio.c:284`–`315`** are the clean
  per-channel PCM readers (duty tables `DUTY_CYCLES` at `audio.c:274`; wave RAM
  in `ch3.wave_ram`; noise LFSR in `ch4.lfsr`). The current shadow operates at
  the post-mix stage; a future revision could re-render each voice from these.

### Video (canon = `runtime/src/ppu.c`, present = `runtime/src/platform_sdl.cpp`)
- **Framebuffers:** `GBPPU` (`include/ppu.h:142`–`148`) carries three buffers:
  `framebuffer` (2-bit indices), `color_framebuffer` (**BGR555 15-bit**, the
  native hardware index — ideal LUT input), and `rgb_framebuffer`
  (**ARGB8888**, the display buffer). `rgb_framebuffer` is filled from
  `color_framebuffer` via `rgb555_to_rgba()` at **`ppu.c:656`–`662`**
  (`rgb555_to_rgba` defined at **`ppu.c:122`**: `0xFF000000 | r<<16 | g<<8 | b`).
- **Texture format:** the SDL streaming texture is `SDL_PIXELFORMAT_ARGB8888`
  (**`platform_sdl.cpp:1788`**).
- **Present/blit path:** `render_frame_internal()` at **`platform_sdl.cpp:1992`**;
  `SDL_LockTexture` at **`platform_sdl.cpp:2081`**; the framebuffer is copied
  into the locked texture `dst` at **`platform_sdl.cpp:2086`–`2105`**;
  `SDL_UnlockTexture` + `SDL_RenderCopy` at **`platform_sdl.cpp:2107`–`2114`**.
- **LUT wiring (added):** immediately before `SDL_UnlockTexture`
  (**`platform_sdl.cpp:2107`**), a lazily-built `ColorLut` (from
  `color_lut_create_from_env()`) is applied **in place** to the texture copy
  `dst` via `color_lut_map_argb8888()`. It touches only the texture COPY, never
  `rgb_framebuffer`/`color_framebuffer` and never the `differential.c` verify
  path (which diffs `color_framebuffer` / `rgb_framebuffer` at
  **`differential.c:393`,`420`**). Default OFF (`GBCRECOMP_SCREEN` unset/`raw`)
  ⇒ `color_lut_create_from_env()` returns `NULL` and the copy is untouched.

### Build
- New sources added to **`runtime/CMakeLists.txt`** `add_library(gbrt …)`:
  `src/audio_shadow.c`, `src/psg_shadow.c`, `src/color_lut.c`. The `gbrt`
  library already links `m` implicitly via the toolchain; `color_lut.c` uses
  `<math.h>` (`pow`).

## Env gating (default OFF — byte-identical)

| Env var | Default | Effect |
|---|---|---|
| `GBCRECOMP_PSG_SHADOW` | unset (OFF) | Any non-empty, non-`0` value enables the float PSG shadow (still only substitutes once the verifier proves a window). |
| `GBCRECOMP_SCREEN` | unset / `raw` (passthrough) | `{raw,unlit,frontlit,backlit,classic}` selects the present-time screen model. |
| `GBCRECOMP_SCREEN_DARKEN` | per-screen default | `[0,1]` override for the reflective-panel gamma darkening. |
| `GBCRECOMP_SCREEN_TARGET` | `srgb` | `p3` to emit Display-P3-encoded bytes. |

## Compile status

- `audio_shadow.c` — **clean** (`gcc -std=c11 -Wall -Wextra -Wpedantic -fsyntax-only`, 0 warnings).
- `color_lut.c` — **clean** (same flags).
- `psg_shadow.c` — **clean** (same flags).
- `audio.c` (with shadow wiring) — **clean** (`-Wall -Wextra`).
- `color_lut.h` C API used from a C++ TU (mirroring the `platform_sdl.cpp`
  edit) — **clean** (`g++ -std=c++17 -Wall -Wextra -fsyntax-only`).
- `platform_sdl.cpp` full TU not built standalone (pulls SDL2 + vendored ImGui +
  many project headers); the load-bearing concern — the C/C++ linkage and usage
  of the LUT API — is verified by the C++ probe above. Verify in a full game
  build during ecosystem review.

## Uncertainty / NOT fabricated

- The GBC panel primaries reused here are the **GBA** measured reflective /
  backlit gamuts from the ported color core. The GBC's original reflective
  screen is in the same BGR555 domain but its *exact* colorimeter-measured
  primaries may differ from the GBA's; the models are offered as opt-in
  approximations, not claimed as GBC-measured ground truth. `classic` and `raw`
  are exact (published gamma-4.0 / 5→8 replication). A GBC-specific measured
  profile can be dropped in later without touching the pipeline.
- The PSG shadow's float DC-blocker uses the canon's `R = 32640/32768`
  coefficient so the two streams stay envelope-aligned for the verifier; this
  matches `audio_apply_highpass()` in `audio.c`, not an invented filter.

## Next steps (for ecosystem review)

1. Build a per-game target (e.g. Tetris / Pokemon Red) against this `gbrt` and
   confirm default-OFF output is byte-identical (frame hash + audio sample
   stream), then A/B each enhancement on.
2. Optionally add a per-game TOML/config gate mirroring the env vars (so the
   enhancement can be defaulted per title once proven).
3. Optionally extend the PSG shadow to a true per-voice float re-render driven
   from `audio_channel*_pcm()` (`audio.c:284`) for higher internal rate /
   anti-aliased pulse edges, still verified against the canon mix.
4. Drop in a GBC-measured screen profile if/when colorimeter data is available.

## Attribution

`ShadowVerifier` (`audio_shadow.{h,c}`) ported from JRickey/gba-recomp
(`crates/gba-core/src/shadow.rs`) via the gbarecomp C++ port and the snesrecomp
C port, © Jrickey, MIT OR Apache-2.0, used with permission. The color-science
core (`color_lut.{h,c}`) ported from JRickey/gba-recomp
(`crates/screen/src/{color,profile,lut}.rs`) via the gbarecomp C++ port
(`src/runtime/color_lut.cpp`), © Jrickey, MIT OR Apache-2.0, used with
permission. The C ports, the ARGB8888 present path, and the GBC PSG post-mix
float shadow are ours.
