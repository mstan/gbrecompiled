# COSIM_ORACLE.md — the GBC first-divergence decision procedure

Status: **DESIGN — BUILD NOT STARTED** (spec written 2026-07-01, worktree `_wt-cosim`,
branch `feat/differential-cosim`). This document is the durable spec so the plan cannot
be lost across handoffs. It is the GBC-specific instantiation of two agnostic docs kept
in `F:\Projects\recomp-template`:
- `DIFFERENTIAL-COSIMULATION.md` — the system-agnostic decision procedure.
- `GBC\DIFFERENTIAL-COSIM-PROPOSAL.md` — the GBC proposal, verified in-tree 2026-06-30.

The PSX build at `F:\Projects\psxrecomp\...\COSIM_ORACLE.md` (+ `runtime/src/cosim.c`,
`cosim_state.c`, `include/cosim_state.h`, `tools/cosim.py`) is the gold standard this
mirrors.

## Why this exists (GBC is different from PSX — read this)

On PSX the co-sim was born to settle a two-week wedge (a string of confident, wrong
root causes). **GBC is not chasing one loud bug.** This recomp is *doing well*: Tetris
boots, renders, and plays; Blargg `cpu_instrs` / `instr_timing` / `01-special` pass;
28/70 of the Blargg+Mooneye matrix passes (`gb-recompiled/ACCURACY.md`). So the GBC
co-sim is a **regression ratchet / correctness guard**:

1. **Pin** the current good behavior so future recompiler/runtime changes cannot
   silently regress it (a green full-state baseline is the pin).
2. **Surface residual** subtle divergences a pass/fail test-ROM matrix cannot localize —
   a PPU mode-timing dot off by a few T-cycles won't necessarily flip a Blargg PASS, but
   it *will* split the full-state hash.

A "no divergence" result here is a **feature, not a null result**: it becomes the pinned
baseline the ratchet defends on every future change. Full discipline still applies — a
regression guard you don't trust is worse than none, so the validation gates below are
load-bearing, not optional.

## ⚠️ "We know subsystem X is correct" is a HYPOTHESIS the co-sim tests

The tree carries plausible beliefs. Treat **every one** as an UNVALIDATED claim the
co-sim exists to grade, never a fact you build the tool around:
- `ACCURACY.md` "Known Limitations" says the CPU core is solid and the only soft spots
  are PPU sub-mode timing, mem_timing RMW, OAM DMA timing, the HALT bug. **Hypothesis.**
  `cpu_instrs` passing proves the *architectural* result on those ROMs; it does NOT prove
  per-cycle state parity on Tetris.
- `CLAUDE.md` blames the Tetris title-glitch on "PPU STAT interrupt timing" and the
  line-clear bug on the "LCD off/on transition". Hypotheses. The co-sim NAMES the split.
- The runtime advertises "cycle-accurate tick placement" with a per-region opt-out that
  "batch[es] ticks back to whole-instruction granularity." That opt-out is exactly the
  kind of thing that hides a divergence — the co-sim must see into it.

**Corollary — do NOT trim the hashed state surface based on any of these.** Hashing only
the PPU "because we know the CPU is correct", or skipping the APU "because audio isn't
the current worry", re-creates the single-signal blind spot the method eliminates and
*assumes the thing under test*. Hash the FULL machine; the per-subsystem sub-hash TELLS
you which subsystem stayed faithful and which split — you learn that from the tool, you
do not decide it in advance.

## The two implementations to compare

The tree owns TWO complete SM83 implementations in one process family — the ideal
starting pairing (agnostic requirement 1), no external emulator needed for v1.

1. **recomp/generated backend vs the in-project interpreter.** `runtime/src/interpreter.c`
   is a full SM83 interpreter (the fallback for uncompiled code). A lockstep harness
   *already exists*: `runtime/src/differential.c` → `gb_run_differential(generated_ctx,
   interpreted_ctx, opts, result)` (API `runtime/include/gbrt.h:744`) steps both via
   `gb_debug_step(ctx, GB_EXECUTION_GENERATED)` / `…_INTERPRETER` and compares full
   contexts each step (`gb_diff_compare_contexts`). **This is the co-sim's skeleton —
   extend it, don't rebuild it.** Because both backends thread a full `GBContext*`
   (unlike PSX's global-state-heavy runtime that forced two OS processes), pairing 1 can
   run **in-process** with two `GBContext` instances — the lightest path to a trustworthy
   tool. Two gaps to close (below): it compares by exact field-equality (no per-subsystem
   hash + checkpoint ring), and its state surface is INCOMPLETE.
2. **recomp vs an external oracle — SameBoy, embedded as a linked libretro core (the
   psxrecomp Beetle pattern).** SameBoy is the accuracy gold standard for GB/GBC AND a
   libretro core (`F:\Projects\SameBoy\libretro\` + the embeddable `Core/`). Follow exactly
   how psxrecomp did its oracle: it took the Beetle PSX libretro core, built it as a static
   lib (`libmednafen_psx.a`), and wrote a driver (`runtime/src/beetle_libretro.cpp`) that
   drives it via the core entry points **and reaches into the core's internal structs**
   (`GPU`/`SPU`/RAM/VRAM) for deep full-state extraction, linked into the runtime. NOT a
   thin DLL-over-socket frontend — the plain libretro API cannot expose the micro-state
   (PPU mode dots, APU LFSR, timer `div_counter`) a full-state co-sim must hash, which is
   precisely why psxrecomp embedded the core. SameBoy's `Core/` is embeddable identically:
   `GB_init` / `GB_run` (returns T-cycles run — ideal for the shared-clock lockstep) /
   `GB_get_direct_access(GB_DIRECT_ACCESS_RAM|VRAM|OAM|IO|HRAM|CART_RAM|…)` /
   `GB_load_rom_from_buffer`, with the full `GB_gameboy_t` struct directly reachable for the
   deep hash. Escalate to it when pairing 1 is ambiguous, or for the class of bug pairing 1
   is *structurally blind to*: the recomp and interpreter share `gb_timing.h`, so a bug in
   that shared table diverges from real hardware **identically** in both and passes A-vs-B
   in pairing 1 — only an independently-authored emulator arbitrates that. (SameBoy's
   `Tester/main.c` is a validation harness, not the embedding path; embed `Core/` + a driver
   like Beetle, not the Tester.) PyBoy is already wired for coverage ground truth
   (`tools/capture_ground_truth.py`, `GROUND_TRUTH_WORKFLOW.md`) but only for unique Bank:PC
   coverage, not full-state lockstep.
3. **recomp-vs-recomp / interp-vs-interp** for Gate 1/2 only (determinism proof).

## The shared alignment clock — the guest T-cycle counter

The ruler is `ctx->cycles` (32-bit T-cycles at ~4.194 MHz), advanced identically by both
backends because both consume the SAME per-opcode tick model (`runtime/include/gb_timing.h`,
shared by `interpreter.c` and the emitter `recompiler/src/codegen/c_emitter.cpp`).
Checkpoint on strides of `ctx->cycles` so both sides park at the identical T-cycle.

`differential.c` today steps by *scheduler step / frame*, which does NOT guarantee equal
clock values at the compare point — **switch the checkpoint key to the T-cycle.**

**CGB double-speed wrinkle (call it out):** in CGB double-speed (`ctx->cgb_double_speed`,
KEY1 @ `0xFF4D`) the CPU runs at ~8.38 MHz but the PPU/APU do not — `gbrt.c:3650`
computes `system_cycles = cgb_double_speed ? cycles/2 : cycles` before ticking PPU/APU.
The T-cycle counter is still the single shared ruler, but the co-sim must snapshot at a
boundary where the /2 PPU/APU sub-tick is in a defined phase, or a mid-CPU-step checkpoint
shows a PPU half a tick behind between backends. **Tetris is DMG — this never arms in v1.**
For CGB targets (this repo also holds Pokémon, Megaman Xtreme 2) checkpoint on even
system-cycle boundaries when `cgb_double_speed` is set.

## Comparison granularity — clock-keyed, NOT block-keyed

The two backends do not share a block structure (recomp = gen-time CFG blocks; interpreter
= per-instruction). So a block-leader hash sequence would not align. Checkpoint the hash
on **T-cycle milestones** (hook the checkpoint into the tick advance): at stride S both
backends are at the same guest instruction (until the real divergence), so their
checkpoints align. Keep a cheap "last leader PC" stash for human-readable reporting only;
compare on the clock-keyed full-state hash. Drill = shrink stride S→1 in the divergent
window and re-run (deterministic ⇒ same divergence reproduces).

*PC-currency caveat:* the recomp may keep `pc` current only at block boundaries while the
interpreter updates it every instruction. **Exclude `pc` from the cross-backend hash**
(still report it) — a real control-flow split shows as a differing GPR/RAM/IO byte within
one checkpoint, so excluding PC costs at most one checkpoint of latency, not a blind spot.

## Full architectural state — the completeness checklist

Hash the complete guest-architectural state. `differential.c` today compares regs + WRAM +
VRAM + OAM + HRAM + IO + ERAM + PPU framebuffer/palettes — **but it OMITS the APU entirely,
the timer `div_counter`, and the DMA/HDMA/serial scheduling micro-state.** Those omissions
are the blind spots the ratchet must close. INCLUDE all of:

- **SM83 CPU** (`GBContext`, `gbrt.h:164`): `a,f(+f_z/f_n/f_h/f_c),b,c,d,e,h,l,sp,pc*`
  and the execution-mode latches that steer the next instruction: `ime`, **`ime_pending`**
  (EI-delay latch), `halted`, `stopped`, `stop_mode_active`, **`halt_bug`**,
  `cgb_double_speed`.
- **Memory:** WRAM (8 banks × 4 KB, CGB-banked via `wram_bank`), VRAM (2 banks × 8 KB,
  CGB-banked via `vram_bank`), OAM, HRAM, the IO page, ERAM (cart RAM).
- **PPU FULL state** (`GBPPU`, `ppu.h:116`): every LCD register (`lcdc,stat,scy,scx,ly,
  lyc,dma,bgp,obp0,obp1,wy,wx,bgpi,obpi,opri`), the mode-3 **latched_*** register set, the
  internal `mode` + **`mode_cycles`** (the mode-timing dot counter — where the "PPU
  sub-mode timing" hypothesis lives), `stat_irq_state`, **`window_line`** +
  `window_triggered`, the segmented mid-scanline fetcher state (`render_x`, `bg_raw_line`,
  `bg_priority_line`, `scanline_sprites` + counts), and the **CGB palette RAM**
  (`bg_palette_ram[0x40]`, `obj_palette_ram[0x40]`, palette RAM @ `ppu.h:179–181`).
  Framebuffers are a *lagging* view — report them, but a divergence shows in
  `mode`/`mode_cycles`/regs before pixels.
- **APU FULL state** (`GBAudio`, `audio.c:213`) — **the biggest current gap:** all 4
  channels' internals — frequency/`wave_pos` (duty position), `env_timer`+`volume`
  (envelope), sweep shadow/timer (ch1), `length_counter`+`length_enabled`, ch3
  `wave_ram[16]` (32 4-bit samples) + `wave_pos`, ch4 `lfsr` — plus `nr50/nr51/nr52`, and
  the **frame sequencer** `fs_timer`+`fs_step` (the DIV-APU step). A serializer exists
  (`gb_audio_save_state`, `audio.c:527`) — reuse it, AUDIT it against this list. Host
  sample timers (`sample_timer_fixed`, DC-blocker `hp_prev_*`, `last_output_*`) are
  host-only — EXCLUDE.
- **Timer:** `div_counter` (internal 16-bit DIV), `tima_reload_pending` (TIMA overflow-
  reload delay), plus TIMA/TMA/TAC in the IO page. The falling-edge detector the timer
  ROMs probe is a function of `div_counter` + TAC — hashing both captures it.
- **OAM DMA + CGB HDMA:** `ctx->dma` (`active,pending,source_high,progress,
  cycles_remaining,startup_delay`) and `ctx->hdma` (`source,dest,blocks_remaining,active,
  hblank_mode`) — in-flight transfer + completion scheduling, the "WHEN a bit flips"
  device state the agnostic doc calls the classic omission.
- **Serial:** `ctx->serial_transfer` (`active,fast_clock,cycles_remaining,deferred,…`).
- **CGB double-speed + speed-switch pending:** `cgb_double_speed` and the KEY1 armed bit
  (IO `0xFF4D` bit 0).
- **MBC/cartridge mapper:** `mbc_type,rom_bank,ram_bank,ram_enabled,mbc_mode,
  rom_bank_upper,rtc_mode,rtc_reg` and the MBC3 **`rtc`** sub-struct (`s,m,h,dl,dh` +
  latched copies + `latch_state`; `last_time` is host-clock-derived → caveat below).

**EXCLUDE (host-only, would cause false divergences):** every `void*` (`ppu,apu,timer,
serial,joypad,sgb,ir,platform,rom,eram,wram,vram,oam,hram,io` pointers, `trace_file`),
`GBPlatformCallbacks`, host audio sample/DC-blocker/mixer output fields, and — critically —
the emulator-internal bookkeeping counters that legitimately differ by backend:
`used_dispatch_fallback`, `frame_dispatch_fallbacks`, `total_interpreter_*`,
`interpreter_hotspots[]`, the LCD-off statistics (`total_lcd_*`, `frame_lcd_*`). Those count
*how the recomp was implemented*, not guest-architectural state — hashing them guarantees a
false A-vs-B split. Serialize little-endian, arrays in fixed order, RTC/DMA scheduling by a
deterministic key. For the MBC3 `rtc.last_time` (host-clock-seeded): for a deterministic
attract run, freeze/seed the RTC identically on both sides, or exclude `last_time` and hash
only the guest-visible RTC registers.

### Cheap per-checkpoint hashing (mirror PSX `cosim_state.c`)

- WRAM/VRAM/(ERAM): **incremental** page hashing — 4 KB page hashes updated on write +
  a top hash; per-checkpoint cost is O(pages touched), not O(all RAM). Hook the RAM/VRAM
  write chokepoints in the cosim build (`cosim_note_ram_write` / `cosim_note_vram_write`).
- CPU + PPU regs + APU + timer + DMA/HDMA/serial + MBC: serialize the small canonical blob
  per checkpoint and hash it.
- `state_hash = H(cpu, mem, ppu, apu, timer, dma, serial, mbc)` — compare sub-hashes first
  on mismatch to localize which subsystem split before a field/byte diff.

## Validation gates — trust NOTHING until these pass

Per the agnostic doc — do NOT believe any A-vs-B result until:

1. **recomp-vs-recomp = 0** across the full attract run (proves coordinator determinism +
   hashing + that all host-only state is excluded). Likely failure suspects: the SDL audio
   callback thread, any wall-clock RTC seed, uninitialized IO. Necessary, not sufficient.
2. **interp-vs-interp = 0** (proves interpreter determinism + context instancing). When
   the SameBoy oracle is in play, also **SameBoy-vs-SameBoy = 0** in headless mode.
3. **Injected fault halts at the right place + names the subsystem.** A knob flips one
   WRAM byte / one APU `lfsr` / one `mode_cycles` in one instance after checkpoint K; the
   tool MUST halt at ~K and name that field. This is the ONLY gate that catches a
   silently-blind compare (a parse bug or `None == None` compare passes Gate 1 trivially
   while catching nothing) — and it directly exercises the fields `differential.c` omits
   today, proving they are now wired. **Never skip it.** Add a hard assert that each
   compared field parsed non-null.
4. **Hash-vs-byte audit** every N checkpoints — force a full byte compare even when hashes
   match, proving the incremental page-hash maintenance is correct.

Only after 1–4 pass do you run **recomp-vs-interp** (pairing 1) and believe its
first-divergence report. The SameBoy oracle (pairing 2) is believed only after its own
Gate 2 (SameBoy-vs-SameBoy = 0) passes.

5. **Independent third check (GBC bonus):** run the extended co-sim across the Blargg ROMs
   (`F:\Projects\gb-test-roms`) and Mooneye/mealybug (`F:\Projects\mealybug-tearoom-tests`)
   as fixtures. A ROM whose ACCURACY.md verdict is PASS but whose full-state hash splits
   mid-run is a *residual* divergence the pass/fail bit hid — exactly the class the ratchet
   exists to expose.

## Determinism via no-input attract

Tetris has a no-input attract/demo (per `CLAUDE.md`, ~frame 600 is "demo gameplay with
pieces falling") — boot to title→demo and neither side needs controller input, so both
runs receive identical input by construction (the easy path to Gate 1). Blargg/Mooneye
ROMs are likewise no-input. Don't over-worry determinism — but Gate 1 MUST be green before
any A-vs-B verdict is believed. If Gate 1 fails, a nondeterminism leak (audio thread, RTC,
uninit) is masquerading as a guest divergence — fix it first.

## Falsifiable predictions (the tool decides, not the doc)

Written down ONLY so the measurement can confirm/refute — do NOT code toward them:
- **If "PPU sub-mode timing" is right,** the first divergence on a clean Tetris attract is
  a PPU sub-hash split at `mode`/`mode_cycles`/`stat` (a mode-0/2/3 boundary landing a few
  T-cycles apart), with CPU + WRAM identical up to it.
- **If the shared `gb_timing.h` table has a bug,** pairing 1 will NOT see it (both share
  the table) — it passes A-vs-B while SameBoy splits. That asymmetry is itself the finding:
  escalate to the SameBoy oracle.
- **If the APU is the residual weak spot** (the untested surface today), the first split is
  an `fs_step`/`env_timer`/`lfsr`/`wave_pos` field — invisible to the current
  `differential.c` precisely because it omits the APU, which is why widening the surface is
  the whole point.
- **If everything is as healthy as ACCURACY.md implies,** the co-sim runs the full attract
  with zero split — and THAT becomes the pinned baseline the ratchet defends.

Do not decide between these in advance. Build the neutral full-state co-sim, pass the
gates, run it, read which sub-hash splits first (or that none does). THAT is the answer.

## Architecture decision — in-process first, TCP + oracle after

Recommended sequencing (matches the proposal; the doc's own phasing, not a shortcut):

- **Phase A — pairing 1, in-process.** Widen `differential.c`'s existing dual-`GBContext`
  lockstep into full-state + per-subsystem sub-hashes (incremental page-hash RAM),
  T-cycle checkpoint ring, and the `chain/sub/regs/dev/window/inject/reset` surface. No
  TCP or second OS process needed for pairing 1 — an in-process C harness (or thin CLI)
  drives both contexts. Fastest route to a trustworthy tool + all four gates on Tetris.
- **Phase B — pairing 2, embedded SameBoy oracle (the Beetle pattern).** Build SameBoy's
  `Core/` as a static lib and add a driver `runtime/src/sameboy_oracle.c` (mirroring PSX
  `beetle_libretro.cpp`) that drives it via `GB_init`/`GB_load_rom_from_buffer`/`GB_run`
  (T-cycle lockstep off `GB_run`'s returned cycle count) and reaches into `GB_gameboy_t`
  for the deep full-state hash — reusing the SAME `cosim_state` sub-hash layout, mapping
  SameBoy's fields onto the checklist. Add the `#ifdef GBC_COSIM` clean stripped build with
  the minimal line-oriented TCP server (mirror PSX `cosim.c`/`cosim_state.c`) + a Python
  coordinator `tools/gbc_cosim.py`. SameBoy is the independent third opinion — the only
  thing that catches shared-`gb_timing.h` bugs. (psxrecomp embeds Beetle in-process; the
  TCP+coordinator layer exists mainly to keep the recomp and oracle globals in separate OS
  processes and to reuse one coordinator across pairings — decide in-process-embed vs
  separate-process at build time, following whatever the Beetle driver's final shape was.)

Phase A is not a lesser tool — it is the *complete* pairing-1 co-sim with all gates. Phase
B *adds* the independent-oracle capability pairing 1 is structurally blind to. Build A,
pass its gates, pin the baseline; then build B.

## Build plan (checkbox ledger — keep current)

Phase A (in-process pairing 1):  ✅ COMPLETE — all four gates pass on Tetris (2026-07-01).
- [x] `COSIM_ORACLE.md` (this file).
- [x] State-hash module `runtime/src/cosim_state.c` (+ `include/cosim_state.h`): canonical
      little-endian FNV-1a serialize → CPU latches + PPU full state (regs/latched/mode/
      fetcher/palette RAM, framebuffers excluded) + APU internals (via `gb_audio_cosim_hash`
      in audio.c, host sample/DC/output excluded) + timer `div_counter`/`tima_reload_pending`
      + DMA/HDMA/serial scheduling + MBC/RTC + full RAM regions → top hash + `CosimSubHashes`
      (14 subsystems). Full-region hashing per checkpoint (page-hashing deferred as a perf
      optimization, not needed at correctness-first stride).
- [x] Widen `differential.c`: added `gb_run_cosim` keyed on the **T-cycle** clock
      (`ctx->cycles`), sub-hash compare + 64-entry checkpoint ring + window dump, `inject`
      (Gate 3, all 5 subsystems incl. APU) and `hash-vs-byte audit` (Gate 4) knobs. Also
      closed the exact-compare's surface gaps (APU internals, serial, `tima_reload_pending`,
      PPU latched/fetcher) so the drill/namer + audit are complete.
- [x] Audited `gb_audio_save_state` (`audio.c:527`): it memcpy's the whole GBAudio incl.
      host-only fields → wrote a curated `gb_audio_cosim_hash` instead (guest fields only).
- [x] CLI: `--cosim [--cosim-pair aa|bb|ab] [--cosim-stride N] [--cosim-frames N]
      [--cosim-checkpoints N] [--cosim-audit N] [--cosim-log N] [--cosim-inject
      wram|ppu|apu|cpu|timer --cosim-inject-at K]` emitted into the generated main by
      `c_emitter.cpp`. Coordinator `tools/gbc_cosim.py` runs all gates + the A-vs-B measurement.
- [x] Gates 1–4 PASS on Tetris DMG attract (recomp-vs-recomp=0 w/ stable chain hash,
      interp-vs-interp=0, injected-fault halts at exact cp & names each subsystem incl. APU,
      hash-vs-byte audit clean). A-vs-B (recomp vs interp) matched over the validated window.
- [x] Full-attract baselines pinned (2026-07-01). Perf: the long-run bottleneck was
      `gb_interpret`'s per-instruction fallback logging (fflush + debug-server ping every
      instruction — it is the interpreter backend's normal path). Gated behind
      `gbrt_interp_fallback_logging` (default on; `gb_run_cosim` disables it). Result:
      **Tetris 700 frames / 109,671 checkpoints in ~13 s; MMX2 (CGB) 1000 frames /
      156,918 checkpoints in ~23 s**, both matched=1 with periodic hash-vs-byte audits.
      Page-hashing was NOT needed (profiled first, per the spec) — killing the log spam
      sufficed. Pinned chains (stride 456) recorded in `tools/cosim_baselines.tsv`:
      tetris 700f = `30573CCD2C2D82CB`, megaman_xtreme2 1000f = `E2A240E56E7E348E`.
      Ratchet assert: `gbc_cosim.py --ab-frames N --expect-chain HEX` (exit 0 on match).
- [ ] Gate 5 fixture sweep: run the co-sim across Blargg (`F:\Projects\gb-test-roms`) and
      Mooneye/mealybug (`F:\Projects\mealybug-tearoom-tests`) as pinned baselines.

Meaning of the all-matched result: over the FULL Tetris attract (incl. demo gameplay) and
deep into MMX2, the recomp and interpreter backends are byte-identical in complete
architectural state at every T-cycle checkpoint. That is the strong pairing-1 baseline the
ratchet defends. Any *residual* divergence from real hardware that both backends share
(e.g. a bug in a shared model) is invisible to pairing 1 by construction — that class is
exactly what Phase B's independent SameBoy oracle exists to catch.

### rom.cfg gotcha (cost a hang; do not relearn)
The generated exe resolves its ROM via `launcher_get_rom_path()` → a one-line plaintext
**`rom.cfg` next to the exe**, or a **blocking GUI file picker** if absent (this is what
hangs a headless `--cosim`/`--differential` run — zero output, looks dead). The exe also
SHA-256-locks to the exact ROM it was recompiled from. So each cosim target needs its own
`rom.cfg`: `printf '%s\n' '<abs path to the exact ROM>' > <target>_test/build/rom.cfg`.

### LLE boot (real BIOS) + LLE-vs-HLE gate — DONE (2026-07-01)

Faithfulness doctrine (per `../psxrecomp`, `../gbarecomp`): support running the real,
user-provided boot ROM (BIOS) LLE, HLE-skip as the production default/fallback. Implemented:
- **Runtime boot-ROM execution** (`gbrt.c`): `gb_context_load_boot_rom` + a read overlay in
  `gb_read8` (DMG/MGB/SGB 0x0000-0x00FF; CGB adds 0x0200-0x08FF) + `0xFF50` unmap in
  `gb_write8` + an LLE branch in `gb_context_reset` (skip_bootrom=false → map BIOS, PC=0,
  power-on-clean regs; HLE fallback if no BIOS loaded). GBContext gains `boot_rom`,
  `boot_rom_size`, `boot_rom_active`.
- **KEY GOTCHA:** the BIOS must execute via the **interpreter**, never generated dispatch.
  Generated code keys on PC and runs the *cartridge's* compiled 0x0000, ignoring the
  read-overlay (which only affects `gb_read8`). The interpreter fetches opcodes through
  `gb_read8`, so it honors the overlay and runs the actual BIOS. (First LLE attempt in
  GENERATED mode escaped BIOS space at instruction 1 — `LD SP,$FFFE` never ran.)
- **LLE-vs-HLE gate** (`gb_run_boot_gate`, CLI `--boot-gate --boot-rom PATH`): runs the real
  BIOS to the 0xFF50 handoff, then compares the CPU handoff registers (+IME) against the HLE
  skip-state. RESULT on Tetris + DMG BIOS: handoff at PC=0x0100 SP=0xFFFE, **0 CPU-handoff
  diffs — our HLE post-boot CPU state is byte-perfect vs the real BIOS.** Informational: DIV
  differs (LLE div_counter=0x0584 after ~23.5M cyc vs HLE constant 0xABCC — a boot-timing
  fidelity signal; the mooneye boot_div expectation is DIV=0xAB, so our interpreted boot
  timing looks off — defer to the SameBoy oracle to arbitrate) and APU differs (expected:
  BIOS chime vs HLE defaults). Boot ROMs live in `boot_roms/` (gitignored; user-provided).
- Only `boot_roms/dmg_boot.bin` (canonical, sha1 4ed31ec6…) is present; **cgb_boot.bin still
  needed** for an MMX2 LLE gate.

This makes Phase B **Option A (cycle-0 lockstep)** viable: both recomp and SameBoy execute
the same real BIOS from power-on. Open follow-up: the DIV/boot-timing discrepancy (our
interpreted boot duration vs real) — a genuine fidelity item the SameBoy oracle will pin.

### Phase B v1 — embedded SameBoy oracle BOOTS + arbitrates (2026-07-01)

The embedded SameBoy Core oracle is built and runs in-process (Tetris, DMG). Delivered:
- **Build:** `runtime/CMakeLists.txt` option `GBC_COSIM_SAMEBOY` (+`SAMEBOY_DIR`) builds all 21
  SameBoy `Core/*.c` + `runtime/src/sameboy_oracle.c` into `sameboy_core`, links it into gbrt,
  defines `GBC_HAVE_SAMEBOY`. Linker needed a local `getline` shim (mingw libmingwex lacks it;
  only SameBoy's debugger/interactive paths reference it — never the oracle).
- **Driver** `sameboy_oracle.c` (Beetle-isolation: only TU that sees SameBoy headers; compiled
  with `GB_INTERNAL`): `sb_oracle_create` (GB_init + load same BIOS+ROM + reset), `GB_run`
  stepping with the 8 MHz→T-cycle /2 map, `GB_get_registers`/`GB_get_direct_access` for the
  neutral extractor.
- **Neutral architectural hash** (`cosim_neutral.h`): CPU regs+IME + WRAM/VRAM/OAM/HRAM/cart-RAM,
  identical field order on both sides (`gb_cosim_neutral_hash` recomp / `sb_oracle_neutral_hash`).
- **CLI** `--oracle-selfcheck` and `--cosim-oracle` (both need `--boot-rom PATH`).

**HEADLINE RESULT (the DIV question, arbitrated):** SameBoy boots the same DMG BIOS and hands
off at **tcycle=23,440,344, DIV=0xAB**; our LLE boot hands off at **23,528,836, DIV=0x05**.
→ (1) SameBoy's DIV=0xAB **matches our HLE constant 0xABCC and the mooneye boot_div spec — our
HLE post-boot state was RIGHT.** (2) Boot durations differ by only **~88,492 cycles (~0.4%)** —
not a gross bug; the accumulated per-M-cycle timing imprecision (the known sub-instruction-timing
gap, `project_cpu_subinstruction_timing`). The oracle is now the standing arbiter for that.

**Lockstep = INSTRUCTION-COUNT aligned (fixed 2026-07-01).** Cycle-alignment is ill-posed here
(the very drift we hunt means the two are never at the same cycle+state), so `gb_run_sameboy_cosim`
now aligns on the **PC stream**: both execute the same instruction sequence from power-on; at
equal instruction counts they share PC+state until a timing-driven branch (a read of a cycle-
derived value like LY) goes differently. Implemented via SameBoy's `GB_set_execution_callback`
(fires per instruction) feeding a PC/DIV/LY FIFO (`sb_oracle_next_instruction`) that decouples
SameBoy's chunky `GB_run` from recomp's per-instruction stepping; HALT idle-ticks are skipped on
the recomp side to stay aligned. The first PC-stream mismatch is the first instruction where the
drift changes control flow.

**FIRST DIVERGENCE (Tetris/DMG, cycle-0 lockstep): instruction 30129 — recomp pc=006A vs
SameBoy pc=0064**, at fetch recomp LY=0x90 vs SameBoy LY=0x1E (DIV=0x47 on both). 0x0064 is the
BIOS frame-wait loop (`LD A,($FF44); CP $90; JR NZ`): recomp's LY already reached 0x90 and it
exited; SameBoy's is only 0x1E and still looping. Same DIV but different LY ⇒ the boot-timing
drift is **PPU scanline timing** (recomp's PPU runs ahead of SameBoy relative to the instruction
stream), NOT CPU cycle-charging — consistent with the known LCD-on/LY soft spot. This is the
oracle naming the first drifting instruction; the next step is to trace/fix the PPU LY-vs-cycle
timing in `ppu.c` against this coordinate. (Note: DIV high-byte alone doesn't pin the exact cycle
— capturing SameBoy's per-instruction cycle in the FIFO would sharpen the report further.)

### Phase B design notes + de-risk (2026-07-01)

**Build de-risk COMPLETE + POSITIVE.** All 21 SameBoy `Core/*.c` compile and archive into
`libsameboy_core.a` (421 KB) with this recipe (verified 2026-07-01):
`gcc -std=gnu11 -D_GNU_SOURCE -DGB_INTERNAL -DGB_VERSION='"cosim"' -I<SameBoy>/Core
-Wno-implicit-function-declaration -c`.
Do **NOT** pass the `-DGB_DISABLE_*` flags: SameBoy's `.c` files are preprocessed by its
custom **CPPP** tool (not standard cpp), so those flags drop struct members in `gb.h` while
leaving CPPP-guarded referencing code in the `.c` → gcc mismatch. Compiling the *full* Core
(all features on) avoids CPPP entirely; the extra features (cheats/sgb/printer/debugger)
just add unused code. The only mingw wrinkle is `getline`/`vasprintf` in debugger.c/gb.c
(interactive/debugger paths the oracle never calls) → `-Wno-implicit-function-declaration`
lets them compile and link against libmingwex. SameBoy Core is a static-lib target in our
build, no external build pipeline needed. API surface confirmed: `GB_init`,
`GB_load_rom_from_buffer`,
`GB_run` (returns cycles in **8 MHz units** = 2× our 4.19 MHz T-cycle — divide by 2 in
single speed to map onto `ctx->cycles`), `GB_get_registers` (AF/BC/DE/HL/SP/PC),
`GB_get_direct_access(RAM|CART_RAM|VRAM|HRAM|IO|OAM|BGP|OBP|IE)`, `GB_reset`.

**Comparison surface = ARCHITECTURAL only (design decision).** SameBoy's micro-arch state
(its `mode_cycles` convention, APU internal counters, PPU fetcher) is a *different
representation* of the same hardware behavior and will NOT bit-match our runtime even when
both are correct. So the cross-oracle hash compares only implementation-neutral
architectural state: CPU registers (A,F,B,C,D,E,H,L,SP,PC — must match), IME, and MEMORY
(WRAM, VRAM, OAM, HRAM, cart RAM) + the IO register *values* (LCDC/STAT/LY/SCX/SCY/DIV/
TIMA/…). This is a NEW neutral extractor, distinct from `gb_cosim_state_hash` (which is
representation-specific and only valid within pairing 1). Micro-arch fields are excluded
from the cross-oracle hash by necessity, not by hypothesis.

**OPEN DECISION — boot alignment (blocks the driver's run loop):** our recomp skips the
boot ROM (`gb_context_reset(ctx, skip_bootrom=true)` → post-boot state); SameBoy runs the
REAL boot ROM. So at cycle 0 the two are NOT aligned (the classic "boot diverges" problem,
PRINCIPLES.md). Options:
- (A) Both real-boot from a shared boot ROM (cleanest cycle-0 alignment; needs dmg_boot.bin/
  cgb_boot.bin AND the recomp to execute a boot ROM — support unverified).
- (B) Both post-boot: feed SameBoy a minimal handoff boot ROM matching our recomp's
  skip-boot end state (needs that state to match exactly).
- (C) Milestone alignment (no boot ROM): free-run both to a common architectural checkpoint
  (e.g. first VBlank / a known PC), snapshot + compare architectural state there, then
  lockstep forward — the PRINCIPLES.md "reproduce by playing, align by order+state" path.
  Weaker than cycle-0 lockstep but asset-free and robust.
SameBoy needs a boot ROM either way for (A)/(B); none are compiled in-tree (`BootROMs/*.asm`
need RGBDS). (C) sidesteps the asset. Decision pending user input.

Phase B (embedded SameBoy oracle — the Beetle pattern):
- [ ] Build SameBoy `Core/` as a static lib (`libsameboy_core.a`) in the cosim build.
- [ ] Driver `runtime/src/sameboy_oracle.c` (mirror `beetle_libretro.cpp`): `GB_init` /
      `GB_load_rom_from_buffer` / `GB_run` (T-cycle lockstep off returned cycles) +
      `GB_get_direct_access` for RAM/VRAM/OAM/IO/HRAM/CART_RAM + reach into `GB_gameboy_t`
      for PPU/APU/timer/DMA micro-state → fill the SAME `CosimSubHashes` layout as Phase A.
- [ ] Clean build target `gbc-cosim` (CMake `#ifdef GBC_COSIM`, heavy diagnostics OFF,
      single-threaded, headless — no SDL audio sink, no window) + minimal TCP server
      (`stride/step/chain/sub/regs/dev/window/inject/reset`), hooked into the T-cycle
      advance, RAM writes page-hashed.
- [ ] Coordinator `tools/gbc_cosim.py`: drive recomp + SameBoy oracle in T-cycle lockstep
      with a launch-fixed stride, first-divergence report. Sits alongside the existing
      `tools/compare_ground_truth.py` / `run_ground_truth.py` PyBoy harness.
- [ ] Pass SameBoy-vs-SameBoy Gate 2 (determinism, headless) before trusting the oracle.

## Build environment (from gb-recompiled/CLAUDE.md — do not relearn the hard way)

CMake/Ninja, msys2 mingw64 (GCC 15.2.0). devkitPro's cmake wins in PATH but can't find
the mingw64 compiler — always exclude it:

```bash
export CLEAN_PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:/c/Windows/system32:/c/Windows"
PATH="$CLEAN_PATH" /c/msys64/mingw64/bin/cmake.exe -G Ninja -B build \
    -DCMAKE_MAKE_PROGRAM=/c/msys64/mingw64/bin/ninja.exe \
    -DCMAKE_C_COMPILER=/c/msys64/mingw64/bin/gcc.exe \
    -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe .
PATH="$CLEAN_PATH" ninja -C build
```
ws2_32 is linked on Windows for TCP sockets (the debug server already does this — the
Phase-B TCP server reuses that). NOTE Rule 0 in `gb-recompiled/CLAUDE.md` (Ghidra-first)
governs *guest-behavior* work; co-sim tooling (state hashing, harness, build target) does
not touch guest semantics, but any fix the co-sim *localizes* re-enters that discipline.

## Pointers (verified in-tree, 2026-06-30 / 2026-07-01)

- Skeleton: `gb-recompiled/runtime/src/differential.c` (`gb_run_differential`,
  `gb_diff_compare_contexts` @427, `gb_diff_compare_ppu` @329); API
  `gb-recompiled/runtime/include/gbrt.h:744` (`GBDifferentialOptions`/`Result` @100/110).
- Second implementation: `gb-recompiled/runtime/src/interpreter.c`.
- Shared tick/clock model: `gb-recompiled/runtime/include/gb_timing.h` (shared by
  interpreter + emitter `recompiler/src/codegen/c_emitter.cpp`).
- CPU/machine state: `gbrt.h:164` (`GBContext`); double-speed tick split `gbrt.c:3650`;
  KEY1 `0xFF4D` @1857/2253/3982.
- PPU: `ppu.h:116` (`GBPPU`, palette RAM @179–181).
- APU + serializer: `audio.c:213` (`GBAudio`), `gb_audio_save_state` @527, frame sequencer
  @383, envelope @356, LFSR @340.
- Savestate serializer (AUDIT for timing fields): `gbrt.c` `GBSavestateCoreState` @197,
  `gbrt_capture_core_state` @734.
- Accuracy burndown (HYPOTHESES the co-sim grades): `gb-recompiled/ACCURACY.md`; Tetris bug
  hypotheses in `gb-recompiled/CLAUDE.md` ("Known Tetris Issues").
- External oracle: SameBoy, embedded as a linked libretro core (Beetle pattern). Core lib
  `F:\Projects\SameBoy\Core\` (`gb.h` `GB_gameboy_t`, `GB_init`/`GB_run`/`GB_get_direct_access`/
  `GB_load_rom_from_buffer`); libretro wrapper reference `F:\Projects\SameBoy\libretro\libretro.c`
  (see its `GB_get_direct_access` descs @592+ and `GB_run` lockstep @1407). PSX driver to
  mirror: `F:\Projects\psxrecomp\psxrecomp\runtime\src\beetle_libretro.cpp` + core at
  `beetle-psx\` (`libmednafen_psx.a`). PyBoy coverage-only ground truth:
  `tools/capture_ground_truth.py`, `GROUND_TRUTH_WORKFLOW.md`.
- Fixtures: Tetris attract (`gb-recompiled/roms/tetris.gb`, demo ~frame 600); Blargg
  `F:\Projects\gb-test-roms`; Mooneye/mealybug `F:\Projects\mealybug-tearoom-tests`.
- PSX gold standard to mirror: `F:\Projects\psxrecomp\_wt-tomba2\psxrecomp\COSIM_ORACLE.md`
  + `runtime/src/cosim.c` + `runtime/src/cosim_state.c` + `runtime/include/cosim_state.h` +
  `tools/cosim.py`.
