# GB Recompiled (Fork)

A **static recompiler** for original Game Boy ROMs that translates SM83 assembly into portable C code. Run classic Game Boy games as native executables — no traditional emulator required.

> **This is a fork of [arcanite24/gb-recompiled](https://github.com/arcanite24/gb-recompiled).** All original recompiler design, analysis engine, and code generation architecture is the work of [arcanite24](https://github.com/arcanite24). This fork adds runtime improvements, PPU fixes, and game-specific enhancements developed while building [PokemonRedAndBlueRecomp](https://github.com/mstan/PokemonRedAndBlueRecomp).

![Compatibility](https://img.shields.io/badge/compatibility-98.9%25-brightgreen)
![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux%20%7C%20Windows-lightgrey)

---

## What This Fork Adds

Changes from upstream focused on runtime correctness and performance:

### PPU (Graphics)
- VRAM/OAM write protection during rendering modes
- SCX/SCY per-scanline latching for correct scroll effects
- LCD on/off transition state reset
- Variable mode 3 timing based on sprite count and fine scroll
- Window WY trigger correctness (exact LY==WY match)
- Tile-batched background rendering (~16x fewer VRAM reads per scanline)
- Optimized sprite rendering with direct VRAM access

### Performance
- `gb_tick` hot path: TIMA fast-path with XOR edge detection, inlined cycle counting, branch prediction hints
- Channel 4 noise LFSR iteration cap
- Removed per-frame debug overhead from RGB framebuffer conversion

### Audio
- Smooth underrun fade-out (holds last sample instead of inserting silence pops)

### Configuration
- Multi-CRC ROM validation (`valid_crcs` in TOML) for supporting multiple ROM versions
- Configurable `output_prefix` for generated file naming
- SRAM save debouncing (5-second cooldown after last write)

---

## Features (from upstream)

- **High Compatibility**: Successfully recompiles **98.9%** of the tested ROM library (1592/1609 ROMs). Recompilation success does not guarantee full playability.
- **Native Performance**: Generated C code compiles to native machine code
- **Runtime Library**: Cycle-aware instruction emulation, scanline PPU, 4-channel APU, MBC1/2/3/5 support
- **SDL2 Platform Layer**: Keyboard/controller input, audio, window display
- **Debugging Tools**: Trace logging, interpreter fallback logging, TCP debug server
- **Cross-Platform**: macOS, Linux, and Windows (via CMake + Ninja)

---

## Quick Start

### Prerequisites

- **CMake** 3.15+
- **Ninja** build system
- **SDL2** development libraries
- A C/C++ compiler (Clang, GCC, or MSVC)

### Building the Recompiler

```bash
git clone https://github.com/mstan/gbrecompiled.git
cd gbrecompiled
cmake -G Ninja -B build .
ninja -C build
```

### Recompiling a ROM

```bash
# Generate C code from a ROM
./build/bin/gbrecomp path/to/game.gb -o output/game

# Build the generated project
cmake -G Ninja -S output/game -B output/game/build
ninja -C output/game/build

# Run
./output/game/build/game
```

### Using a TOML Config (Recommended for complex games)

For games like Pokemon that need manually specified entry points:

```bash
./build/bin/gbrecomp --config game_config.toml
```

See [PokemonRedAndBlueRecomp](https://github.com/mstan/PokemonRedAndBlueRecomp) for a complete example of a TOML-configured game project.

---

## How It Works

### 1. Analysis Phase

The recompiler performs static control flow analysis:
- Discovers reachable code from entry points (`0x100`, interrupt vectors, TOML config)
- Tracks bank switches to follow cross-bank calls and jumps
- Detects computed jumps (`JP HL`) and resolves jump tables
- Separates code from data using heuristics

### 2. Code Generation

Instructions are translated to C:
```c
// Original: LD A, [HL+]
ctx->a = gb_read8(ctx, ctx->hl++);

// Original: JP NZ, 0x1234
if (!ctx->flag_z) { func_00_1234(ctx); return; }
```

Each ROM bank becomes a separate C file. Code paths not discovered during analysis fall back to an interpreter at runtime.

### 3. Runtime

The generated code links against `libgbrt`, which provides:
- Memory-mapped I/O
- PPU scanline rendering with per-scanline scroll latching
- Audio sample generation (4 channels, 44.1kHz)
- Timer, interrupt, and DMA handling
- ROM file picker with CRC32 validation
- SRAM battery save persistence

---

## Debugging

| Flag | Description |
|------|-------------|
| `--trace` | Print every instruction during analysis |
| `--limit <N>` | Stop analysis after N instructions |
| `--add-entry-point b:addr` | Manually add entry point (e.g., `1:4000`) |
| `--no-scan` | Disable aggressive code scanning |
| `--verbose` | Show detailed analysis statistics |
| `--use-trace <file>` | Seed analyzer with runtime trace |

At runtime, interpreter fallbacks are logged to `interp_fallbacks.log` with bank and address. Add these addresses to your TOML config to eliminate fallbacks in the next build.

A TCP debug server listens on `localhost:4370` for runtime inspection (register state, watchpoints, OAM, frame stepping).

---

## Credits

- **[arcanite24](https://github.com/arcanite24)** — Original author of gb-recompiled. All recompiler architecture, static analysis engine, code generation, and initial runtime implementation.
- **[pret](https://github.com/pret)** — Game Boy reverse engineering community whose disassembly projects provide invaluable reference material.

## Compatibility

See [COMPATIBILITY.md](COMPATIBILITY.md) for the full recompilation test report.

| Status | Count | Percentage |
|--------|-------|------------|
| SUCCESS | 1592 | 98.94% |
| RECOMPILE_FAIL | 1 | 0.06% |
| RUN_TIMEOUT | 1 | 0.06% |
| EXCEPTION | 7 | 0.44% |

> **Note:** Recompilation success means the ROM was successfully translated to C. It does not guarantee the game is fully playable — runtime correctness depends on entry point coverage and runtime accuracy.
