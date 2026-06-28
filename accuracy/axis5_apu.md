# Axis 5b — Audio / APU deep-dive

Our impl: `runtime/src/audio.c` (1020 lines), `runtime/include/audio.h`.
Oracle: SameBoy `Core/apu.c`. Spec: Pan Docs "Audio". Status: **APPROXIMATE.**

## §1 What our impl does

Hand-written APU (NOT borrowed from Peanut-GB; the Peanut-GB copy is only used by the
TCP oracle tooling). All four channels modeled with per-channel structs
(`audio.c:109-185`) in one `GBAudio` (`audio.c:187-220`).

**Implemented & largely faithful:**
- **Frame sequencer clocked off the DIV falling edge** — the hardware-accurate method
  (`audio.c:993-1015`, `gb_audio_div_tick`), walking DIV bit 12 (bit 13 in double-speed),
  incl. DIV-reset edge handling. 512 Hz sequencer; length @ 256 Hz (steps 0/2/4/6,
  `audio.c:360-378`), envelope @ 64 Hz (step 7, `audio.c:404-409`), sweep @ 128 Hz.
- **CH1/CH2 square**: duty table (`audio.c:274-279`), period `(2048-freq)*4`, 8-step duty
  walk (`audio.c:766-789`).
- **CH3 wave**: 16-byte wave RAM, period `(2048-freq)*2`, 32-step walk, volume
  0/1/2/3→mute/100/50/25% (`audio.c:791-802,884-890`).
- **CH4 noise**: 15-bit LFSR, 7-bit width via NR43 bit 3, divisor table, reload 0x7FFF on
  trigger (`audio.c:805-836,745`); step loop capped at LFSR period (valid optimization).

## §2 Discrepancies (our_file:line vs behavior)

1. **Per-instruction sync, NOT sample-accurate** (P0). `gb_audio_step(ctx, system_cycles)`
   advances channels by a whole instruction's cycles then emits all samples for that window
   (`audio.c:757,842-847`; `SAMPLE_PERIOD_FIXED=6233460` Q16.16 = 95.108 cyc/sample). Register
   writes are observed only at instruction boundaries → the exact sample at which a write
   takes effect can be off by up to one instruction. **Dominant source of fine-grained
   drift vs SameBoy** (matches the measured ~26 ms onset jitter, 2026-06-28 run).
2. **No band-limited resample** (P0). APU generates directly at 44.1 kHz, point-sampled
   (`audio.c:842`); the native ~1.048 MHz stream is never produced. Aliasing on high-freq
   square/noise content vs SameBoy's band-limited output.
3. **`±1×vol` bipolar mixer** (P1). Output stage treats each channel as `DUTY?+1:-1` × volume
   (`audio.c:862`), ignoring the 4-bit DAC. Master volume `vol*(vol_l+1)*64` with a hand-tuned
   `64` (`audio.c:906-915`), then a Q15 one-pole DC blocker (`audio.c:412-421`). A *second,
   correct* digital model exists only in the CGB PCM helpers (`audio.c:284-315`) — two
   inconsistent output models, and the wrong one reaches the speakers. Makes absolute
   amplitude non-comparable (recomp peak 30217 vs oracle 4998 on the same music) → diffs
   MUST be DC+amplitude normalized.
4. **CH3 wave-RAM write-while-enabled blocked** (P2) (`audio.c:599`) instead of the DMG
   "write to the byte currently being read" quirk; trigger wave-position reset differs
   (`audio.c:716`) — misses the DMG first-sample timing quirk.
5. **CH1 sweep shift=0** skips the mandatory overflow check that hardware still performs
   each tick (`audio.c:389`) (P3).
6. **Known glitching** (P1): CLAUDE.md "Audio — General glitching"; CHANGELOG "(glitchy)".
   Underrun handling fades last sample to zero (`platform_sdl.cpp:4342-4375`); producer drops
   on buffer-full. (Playback-path only; the deterministic capture path is unaffected.)

## §3 Prioritized fix list

- **P0** Sample-accurate register-write timing: step the APU to the exact sub-instruction
  cycle of each NRxx write (gate on the Axis-2 per-instruction tick lever).
- **P0** Band-limited resample (blip-buf / sinc) replacing point-sampling.
- **P1** Unify the mixer on the 4-bit DAC digital model (delete the `±1×vol` path; keep the
  `*_pcm` model); calibrate level to SameBoy.
- **P2** CH3 wave-RAM read/write quirk; trigger timing quirks.
- **P3** CH1 sweep shift=0 overflow check.

## §4 Validation method (the GREEN gate for this axis)

1. **Static cross-ref** each fix against Pan Docs + SameBoy `Core/apu.c`.
2. **Runtime oracle**: drift-tolerant sample-stream diff vs SameBoy
   (`accuracy/tools/audio_drift_diff.py`) — log-mel cosine + onset histogram + (harmonic-aware)
   pitch error. Track the metrics in the burndown's "First audio comparison" table across fixes:
   onset jitter and alignment lag should fall toward 0 as P0 lands; spectral cosine toward 1.0
   as the mixer/resample P1/P0 land.
3. **Aspirational GREEN**: sample-exact PCM vs SameBoy from identical input (reachable in
   principle since both are deterministic and sample-accurate once P0 lands).
4. **Regression guard**: a unit test pinning the frame-sequencer DIV-edge stepping
   (`gb_audio_div_tick`), the one structurally-faithful part, across DIV-reset edges.

## §5 Oracle & capture (stood up 2026-06-28)

- **Oracle**: `accuracy/oracle/gb_audio_oracle.c` links SameBoy `Core/*.c` directly
  (`build_oracle.sh`; the upstream Makefile is clang/`--target`-only on Windows). Skip-boot to
  the game's 0x100 entry (post-boot DMG regs + `boot_rom_finished`), so it starts where the
  recomp starts — no boot chime. Emits S16 stereo @ 44.1 kHz + a per-sample guest-cycle sidecar
  (accumulated `GB_run()` 8 MHz ticks — the GB analog of the PSX Beetle guest-cycle hook).
- **Recomp capture**: `accuracy/oracle/capture_recomp.sh` runs the prebuilt recomp headless
  (`GBRECOMP_BENCHMARK=1`, dummy SDL drivers) with `--debug-audio` → `debug_audio.raw`
  (the always-on sample chokepoint at `audio.c:918`, dumped from boot).
- Both are capture-from-boot (doctrine-compliant: never arm-then-time).
- **Model-match:** SGB-enhanced carts run the recomp's SGB engine under AUTO; the harness forces
  `GBRT_HARDWARE_MODE=dmg` so the comparison matches the DMG oracle (see burndown SGB section).

## §6 Per-channel comparison (active)

The mixed-stream cosine (~0.95) does not localize error. Per-channel tap:
- **Recomp:** emit CH1–4 pre-mix digital output (0–15 scaled) alongside the mix, from the chokepoint
  in `gb_audio_step`/`gb_audio_callback` (`runtime/src/audio.c:852-919`) gated by a debug flag.
- **Oracle (SameBoy):** read each channel's current output in the APU sample callback
  (`Core/apu.c` per-channel `current_sample`/DAC), one column per channel.
- **Diff:** run `audio_drift_diff.py` per channel → a cosine/onset/pitch row per CH1–4, so the
  aggregate 0.95 decomposes (e.g. CH1 0.99 / CH3 0.97 / CH4 0.6) and the worst channel is fixed
  first. Drift-tolerant per channel (amplitude-normalized); CH4 (noise) uses energy-envelope, not pitch.
