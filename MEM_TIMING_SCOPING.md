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

## Out of scope / watch-outs

- Do NOT tune the first-line-after-enable value to mask this (different bug).
- WRITE ops to IO (e.g. `LDH ($44),A`? — LY is read-only, but `STAT`/`LYC`/`DIV`
  writes) have the same boundary-race question; C covers them, B would need the
  WRITE split too.
- CGB double-speed: the read/increment race is in CPU-cycle units; re-verify MMX2
  once DMG is clean.
