# GB Recompiled

A **static recompiler** for original GameBoy ROMs that translates Z80 assembly directly into portable, modern C code.

<p align="center">
  <img src="dino.png" alt="GB Recompiled Screenshot" width="400">
</p>

---

## Downloads

Pre-built binaries are available on the [Releases](https://github.com/arcanite24/gb-recompiled/releases) page:

| Platform | Architecture | File |
|----------|--------------|------|
| **Windows** | x64 | `gb-recompiled-windows-x64.zip` |
| **Linux** | x64 | `gb-recompiled-linux-x64.tar.gz` |
| **macOS** | x64 (Intel) | `gb-recompiled-macos-x64.tar.gz` |
| **macOS** | ARM64 (Apple Silicon) | `gb-recompiled-macos-arm64.tar.gz` |

> **Note**: The recompiler (`gbrecomp`) is what you download. After recompiling a ROM, you'll still need CMake, Ninja, SDL2, and a C compiler to build the generated project.

---

## Quick Start

### Prerequisites

- **CMake** 3.15+
- **Ninja** build system
- **SDL2** development libraries
- A C/C++ compiler (Clang, GCC, or MSVC)

### Building

```bash
# Clone and enter the repository
git clone https://github.com/arcanite24/gb-recompiled.git
cd gb-recompiled

# Configure and build
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

---

## Quick Setup

### Automated Setup (Recommended)

**macOS:**
```bash
# Download and run the setup script
git clone https://github.com/arcanite24/gb-recompiled.git
cd gb-recompiled
chmod +x tools/setup.sh
./tools/setup.sh
```

**Linux:**

First install build dependencies:
- git
- build-essential
- cmake
- ninja-build
- python3 
- python3-pip
- python3-venv
- libsdl2-dev
```bash
# Download the setup script, create a venv for python packages, and run the setup
git clone https://github.com/arcanite24/gb-recompiled.git
cd gb-recompiled
python3 -m venv gb
source gb/bin/activate
chmod +x tools/setup.sh
./tools/setup.sh
```

**Windows:**
```bash
# Download and run the setup script (run as Administrator)
git clone https://github.com/arcanite24/gb-recompiled.git
cd gb-recompiled
powershell -ExecutionPolicy Bypass -File tools/setup.ps1
```

### Manual Setup

**Prerequisites:**
- CMake 3.15+
- Ninja build system
- SDL2 development libraries
- A C/C++ compiler (Clang, GCC, or MSVC)

**Building:**
```bash
# Clone and enter the repository
git clone https://github.com/arcanite24/gb-recompiled.git
cd gb-recompiled

# Configure and build
cmake -G Ninja -B build .
ninja -C build
```

## Usage

### Basic Recompilation

```bash
./build/bin/gbrecomp <rom.gb> -o <output_dir>
```

The recompiler will:

1. Load and parse the ROM header
2. Analyze control flow across all memory banks
3. Decode instructions and track bank switches
4. Generate C source files with the runtime library

### Debugging Options

| Flag | Description |
|------|-------------|
| `--trace` | Print every instruction during analysis |
| `--limit <N>` | Stop analysis after N instructions |
| `--add-entry-point b:addr` | Manually specified entry point (e.g. `1:4000`) |
| `--no-scan` | Disable aggressive code scanning (enabled by default) |
| `--symbols <file>` | Load a `.sym` symbol file and use those names for generated functions and internal labels |
| `--verbose` | Show detailed analysis statistics |
| `--use-trace <file>` | Use runtime trace to seed entry points |

**Example:**

```bash
# Debug a problematic ROM
./build/bin/gbrecomp game.gb -o output/game --trace --limit 5000
```

### Advanced Usage

**Automated Ground Truth Workflow (Recommended):**
For complex games (like *Pokémon Blue*) with computed jumps, use the automated workflow:

```bash
# Full automated workflow (capture trace, compile, verify coverage)
python3 tools/run_ground_truth.py roms/pokeblue.gb
```

**Trace-Guided Recompilation (Recommended):**
Complex games (like *Pokémon Blue*) often use computed jumps that static analysis cannot resolve. You can use execution traces to "seed" the analyzer with every instruction physically executed during a real emulated session.

1. **Generate a trace**: Run any recompiled version of the game with tracing enabled, or use the **[Ground Truth Capture Tool](GROUND_TRUTH_WORKFLOW.md)** with PyBoy.

   ```bash
   # Option A: Using recompiled game
   ./output/game/build/game --trace-entries game.trace --limit 1000000

   # Option B: Using PyBoy "Ground Truth" Capture (Recommended for new games)
   python3 tools/capture_ground_truth.py roms/game.gb -o game.trace --random
   ```

2. **Recompile with grounding**: Feed the trace back into the recompiler.

   ```bash
   ./build/bin/gbrecomp roms/game.gb -o output/game --use-trace game.trace
   ```

For a detailed walkthrough, see **[GROUND_TRUTH_WORKFLOW.md](GROUND_TRUTH_WORKFLOW.md)**.

**Symbol Maps for Better Generated Code:**
If you have a community `.sym` file, you can feed it into the recompiler so emitted functions, internal block labels, and safe RAM/HRAM/data address constants use readable names instead of only `func_xx_yyyy`, `loc_xx_yyyy`, and raw `0xCxxx`-style literals.

```bash
./build/bin/gbrecomp roms/pokeblue.gb -o output/pokeblue --symbols pokeblue.sym
```

For the Pokémon Blue validation work in this repo, we used:

- ROM MD5: `50927e843568814f7ed45ec4f944bd8b`
- Symbol file: [`pokeblue.sym`](https://github.com/pokemon-speedrunning/symfiles/blob/master/pokeblue.sym)

If your ROM hash differs, imported symbol names may not line up with the same code locations.

Imported names are sanitized into valid C identifiers and emitted with a `sym_` prefix, so labels like `EnterMap`, `DisableLCD`, or `OverworldLoopLessDelay.checkIfStartIsPressed` become safe generated names such as `sym_EnterMap` and `sym_OverworldLoopLessDelay_checkIfStartIsPressed`. Imported RAM/HRAM/data labels are also lifted into generated address constants like `GB_ADDR_wSoundID` and `GB_ADDR_hJoyHeld`. Imported ROM data labels are lifted into `GB_ROM_ADDR_*` address constants and `GB_ROM_PTR_*` pointers into the generated `rom_data[]` blob.

When `--symbols` is enabled, the generated output also includes:

- `*_metadata.json`: a sidecar with emitted function names, block labels, RAM/HRAM data constants, ROM data symbols, builtin hardware aliases, source symbol names, and provenance (`imported`, `builtin`, or `autogenerated`)
- named hardware/RAM/ROM constants in the internal header, so immediate accesses become easier to read (for example `GB_IO_LCDC`, `GB_ADDR_wSoundID`, `GB_ADDR_hJoyHeld`, and `GB_ROM_ADDR_MapHeaderPointers`)

**Generic Indirect Jump Solver:**
The recompiler includes an advanced static solver for `JP HL` and `CALL HL` instructions. It tracks the contents of all 8-bit registers and 16-bit pairs throughout the program's control flow.

- **Register Tracking**: Accurately handles constant pointers loaded into `HL` or table bases loaded into `DE`.
- **Table Backtracking**: When a `JP HL` is encountered with an unknown `HL`, the recompiler scans back for jump table patterns (e.g., page-aligned pointers) and automatically discovers all potential branch targets.
- **Impact**: Provides >98% code discovery for complex RPGs like *Pokémon* without requiring dynamic traces.

**Manual Entry Points:**
If you see `[GB] Interpreter` messages in the logs at specific addresses, you can manually force the recompiler to analyze them:

```bash
./build/bin/gbrecomp roms/game.gb -o out_dir --add-entry-point 28:602B
```

**Aggressive Scanning:**
The recompiler automatically scans memory banks for code that isn't directly reachable (e.g. unreferenced functions). This improves compatibility but may occasionally misidentify data as code. To disable it:

```bash
./build/bin/gbrecomp roms/game.gb -o out_dir --no-scan
```

### Runtime Options

When running a recompiled game:

| Option | Description |
|--------|-------------|
| `--input <script>` | Automate input from an inline script. Legacy frame entries look like `120:S:1`; cycle-anchored entries look like `c4412912:S:8192` |
| `--record-input <file>` | Record live keyboard/controller input to a text file. New recordings are cycle-anchored for stable repro across builds |
| `--dump-frames <list>` | Dump specific frames as screenshots |
| `--dump-present-frames <list>` | Dump every host present that occurs while the selected guest frame(s) are current |
| `--screenshot-prefix <path>` | Set screenshot output path |
| `--log-file <file>` | Redirect stdout and stderr to a text log file for later inspection |
| `--debug-performance` | Enable the full slowdown-debug preset: frame logs, vsync logs, fallback logs, and periodic audio stats |
| `--trace-entries <file>` | Log all executed (Bank, PC) points to file |
| `--log-slow-frames <ms>` | Log frames whose emulation plus render time exceeds the threshold |
| `--log-slow-vsync <ms>` | Log pacing waits whose `gb_platform_vsync()` time exceeds the threshold |
| `--log-frame-fallbacks` | Log any rendered frame that hit generated->interpreter fallback |
| `--log-lcd-transitions` | Log exact LCDC on/off transitions and LCD-off span lengths in guest cycles |
| `--limit-frames <n>` | Stop the run after `n` completed guest frames |
| `--smooth-lcd-transitions` | Force the SDL host smoother on for long guest frames, including LCD-off stretches (default) |
| `--no-smooth-lcd-transitions` | Disable the SDL host smoother for long guest frames |
| `--differential [steps]` | Compare generated execution against the interpreter for N steps (default `10000`) |
| `--differential-frames <n>` | Stop differential mode after N completed frames |
| `--differential-log <n>` | Print progress every N matched steps during differential mode |
| `--differential-no-memory` | Skip full mutable-memory comparisons to speed up differential runs |
| `--differential-log-fallbacks` | Log when generated execution falls back to the interpreter |
| `--differential-fail-on-fallback` | Treat any generated->interpreter fallback as a failure |

Example:

```bash
./output/game/build/game --differential 500000 --differential-log 100000
```

Capture a sharable log and record a manual repro session:

```bash
./output/game/build/game --log-file session.log --record-input session.input
```

Capture a useful slowdown log without manually enabling each individual perf flag:

```bash
./output/game/build/game --log-file perf.log --record-input perf.input --debug-performance
```

`--debug-performance` now also includes exact `[LCD]` transition lines plus `lcd_off_cycles`, `lcd_transitions`, and `lcd_spans` in the `[FRAME]` logs, which makes LCD-off slowdown analysis much easier.

Input script notes:

- Plain `120:S:1` entries are frame-anchored and kept for backwards compatibility.
- `c4412912:S:8192` entries are cycle-anchored and replay much more reliably after timing/runtime changes.
- `--record-input` now writes cycle-anchored scripts by default so the same file can reproduce scene-specific bugs more accurately.

The SDL menu now enables `Smooth Slow Frames` by default. It keeps host presentation and pacing steady during long guest frames by re-presenting the last completed framebuffer instead of showing an in-progress frame, without changing guest emulation timing. You can toggle it at runtime from the settings menu or override it on launch with `--smooth-lcd-transitions` / `--no-smooth-lcd-transitions`.

Replay the recorded input later with the existing `--input` flag:

```bash
./output/game/build/game --input "$(cat session.input)"
```

Compare LCD-off slowdown spans against a PyBoy replay of the same input script:

```bash
python3 tools/compare_lcd_transitions.py roms/game.gb \
  --runtime-log perf.log \
  --input-file perf.input \
  --start-frame 200 \
  --end-frame 500
```

The runtime side of that report uses exact LCDC transition logs. The PyBoy side is sampled at frame boundaries, so it is best used to confirm whether our LCD-off stretches are obviously longer or happening in different places than the reference. The PyBoy helper tools accept cycle-anchored scripts too, but they quantize those presses to frame boundaries during replay.

### Controls

| GameBoy | Keyboard (Primary) | Keyboard (Alt) |
|---------|-------------------|----------------|
| **D-Pad Up** | ↑ Arrow | W |
| **D-Pad Down** | ↓ Arrow | S |
| **D-Pad Left** | ← Arrow | A |
| **D-Pad Right** | → Arrow | D |
| **A Button** | Z | J |
| **B Button** | X | K |
| **Start** | Enter | - |
| **Select** | Right Shift | Backspace |
| **Quit** | Escape | - |

---

## How It Works

### 1. Analysis Phase

The recompiler performs static control flow analysis:

- Discovers all reachable code starting from entry points (`0x100`, interrupt vectors)
- Tracks bank switches to follow cross-bank calls and jumps
- Detects computed jumps (e.g., `JP HL`) and resolves jump tables
- Separates code from data using heuristics

### 2. Code Generation

Instructions are translated to C:

```c
// Original: LD A, [HL+]
ctx->a = gb_read8(ctx, ctx->hl++);

// Original: ADD A, B
gb_add8(ctx, ctx->b);

// Original: JP NZ, 0x1234
if (!ctx->flag_z) { func_00_1234(ctx); return; }
```

Each ROM bank becomes a separate C file with functions for reachable code blocks.

### 3. Runtime Execution

The generated code links against `libgbrt`, which provides:

- Memory-mapped I/O (`gb_read8`, `gb_write8`)
- CPU flag manipulation
- PPU scanline rendering
- Audio sample generation
- Timer and interrupt handling

---

## Compatibility

This is a living list, not a claim that every ROM that recompiles is fully compatible. If you want to reproduce the same behavior or compare logs, make sure your ROM hash matches one of the entries below. On macOS use `md5 <rom>`; on Linux use `md5sum <rom>`.

| Game | MD5 | Current state | Notes |
|------|-----|---------------|-------|
| Tetris | `982ed5d2b12a0377eb14bcdc4123744e` | Playable | Rebuilt repeatedly as the baseline test ROM; differential smoke-tested after recent runtime and recompiler changes |
| Pokemon Blue | `50927e843568814f7ed45ec4f944bd8b` | Playable | Audio, PPU timing, slowdown, and recompiler correctness issues were investigated and fixed against this dump |
| Donkey Kong Land | `89bb0d67d5af35c2ebf09d9aef2e34ad` | Playable | Startup freeze, DMA/HRAM behavior, and raster flicker issues were debugged and fixed against this dump |
| Kirby's Dream Land | `a66e4918edcd042ec171a57fe3ce36c3` | Playable | HRAM DMA overlay detection was fixed so Kirby renders correctly |
| The Legend of Zelda: Link's Awakening | `c4360f89e2b09a21307fe864258ecab7` | Playable | Built, launched, and smoke-tested successfully |
| Castlevania: The Adventure | `0b4410c6b94d6359dba5609ae9a32909` | Playable | Built, launched, and smoke-tested successfully |
| Super Mario Land | `b48161623f12f86fec88320166a21fce` | Playable | Startup `HALT`/`STOP` differential mismatch was fixed for better timing accuracy |
| Adventures of Star Saver, The | `91ecec5f8d06f18724bd1462b53c4b3d` | Playable (minor issues) | Fixed runtime issues |
| Metroid II: Return of Samus | `9639948ad274fa15281f549e5f9c4d87` | Playable |  |

If you report a game-specific bug, please include:

- The ROM filename and MD5
- The exact command you ran
- A `--log-file` capture
- A `--record-input` capture if the bug depends on gameplay steps

---

## Roadmap

- [x] Tools to identify entry-points (Trace-Guided Analysis)
- [x] Tools for better graphical debugging (outputting PNGs grid instead of raw PPMs)
- [ ] Android builds
- [ ] Game Boy Color support
- [ ] Cached interpreter
- [ ] Improve quality of generated code
- [ ] Reduce size of output binaries

---

## Tools

The `tools/` directory contains utilities for analysis and verification:

### 1. Ground Truth Capturer

Automate instruction discovery using a high-speed headless emulator (**PyBoy** recommended).

```bash
python3 tools/capture_ground_truth.py roms/game.gb --frames 3600 --random -o game.trace
```

### 2. Coverage Analyzer

Audit your recompiled code against a dynamic trace to see exactly what instructions are missing.

```bash
python3 tools/compare_ground_truth.py --trace game.trace output/game
```

---

## Development

### Project Architecture

The recompiler uses a multi-stage pipeline:

```
ROM → Decoder → IR Builder → Analyzer → C Emitter → Output
         ↓           ↓            ↓
     Opcodes   Intermediate   Control Flow
               Representation   Graph
```

Key components:

- **Decoder** (`decoder.h`): Parses raw bytes into structured opcodes
- **IR Builder** (`ir_builder.h`): Converts opcodes to intermediate representation
- **Analyzer** (`analyzer.h`): Builds control flow graph and tracks bank switches
- **C Emitter** (`c_emitter.h`): Generates C code from IR

---

## License

This project is licensed under the MIT License.

**Note**: GameBoy is a trademark of Nintendo. This project does not include any copyrighted ROM data. You must provide your own legally obtained ROM files.

---

## Acknowledgments

- [Pan Docs](https://gbdev.io/pandocs/) - The definitive GameBoy technical reference
- [mgbdis](https://github.com/mattcurrie/mgbdis) - GameBoy disassembler (included in tools/)
- The gbdev community for extensive documentation and test ROMs
- [N64Recomp](https://github.com/Mr-Wiseguy/N64Recomp) - The original recompiler that inspired this project
