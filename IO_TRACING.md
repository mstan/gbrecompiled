# GB I/O Register Tracing Guide

## Overview

I/O tracing logs all reads and writes to GameBoy hardware registers (0xFF00–0xFF7F) that
the game performs. This is invaluable for diagnosing black screens, missing audio, broken
rendering, and hangs where the game's hardware interactions are silently failing.

On a real GameBoy, these addresses correspond to the PPU, APU, timer, serial, joypad, and
interrupt controller. In our runtime, missing or incorrect behavior here causes game-specific
bugs. Tracing reveals exactly which registers the game expects to interact with, and in what order.

---

## How to Enable I/O Tracing

In `runtime/src/gbrt.c`, the `gb_write8` and `gb_read8` functions handle all I/O register access.
Add a trace log around the `0xFF00–0xFF7F` dispatch in each:

```c
/* I/O Trace — enable with GB_IO_TRACE=1 env var or --io-trace flag */
static FILE* g_io_trace_file = NULL;

static void io_trace(const char* op, uint16_t addr, uint8_t value) {
    if (!g_io_trace_file) return;
    fprintf(g_io_trace_file, "%s,0x%04X,0x%02X\n", op, addr, value);
}
```

Add `io_trace("R", addr, result)` and `io_trace("W", addr, value)` in the I/O dispatch paths.

Output: `logs/io_trace.log` — CSV with columns OP, ADDR, VALUE.

---

## Key Registers to Watch

### PPU (0xFF40–0xFF4B)

| Address | Register | Notes |
|---------|----------|-------|
| 0xFF40 | LCDC | Bit 7 = LCD enable — toggling this during line-clear is a common bug source |
| 0xFF41 | STAT | Mode flags + interrupt enables — bits 3–6 trigger STAT interrupt |
| 0xFF42 | SCY | Background scroll Y |
| 0xFF43 | SCX | Background scroll X |
| 0xFF44 | LY | Current scanline (read-only) — games poll this to sync with display |
| 0xFF45 | LYC | LY compare — triggers STAT interrupt when LY==LYC |
| 0xFF46 | DMA | OAM DMA source (value × 0x100) |
| 0xFF47 | BGP | Background palette |
| 0xFF48 | OBP0 | Sprite palette 0 |
| 0xFF49 | OBP1 | Sprite palette 1 |
| 0xFF4A | WY | Window Y position |
| 0xFF4B | WX | Window X position (+7) |

### Interrupts (0xFF0F, 0xFFFF)

| Address | Register | Notes |
|---------|----------|-------|
| 0xFF0F | IF | Interrupt Flag — game writes here to acknowledge interrupts |
| 0xFFFF | IE | Interrupt Enable — which interrupts are unmasked |

**Critical**: If IF is not cleared after an interrupt handler runs, the interrupt fires again
immediately, causing an infinite loop. Verify that generated interrupt handlers write 0 to
the appropriate IF bit.

### Timer (0xFF03–0xFF07)

| Address | Register | Notes |
|---------|----------|-------|
| 0xFF04 | DIV | Divider — resets to 0 on any write; increments at 16384 Hz |
| 0xFF05 | TIMA | Timer counter — game polls or uses timer interrupt |
| 0xFF06 | TMA | Timer modulo — TIMA reloads from this on overflow |
| 0xFF07 | TAC | Timer control — bit 2 = enable, bits 0–1 = clock select |

Games that use timer interrupts (0x0050) require accurate TIMA behavior. A timer that
never overflows will hang any game that waits for it.

### Joypad (0xFF00)

| Address | Register | Notes |
|---------|----------|-------|
| 0xFF00 | P1/JOYP | Write to select D-pad (bit 4=0) or buttons (bit 5=0); read back state |

The joypad register is frequently read (every frame or more). Returning 0xFF (all released)
is safe as a stub. Returning 0x00 (all pressed) will cause immediate joypad interrupt + chaos.

### APU (0xFF10–0xFF3F)

| Address | Range | Notes |
|---------|-------|-------|
| 0xFF10–0xFF14 | Channel 1 (Square + Sweep) | |
| 0xFF16–0xFF19 | Channel 2 (Square) | |
| 0xFF1A–0xFF1E | Channel 3 (Wave) | |
| 0xFF20–0xFF23 | Channel 4 (Noise) | |
| 0xFF24 | NR50 | Master volume + VIN routing |
| 0xFF25 | NR51 | Sound panning |
| 0xFF26 | NR52 | Sound enable (bit 7) + channel status |
| 0xFF30–0xFF3F | Wave RAM | 32 4-bit samples for channel 3 |

---

## Analysis Tips

1. **Repeated LY polls** — The game reads 0xFF44 (LY) in a tight loop waiting for a specific
   scanline. If LY never reaches that value, the game hangs. Confirm `ppu_tick` is advancing LY.

2. **LCDC bit 7 toggle** — A write of 0x00 to 0xFF40 turns off the LCD. A subsequent write
   of 0x91 (or 0x83, etc.) turns it back on. The PPU must reset LY=0 and restart from OAM mode.
   Tetris's line-clear animation does this.

3. **STAT interrupt not firing** — If STAT (0xFF41) has interrupt enable bits set (bits 3–6)
   but the runtime never sets IF bit 1, games that rely on STAT interrupts will behave wrong.
   Check `check_stat_interrupt` in `ppu.c`.

4. **Timer overflow not triggering** — If the game sets TAC (enables timer) and waits for
   IF bit 2, a broken timer means it waits forever. Check `gb_tick`'s timer logic.

5. **IF not being cleared** — After VBlank handler runs, IF bit 0 should be cleared. If the
   runtime sets IF but the interrupt handler doesn't clear it (and the generated code doesn't
   either), spurious re-entry occurs.

---

## Frequency Analysis

To find the most-accessed I/O registers:

```bash
# Top 20 most-read registers
grep "^R," logs/io_trace.log | cut -d',' -f2 | sort | uniq -c | sort -rn | head -20

# Top 20 most-written registers
grep "^W," logs/io_trace.log | cut -d',' -f2 | sort | uniq -c | sort -rn | head -20

# Detect LY-polling loops (same addr read >500 times)
awk -F',' '$1=="R" && $2=="0xFF44" {count++} END {print count, "LY reads"}' logs/io_trace.log
```

---

## Integration with Ghidra

When tracing shows repeated reads of a register that's not advancing:
1. Note the address in Ghidra where the spin loop is
2. `mcp__ghidra__get_code` at that address
3. Identify what value the game expects the register to return
4. Implement the correct behavior in `gbrt.c` or `ppu.c`
5. Log the fix in the appropriate `.log` file
