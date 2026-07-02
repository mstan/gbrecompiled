# Per-M-cycle (cycle-exact) timing initiative — scoping (2026-07-02)

## Thesis: four open divergences, one root

Every remaining co-sim/accuracy divergence reduces to the SAME root — hardware
(PPU LY/STAT, timer TIMA/DIV/IF) advances and is sampled/mutated at
whole-instruction granularity, not at the exact sub-instruction M-cycle:

| Divergence | Where it bites |
|-----------|----------------|
| #3 `mem_timing` 01/02 + 4 mooneye timer ROMs | TIMA overflow→reload window read/written at wrong cycle |
| DMG oracle split @ instr 2,266,364 | PPU LY read in HRAM OAM-DMA wait loop (LY 8F/90 phase) |
| MMX2/CGB oracle split @ instr 81,603 | PPU LY reaches VBlank line late → VBlank-IF wait loop exits late |
| mooneye `ei_*` / `di_timing` / `reti_*` | timer/PPU sets IF at the wrong cycle → recognized one boundary off |

Key unification: SM83 checks interrupts at INSTRUCTION boundaries (real hardware
too), so the interrupt tests are NOT a separate "dispatch timing" axis — they fail
because IF is SET at the wrong cycle (hardware advanced at instruction
granularity). Fix the cycle at which hardware advances and is accessed, and all
four move together. The EI one-instruction delay (fixed 2026-07-02, af03ad1) and
the interrupt dispatch boundary are already correct; what remains is cycle-exact
hardware.

## What already exists (the initiative is a refinement, not a rewrite)

Per the cycle-accuracy decision ([[project_cycle_accuracy_decision]]): the core is
cycle-accurate by default; this is a targeted tick-before refinement + per-region
opt-out, NOT a rewrite.

- `runtime/include/gb_timing.h`: `GB_OP_TIMING[256]` / `GB_CB_TIMING[256]` —
  per-opcode data-bus access kind (`GB_ACC_READ/WRITE/RMW`) + `split_t`. Complete
  and reviewed.
- **RMW split is LIVE** in `interpreter.c` (L267-275, 784-825) and
  `c_emitter.cpp`: `gb_tick(cycles - split_t)`, read, `gb_tick(split_t)`, write.
  This is why blargg `mem_timing` subtest 03 (`modify_timing`) PASSES.
- Single READ/WRITE opcodes use `MEM_TICK_BEFORE[256]`: `gb_tick(whole)` then the
  inline access — access sampled at the instruction's FINAL T-cycle.
- LY/STAT read-before-increment race: an address-scoped patch in `gb_read8`
  (`race_win` window) returning the pre-edge value. CGB double-speed unit fixed
  2026-07-02 (1d977d3).
- Timer: `gb_tick` advances DIV/TIMA with a cycle-accurate falling-edge loop and a
  `tima_reload_pending` countdown, but the reload EFFECT and TIMA/IF reads land at
  the enclosing `gb_tick` boundary.

## The precise gap

1. **Single READs are sampled ~4 T too late.** Hardware latches a read at the
   START of its access M-cycle; `MEM_TICK_BEFORE` ticks the WHOLE instruction then
   reads (final T-cycle). For a read of a counting register (LY/DIV/TIMA/STAT) this
   is up to one M-cycle late — exactly what the LY/STAT race patches around for two
   addresses, and what `read_timing` (01) probes generally.
2. **WRITES land at the last-M-cycle end (roughly right for the value) but their
   EFFECT on hardware** (e.g. a TAC/DIV/TIMA write mid-instruction) is applied at
   the boundary, so write-vs-hardware-event ordering (`write_timing` 02, the timer
   reload-window writes) is wrong.
3. **Timer reload window is whole-tick.** TIMA reads 0x00 for 4 T then reloads TMA
   + sets IF; at whole-`gb_tick` granularity, reads/writes inside that window and
   the IF edge do not fall on the exact cycles the mooneye timer ROMs sample.

## Why the prior surgical attempts failed (do not repeat)

- **Option B (fixed `cycles-4` for ALL reads)** — regressed `modify_timing`
  (03 ok→fail). A crude global split double-perturbs the already-correct RMW path
  and mis-offsets the timer. The fix must be PER-OPCODE-KIND (reuse `GB_OP_TIMING`),
  never a blanket offset.
- **LY/STAT race patch** — correct but a two-address SUBSET of the general read
  timing; it will be SUBSUMED by proper read-M-cycle sampling and then retired.
- **Discrete timer write-interaction fix (2026-07-02, reverted)** — TAC glitch +
  TIMA write-cancel + TMA fresh-latch produced ZERO movement on the 4 timer ROMs:
  necessary but insufficient WITHOUT cycle-exact reload. Re-apply only ON TOP of a
  cycle-exact reload.

## Mechanism

Extend the split the RMW path already uses to READ and WRITE, driven by
`GB_OP_TIMING` (never a blanket offset):

- `GB_ACC_READ`: `gb_tick(cycles - 4)` → read at the access-M-cycle start →
  `gb_tick(4)`. (Retires the LY/STAT race for FF44/FF41; generalizes to DIV/TIMA.)
- `GB_ACC_WRITE`: `gb_tick(cycles - 4)` → write → `gb_tick(4)`, so the write's
  hardware effect is ordered at the write M-cycle. (Validate the exact edge vs
  Gekkio; write may be last-T — tune per the oracle, per-opcode, not globally.)
- Timer: make the reload a cycle-exact event (schedule the TMA-latch + IF-set at
  `overflow_cycle + 4`, consulted by reads/writes at their now-exact cycle), then
  re-apply the reverted write-interaction + TAC-glitch behaviors.
- **Both backends atomically** each phase (interpreter + `c_emitter.cpp`), or the
  A-vs-B gate splits. CGB double-speed: window is in SYSTEM cycles (2 not 4) — the
  `race_win` precedent applies.

## Phased plan — each phase gated, `modify_timing` is the tripwire

Every phase: land interpreter + emitter together; regen+build; then run the gate
set below. If blargg `mem_timing` subtest 03 regresses to Failed 3, STOP — that is
the exact prior-failure signature.

- **Phase 0 — measurement.** Extend the SameBoy oracle diff to a per-M-cycle mode
  (the SBWIN ring already carries IME/IF/IE as of af03ad1; add per-access cycle
  logging). Deliver: each fix measured against SameBoy, not a pass/fail bit.
- **Phase 1 — read M-cycle sampling** (`GB_ACC_READ` split). Target: `read_timing`
  (01) → ok; DMG oracle 2,266,364 advances; retire LY/STAT race. Gate: 01 ok, 03
  still ok, oracle advances, 8 pairing gates + boot gate + A-vs-B all green.
- **Phase 2 — write M-cycle ordering** (`GB_ACC_WRITE` split). Target:
  `write_timing` (02) → ok. Gate as Phase 1.
- **Phase 3 — cycle-exact timer reload + write interactions.** Re-apply the
  reverted TIMA/TMA/TAC logic on top of a cycle-exact reload. Target: all 4 mooneye
  timer ROMs green; blargg `mem_timing` all ok. Gate: no `modify_timing` regress.
- **Phase 4 — verify interrupt tests fall out.** mooneye `ei_*`/`di_timing`/
  `reti_*` should PASS once IF is set at the exact cycle (no dispatch change
  expected). If any remain, investigate as a genuinely separate sub-axis.
- **Phase 5 — PPU sub-scanline** (DMG 2,266,364 / MMX2 81,603). Likely largely
  resolved by Phase 1 (LY read) + existing segmented mid-scanline render; residual
  is the PPU LY-increment ordering ([[project_ppu_mealybug_scorecard]]).
- **Phase 6 — CGB double-speed + performance.** Re-verify MMX2 + cgb_boot across
  all phases (double-speed cycle units); confirm the per-region tick-batch opt-out
  still applies to hot paths touching no timing-sensitive hardware; full
  regen/rebuild/gate sweep + re-pin all baselines.

## Phase 0 findings (2026-07-02) — measured, plan refined

Extended the SameBoy oracle to expose its internal 16-bit divider
(`sb_oracle_last_div16`; SBWIN now prints `div16 r/o`). Ran the `mem_timing` ROM
through the DMG oracle (LLE). First divergence: instr 2,492,083 (recomp pc=C2BA
vs SameBoy C2C9, dcyc=+4), a WRAM test routine that reads DIV and branches.

**Root: a constant +8 DIV-counter phase offset.** SameBoy's `div_counter` is
exactly 8 ahead of recomp's at EVERY instruction in the window (D6C8/D6D0,
D6FC/D704, …). At the split it straddles the 0x700 high-byte boundary → recomp
DIV reg=D6, SameBoy=D7 → the read-DIV test branches differently. Not a race, not a
read-M-cycle offset — a fixed divider phase error.

**SameBoy arbitrates the boot-DIV signal.** The boot gate long flagged LLE
`div_counter`=ABC4 vs HLE=ABCC (Δ8). SameBoy matches the HLE phase (ABCC), so the
**LLE boot accumulates 8 too few DIV cycles** — an LLE-boot-timing bug, and the
direct cause of this oracle divergence.

**Consequences for the plan:**
- The blanket read-M-cycle split (old Phase 1) is the WRONG tool for the
  `mem_timing` oracle divergence — sampling reads earlier would worsen a phase
  error. Reordered: fix DIV/timer phase FIRST.
- **The oracle (LLE) and production (HLE) have DIFFERENT DIV phases** (ABC4 vs
  ABCC). Production `mem_timing` 01/02 fails under HLE (the CORRECT phase), so its
  cause is NOT this LLE +8 artifact — it needs measurement against the HLE path
  (or fix the LLE boot DIV so the oracle matches production, then re-measure).
- Revised Phase 1 = **DIV/timer phase calibration**: (a) find where LLE boot loses
  8 DIV cycles (makes the oracle honest), then (b) re-measure production
  `read_timing` against the now-aligned oracle to see the REAL read-timing gap.

## Phase 1a DONE — DMG power-on divider = 8 (2026-07-02)

Traced the +8 DIV offset to its origin: at instruction 1, cycle 0 (before any
instruction executes), recomp `div16=0000` but SameBoy `div16=0008`. The entire
offset is the **power-on internal divider value** — the divider has advanced 8
T-cycles before the CPU begins the boot ROM. Every cycle after tracks perfectly.

Fix (gbrt.c LLE power-on): `div_counter = cgb ? 0 : 8`. Verified on tetris (DMG):
- Boot gate DIV **ABC4 → ABCC**, now == HLE == SameBoy: the long-standing
  boot-gate DIV fidelity signal is RESOLVED.
- A-vs-B chain unchanged (`E92927C083145FD7`); 8/8 pairing gates; HLE untouched
  (production already used ABCC).
- Tetris oracle still 2,266,364 (that split is the PPU sub-scanline axis, correctly
  independent of DIV).

**Phase 1b DONE: CGB power-on divider is ALSO 8.** Traced MMX2 (real CGB game,
cgb_boot.bin) at cycle 0: recomp div16=0000 vs SameBoy 0008 — identical to DMG.
Set the LLE power-on divider to 0x0008 unconditionally (both models). Verified:
CGB cycle-0 offset now +0; MMX2 oracle divergence unchanged at 81603 (PPU LY
phase, DIV already matched there); MMX2 A-vs-B chain unchanged
(B02E9D35794D298E); tetris DMG unchanged (boot gate ABCC, A-vs-B
E92927C083145FD7). LLE-only change → all HLE production baselines untouched.

Now the oracle (LLE) shares production's (HLE) DIV phase, making it honest for the
REAL read/write-timing measurement. The mem_timing split at 2,492,083 persisted
with DIV matching — that residual is the genuine read-timing gap, the next target
(Phase 1 body). NOTE: the HLE CGB post-boot div constant (reset path, CGB branch =
0x0000) is a SEPARATE question (CGB HLE boot-div accuracy) not addressed here.

## Risks / watch-outs

- **A-vs-B determinism**: interpreter and emitter MUST move together each phase.
- **`modify_timing` tripwire**: gate every phase on subtest 03 staying ok.
- **Performance**: split ticks add per-access overhead; the per-region opt-out
  (batch to whole-instruction where no timing-sensitive HW is touched) contains it.
- **CGB double-speed units**: windows/splits are in SYSTEM cycles under
  double-speed (`race_win` = 2). Re-verify on MMX2.
- **Do NOT** tune the PPU first-line-after-enable value to mask any of this.

## Validation gate set (run every phase)

```
# oracle (DMG + CGB) — first-divergence must advance, never regress
tetris.exe    --cosim-oracle --boot-rom dmg_boot.bin --cosim-checkpoints 6000000
megaman_xtreme2.exe --cosim-oracle --boot-rom cgb_boot.bin --cosim-checkpoints 6000000
# pairing + boot gates
python tools/gbc_cosim.py --exe tetris.exe --checkpoints 300      # 8/8, chain 1CB1212F869F05F6
tetris.exe --boot-gate --boot-rom dmg_boot.bin                    # 0 diffs
# A-vs-B baselines (re-pin only on intended behavior change)
python tools/gbc_cosim.py --exe tetris.exe --ab-frames 700 --expect-chain E92927C083145FD7
python tools/gbc_cosim.py --exe megaman_xtreme2.exe --ab-frames 1000 --expect-chain B02E9D35794D298E
# blargg (TRIPWIRE: mem_timing must stay >= current; 03 must stay ok)
build+run mem_timing / mem_timing-2 / instr_timing   (read screens)
# mooneye timer + interrupt subsets (tools/mooneye_sweep.sh)
```
