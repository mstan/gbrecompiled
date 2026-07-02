# Per-M-cycle memory-read timing — scoping (2026-07-02)

Scoping for the residual divergence the SameBoy oracle pinned AFTER the LLE
power-on + first-line-after-enable fixes (see `COSIM_ORACLE.md`). This is NOT
yet an implementation — it is the analysis + options + verification plan.

## The bug (one sentence)

A cycle-derived I/O read that lands exactly on a PPU/timer boundary cycle (e.g.
`LDH A,($44)` reading `LY` as it ticks `0x8F→0x90`) samples the **post-boundary**
value in the recomp but the **pre-boundary** value on hardware, because the
recomp advances the whole instruction's clock (and thus the PPU) *before* the
inline bus read.

## Oracle coordinate (the thing a fix must move)

Tetris/DMG LLE lockstep, first divergence at **instr 61348**:
```
61345  LDH A,($44)  recomp cyc=613008  SameBoy cyc=613008   LY 8F/8F
61346               ...                                       LY 90/90  (both tick here)
61348  recomp pc=006A (exited)  SameBoy pc=0064 (looped)     dcyc=-4
```
Both PPUs tick `LY 8F→90` at the same cycle (613020). The `LDH A,($44)` at 61345
does its bus read at that boundary: recomp reads `0x90` (exits the wait loop),
SameBoy reads `0x8F` (loops once more). Success = the read samples `0x8F`, so the
divergence moves PAST 61348 (ideally into the cart's own code).

## Current architecture (both halves already exist)

The "tick-before-access" fix is already implemented and consistent across the
two code generators:

- **Interpreter** (`runtime/src/interpreter.c`): `MEM_TICK_BEFORE[256]` +
  `GB_OP_TIMING[opcode]` (`runtime/include/gb_timing.h`). Dispatch (≈L267-278):
  - `GB_ACC_RMW`  → `gb_tick(cycles - split_t)`, read, then `gb_tick(split_t)`
    before the write (`split_t = 4`: read one M-cycle before the write). ✅ correct.
  - `MEM_TICK_BEFORE[op]` (single READ/WRITE/IO/ALU-(HL)) → `gb_tick(cycles)`
    (the WHOLE instruction), then the inline read. ← **read placed at the
    instruction's FINAL cycle.**
- **Generated code** (`recompiler/src/codegen/c_emitter.cpp` ≈L1239-1255, L2249):
  `tick_before_access` / `tick_rmw_split` — same policy, emitted into bank C.
- `gb_tick(ctx, uint32_t)` accepts arbitrary T-cycle counts (not just multiples
  of 4), and `gb_read8` → `gb_sync` advances the PPU to `ctx->cycles` before the
  register read. So sub-M-cycle tick splits are already mechanically possible.

The authors flagged this exact limitation in `interpreter.c:62-78` and
`gb_timing.h:23`: READ/WRITE are "correct under tick-before" only at
whole-instruction granularity; the read is registered at the instruction's first
cycle without tick-before and at its **last** cycle with it. Neither is the true
mid-instruction M-cycle.

## Why the whole-instruction tick-before is ~1-3 T-cycles late

`LDH A,($44)` = 12 T-cycles / 3 M-cycles: M1 fetch op (0-4), M2 fetch n (4-8),
M3 read `$FF44` (8-12). Hardware latches the read late in M3 and the PPU's
`LY` increment for the line boundary lands on the same last cycle — hardware
orders **read-before-increment**. The recomp does `gb_tick(12)` (PPU advances
through the boundary → `LY=0x90`) and *then* reads → sees `0x90`. So it is the
within-last-cycle ORDER (and ~1 T-cycle of phase) that is inverted, not the
M-cycle.

## Options

### A. M-cycle split for READ/WRITE (mirror RMW) — REJECTED by analysis
`gb_tick(cycles-4)`, read, `gb_tick(4)` → read at M3-*start* (cycle 8). Hardware
is at ~cycle 11 → this is **3 cycles early** vs the current 1 late. Moves the
error, doesn't fix it. (Documented here so we don't try it.)

### B. T-cycle-precise read split — plausible, needs a per-op offset
`gb_tick(cycles - N)`, read, `gb_tick(N)` with `N` = T-cycles from the read latch
to the instruction end (small, e.g. 1-2). Extend `GBOpTiming` so `GB_ACC_READ`/
`GB_ACC_WRITE` carry a `split_t` (currently 0/unused for them), set it per-opcode,
and change both halves to honor it for READ/WRITE (not just RMW). Effort: small
code change in 2 places; the RISK/COST is *determining `N`* — it is dot-precise
and must be tuned against the oracle, and may differ for 2-M-cycle (`LD A,(HL)`,
8 cyc) vs 3-M-cycle (`LDH A,(n)`, 12 cyc) vs 4-M-cycle (`LD A,(nn)`, 16 cyc) loads.

### C. PPU-side boundary-ordering (read-before-increment) — most faithful
Keep tick-before-all, but make the PPU's `LY`-increment (and the coupled mode /
`LYC=LY` / `STAT` / and the timer `DIV`/`TIMA` boundary events) register so that a
read landing on the boundary cycle sees the PRE-increment value — i.e. the CPU
read wins the race on the shared cycle, matching hardware. Concretely: when
`ppu_tick`/`timer` cross a boundary exactly at `ctx->cycles`, expose the
"about-to-increment" value to reads for that one cycle. This is closest to the
real hardware race and fixes ALL cycle-derived reads at once (LY, STAT mode,
LYC-match, DIV, TIMA) rather than per-opcode. Effort: larger and more invasive
(touches `ppu.c` + timer + `gb_sync` ordering), but no per-opcode tuning and no
duplicated policy in two code generators.

## Recommendation

Prototype **B** first as the cheapest experiment purely to LEARN `N` against the
oracle (it is a 2-site change gated to `GB_ACC_READ`), because it directly answers
"is this a clean ~1-2 T-cycle phase fix?" If `N` turns out uniform (e.g. always
the last T-cycle), B is a clean, low-risk fix. If `N` is messy / per-context, that
is the signal to invest in **C** (the read-before-increment race), which is the
faithful model and fixes the whole class (STAT/LYC/DIV/TIMA reads too), at the
cost of a deeper `ppu.c`+timer+`gb_sync` change.

Either way: verify with `--cosim-oracle --boot-rom dmg_boot.bin` (divergence must
move past 61348), then re-run the 8 pairing-1 gates (HLE chain must stay
`1CB1212F869F05F6`) + the blargg `mem_timing` fixture (this is the same gap it
tests — [[project_cpu_subinstruction_timing]]) + the LLE-vs-HLE boot gate. Apply
to BOTH the interpreter and `c_emitter.cpp` (production runs generated code).

## Option-B prototype results (2026-07-02) — mechanism validated, plus a key architectural find

Prototyped the T-cycle read split in the interpreter only (`GB_ACC_READ` ops:
`gb_tick(cycles - N)`, read, `gb_tick(N)`), tuned `N` against the oracle:

- **`N=1`**: NO change (divergence still 61348). The LY boundary lands ≤1 T-cycle
  before the instruction end AND is not instruction-aligned, so a read at +11 of a
  12-cycle op still catches it. → a *fixed* `N` is inherently fragile.
- **`N=4`** (read at the last M-cycle's START): **divergence jumped 61348 →
  2,266,273** — out of the shared BIOS entirely and into cart (Tetris) code. The
  new split is `recomp pc=01FD vs SameBoy pc=0040` — **0x0040 is the VBlank vector**,
  i.e. an interrupt-*dispatch* timing split, a different class. So the read-split
  MECHANISM clearly works and is the right direction.

**But two things the prototype revealed that change the plan:**
1. **Must apply to BOTH backends together.** With only the interpreter changed, the
   pairing-1 **A-vs-B** gate splits (`last_sync_cycles 98960 != 98956`, Δ=N) —
   generated code still ticks-before-all. Correctness aside, the two backends must
   move in lockstep. So B is a two-file change (interpreter + `c_emitter.cpp`),
   landed atomically.
2. **`gb_tick` does NOT sync the PPU (lazy sync).** The PPU only advances inside
   `gb_read8`/`gb_write8`→`gb_sync`. So `gb_tick(N)` after the read advances
   `ctx->cycles` (and DIV/TIMA) but leaves the PPU synced only to `start+cycles-N`
   — the PPU is **N cycles behind at the instruction boundary**. That is the
   `last_sync_cycles` mismatch, and it means a VBlank in an instruction's last N
   cycles sets `IF` late → **this is almost certainly the 2,266,273 VBlank split**.
   The baseline `tick-before-all` load avoids this because its read syncs to the
   full `start+cycles`.

**Consequence:** the *complete* correct fix is **B + a boundary sync** — after the
split read, catch the PPU up to the full instruction cycle so interrupt checks at
the boundary see current PPU/IF state. `gb_sync` is `static` in `gbrt.c`, so this
needs a small public `gb_hw_sync(GBContext*)` (or equivalent) exposed and called by
both backends. That boundary-sync is really the Option-C "sync-before-observe"
insight arriving through the B prototype: precise interrupt timing needs the PPU
synced before every interrupt-recognition point, not just at memory accesses (the
baseline lazy-sync already approximates this for register-only instruction runs).

**Revised recommendation:** implement B (read split, `N=4` = last-M-cycle-start,
per-op-length aware if needed) in BOTH `interpreter.c` and `c_emitter.cpp`, PLUS a
public boundary PPU sync, landed together; then re-check the oracle (expect the
2,266,273 VBlank split to move or change class), the 8 gates (A-vs-B must
re-converge), blargg `mem_timing`, and the boot gate. Treat the VBlank/interrupt
timing as the next distinct axis once reads are clean. This is a bigger, more
architectural change than a targeted PPU patch — it touches the shared tick/sync
model used by every instruction in both backends.

## RESOLUTION (2026-07-02) — Option C shipped for LY/STAT; B rejected; timer deferred

Implemented and verified on branch `feat/mem-read-race` (off the clean checkpoint
`feat/differential-cosim @ 347ed56`). Two commits: LY race, then STAT race.

**What shipped (Option C, address-scoped read-before-increment race):**
- The fix lives in the READ path (`gb_read8`) + PPU edge tracking, NOT the tick
  model. A CPU read of a cycle-derived PPU register (`LY` 0xFF44, `STAT` 0xFF41)
  whose read M-cycle (last 4 T) coincides with the register's edge returns the
  PRE-edge value ("read wins the race"). We keep the true value in `ppu->ly/stat`
  (+ `ctx->io`) for internal state/hashing, and remember `{ly_prev,
  ly_change_cycle}` / `{stat_prev, stat_change_cycle}` (edge cycle =
  `ctx->cycles - mode_cycles`, recorded at the `ppu_tick` transitions /
  `update_stat`). `gb_read8` serves the raced value when
  `change_cycle + 4 > ctx->cycles`.
- Because it is ADDRESS-SCOPED to the PPU registers, the timer is untouched — so
  it does NOT regress `mem_timing` the way the fixed read-split (Option B) did.

**Verified (Tetris/DMG, LLE from cycle 0):**
- SameBoy oracle first-divergence **61348 → 2,266,273** (identical to Option B's
  win, but with no tick-model change) — out of the BIOS into cart code; the new
  split is `recomp pc=01FD vs SameBoy pc=0040`, the **VBlank-vector interrupt-
  dispatch axis** (a separate, next bug).
- LLE boot handoff vs SameBoy: **+140,140 cyc → +20 cyc**; DIV **0xCF → 0xAB**
  (matches SameBoy + mooneye boot_div).
- blargg `mem_timing`: **Failed 2 (modify_timing ok)** — unchanged from baseline,
  NO regression (Option B was Failed 3).
- 8 pairing-1 gates pass, A==B, HLE full-state chain preserved `1CB1212F869F05F6`.
- LLE-vs-HLE boot gate: 0 CPU-handoff diffs.

**Option B (fixed read-split, tick `cycles-4` for all reads) — REJECTED.** It hit
the same oracle/boot win but regressed `mem_timing` subtest 3 (`modify_timing`
ok→fail): reading the timer 4 cycles early corrupts the timer-based measurement.
A fixed read offset cannot satisfy both the PPU LY race and the timer. Isolation
confirmed the regression is the read-split itself (not the boundary sync).

**Timer (DIV/TIMA) race — ATTEMPTED, BACKED OUT, DEFERRED.** A surgical TIMA/DIV
read race (return prev only on coincidence) was tried on top of LY/STAT. It
**regressed `modify_timing` (Failed 2 → 3) and did NOT fix read/write timing**
(subtests 1/2 stayed 81). Conclusion: `mem_timing` read/write failures are NOT a
read-M-cycle-offset problem — they are a deeper timer-alignment issue (likely the
TIMA increment cycle / reload timing). The read-before-increment race is the
wrong tool for the timer. This is a SEPARATE axis, not pointed to by the current
oracle coordinate (which is now the VBlank interrupt-dispatch split at 2,266,273).
Left for a focused follow-up gated on `mem_timing` read/write root-cause.

**CGB caveat:** the edge-cycle math (`ctx->cycles - mode_cycles`, `+4` window) is
in CPU-T-cycle units and exact for single-speed DMG. CGB double-speed re-verify
(MMX2 + cgb_boot.bin) is still pending.

## Out of scope / watch-outs

- Do NOT tune the first-line-after-enable value to mask this (different bug).
- WRITE ops to IO (e.g. `LDH ($44),A`? — LY is read-only, but `STAT`/`LYC`/`DIV`
  writes) have the same boundary-race question; C covers them, B would need the
  WRITE split too.
- CGB double-speed: the read/increment race is in CPU-cycle units; re-verify MMX2
  once DMG is clean.
