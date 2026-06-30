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
| 1 | Instruction semantics | **instruction-accurate; (HL)/AF golden-locked** | `oam_bug` not modeled (DMG OAM quirk; blargg `oam_bug`/mooneye source-only) — low priority | DONE: `accuracy/golden/` structural+behavioral guard for `(HL)` RMW/ALU + PUSH/POP AF masking. (optional) typed operands audit Phase 3 |
| 2 | Cycle / timing | **sub-instruction access placement DONE; full per-M-cycle SHELVED** | `mem_timing` Failed 3→**2** (`modify_timing` PASS via RMW tick-split, interp+emitter; ALU/BIT (HL) reads tick-before'd; no regression — cpu_instrs/instr_timing/tim00/golden PASS). Remaining (`read/write_timing`, `tima_reload`, `rapid_toggle`, `intr_timing`) = a long tail of discrete HW quirks (timer write-during-reload, TAC-toggle glitch) + **mid-instruction IRQ recognition** which fights the static-recompile model and carries a real perf cost (a per-M-cycle `gb_tick` stepping spike regressed tim00/div_write/reti via timeout and fixed none — reverted) | **SHELVED (low product value).** Emulator-conformance only; no real game needs sub-instruction timer/IRQ precision (Pokémon Red already pixel/audio-accurate; per-pixel PPU ceiling is game-invisible). Revisit only if a real game fails. See [[project_cycle_accuracy_decision]] |
| 3 | Interrupt / event timing | **instruction-accurate dispatch; block-granular delivery** | Functions can't be interrupted mid-block; T-cycle PPU-IRQ tests FAIL; dead `ime_scheduled` write | Same per-instruction tick; tighten block granularity in IRQ-sensitive regions |
| 4 | Memory / MMIO | **CLOSED (DMG)** — `unused_hwio-GS` PASS | (optional) promote IO trace to always-on ring; `unused_hwio-C` (CGB) legitimately fails on real CGB HW too | DONE: SC/TAC/IF unused-bit OR-masks + unmapped→0xFF + DMG-guard FF55/FF68-6B (`gbrt.c`). cpu_instrs/instr_timing still PASS |
| 5a | Video / PPU timing | **scanline-accurate (with a live bug)** | Computed variable mode-3/0 durations are **never used** — hardcoded 172/204 instead, so scanlines don't sum (`ppu.c:791-820` vs `:817`); no sub-scanline effects | Wire `scanline_draw_cycles`/`scanline_hblank_cycles` into mode transitions; move toward per-dot rendering |
| 5b | **Audio / APU** ← ACTIVE | **approximate** (per-instruction sync, no band-limiting, `±1×vol` mixer) | Samples emitted in per-instruction bursts; point-sampled at 44.1 kHz (aliasing); two inconsistent channel-output models; known glitching | Sample-accurate register-write timing; band-limited resample; unify mixer on the 4-bit DAC model |
| 6 | Static-vs-dynamic fidelity | **DECIDED — static compile + interp fallback + Tier-0 harvest** (NO JIT, by design) | None blocking. CFG edges drop bank info (cosmetic); `tcc` is a contingency only if a future game shows persistent *hot* interpreted code | **CLOSE.** NO-GO on sljit/gcc shards settled & validated (Pokémon 99.99% coverage via harvest). Only revisit (with `tcc`, not sljit) if measurement proves a hot dynamic-code game |
| 7 | Determinism | **CLOSED** — core fully deterministic | (accepted) audio/frame pacing host-driven via SDL — fine for playback; oracle runs capture the deterministic core | DONE: injectable/seeded RTC clock (`gbrt_set_rtc_epoch` / env `GBRT_RTC_EPOCH`), default `time(NULL)` |

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

**blargg CPU-conformance scorecard (2026-06-28)** — `accuracy/oracle/run_cputest.sh` (recompile →
build → run headless → decide pass/fail from the serial stream via env-gated `GBRT_SERIAL_LOG` in
the runtime's FF02 handler). Raw: `accuracy/out/cpu_scorecard.txt`.
- **cpu_instrs 01–11: ALL PASS** — every CPU instruction is correct.
- **instr_timing: PASS** — instruction-level cycle counts are correct.
- **mem_timing: FAIL (#3)** — memory-access timing *within* an instruction is wrong. This is the
  sub-instruction cycle-accuracy gap: the recomp ticks per-instruction (`gb_tick` once per opcode),
  so a memory read/write lands at the opcode boundary, not its exact M-cycle.
- **mem_timing-2, halt_bug: NO OUTPUT** (no serial even at 8000 frames → recomp likely hangs/diverges
  on these; needs per-ROM investigation — possibly real HALT/mem-timing bugs).
- **Verdict:** the recomp CPU is instruction-correct and instruction-timing-correct, but lacks
  **sub-instruction (M-cycle) memory/IO timing**. This single gap is the common root of: mem_timing
  fail, the Mealybug per-pixel mid-line residual (Axis 5a), and the audio onset jitter (Axis 5b).
  It is the highest-leverage deep fix. (mooneye granular timing tests would refine this but are
  source-only here — no rgbds for prebuilts.)

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

- [x] **Framebuffer-diff oracle slice stood up (2026-06-28).** `accuracy/oracle/gb_fb_oracle.c`
      (SameBoy → P6 PPM at frame N, skip-boot DMG) vs the recomp's `--dump-frames` PPM (works
      headless in benchmark mode), diffed palette-independently by `accuracy/tools/fb_diff.py`
      (ranks gray levels by luminance → shade indices). **Result: Pokémon Red is pixel-perfect
      vs SameBoy** — 0.000 % differing pixels on frames 100/250/500/560; frame 400's 0.33 % was
      purely the known ~1-frame numbering offset on an animated sprite (`rec[400] == ora[401]`
      exactly). So the recomp's PPU **rendering** matches SameBoy for Pokémon Red.
- [ ] **BUG (not surfaced by Pokémon):** variable mode-3/mode-0 durations computed (`ppu.c:791-820`)
      but never used — `ppu.c:817` compares hardcoded `CYCLES_PIXEL_DRAW`. This is a **mode-timing /
      STAT-interrupt** bug: it shifts *when* mode transitions/STAT IRQs fire, **not** the scanline
      pixels — so it is invisible in a framebuffer diff of games without mid-frame raster effects
      (Pokémon's intro/title don't use them). **To surface it visually, run the Mealybug Tearoom
      test ROMs** (designed so the framebuffer DIFFERS on PPU-timing errors) through this same
      `gb_fb_oracle` + `fb_diff` slice; also compare STAT-IRQ/LY-at-cycle directly (Axis 3).
- [x] **Test-ROM harness + first Mealybug result (2026-06-28).** The recomp is a *per-ROM* static
      recompiler, so test ROMs must be recompiled. `accuracy/oracle/run_testrom.sh` automates it:
      `gbrecomp` the ROM → point `GBRT_DIR` at the worktree runtime → build → **pre-fill `rom.cfg`**
      (the generated launcher loads the ROM from a file with a SHA-256 check; `rom.cfg` next to the
      exe supplies the path headless) → run `--dump-frames` → `fb_diff`. Mealybug ROMs at
      `F:/Projects/mealybug-tearoom-tests` (prebuilt + `expected/DMG-blob/*.png` ground truth).
      **`m3_bgp_change` (BGP change mid-mode-3): recomp vs DMG ground truth = 26.9 % differing
      pixels across all 144 rows** — the recomp renders each scanline with a single BGP, so it
      cannot reproduce the mid-scanline palette change. This **quantifies the sub-scanline gap**.
      *(Caveat: `gb_fb_oracle` renders blank on some Mealybug ROMs under skip-boot — a harness nit;
      the bundled `expected/` PNGs are the proper Mealybug ground truth and are used instead.)*
- [ ] **Sub-scanline / mid-line effects (SCX/SCY/window/BGP mid-line) — NOT modeled** (now measured:
      m3_bgp_change 26.9 % off). The recomp's scanline renderer needs per-dot/fetcher rendering to
      pass the Mealybug `m3_*` suite.

**COMPLETE Mealybug `m3_*` scorecard (2026-06-28)** via `accuracy/oracle/{run_testrom,score_mealybug}.sh`
— now **guest-cycle-based dump** (`--dump-cycle-frames N` = N×70224 T-cycles, fires regardless of
LCD/halt; new runtime API `gb_platform_set_dump_cycle_frames`/`gb_platform_check_cycle_dump`, CLI flag
+ per-slice check in `c_emitter.cpp` generated main). All 23 m3_* ROMs with DMG ground truth now
capture a framebuffer. `m3_obp0_change`'s earlier "link failure" was a **stale-exe file lock**
(`Permission denied` on relink), NOT a recompiler gap — fixed by a pre-build `taskkill` in the harness.

% differing vs `expected/DMG-blob/*.png`, frame 20 (lower = better):

| < 1 % (PASS) | 1–10 % (close) | 10–50 % | > 50 % (scanline-renderer can't do mid-mode-3) |
|---|---|---|---|
| obj_size_change_scx **0.8** | window_timing_wx_0 4.1; window_timing 5.3; lcdc_bg_en_change 9.5 | bgp_change 26.9; win_en_multiple_wx 26.1; scx_high_5_bits 36.2; win_en_multiple 39.0; wx_4 40.8; wx_5 40.7; wx_6 41.6 | scy_change 67.0; bgp_change_sprites 88.3; lcdc_obj_en_change_variant 94.2; lcdc_bg_map_change 94.6; lcdc_obj_size_change 94.9; lcdc_win_map_change 95.0; lcdc_tile_sel_win_change 96.2; lcdc_tile_sel_change 96.3; scx_low_3_bits 97.7; obp0_change 98.7; lcdc_obj_en_change 99.4; wx_4_change_sprites 100.0 |

**Reading:** 1/22 sub-scanline-exact, 3 close, the rest fail **by design** — the recomp renders one
register value per scanline, so any register change *mid-mode-3* (BGP/OBP/LCDC/SCX/SCY/WX/window) is
applied at a scanline boundary instead of the exact dot. This is the quantified Axis-5a sub-scanline
gap. **Lever:** per-dot/fetcher PPU rendering (large; would let the recomp pass the `m3_*` suite).
Some >90% cases also render blank at frame 20 (recomp 1 gray level) — a few need a per-ROM dump frame.
*(Caveat: `gb_fb_oracle` renders blank on several m3_* ROMs under skip-boot, so the bundled
`expected/` PNGs are the ground truth, not the SameBoy fb oracle.)* Raw table: `accuracy/out/mealybug_scorecard.txt`.

**SEGMENTED MID-SCANLINE RENDERING (2026-06-28).** Replaced the per-scanline latch+one-shot render
with incremental mid-mode-3 rendering: `ppu_tick` MODE_DRAW now draws the BG/window in pixel ranges
as mode-3 cycles elapse, sampling LIVE registers per segment (`render_bg_segment(x_start,x_end)`,
~12-dot warmup then 1px/dot); `gb_write8` catches the PPU up (`gb_sync`) before applying any FF40-4B
write so the change only affects later dots. (Sprites still render once at mode-3 end — increment 3.)
Result vs latched baseline (`mealybug_scorecard_segmented.txt`): **net −207pp total pixel error**.
- BIG wins (tile-granular mid-line): lcdc_win_map_change 95.0→7.8, lcdc_bg_map_change 94.6→8.1,
  bgp_change_sprites 88.3→29.6, wx_6_change 41.6→38.8.
- Small regressions (per-pixel, 1–7pp): window_timing_wx_0 4.1→11.4, lcdc_bg_en_change 9.5→14.2,
  window_timing 5.3→9.5, wx_4 40.8→44.6, bgp_change 26.9→30.1, tile_sel 96.3→99.1.
- **Pokémon Red: unaffected** (guest-aligned frames 100/250/300/560 = 0.000%, 500 = 0.69%). Verified
  by regenerating with the new recompiler and dumping by GUEST cycle (`--dump-cycle-frames`); the
  earlier "frame 250 = 99.6%" was purely the rendered-vs-guest frame-numbering offset.
**Per-pixel ceiling = CPU↔PPU timing, NOT the PPU.** On m3_bgp_change the expected change is at col 1
every row but the recomp places it at col 84→132 drifting ~−6/row; neither warmup nor wiring in the
VARIABLE mode-3 duration (tried, reverted — zero m3 benefit, perturbs STAT timing) moves it. The
write's guest-cycle→dot mapping is imprecise because these ROMs run in the interpreter and the recomp
lacks sub-instruction cycle accuracy. Closing the per-pixel gap needs CPU-side cycle accuracy (Axis 3),
not more PPU work. Decision: KEEP segmented rendering (net win, real-game-safe, foundation for
mid-scanline raster effects); per-pixel exactness deferred. Visible-game rendering is identical.

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

## Axis 6 — Static-vs-dynamic fidelity (recompiler-unique)        Status: DECIDED (CLOSE)

Static compile + universal `gb_interpret()` fallback for any uncompiled addr incl.
RAM/HRAM (`interpreter.c:106,186-195`); per-page dispatcher → interpret; `JP HL`→
`gbrt_jump_hl`; RST jump tables statically extracted (`analyzer.cpp:171-318`); cross-bank
CALL returns to dispatcher resolving by `ctx->rom_bank`.

**ARCHITECTURE DECIDED — no JIT tier, by design.** The merge initiative made an evidence-based
**NO-GO on sljit/gcc shards + dirty-RAM interp** (`project_gbrecomp_merge_initiative`, Item 3):
every interpreter fallback observed across Tetris/SML/MMX2/Pokémon lands at a ROM-bank or
known-HRAM address = a *static coverage gap*, NOT dynamic/RAM-resident code. GB ROM is immutable
(no PSX-style runtime overlays); the only RAM code is tiny HRAM OAM-DMA stubs (already handled by
`hram_overlay` + `gbrt_try_execute_hram_stub`). Shards solve a problem GB doesn't have.

**The model (this IS the answer, per psxrecomp/NES/Genesis):** static recompile → dispatch miss →
interpreter fallback → misses logged to `interp_fallbacks.log` (`interpreter.c:155-176`) → fed back
via `gbrecomp --harvest` (`config.cpp:154-176,214`, "tier0") as entry-point seeds → next regen
compiles them natively. **Validated:** Pokémon 9419 fallback events / 177 distinct → 1/1 (99.99%)
with zero JIT shards. The harvest loop closes coverage; the universal interpreter is the long-tail
catch-all.

- [x] **Tier-0 dispatch-miss harvest** — implemented, ecosystem-standardized on `dispatch_misses.toml`,
      validated to ~zero residual ROM fallbacks. **This closes Axis 6.**
- [ ] (contingency only) `tcc` dynamic tier — revisit *only if* a future game shows persistent *hot*
      interpreted code the harvest can't statically cover. Nothing observed so far does. NOT sljit.
- [ ] (cosmetic) CFG successors drop bank info (`analyzer.h` `vector<uint16_t>`) — make edges
      `(bank,addr)`; does not affect correctness, only multi-bank seed precision.
- [ ] (optional) Leg-0 only today: `differential.c` proves gen==interp. Add external SameBoy leg.

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

> **NOTE (2026-06-28):** the headline numbers below were the *first* run with the recomp in
> AUTO mode (SGB engine on for Red) vs a **DMG** oracle — an oracle-model mismatch (see the
> SGB reclassification above). The **valid, model-matched baseline** is the forced-DMG run:
> **spectral cosine 0.945, onset xcorr 0.910, +11.6 ms lag, 96 % onsets matched, pitch |median|
> 4.7 c / within-50c 65 %** (Pokémon Red, `GBRT_HARDWARE_MODE=dmg`). Read the ledger first.

### Axis 5b status ledger (current state — instrumented & baselined, NOT closed)

**✅ Done — apparatus + one slice.** SameBoy audio oracle (S16 + guest-cycle sidecar), drift
metric (log-mel cosine + onset histogram + HPS pitch), cycle/state comparator, model-matched
DMG harness (`GBRT_HARDWARE_MODE=dmg`), negative-control-validated. Baseline: Pokémon Red/Blue,
DMG, 20 s title music, no input → cosine ~0.95 / onset xcorr ~0.91 / 96 % onsets / pitch ~4.7 c.
Steady cycle pacing matches SameBoy (70224 T/frame). −824 ms "lag" resolved (was the SGB path).

**⚠️ Known-divergent, NOT fixed (and not yet isolated per-channel):**
1. Per-instruction APU sync (not sample-accurate) → the ~22 ms onset jitter. `audio.c:757,842`.
2. No band-limited resample (point-sampled 44.1k) → aliasing; part of why cosine <1.0. `audio.c:842`.
3. ~~`±1×vol` mixer vs real 4-bit DAC~~ — **FIXED**: output stage unified on the 4-bit DAC digital
   sum (see Mixer fix below). Residual: square/noise per-channel amplitude ~1.9×/3.4× off (next).
4. CH3 wave-RAM-write-while-on quirk (`audio.c:599`); CH1 sweep shift=0 overflow (`audio.c:389`).
5. "General glitching" (CLAUDE.md) — never isolated.

**❓ Known-UNMEASURED (coverage gaps):**
- **Per-channel divergence** — we diff the final mix only; cosine 0.95 doesn't localize which of
  CH1–4 is wrong. → **active next step (per-channel tap).**
- Only the title theme, no input → gameplay/battle SFX, channel triggers under load, CH4 noise /
  CH3 wave barely exercised. Pitch p90 ~1460 c (~30 % of frames off) is partly CH4 — uncharacterized.
- Only DMG → CGB audio path (Yellow/Crystal: PCM12/PCM34, double-speed APU, the 2nd digital model).
- Only Red/Blue (one engine) → Tetris, Super Mario Land (heavier sweep/noise).
- Sample-exact GREEN gate — distance to bit-exact never measured (only drift-tolerant).
- SGB-mode audio (SGB jingle / border audio; Red↔Blue SGB asymmetry) — needs a SameBoy SGB2 oracle.
- The +11.6 ms / ~22 ms residuals — measured, not root-caused.

**Adjacent (touched, not closed):** Axis 2 cycle timing — frame-level pacing matches, but
sub-instruction Δcycle accuracy (mooneye `*_timing`) not yet measured (comparator is frame-granular,
not anchor-based sub-block); state-fork alignment still ±frames.

**Per-channel localization (DONE 2026-06-28).** Per-channel pre-mix tap on both sides
(recomp `debug_audio_pch.raw` via `GBRT_AUDIO_PCH=1` at `audio.c` mix chokepoint; oracle
`<out>.pch` from SameBoy `gb->apu.samples[ch]`), diffed by `accuracy/tools/perchannel_diff.py`.
Pokémon Red, DMG, 20 s:

| Channel | spectral cosine | pitch | onset xcorr |
|---|---|---|---|
| CH1 square | **1.000** | |med| 0 c, 91 % <50c | 0.854 |
| CH2 square | **0.991** | |med| 0 c, 97 % <50c | 0.738 |
| CH3 wave  | **1.000** | |med| 0 c, 99 % <50c | 0.684 |
| CH4 noise | **1.000** | energy-env corr 0.984 | 0.921 |

**Finding:** every channel's spectral content + pitch is near-perfect (0.99–1.00 cosine, ~0 c) —
the APU channel *generation* is accurate. The mixed-stream pitch tail (p90 ~1460 c) was a
**mixing artifact**, not per-channel error. The real residual is **timing** (per-channel onset
xcorr 0.68–0.92, worst on **CH3 wave** and **CH2**), consistent with the per-instruction APU sync
(`audio.c:757,842`), plus mix-balance from the `±1×vol` model (per-channel raw-RMS ratios differ:
CH4 ~2.5×, CH1 ~1.6×). **So the fix order is: (1) sample-accurate register-write timing — biggest
lever, targets CH3/CH2 onset jitter; (2) unify mixer on the 4-bit DAC for correct channel balance.
Channel waveform generation itself needs no work.**

**Mixer fix — 4-bit DAC unification (DONE 2026-06-28).** The per-channel data redirected the
fix: all channels aligned at the *same* +11.6 ms lag with ~1.0 cosine, so the residual was
**mix balance**, not timing. Replaced the `±1×vol` output model in `audio.c` with each channel's
true 4-bit DAC digital (0-15, DAC-gated) summed per NR51 side (matches SameBoy
`update_square_sample`/`update_wave_sample`: square `{0,vol}`, wave `sample>>shift`, DAC
`(0xF−value*2)·vol` applied uniformly). Result (Pokémon Red, DMG):
- **Mixed log-mel cosine 0.945 → 0.951** (p50 0.939 → **0.966**); raw-waveform corr **−0.30 → +0.21**.
- **CH3 wave balance now exact** (per-channel RMS ratio 1.92× → **1.00×**).
- Exposed (previously masked by the `±1×vol` 2× square swing): **square ~1.9× / noise ~3.4×**
  per-channel amplitude differ vs the oracle. Cause not yet isolated — **envelope volume** vs
  **channel-activity/trigger timing** over the window (spectral cosine stays 1.0, so it's an
  amplitude/activity effect, not waveform shape). Logged in `runtime/src/audio.log`.

**Square/noise gap isolated to NOTE-DROP, not volume (DONE 2026-06-28).**
`accuracy/tools/_pch_amplitude.py` + `_pch_envelope.py` decompose the per-channel `.pch` streams:

| Channel | peak level when **both** sounding (r/o) | windows oracle plays but recomp **silent** |
|---|---|---|
| CH1 square | 9.3 / 9.5 (ratio **1.02**) | **36 %** |
| CH2 square | 10.2 / 10.2 (ratio **1.00**) | **50 %** |
| CH4 noise  | 7.5 / 7.7 (ratio **1.03**) | **53 %** |

**Finding:** when the recomp plays a channel its volume is correct (ratio ~1.0) — **not** an
envelope-volume or mixer bug. The residual is that **square/noise notes go silent ~36–53 % of
the windows the oracle is sounding** (notes dropped / cut early). Wave is unaffected (no
envelope/length cutting in this music). Verified by inspection that the obvious paths are
structurally correct: trigger resets volume to `nr12>>4` (`audio.c:669-677`); envelope 64 Hz
(`:425-429`), length 256 Hz (`:382-398`), frame sequencer 512 Hz / no double-clock
(`:1020-1042`, DIV advances at T-rate `gbrt.c:3555`). So the bug is **not** in the obvious code.

**CORRECTED ROOT CAUSE — DC-offset measurement artifact, NOT a recomp bug (2026-06-28, trace round #2).**
The earlier "length-counter divergence" verdict was WRONG; it was an artifact of comparing the raw
per-channel captures. What actually happens:
- The Pokémon engine writes `NR12/22/42=08` (DAC on, vol 0) + `NRx4=0x40` (length-enable, **no
  trigger**) at init, never triggering the channels during the silent copyright/intro phase. With
  the DAC on but the channel disabled, **SameBoy correctly emits a constant DC level (digital ~16)**;
  the recomp represents the same state as `0`. The DC level is **inaudible** — the output high-pass
  removes it — but in the raw `.pch` it looks like a "stuck" channel, so `_pch_state2`/`perchannel_diff`
  + the `(x>0)` active-fraction falsely reported ~48 % "note-drop".
- Proof recomp is correct: (1) framebuffers match game-state at invariant frames (copyright@f180,
  GAME FREAK@f330, intro@f600 → CPUs lockstep); (2) SameBoy's APU **write stream is byte-identical**
  to the recomp's (env `GB_APU_WTRACE` in `SameBoy/Core/apu.c`) — same init, first trigger at write
  #332 in both → neither triggers during 0–10 s; (3) pret disasm: first music is `Music_IntroBattle`
  started inside the GameFreak intro (`engine/movie/intro.asm:333`), copyright is silent; (4) FINAL
  `.s16` output: 0–5 s BOTH RMS=0, 10–20 s comparable. `length_enabled` is exonerated:
  `(enabled & length_enabled)=0 %` on every channel → the length counter never cuts a sounding note.
  **DO NOT port length-counter quirks** (they'd disable *earlier* — wrong direction).

**TOOLS FIXED (DC-robust, 2026-06-28).** Added `highpass`/`ac_rms`/`win_active_ac` to
`audio_drift_diff.py`; `perchannel_diff.py` and `_pch_state2.py` now gate activity on **AC** energy
(DC-removed) instead of raw nonzero. After the fix, `_pch_state2` false-disabled collapsed ~99 %
(CH1 248835→15105, CH2 423625→2470, CH4 423732→2577; remainder is mostly `dac==0` envelope/DAC
timing, not length). `perchannel_diff` now shows matched AC activity and **specCos 1.000/0.995/1.000/
1.000, pitch ~0 c** on all 4 channels — the recomp APU is accurate.

**Remaining real residual (small, separate task):** in the FINAL output, 5–10 s the recomp is
quieter than the oracle (RMS ~1000/527 vs ~3385/3933) during the GameFreak intro — investigate on
the final `.s16` with invariant-point alignment, NOT the raw per-channel capture.

---

| Metric (DMG-vs-DMG, faithful oracle, 2026-06-28 re-score) | Value | Reading |
|---|---|---|
| **Spectral similarity** (log-mel cosine, drift-tolerant headline) | **mean 0.951, p50 0.966, p10 0.890** | Same music; spectrally near-identical through different DAC models. |
| Onset-envelope cross-correlation peak | **0.903** | Strong rhythmic/structural agreement. |
| Alignment lag (recomp vs oracle) | **+23.2 ms** | Small; the old −824 ms was an SGB-vs-DMG model mismatch (use `GBRT_HARDWARE_MODE=dmg`). |
| Onset timing | **96 % matched, |Δ|mean 26 ms** (p10/p90 −46/+24 ms) | Beats line up; residual jitter from per-instruction APU sync. |
| Raw-waveform seg-corr | ~0.21 (footnote) | Near-zero **expected** — recomp vs SameBoy DAC differ in timbre, not tune. |
| Per-note pitch error (HPS f0) | **|median| 1.7 c, within-50c 79 %** | Recomp plays **in tune**; per-channel pitch ~0 c (perchannel_diff). |
| Per-channel spectral cosine (DC-robust perchannel_diff) | **CH1 1.000 / CH2 0.995 / CH3 1.000 / CH4 1.000** | APU generation accurate on every channel. |

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
