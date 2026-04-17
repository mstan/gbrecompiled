# GB Recompiled — Session Memory

## Build Setup (Critical)

**PATH fix required every session:**
```bash
PATH="/c/msys64/mingw64/bin:$PATH"
```
Without this, gcc fails silently — cc1.exe loads `libgmp-10.dll` from Git's mingw64
(`/c/Program Files/Git/mingw64/bin/`) instead of MSYS2's, causing a version mismatch crash.
Symptom: gcc exits with code 1, no error output.

**ROM name:** `roms/Tetris (W) (V1.0) [!].gb` (with spaces and brackets)
Use `roms/tetris.gb` — a copy is kept there for the recompiler.
MD5: `084f1e457749cdec86183189bd88ce69`

**imgui submodule** must be initialized:
```bash
git submodule update --init runtime/third_party/imgui
```

**Build output:** `tetris_test/build/tetris.exe` (not `game`)

**Full pipeline:**
```bash
PATH="/c/msys64/mingw64/bin:$PATH"
ninja -C build
./build/bin/gbrecomp roms/tetris.gb -o tetris_test
cmake -G Ninja -S tetris_test -B tetris_test/build
ninja -C tetris_test/build
mkdir -p logs
./tetris_test/build/tetris.exe --dump-frames 60,300,600 --screenshot-prefix logs/gb_shot 2>logs/game.log
```

## Current State (2026-03-11)

**Session result:** Full pipeline working. Screenshots look correct:
- Frame 60: Clean copyright screen (TM/©1987 ELORG...)
- Frame 300: Game type selection screen (GAME TYPE / MUSIC TYPE) — correct
- Frame 600: Still on game type selection (no input given) — expected
- Zero interpreter messages — all code paths compiled

**Known bugs (not yet investigated):**
1. Post-line-clear graphics break until pause/resume — LCD off/on in ppu.c
2. Title screen occasionally glitches — STAT interrupt timing
3. Audio glitches — APU channel timing/envelope

## Ghidra Setup

- Program: `Tetris (W) (V1.0) [!].gb`
- Language: Z80/little/16/Z8401x (NOT GhidraBoy — that's not installed)
- Base: 0x0000
- MCP port: 7000 (configured in .mcp.json)
- Functions tagged: interrupt vectors (0x0040, 0x0048, 0x0050), entry (0x0100)

## Recompiler Output Format

Generator outputs: `tetris.h`, `tetris.c`, `tetris_main.c`, `CMakeLists.txt`, `tetris_rom.c`
(NOT `bank_*.c` + `main.c` as CLAUDE.md describes — format has changed)
