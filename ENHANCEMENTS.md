# Enhancements — opt-in QoL layers and follow-up work

This tracks **opt-in, present-time enhancement layers** (faithfulness stays the
product; these sit on top, default OFF) and their perceived value. Governing
principle: *verified-enhancement HLE* only — the canon path stays authoritative
and the verify oracle, the shadow reverts loudly (`DEGRADED`) the instant it
stops matching, and with the layer off the output is byte-identical.
See `docs/SHADOW_ENHANCEMENTS.md` for the full design.

## Shipped (in master @ c5d6b1a) — the "shadow" layer

Merged 2026-07-02. Additive files + small hooks into `audio.c` and the GL
present path in `platform_sdl.cpp`. Default OFF ⇒ byte-identical.

| Enhancement | Env gate | What it does | Perceived value |
|---|---|---|---|
| **PSG float audio shadow** | `GBCRECOMP_PSG_SHADOW=1` | Keeps per-channel APU contributions in float through the master mix + a matched float DC-blocker (canon crushes to int with double requantization). Same notes/envelopes/timing; verifier proves structural identity before substituting. | **Low observed.** A/B on MMX2 was not audibly distinguishable. No regression. Kept for headroom on quieter/denser mixes and as the base for the per-voice re-render below. |
| **Screen color LUT** | `GBCRECOMP_SCREEN={raw,unlit,frontlit,backlit,classic}` (+ `GBCRECOMP_SCREEN_DARKEN`, `GBCRECOMP_SCREEN_TARGET=srgb\|p3`) | LCD-panel color model (primaries → Bradford adaptation → sRGB OETF) applied to the GBC BGR555→ARGB8888 frame. | **Low observed.** A/B on MMX2 (`backlit`) was not obviously different from `raw`. No regression. Value is authenticity/personal-taste, not a fidelity fix. |

### Integration note (important for future edits)
The shadow branch predated master's renderer rewrite (SDL_Renderer → **OpenGL +
shader pipeline**). The color LUT was therefore **re-ported** into the GL upload
path, NOT taken from the branch: it maps the ARGB `framebuffer` into a scratch
buffer that feeds the existing ARGB→RGBA repack in `platform_sdl.cpp`
(`present`/upload region), immediately before `glTexSubImage2D`. It never
touches the PPU framebuffer or the `differential.c` verify path.

**Interaction gotcha:** the present path already has independent per-pixel
color-correction matrices (`cc_gbc`/`cc_gba`, gated by the in-app
`g_color_correction` UI setting). With BOTH the LUT and a cc matrix on they
compound. The LUT alone is the intended path — leave color-correction off when
using `GBCRECOMP_SCREEN`.

## Follow-up enhancement work (not started) — with perceived value

1. **Per-voice float PSG re-render** — re-render each of the 4 voices from the
   clean per-channel PCM readers (`audio.c` `audio_channel{1..4}_pcm`) at a
   higher internal rate with anti-aliased pulse edges, still verified against
   the canon mix. **Value: medium** — this is where audible improvement (less
   aliasing on high-pitch pulses) would actually come from; the current post-mix
   float shadow was inaudible, so this is the real lever if audio polish is ever
   wanted.
2. **GBC-measured screen color profile** — the current LUT reuses the GBA's
   measured reflective/backlit primaries as an approximation (same BGR555
   domain, but the GBC panel's exact colorimetry may differ). Drop in a
   GBC-measured profile if/when colorimeter data exists. **Value: low** — only
   matters if the color layer is used at all, which the A/B suggests is marginal.
3. **Per-game TOML/config gate** — mirror the env vars in the per-game TOML so a
   proven enhancement can be defaulted per title. **Value: low-medium** — only
   worth it once a specific game+model combo is judged clearly better.

## Bottom line

The shadow layer is in, costs nothing when off, and can't regress faithfulness.
Observed benefit on MMX2 was negligible — so treat these as **optional polish
knobs**, not priorities. If audio polish is ever pursued, item (1) is the only
one likely to be audible.
