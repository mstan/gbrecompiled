# GB accuracy harness

Audio-first accuracy tooling for the GB recomp, modeled on the PSX recomp methodology.
See `../GB_ACCURACY_BURNDOWN.md` (7-axis scorecard) and `axis5_apu.md` (audio deep-dive).

## Oracle: SameBoy (Axis 5b)

`oracle/gb_audio_oracle.c` links SameBoy's portable `Core/*.c` directly and emits a
deterministic S16 stereo reference stream + a per-sample guest-cycle sidecar.

```bash
# 1. one-time: clone SameBoy next to the project
git clone https://github.com/LIJI32/SameBoy /f/Projects/SameBoy

# 2. build the oracle (needs mingw64/bin on PATH so cc1.exe finds its DLLs)
bash oracle/build_oracle.sh                       # -> oracle/gb_audio_oracle.exe

# 3. capture the SameBoy reference (skip-boot DMG, 20 s, no input)
oracle/gb_audio_oracle.exe out/pokered.gb out/oracle_pokered.s16 20 dmg

# 4. capture the recomp's stream headless (benchmark mode, deterministic from boot)
bash oracle/capture_recomp.sh \
    "/f/Projects/gbcrecomp/Pokemon Red/generated/build/Pokemon_Red_Blue.exe" \
    20 out/recomp_pokered.s16

# 5. drift-tolerant diff
python tools/audio_drift_diff.py \
    --recomp out/recomp_pokered.s16 --oracle out/oracle_pokered.s16 \
    --label pokered --json out/pokered_drift.json
```

### Hardware mode (SGB vs DMG)

The recomp emulates a Super Game Boy for SGB-enhanced DMG carts (header byte 0x146==0x03,
e.g. Pokémon Red) under its AUTO hardware mode — it runs the cart's SGB border/palette setup,
which pulses the LCD and adds ~0.8 s of boot work a DMG console/oracle skips. To compare
apples-to-apples against a DMG SameBoy oracle, force DMG with the runtime env override:

```bash
GBRT_HARDWARE_MODE=dmg   <recomp_exe> ...    # auto|dmg|sgb|cgb|gba (default auto)
```

`capture_recomp.sh` and `run_cycle_compare.sh` default `GBRT_HARDWARE_MODE=dmg` for this reason.
(Implemented in `gb-recompiled/runtime/src/gbrt.c` `gb_context_load_rom`.) For SGB-fidelity work,
set `sgb` and compare against a SameBoy `GB_MODEL_SGB2` oracle instead.

Notes:
- ROM paths with spaces/`[]` trip SameBoy's UTF-8 fopen shim — copy to a clean name first
  (`out/pokered.gb`).
- Both captures are **capture-from-boot** (never arm-then-time).
- The recomp uses a `±1×vol` mixer and point-sampling, so the diff is amplitude-normalized
  and drift-tolerant; bit-exact vs SameBoy is the aspirational GREEN gate.

## Cycle/state comparator (Axis 2)

```bash
# SameBoy per-frame cycle+state ring (skip-boot, 500 frames)
oracle/gb_state_oracle.exe out/pokered.gb out/oracle_state_pokered.csv 500 dmg

# launch the windowed recomp (debug server :4370), pull its frame ring, diff:
bash oracle/run_cycle_compare.sh \
    "/f/Projects/gbcrecomp/Pokemon Red/generated/build/Pokemon_Red_Blue.exe" \
    out/oracle_state_pokered.csv 500 pokered out/recomp_state_pokered.csv
# or offline, if you already saved the recomp ring:
python tools/gb_cycle_compare.py --oracle out/oracle_state_pokered.csv \
    --recomp-csv out/recomp_state_pokered.csv --label pokered
```

Reports steady cycle pacing (is it a clock bug?), the LCD-off/odd-frame locus (where the
two diverge), and a cycle-aligned game-progress lag. Note: the recomp's debug server only
comes up on the **windowed** path (benchmark/headless mode skips it), so this capture runs
the recomp with a window. The recomp frame ring is always-on (36000 frames) — query it.

## Metrics (`tools/audio_drift_diff.py`)

- **log-mel cosine** — drift-tolerant similarity headline (timbre-aware, phase-invariant).
- **onset-envelope cross-correlation** + lag — structural/rhythmic alignment.
- **onset-timing histogram** — matched-onset Δms (rhythm fidelity).
- **per-note pitch error (cents)** — dominant-bin estimator, harmonic-aware upgrade TODO.
- raw-waveform seg-corr — footnote only (≈0 when timbres differ; not a quality signal).
