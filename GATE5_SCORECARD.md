# Gate-5 fixture sweep scorecard (2026-07-02)

Gate 5 (COSIM_ORACLE.md §"Validation gates"): run the recomp against Blargg +
mooneye fixtures as an independent third check. Captured AFTER the EI
one-instruction-delay + co-sim dispatch-boundary fix (commit af03ad1).

Harness (reusable, hang-proof):
- `tools/gate5_sweep.sh <frames> <name>=<rom> ...` — Blargg-style ROMs; reads the
  on-screen verdict (dumps the final frame as PNG).
- `tools/mooneye_sweep.sh <frames> <name>=<rom> ...` — mooneye acceptance ROMs;
  reads the Fibonacci register magic (PASS = B C D E H L = 03 05 08 0D 15 22) via
  `GBRT_REGS_LOG`, with a 25 s per-ROM `timeout` guard (mooneye LCD-off tests can
  busy-loop past a frame cap).

## Blargg (screen verdict)

| ROM              | Verdict            | Axis / note |
|------------------|--------------------|-------------|
| cpu_instrs (1-11)| **Passed all**     | CPU |
| 02-interrupts    | **Passed**         | interrupt/EI |
| instr_timing     | **Passed**         | instruction timing |
| interrupt_time   | **Passed**         | interrupt timing |
| halt_bug         | **Passed**         | HALT/interrupt |
| mem_timing       | Failed 2 (01,02)   | mem read/write timer align (#3) |
| mem_timing-2     | Failed 2 (01,02)   | same |
| oam_bug          | Failed #2 (02,04,05)| OAM-corruption emulation (separate) |

Every CPU/interrupt/EI/HALT behavioral test passes — the EI-delay fix introduced
zero regressions in the coarse-timing suite.

## Mooneye acceptance (Fibonacci register magic)

### interrupt / EI / DI / RETI subset
| ROM            | Result | Note |
|----------------|--------|------|
| ei_sequence    | FAIL   | strict sub-instruction interrupt-*cycle* timing |
| ei_timing      | FAIL   | " |
| rapid_di_ei    | FAIL   | " |
| di_timing-GS   | FAIL   | " |
| reti_timing    | HANG*  | run busy-looped past frame cap (LCD-off); needs cycle-cap harness |

These fail on the per-M-cycle interrupt-*dispatch* axis: our dispatch is
instruction-granular (`gb_handle_interrupts` at step/loop boundaries), so the
exact-T-cycle checks fail. The EI fix corrects the one-instruction *delay*
(architecture), which is a different property and is confirmed correct by blargg
`02-interrupts` + `instr_timing` passing. Not attributable to this session's fix
(pre-existing per-M-cycle limitation, same family as `mem_timing`).

### timer subset — DIAGNOSTIC for #3
| ROM                     | Result |
|-------------------------|--------|
| div_write               | PASS |
| tim00 / tim01 / tim10 / tim11 | PASS (all 4 TAC freqs) |
| tim00_div_trigger … tim11_div_trigger | PASS (all 4) |
| tima_reload             | FAIL |
| tima_write_reloading    | FAIL |
| tma_write_reloading     | FAIL |
| rapid_toggle            | FAIL |

**9/13 PASS.** All 4 failures cluster on the TIMA overflow→TMA reload window —
localizing the `mem_timing` 01/02 timer axis (#3) to the reload-window logic in
`gb_tick` + TIMA/TMA/TAC write handling. See MEM_TIMING_SCOPING.md
"Timer root-cause LOCALIZED".

### harness sanity (known-pass controls)
`bits/reg_f` PASS · `oam_dma/basic` PASS · `timer/tim01` PASS — confirms the
Fibonacci-magic detection is valid, so the FAILs above are real.

## Follow-ups surfaced
- **#3 timer reload window** — pinpointed to the 4 mooneye ROMs, then root-caused:
  a discrete write-interaction/TAC-glitch fix was prototyped and proven NOT
  sufficient (zero movement, no regression), so the real blocker is sub-instruction
  (per-M-cycle) reload-cycle precision — the architectural per-M-cycle timer model.
  See MEM_TIMING_SCOPING.md "Timer root-cause".
- **Harness**: add an instruction/cycle cap to `mooneye_sweep.sh` so LCD-off
  busy-loopers (e.g. reti_timing) report instead of hanging on the frame cap.
- **Per-M-cycle interrupt dispatch** — the mooneye ei_*/di_* axis; larger than this
  session's EI-delay fix, tracked with the per-M-cycle memory-timing family.
