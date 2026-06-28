# GB / GBC Accuracy Burndown (living doc)

Companion scorecard for the GB-Recomp static recompiler, modeled axis-for-axis on
the PSX recomp methodology (`F:\Projects\psxrecomp\_wt-tomba2\psxrecomp\ACCURACY_BURNDOWN.md`).
This is the thin all-axes scorecard. Active deep-dives live under `accuracy/`.

- **Oracle:** SameBoy (T-cycle/sub-instruction accurate, MIT, embeddable `libsameboy`).
- **Cross-reference shelf:** Pan Docs, gbdev mooneye-test-suite / blargg / dmg-acid2 /
  Mealybug Tearoom, SameBoy `Core/` source.
- **Working branch:** `accuracy/discovery` (worktree `F:\Projects\gbcrecomp\_wt-accuracy`).
- **First active axis:** Axis 5 — Audio (APU), drift-tolerant sample-stream diff vs SameBoy.

---

## ⚠️ LESSON (carried from PSX): oracle-validate OUTPUT before applying a fix

A research-claimed or comment-claimed discrepancy is a **HYPOTHESIS, not a bug.**
Before changing code, reproduce the divergence as an **output diff vs SameBoy on the
same input.** "Matches Pan Docs convention" or "matches SameBoy's source structure"
does **not** justify rewriting code whose *output* is already correct. Self-agreement
(generated code == our interpreter, via `runtime/src/differential.c`) proves backend
equivalence, **NOT** correctness — both can be identically wrong.

---

## Method (non-negotiable)

An item flips to **GREEN** only when it satisfies **both** legs:

1. **Static cross-reference** against at least one reference-shelf item
   (Pan Docs section, a mooneye/blargg/Mealybug test ROM, or SameBoy `Core/*` source).
2. **Runtime oracle validation** = a first-divergence ring-buffer diff against SameBoy
   on the same input: framebuffer/VRAM diff for PPU, **audio sample-stream diff for APU**,
   accumulated `GB_run()` 8 MHz ticks for cycle timing, register/state blob
   (`GB_save_state_to_buffer`) for memory/CPU divergence.

Self-agreement is **not sufficient**. The project already has a generated-vs-interpreter
oracle (`differential.c`) — that is leg-0 (backend equivalence), never a substitute for
leg 2 (an *external accurate oracle*).

**Always-on ring buffers, never arm-then-capture.** Probes QUERY a continuously-recording
ring for the window of interest; they never attach-then-record. The recomp's existing
`--debug-audio` path (`runtime/src/audio.c:969-988`) captures every emitted sample from
process start — capture-from-boot, not arm-at-probe-time, so it is doctrine-compliant.
The in-memory query ring (this branch) makes the same stream live-queryable.

---

## Comparative sources (the reference shelf)

| Source | Role |
|---|---|
| **Pan Docs** (gbdev.io/pandocs) | Canonical hardware spec — cite the section. |
| **SameBoy `Core/`** (MIT) | Second-opinion accurate implementation; the runtime oracle. |
| **mooneye-test-suite / blargg / dmg-acid2 / Mealybug Tearoom** | Hardware-derived ground-truth ROMs (above any emulator). |
| **GB Emulator Shootout** (gbdev.io/GBEmulatorShootout) | Cross-emulator test-ROM grid. |
| `runtime/src/differential.c` | Leg-0 backend-equivalence oracle (gen == interp). |
| `tools/peanut-gb-debug/` (port 4371) | Frame-level state oracle — **NOT** cycle-accurate. |
| `tools/*ground_truth*` (PyBoy) | Coverage discovery only — no state compare. |

---

## Validation infrastructure to BUILD (prerequisite tooling)

- [x] **SameBoy audio oracle** — `accuracy/oracle/gb_audio_oracle.c` links `libsameboy`,
      pins model/sample-rate, `GB_apu_set_sample_callback`, accumulates `GB_run()` ticks
      → S16 stereo reference stream + per-sample guest-cycle timestamps. *(this branch)*
- [x] **Drift-tolerant audio diff** — `accuracy/tools/audio_drift_diff.py`:
      DC+amplitude normalize → cross-correlation lag alignment → onset-timing histogram
      → per-note pitch error. *(this branch)*
- [x] **Always-on recomp audio ring** — in-memory S16 query ring at the mix chokepoint
      (`runtime/src/audio.c:918`, `gb_audio_callback`). *(this branch)*
- [ ] **SameBoy state/divergence oracle** — `GB_save_state_to_buffer` blob diff +
      `GB_set_execution_callback` instruction-granular first-divergence (Axis 1/3/4/6).
- [x] **SameBoy cycle/state oracle + comparator (Axis 2)** — `accuracy/oracle/gb_state_oracle.c`
      (per-frame T-cycle + CPU/PPU state ring at each VBlank) vs the recomp's always-on
      `frame_timeseries` ring (debug server :4370), diffed by `accuracy/tools/gb_cycle_compare.py`
      (steady pacing, LCD-off/odd-frame locus, cycle-aligned progress lag). *(this branch)*
- [ ] **Always-on PPU framebuffer ring** + VRAM diff vs SameBoy (Axis 5 video).

---

## Axis status table (verdict · gap · lever)

| # | Axis | Verdict | Gap | Lever |
|---|------|---------|-----|-------|
| 1 | Instruction semantics | **instruction-accurate** | `oam_bug` FAIL; `(HL)`-as-magic-index-6 operand design already caused a real `AND (HL)` miscompile | Typed operands (audit Phase 3); emitter golden tests for `(HL)` RMW + AF push/pop masking |
| 2 | Cycle / timing | **block-batched (approximate)**; control-flow instruction-accurate | `gb_tick` batched at block boundaries → timer/DMA/IME side-effects land late; most mooneye `*_timing` FAIL | Per-instruction `gb_tick()`; re-batch only timing-inert blocks (audit Phase 4) |
| 3 | Interrupt / event timing | **instruction-accurate dispatch; block-granular delivery** | Functions can't be interrupted mid-block; T-cycle PPU-IRQ tests FAIL; dead `ime_scheduled` write | Same per-instruction tick; tighten block granularity in IRQ-sensitive regions |
| 4 | Memory / MMIO | **instruction-accurate (mostly)** | `unused_hwio` FAIL (undocumented CGB bit masking); IO trace is doc-recipe, not always-on ring | Mask unused HWIO bits; promote IO trace to always-on ring |
| 5a | Video / PPU timing | **scanline-accurate (with a live bug)** | Computed variable mode-3/0 durations are **never used** — hardcoded 172/204 instead, so scanlines don't sum (`ppu.c:791-820` vs `:817`); no sub-scanline effects | Wire `scanline_draw_cycles`/`scanline_hblank_cycles` into mode transitions; move toward per-dot rendering |
| 5b | **Audio / APU** ← ACTIVE | **approximate** (per-instruction sync, no band-limiting, `±1×vol` mixer) | Samples emitted in per-instruction bursts; point-sampled at 44.1 kHz (aliasing); two inconsistent channel-output models; known glitching | Sample-accurate register-write timing; band-limited resample; unify mixer on the 4-bit DAC model |
| 6 | Static-vs-dynamic fidelity | **approximate** (static compile + universal interp fallback; **no JIT tier**) | RAM/SMC runs at full interpreter cost; analyzer CFG edges drop bank info | Implement planned sljit middle tier; make CFG edges full `(bank,addr)` (audit Phase 5) |
| 7 | Determinism | **deterministic except MBC3 RTC** | RTC reads host `time(NULL)` (`gbrt.c:570,598`); audio/frame pacing host-driven (SDL) | Injectable/seeded RTC clock for reproducible RTC games |

---

## Axis 1 — Instruction semantics (SM83 / LR35902)        Status: STRONG (instruction-accurate)

Decoder covers ~500 opcodes incl. all 11 illegal (`recompiler/src/decoder.cpp:826-834`).
Split CPU: static emitter `recompiler/src/codegen/c_emitter.cpp` (compiled path) +
`runtime/src/interpreter.c` (fallback). Both must agree (`differential.c`).

- [ ] DAA correctness — Pan Docs §DAA; cross-validate vs SameBoy on full operand sweep.
      *(unit logic present `gbrt.c:2393-2409`; blargg `daa` PASS — needs oracle leg)*
- [ ] HALT bug — Pan Docs §HALT; modeled both paths (`interpreter.c:307-315`,
      `c_emitter.cpp:1937-1961`, refetch `gbrt.c:3756`) — needs SameBoy state diff.
- [ ] `(HL)` RMW + AF push/pop masking — emitter golden test; magic-index-6 operand
      design caused a real `AND (HL)` miscompile (`RECOMPILER_CORRECTNESS_AUDIT_PLAN.md:36-48`).
- [ ] `oam_bug` — mooneye `oam_bug/*` (currently FAIL).

---

## Axis 2 — Cycle / timing  ← root cause of most timing-test failures        Status: APPROXIMATE (block-batched)

Per-opcode cycle table with taken/not-taken split (`decoder.cpp:59,236-237,645-646`);
`gb_tick(ctx,N)` emitted **per basic block** (`c_emitter.cpp:585-591`), branches tick
individually. T-states at 4 MHz, double-speed aware (`gbrt.c:3503-3524`).

- [ ] Per-instruction tick — cross-ref mooneye `*_timing`; validate via Δcycle comparator
      vs SameBoy (`GB_run()` 8 MHz ticks). Block-batching makes side-effects land late.
- [ ] `mem_timing` / `mem_timing-2` (currently FAIL).
- [ ] Build a GB `cycle_compare` — anchor PC on both sides, free-run, diff **Δcycles**
      (offset-independent), GB analog of PSX `tools/cycle_compare.py`.

---

## Axis 3 — Interrupt / event timing        Status: PARTIAL (instruction-accurate dispatch)

`gb_handle_interrupts()` priority-selects IF&IE&0x1F, vectors 0x40-0x60, 5-M-cycle ISR
penalty (`gbrt.c:3610-3649`); EI 1-instr delay via `ime_pending` (`gbrt.c:3607`).
mooneye `ei_timing`/`di_timing`/`reti_intr_timing`/`stat_irq_blocking` PASS.

- [ ] Mid-block interruptibility — functions only interrupt at block return
      (`c_emitter.cpp:2949`); `intr_2_mode*_timing` FAIL — needs per-instruction tick.
- [ ] Delete dead `ime_scheduled` write (`c_emitter.cpp:520`).

---

## Axis 4 — Memory map / MMIO        Status: MODERATE-STRONG

Full FF00-FF7F dispatch with per-register masking (`gbrt.c:1756-1827`); echo RAM
(`gbrt.c:1748`); prohibited 0xFEA0-0xFF00→0xFF; OAM mode-2/3 read-block; MBC1/2/3/5/7+HuC1,
RTC, MBC1 0x20/40/60 quirk, MBC2 512×4 echo.

- [ ] Undocumented CGB/unused-HWIO bit masking — `unused_hwio-GS/-C` FAIL (`GBC.md:25,30-31`).
- [ ] Promote `IO_TRACING.md` CSV recipe to an **always-on MMIO ring**; diff vs SameBoy.

---

## Axis 5 — Peripherals / devices        Status: MIXED

### 5a — Video / PPU        scanline-accurate (live bug)

`ppu_tick()` walks modes 0/1/2/3 (`ppu.c:773-860`); LY/LYC + rising-edge STAT IRQ
blocking; registers latched at mode-3 start (scanline-granular).

- [ ] **BUG:** variable mode-3/mode-0 durations computed (`ppu.c:791-820`) but never used —
      `ppu.c:817` still compares the hardcoded `CYCLES_PIXEL_DRAW`. Scanlines don't sum.
      Cross-ref Mealybug; validate framebuffer diff vs SameBoy.
- [ ] Sub-scanline / mid-line effects (SCX/SCY/window mid-line) — not modeled.

### 5b — Audio / APU  ← FIRST ACTIVE SLICE        Status: APPROXIMATE

Hand-written APU in `runtime/src/audio.c` (1020 lines); 4 channels modeled.
Frame sequencer clocked off DIV falling edge (`audio.c:993-1015`) — hardware-accurate method.
See `accuracy/axis5_apu.md` for the full deep-dive. Key defects:

- [ ] **Per-instruction sync, not sample-accurate** — `gb_audio_step` emits samples in
      bursts per instruction (`audio.c:757,842-847`); register writes observed only at
      instruction boundaries → dominant fine-grained drift vs SameBoy.
- [ ] **No band-limited resample** — generates directly at 44.1 kHz, point-sampled
      (`audio.c:842`); aliasing on high-freq square/noise vs SameBoy.
- [ ] **`±1×vol` bipolar mixer** (`audio.c:862,906-915`) ignores the 4-bit DAC; a second,
      *correct* digital model exists only in the CGB PCM helpers (`audio.c:284-315`) —
      two inconsistent output models. Diff must be DC+amplitude-normalized.
- [ ] CH3 wave-RAM-write-while-enabled blocked (`audio.c:599`) vs DMG quirk; CH1 sweep
      shift=0 skips mandatory overflow check (`audio.c:389`).
- [ ] Known glitching (CLAUDE.md "Audio — General glitching"; CHANGELOG "(glitchy)").
- [x] **Always-on output ring + sample-stream diff vs SameBoy** — first comparison: see
      "First audio comparison" below.

### 5c — Serial / Joypad / Timer

- [ ] Timer (DIV/TIMA/TMA/TAC) cycle behavior — mooneye `timer/*`; functional PASS,
      cycle-exact needs Axis-2 tick.
- [ ] Serial transfer timing — not yet oracle-validated.

---

## Axis 6 — Static-vs-dynamic fidelity (recompiler-unique)        Status: APPROXIMATE

Static compile + universal `gb_interpret()` fallback for any uncompiled addr incl.
RAM/HRAM (`interpreter.c:106,186-195`); per-page dispatcher → interpret; `JP HL`→
`gbrt_jump_hl`; RST jump tables statically extracted (`analyzer.cpp:171-318`); cross-bank
CALL returns to dispatcher resolving by `ctx->rom_bank`.

- [ ] **No JIT tier** — only a profile-guided entry-point harvest loop (`config.cpp:174,284`);
      RAM/SMC runs at full interpreter cost. Implement planned sljit middle tier.
- [ ] CFG successors drop bank info (`analyzer.h` `vector<uint16_t>`) — make edges
      `(bank,addr)` (audit Phase 5).
- [ ] Leg-0 only today: `differential.c` proves gen==interp. Add external SameBoy leg.

---

## Axis 7 — Determinism        Status: SOFT SPOT (one source)

Core CPU/PPU/timer/APU are cycle-counter driven; no `rand()` in `gbrt.c`; differential
harness supports scripted frame/cycle-indexed input replay (`differential.c:143-241`);
save/load covers full ctx.

- [ ] **MBC3 RTC reads host `time(NULL)`** (`gbrt.c:570,598`) — sole core nondeterminism.
      Add injectable/seeded RTC clock.
- [ ] Audio output + frame pacing depend on host SDL, not the deterministic core —
      acceptable for playback; for oracle runs, capture from the deterministic core
      (the `--debug-audio` path) rather than the SDL output ring.

---

## First audio comparison (Axis 5b) — 2026-06-28

**Pokémon Red, 20 s from game entry, no input, DMG model.** Oracle = SameBoy (skip-boot,
44.1 kHz, S16 stereo); recomp = prebuilt `Pokemon_Red_Blue.exe` headless
(`GBRECOMP_BENCHMARK=1 --debug-audio --debug-audio-seconds 20` → `debug_audio.raw`).
Diff = `accuracy/tools/audio_drift_diff.py`. Both streams captured deterministically
from t=0 (capture-from-boot, not arm-at-probe-time).

| Metric | Value | Reading |
|---|---|---|
| **Spectral similarity** (log-mel cosine, drift-tolerant headline) | **mean 0.921, p50 0.939, p10 0.831** | Same music; ~92% spectrally close through different DAC models. |
| Onset-envelope cross-correlation peak | **0.817** | Strong rhythmic/structural agreement. |
| Alignment lag (recomp vs oracle) | **−824 ms** | Recomp reaches audio-engine init ~0.8 s later; removed before similarity. |
| Onset timing | **62 % matched, |Δ|mean 26 ms** (p10/p90 ±46 ms) | Beats line up but with audible jitter. |
| Raw-waveform seg-corr | −0.30 (footnote) | Near-zero **expected** — recomp ±1×vol square vs SameBoy DAC differ in timbre, not tune. |
| Per-note pitch error (HPS f0) | **|median| 4.3 c, within-50c 69 %** | Recomp plays **in tune** with SameBoy where pitched; residual p90 is noise-channel/octave frames. |

### Harness validation (multi-ROM + negative control)

The metric discriminates matched from mismatched audio by a wide margin — the matched
scores are real signal, not a metric that always reads high:

| Metric | Red (matched) | Blue (matched) | Red-vs-Tetris (neg. ctrl) |
|---|---|---|---|
| Spectral log-mel cosine | 0.921 | **0.945** | **0.632** |
| Onset-env xcorr peak | 0.817 | **0.911** | **0.091** |
| Onsets matched | 62 % | **96 %** | **17 %** |
| Pitch within-50c | 69 % | 65 % | **0 %** |
| Pitch |median| cents | 4.3 | 4.4 | **1568** |
| Alignment lag | −824 ms | **+12 ms** | −7338 ms (spurious) |

Blue aligns at +12 ms while Red lags −824 ms — a real per-title startup-timing
difference to chase (Axis 2/5b). Artifacts: `accuracy/out/{blue,negctrl}_drift.json`.

### Confirmed divergence: Red intro runs ~49 guest-frames long (2026-06-28)

Probed with `accuracy/tools/_lag_probe.py`:
- Red: recomp first-audio **6.304 s** vs oracle **5.480 s** = **+824 ms (~49 frames @ 59.7 Hz)**;
  the −824 ms onset xcorr peak (0.815) dominates the runner-up (0.566), so it is **not** loop
  aliasing — it is real.
- Blue: recomp **5.457 s** vs oracle **5.480 s** — aligned. Oracle starts Red and Blue at the
  *same* 5.480 s (identical intro), so the divergence is **recomp-side and Red-specific**.
- **Not** interpreter-fallback volume: Blue logs **14 122** fallbacks (15× Red's 917) yet aligns;
  Red's 917 don't explain it. Both interpret the per-frame bank-6 audio routine (Red `0x4E3A`,
  Blue `0x4E44`) — normal.
- **Conclusion:** Red's recomp executes the intro (copyright→Game Freak→title) for ~49 extra
  guest-frames before triggering title music — a guest-execution/timing divergence, not a
  capture artifact. **Root-cause needs first-divergence cycle/state attribution → Axis-2 cycle
  comparator (below).** Per RULE 0, the gating intro wait-loop is a Ghidra target once the
  comparator pins the divergence frame.

### ⚠️ RECLASSIFIED: the LCD-off frames are SGB code, and the oracle model was wrong (2026-06-28)

Resolving the LCD-off frames against the **pret/pokered disassembly** (authoritative source;
RULE-0-compatible) changed the diagnosis fundamentally:

- The intro busy-loop (bank `0x1C` `0x614D`) and the LCD-off frames (`0x618C`, bank-0 `DisableLCD.wait`
  `0x006B`) are **Super Game Boy code**: `CheckSGB`, `SendSGBPackets`, `CopyGfxToSuperNintendoVRAM`
  (→ `DisableLCD`), `Wait7000` (palettes.asm:455+). SGB packet transmission **pulses the LCD** — that
  IS the LCD-off at frames 54/59/64. **A real SGB does this; it is not a PPU bug.**
- `CheckSGB` decides SGB-presence purely from `rJOYP` (0xFF00) bits 0-1 after the packet handshake.
  The recomp returns SGB responses via `gb_sgb_modify_joyp_read` (`runtime/src/gbrt.c:1765`) — it has a
  **full SGB engine** (`runtime/src/sgb.c`, `GBSgbState`). Its AUTO hardware mode runs **DMG carts that
  advertise SGB support (byte 0x146==0x03) as DMG + SGB-engine-ON** (`runtime/include/gbrt.h:38-53`).
  Pokémon Red/Blue are exactly that; the run log confirms `[SGB] enabled` and both execute the SGB
  code at bank `0x1C` `0x614E/0x6153`.
- **Therefore: no `ppu.c` LCD-off bug.** The Red audio "−824 ms lag" was measured against a **DMG**
  SameBoy oracle while the recomp ran as **SGB** — an **oracle-model mismatch in the harness**, not a
  recomp defect. Reading the disassembly (RULE 0) prevented "fixing" correct SGB behavior.

**RESOLVED (2026-06-28).** Added a configurable runtime override `GBRT_HARDWARE_MODE`
(`auto|dmg|sgb|cgb|gba`, default AUTO preserved) in `gb_context_load_rom` (`runtime/src/gbrt.c`;
works headless). Forcing **DMG** on Red collapses the lag: first-audio 6.304 s → **5.457 s**
(oracle 5.480 s), lag −824 ms → **+11.6 ms**, onset xcorr 0.817 → **0.910**, spectral cosine
0.921 → **0.945**, onsets matched 62 % → **96 %** — i.e. forced-DMG Red now matches the DMG
oracle (and Blue's profile). **The entire −824 ms was the SGB border/palette setup.** The
accuracy harness now defaults `GBRT_HARDWARE_MODE=dmg` (`capture_recomp.sh`,
`run_cycle_compare.sh`) so SGB carts are compared apples-to-apples against the DMG oracle.

**Secondary observation (not blocking):** Red and Blue are both SGB-flagged (byte 0x146==0x03),
yet under AUTO the recomp runs the SGB setup for Red but **not** Blue (Blue aligns in both AUTO
and DMG, no `[SGB] enabled`). So the recomp's SGB path is inconsistent across carts — a real
SGB-fidelity gap, separate from the (now-clean) DMG baseline. Tracking under Axis 5 (peripherals).

**Harness fix (the real action item):** for SGB-enhanced carts the recomp runs as SGB, so the oracle
must match — either (a) run SameBoy as `GB_MODEL_SGB2` (needs an SGB boot ROM), or (b) force the recomp
to DMG (`hardware_mode=dmg` per-game pref / disable the SGB engine) and compare to the DMG oracle. Then
re-measure Red AND Blue. Until then, the Red/Blue audio-lag numbers above are **not apples-to-apples**
for these SGB carts and should not be read as recomp accuracy defects.

### (superseded) Cycle comparator findings on the Red lag (2026-06-28)
*The LCD-off divergence below is now explained by SGB emulation + oracle-model mismatch (above), not a
PPU bug. Retained for the method; the pacing-matches result still stands.*

`gb_cycle_compare.py` — SameBoy `gb_state_oracle` ring vs the recomp's always-on
`frame_timeseries` ring (debug server :4370), 500 frames:
- **Steady cycle pacing MATCHES** — oracle 70224.0 ± 1.8, recomp 70227.1 ± 36.5 (DMG ideal
  70224). Red's lag is **not** a clock-rate bug; it is a control-flow/sequencing divergence.
- **LCD-off events occur at different frames.** Oracle's >1-frame (LCD-off) gaps land at
  frames 1, 247; the recomp's at 55, 60, 65 (three 4-frame gaps), 288, 352. The two turn the
  LCD off at different intro points → the intro's PPU-control sequencing diverges. Implicates
  the **known LCD-off/PPU weak spot** (Axis 5a bug; CLAUDE.md Tetris line-clear LCD-off bug).
- **PINNED to LCD-off handling (LCDC anchor, 2026-06-28).** Sampling LCDC in both rings
  (the oracle ring now also samples on a fixed cycle cadence so it is not blind to LCD-off)
  gives a fair LCD on/off timeline:
  - oracle: `1:off → 12:on` (boot), `259:off → 262:on`.
  - recomp: `54:off→55:on, 59:off→60:on, 64:off→65:on` (**three ~4-frame LCD-off stalls the
    oracle does NOT have**), then `287:off→288:on`.
  - The shared pre-title LCD-off is at **recomp frame 287 vs oracle 259 — a +28-frame offset
    for the same event**; cycle-aligned progress lag is **−25 frames** by frame 500; the
    recomp carries **~12.4 frames of extra LCD-off gap cycles**.
  - **Root cause class: the recomp holds the LCD off several extra frames per toggle during
    the Red intro** — the LCD-off/PPU re-enable weak spot (Axis 5a; same class as the CLAUDE.md
    Tetris line-clear LCD-off bug). **Next:** Ghidra the intro LCD-disable/enable routine
    (RULE 0) and fix the recomp's LCD re-enable frame accounting in `ppu.c`; re-run the
    comparator — the three 55/60/65 stalls and the +28-frame offset should collapse.
  - *Tool caveat:* the NR52 (APU power-on) anchor would need adding NR52 to the recomp's frame
    ring (a rebuild) to avoid an arm-then-race; LCDC was already in both rings, is the
    divergence locus, and needs no rebuild — so it is the better available anchor.

**Verdict:** the recomp plays the correct Pokémon Red title music — spectrally close (0.92)
and rhythmically aligned (0.82) — but with a measurable ~0.8 s startup offset and ~26 ms
onset jitter consistent with the per-instruction (non-sample-accurate) APU sync and the
non-band-limited mixer. This is the drift-tolerant baseline; sample-exact vs SameBoy stays
the aspirational GREEN gate. Artifacts: `accuracy/out/{oracle,recomp}_pokered.s16`,
`accuracy/out/pokered_drift.json`. **Next:** harmonic-aware pitch metric, then attack the
−824 ms offset + 26 ms jitter via Axis-2 per-instruction ticking and APU sample-accurate
register-write timing (`runtime/src/audio.c`).

---

## Phasing

1. **Audio slice (active):** oracle + ring + drift diff → first Pokémon Red comparison.
2. **Cycle comparator:** GB `cycle_compare` (Δcycle vs SameBoy) → unblocks Axes 2/3.
3. **PPU framebuffer ring + VRAM diff:** fix the mode-3/0 timing bug, validate vs SameBoy.
4. **State/divergence oracle:** `GB_save_state_to_buffer` + execution callback → Axes 1/4/6.
5. **Per-instruction tick:** the cross-axis lever for 2/3/5 timing — gate every change on
   the Δcycle comparator and the relevant mooneye suite.
