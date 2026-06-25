# GB Recompiled

**Goal**: Get classic GameBoy ROMs fully playable via static recompilation — Z80 assembly → C → native binary.

**Current target**: Tetris (Japan) — fully playable, no glitches. That proves the runtime is correct
and builds the foundation that makes every subsequent game easier.

This is NOT a GB emulator. We are NOT cycle-accurate in generated code. We are translating Z80 machine
code to C, compiling it natively, and running it. The runtime handles hardware. That's it.

Tetris is the vehicle. The recompiler + runtime is the product.

---

## ██████████████████████████████████████████████████
## ██  RULE 0: NO GHIDRA = NO ACTION. FULL STOP.  ██
## ██████████████████████████████████████████████████

**At the start of EVERY session, before touching ANY file:**

Call `mcp__ghidra__get_program_info`. If it does not respond:

> GHIDRA IS NOT RUNNING.
> I will not read files, write code, or make any suggestions.
> Load the target ROM into Ghidra with GhidraBoy (SM83 processor), base address 0x0000.
> Start the Ghidra MCP server, reconnect with /mcp, then try again.

**This rule has no exceptions. No "I'll just check source while Ghidra loads."**

Before touching any crash address: Ghidra first.
Before implementing any runtime behavior: Ghidra first.
Before guessing what any GB code does: Ghidra first.

**Address mapping:**
- Bank 0: 0x0000–0x3FFF → always mapped
- Bank N (switchable): 0x4000–0x7FFF → physical offset = N × 0x4000 + (addr − 0x4000)
- Interrupt vectors: VBlank=0x0040, STAT=0x0048, Timer=0x0050, Serial=0x0058, Joypad=0x0060

---

## ██████████████████████████████████████████████████
## ██  RULE 1: NEVER TOUCH generated bank files    ██
## ██████████████████████████████████████████████████

`tetris_test/bank_*.c` and `tetris_test/main.c` are BUILD ARTIFACTS. Output from the recompiler.

**NEVER read them whole. NEVER modify them. NEVER patch them.**

If something is wrong in generated code → fix `recompiler/src/generator.cpp` and regenerate.
If code is missing (interpreter fires at a known address) → add `--add-entry-point` and regenerate.

Bank files can be large. Reading them whole **destroys the context window**.
- Need to find a function? Grep for the address (e.g. `func_00_0150`)
- Need to read one function? `Read with offset + limit`
- Need to change it? **You don't. Fix the recompiler or add an entry point.**

---

## The Loop (this is the entire development methodology)

```
1. BUILD recompiler + runtime    →  ninja -C build
2. REGENERATE test ROM           →  ./build/bin/gbrecomp roms/tetris.gb -o tetris_test
3. BUILD test artifact           →  cmake -G Ninja -S tetris_test -B tetris_test/build && ninja -C tetris_test/build
4. RUN game (timed)              →  ./tetris_test/build/game --dump-frames 60,300,600 --screenshot-prefix logs/gb_shot
5. OBSERVE visual output         →  Read logs/gb_shot_*.ppm
6. OBSERVE log output            →  stderr — watch for [GB] Interpreter messages
7. IDENTIFY bug                  →  glitch → PPU; hang → missing compiled code; wrong logic → Ghidra
8. GHIDRA if needed              →  understand what the GB code actually does
9. FIX the bug                   →  ppu.c / audio.c / gbrt.c / interpreter.c
10. GOTO 1
```

---

## Debugging Hierarchy (DO NOT SKIP STEPS)

**Step 1 — Ghidra any unknown address immediately.**
When the runtime logs `[GB] Interpreter called at bank:addr`, Ghidra that address before
reading source or adding any printf. Do not guess. Do not read bank files for context.

**Step 2 — Use interpreter trace to find missing compiled code.**
`[GB] Interpreter` messages at specific addresses = the recompiler missed that code path.
Ghidra the address → understand what the function does → either:
- Add `--add-entry-point bank:addr` and regenerate, or
- Fix the analyzer pattern that missed it (jump table, computed call, etc.)

**Step 3 — Use PyBoy as ground truth for behavioral questions.**
"What should register A be here?" → capture a PyBoy trace, not a guess.
```bash
python3 tools/capture_ground_truth.py roms/tetris.gb -o tetris.trace --frames 600
```
See `GROUND_TRUTH_WORKFLOW.md` for full workflow.

**Step 4 — Add targeted printf traces as a last resort.**
Log ONE specific value you need. Not hex dumps of VRAM or OAM.
If you're adding more than one new printf per investigation cycle, stop and use Ghidra instead.

**NEVER do this:**
- Manually decode Z80 bytes without Ghidra
- Dump large sections of bank_*.c for "context"
- Guess what a function does — Ghidra it
- Add instruction limit (`--limit`) to "find where it gets stuck" before checking Ghidra

Session resume: **say "Run the game."** Screenshot + Ghidra are the only source of truth.

---

## Visual Debugging

Dump specific frames as screenshots with `--dump-frames`:

```bash
./tetris_test/build/game --dump-frames 60,300,600 --screenshot-prefix logs/gb_shot
```

| File | Contents |
|------|----------|
| `logs/gb_shot_frame0060.ppm` | ~1 second in |
| `logs/gb_shot_frame0300.ppm` | ~5 seconds in — title screen should be stable |
| `logs/gb_shot_frame0600.ppm` | ~10 seconds in — demo mode or gameplay |

**What correct Tetris looks like:**
- Frame 60: solid TETRIS logo on a clean background, Nintendo copyright below
- Frame 300: title screen stable, block previews cycling
- Frame 600: demo gameplay with pieces falling

Compare against `docs/tetris_reference.png` (add this when you have a reference screenshot from BGB or PyBoy).

---

## Build Commands

Build system is **CMake/Ninja**. Always use Ninja.

```bash
# IMPORTANT: devkitPro's cmake/PATH conflicts with msys2 mingw64 toolchain.
# Always set CLEAN_PATH to exclude devkitPro before running cmake/ninja/gcc:
export CLEAN_PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:/c/Windows/system32:/c/Windows"

# 0. Initial configure (only needed once, or after deleting build/)
PATH="$CLEAN_PATH" /c/msys64/mingw64/bin/cmake.exe -G Ninja -B build \
    -DCMAKE_MAKE_PROGRAM=/c/msys64/mingw64/bin/ninja.exe \
    -DCMAKE_C_COMPILER=/c/msys64/mingw64/bin/gcc.exe \
    -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe .

# 1. Build recompiler + runtime tools
PATH="$CLEAN_PATH" ninja -C build

# 2. Regenerate tetris_test from ROM (after recompiler/analyzer changes)
PATH="$CLEAN_PATH" ./build/bin/gbrecomp roms/tetris.gb -o tetris_test

# 3. Build the generated project (configure + build)
PATH="$CLEAN_PATH" cmake -G Ninja -S tetris_test -B tetris_test/build \
    -DCMAKE_C_COMPILER=/c/msys64/mingw64/bin/gcc.exe \
    -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe \
    -DCMAKE_MAKE_PROGRAM=/c/msys64/mingw64/bin/ninja.exe
PATH="$CLEAN_PATH" ninja -C tetris_test/build

# 4. Run with screenshot capture (put all output in logs/)
mkdir -p logs
PATH="$CLEAN_PATH" ./tetris_test/build/tetris.exe --dump-frames 60,300,600 --screenshot-prefix logs/gb_shot

# Full pipeline (steps 1–3)
PATH="$CLEAN_PATH" ninja -C build && \
PATH="$CLEAN_PATH" ./build/bin/gbrecomp roms/tetris.gb -o tetris_test && \
PATH="$CLEAN_PATH" ninja -C tetris_test/build

# Debug recompiler (trace + limit)
PATH="$CLEAN_PATH" ./build/bin/gbrecomp roms/tetris.gb -o tetris_test --trace --limit 10000 2> logs/recompiler.log
```

### Build Environment Notes (Windows)

- **Toolchain**: msys2 mingw64 (GCC 15.2.0, x86_64-w64-mingw32)
- **PATH conflict**: devkitPro installs its own cmake at `/c/devkitPro/msys2/usr/bin/cmake` which
  wins in PATH but can't find mingw64's compiler. Always use `CLEAN_PATH` to exclude it.
- **SDL2**: Provided by msys2 mingw64 (`pacman -S mingw-w64-x86_64-SDL2`)
- **ws2_32**: Linked on Windows for the debug server's TCP sockets
- **Weak symbols**: `__attribute__((weak))` doesn't work reliably with mingw-w64 static archives.
  `game_extras_default.c` uses plain (non-weak) definitions. Game-specific overrides replace
  the file in the link rather than relying on symbol weakness.
- **Generated executable**: `tetris_test/build/tetris.exe` (not `game` — name matches ROM prefix)

---

## Key Files

| File | Purpose | Touch? |
|------|---------|--------|
| `recompiler/src/generator.cpp` | Z80→C emitter — THE PRODUCT | Yes — fix codegen here |
| `recompiler/src/analyzer.cpp` | Control flow, entry point discovery | Yes — fix coverage here |
| `recompiler/src/decoder.cpp` | Z80 opcode decoder | Yes if decoder bugs |
| `recompiler/src/bank_tracker.cpp` | MBC bank switch tracking | Yes if bank tracking bugs |
| `runtime/src/gbrt.c` | Memory map, MBC, timing, dispatch, interrupts | Yes — core runtime |
| `runtime/src/ppu.c` | PPU scanline renderer | Yes — all graphics bugs |
| `runtime/src/audio.c` | APU (4 channels: square×2, wave, noise) | Yes — audio bugs |
| `runtime/src/interpreter.c` | Fallback interpreter for uncompiled code | Yes — interpreter accuracy |
| `runtime/src/platform_sdl.cpp` | SDL2 window, input, audio callback | Rarely |
| `tetris_test/bank_*.c` | **GENERATED. NEVER TOUCH.** | **NEVER** |
| `tetris_test/main.c` | **GENERATED. NEVER TOUCH.** | **NEVER** |
| `roms/tetris.gb` | Test ROM | Never |

---

## Log File Rule

**Every `.c` file that implements hardware behavior gets a sibling `.log` file.**

```
runtime/src/ppu.c       →  runtime/src/ppu.log
runtime/src/gbrt.c      →  runtime/src/gbrt.log
runtime/src/audio.c     →  runtime/src/audio.log
runtime/src/interpreter.c → runtime/src/interpreter.log
```

Log entry format:
```
[register_or_function_name]
Ghidra: <what the ROM code does at this address / what it expects>
Rationale: <why implemented this way>
```

This is the audit trail. Lives next to the code. Does NOT go in source as comments.
Does NOT appear when grepping source. Reference only.

---

## Architecture

**This is a static recompiler.** Z80 instructions are translated to C once. C compiles to native x64. No interpreter loop for compiled code — only for missed code paths.

**CALL = direct C function calls.** `func_00_0150(ctx)` calls `func_00_0200(ctx)` directly.
`gb_interpret()` handles only uncompiled addresses. Every `[GB] Interpreter` log is a gap to close.

**All interrupt vectors are compiled entry points.** VBlank (0x0040), STAT (0x0048), Timer (0x0050), Serial (0x0058), Joypad (0x0060) must be in the recompiled output. If an interrupt isn't firing, it's a runtime bug (interrupt flag / IME handling), not a coverage gap.

**Do not pre-implement anything.** If the game hasn't failed on it yet, it doesn't need fixing yet.

---

## Known Tetris Issues (current state)

| Bug | Symptom | Suspected cause |
|-----|---------|-----------------|
| Title screen glitch | Occasional corruption | PPU STAT interrupt timing |
| Post-line-clear graphics | Broken until pause/resume | LCD off/on transition in `ppu.c` |
| Audio | General glitching | APU channel timing / envelope |

The line-clear bug is the highest priority: Tetris disables the LCD (LCDC bit 7) during the
line-clear animation to update VRAM safely, then re-enables it. The PPU must restart the
full frame cycle (LY=0, mode=OAM) when LCD is re-enabled. Ghidra the VBlank handler and
the line-clear routine before touching `ppu.c`.

---

## What NOT to Do

- Do not pre-emptively fix hardware behavior "just in case"
- Do not read large sections of bank_*.c for "context"
- Do not guess what GB code does — Ghidra it
- Do not patch generated C to avoid fixing the recompiler/runtime
- Do not carry assumptions from any previous game or session
- Do not write verbose comments — a one-line log entry is enough
- Do not run unit tests as primary driver — run the game

---

## Progress Milestones

| Milestone | Status |
|-----------|--------|
| Recompiler generates valid C for Tetris | ✅ |
| Runtime boots and renders frames | ✅ |
| Gameplay functional (pieces move, rotate) | ✅ |
| Title screen stable (no glitch) | ⬜ |
| Line clear animation correct (no pause/resume hack) | ⬜ |
| Audio correct | ⬜ |
| Tetris fully playable end-to-end | ⬜ |
| Second game target chosen and booting | ⬜ |
