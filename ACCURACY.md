# GameBoy Recompiler — Accuracy Report

> Generated: 2026-03-17  
> Recompiler version: `gbrecomp` at `build/bin/gbrecomp`  
> Test suites: [Mooneye MTS 2024-09-26](https://github.com/Gekkio/mooneye-test-suite), [Blargg GB Test ROMs](https://github.com/retrio/gb-test-roms)

## Summary

| Suite | Passed | Total | Pass Rate |
|-------|--------|-------|-----------|
| Blargg | 3 | 8 | 38% |
| Mooneye acceptance | 25 | 62 | 40% |
| **Total** | **28** | **70** | **40%** |

---

## Blargg CPU / Timing Tests

These ROMs output ASCII text via the serial port. "Passed" in the output = pass.

| Test | Result | Serial output |
|------|--------|---------------|
| 01-special | ✅ PASS | 01-special ·  ·  · Passed |
| cpu_instrs | ✅ PASS | cpu_instrs ·  · 01:ok  02:ok  03:ok  04:ok  05:ok  06:ok  07:ok  08:ok  09:ok  1 |
| instr_timing | ✅ PASS | instr_timing ·  ·  · Passed |
| halt_bug | ❌ FAIL |  |
| interrupt_time | ❌ FAIL |  |
| mem_timing-1 | ❌ FAIL | mem_timing ·  · 01:01  02:01  03:01   ·  · Failed 3 tests. |
| mem_timing-2 | ❌ FAIL |  |
| oam_bug | ❌ FAIL |  |

---

## Mooneye Acceptance Tests

Mooneye tests signal pass by writing the Fibonacci sequence `03 05 08 0D 15 22` to the serial port.
Tests marked **GS** target DMG/SGB hardware specifically.

### bits
| Test | Result | Notes |
|------|--------|-------|
| mem_oam | ✅ PASS |  |
| reg_f | ✅ PASS |  |
| unused_hwio-GS | ❌ FAIL |  |

### instructions
| Test | Result | Notes |
|------|--------|-------|
| daa | ✅ PASS |  |

### interrupts
| Test | Result | Notes |
|------|--------|-------|
| ie_push | ❌ FAIL |  |

### OAM DMA
| Test | Result | Notes |
|------|--------|-------|
| basic | ✅ PASS |  |
| oam_dma_restart | ❌ FAIL |  |
| oam_dma_start | ❌ FAIL |  |
| oam_dma_timing | ❌ FAIL |  |
| reg_read | ❌ FAIL |  |
| sources-GS | ❌ FAIL |  |

### PPU
| Test | Result | Notes |
|------|--------|-------|
| stat_irq_blocking | ✅ PASS |  |
| hblank_ly_scx_timing-GS | ❌ FAIL |  |
| intr_1_2_timing-GS | ❌ FAIL |  |
| intr_2_0_timing | ❌ FAIL |  |
| intr_2_mode0_timing | ❌ FAIL |  |
| intr_2_mode0_timing_sprites | ❌ FAIL |  |
| intr_2_mode3_timing | ❌ FAIL |  |
| intr_2_oam_ok_timing | ❌ FAIL |  |
| lcdon_timing-GS | ❌ FAIL |  |
| lcdon_write_timing-GS | ❌ FAIL |  |
| stat_lyc_onoff | ❌ FAIL |  |
| vblank_stat_intr-GS | ❌ FAIL |  |

### Timer
| Test | Result | Notes |
|------|--------|-------|
| div_write | ✅ PASS |  |
| tim00 | ✅ PASS |  |
| tim00_div_trigger | ✅ PASS |  |
| tim01 | ✅ PASS |  |
| tim01_div_trigger | ✅ PASS |  |
| tim10 | ✅ PASS |  |
| tim10_div_trigger | ✅ PASS |  |
| tim11 | ✅ PASS |  |
| tim11_div_trigger | ✅ PASS |  |
| rapid_toggle | ❌ FAIL |  |
| tima_reload | ❌ FAIL |  |
| tima_write_reloading | ❌ FAIL |  |
| tma_write_reloading | ❌ FAIL |  |

### Misc timing
| Test | Result | Notes |
|------|--------|-------|
| di_timing-GS | ✅ PASS |  |
| div_timing | ✅ PASS |  |
| ei_sequence | ✅ PASS |  |
| ei_timing | ✅ PASS |  |
| halt_ime0_ei | ✅ PASS |  |
| halt_ime0_nointr_timing | ✅ PASS |  |
| halt_ime1_timing | ✅ PASS |  |
| halt_ime1_timing2-GS | ✅ PASS |  |
| intr_timing | ✅ PASS |  |
| rapid_di_ei | ✅ PASS |  |
| reti_intr_timing | ✅ PASS |  |
| add_sp_e_timing | ❌ FAIL |  |
| call_cc_timing | ❌ FAIL |  |
| call_cc_timing2 | ❌ FAIL |  |
| call_timing | ❌ FAIL |  |
| call_timing2 | ❌ FAIL |  |
| if_ie_registers | ❌ FAIL |  |
| jp_cc_timing | ❌ FAIL |  |
| jp_timing | ❌ FAIL |  |
| ld_hl_sp_e_timing | ❌ FAIL |  |
| pop_timing | ❌ FAIL |  |
| push_timing | ❌ FAIL |  |
| ret_cc_timing | ❌ FAIL |  |
| ret_timing | ❌ FAIL |  |
| reti_timing | ❌ FAIL |  |
| rst_timing | ❌ FAIL |  |

---

## Known Limitations

- **Cycle-exact timing**: The static recompiler batches instructions between sync points; cycle-exact Mooneye timing tests (call/ret/jp/push/pop timing, TIMA edge cases) may fail even when the interpreter is otherwise correct.
- **Boot ROM**: Tests that check post-boot register state (`boot_regs-*`, `boot_div-*`, `boot_hwio-*`) are skipped — the recompiler does not emulate the boot ROM.
- **CGB-only tests**: Any test suffixed `-C` (CGB-only mode) is expected to fail on this DMG-targeted recompiler.
- **mem_timing / halt_bug / oam_bug / interrupt_time**: These Blargg tests exercise edge cases around memory access timing, the HALT bug, OAM corruption, and interrupt timing that the current runtime does not fully model.
- **PPU sub-mode timing**: Most Mooneye PPU tests check interrupt timing at exact T-cycle boundaries; the PPU emulation does not currently reach that granularity.
