# GB Recompiled

A **static recompiler** for Game Boy and Game Boy Color ROMs that translates LR35902 code directly into portable, modern C code.

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
# or
./build/bin/gbrecomp path/to/game.gbc -o output/game

# Build the generated project
cmake -G Ninja -S output/game -B output/game/build
ninja -C output/game/build

# Run
./output/game/build/game
```

Generated projects now default to a smaller manual-testing profile: `MinSizeRel`, generated ROM sources at `-O1`, IPO/LTO off, section-based dead stripping on supported toolchains, and post-link symbol stripping enabled by default. For maximum runtime performance, configure explicitly with something like `-DCMAKE_BUILD_TYPE=Release -DGBRECOMP_GENERATED_OPT_LEVEL=3 -DGBRECOMP_ENABLE_IPO=ON -DGBRECOMP_ENABLE_STRIP=OFF`.

`gbrecomp` also uses parallel code generation by default on multi-core machines. Pass `--jobs <n>` if you want to cap it manually, or `--jobs 1` for single-threaded debugging.

Game Boy Color support is now implemented in the main runtime and generated projects. Representative CGB games such as Pokemon Crystal and Link's Awakening are working well, but some CGB hardware-test and edge-case accuracy work is still in progress. See [GBC.md](GBC.md) for the current status.

### Generating an Android Project

```bash
# Generate the normal desktop project plus an Android scaffold
./build/bin/gbrecomp path/to/game.gb -o output/game --android

# Build the Android app (SDL2_SOURCE_DIR must point to an SDL2 source checkout)
SDL2_SOURCE_DIR=/path/to/SDL gradle -p output/game/android :app:assembleDebug

# Install it on a connected device or emulator
adb install -r output/game/android/app/build/outputs/apk/debug/app-debug.apk
```

Android output is currently single-ROM only. The generated Android project is controller-first, uses landscape orientation, targets `arm64-v8a`, and keeps SDL2 external instead of vendoring it into this repo or the generated output.

Default Android controller mapping is position-based so it feels natural on Xbox-style handhelds:

- D-pad or left stick: move
- Bottom face button (`Xbox A` / `Switch B` / `Cross`): Game Boy `B`
- Right face button (`Xbox B` / `Switch A` / `Circle`): Game Boy `A`
- Left shoulder: Game Boy `B`
- Right shoulder: Game Boy `A`
- Start / Menu: `Start`
- Back / View / Share: `Select`
- Guide / Home, or on Android `Guide`, `L3`, `R3`, or Android Back: open the runtime settings menu

Those defaults are now remappable at runtime from the SDL settings menu, and the mapping is persisted through SDL's per-app preference storage.

See [ANDROID.md](ANDROID.md) for the full end-to-end workflow, including prerequisites, APK builds, `adb` install commands, and Android-specific notes.

### Recompiling Multiple ROMs Into One Launcher

```bash
# Generate one shared project from a folder of ROMs
./build/bin/gbrecomp path/to/roms -o output/multi_rom --jobs 8

# Build the generated launcher project
cmake -G Ninja -S output/multi_rom -B output/multi_rom/build
ninja -C output/multi_rom/build

# Inspect and launch one of the generated games
./output/multi_rom/build/multi_rom --list-games
./output/multi_rom/build/multi_rom --game tetris
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

Use `--jobs <n>` to control parallel code generation. `--jobs 0` or omitting the flag lets `gbrecomp` pick a worker count automatically from the current machine.

The recompiler will:

1. Load and parse the ROM header
2. Analyze control flow across all memory banks
3. Decode instructions and track bank switches
4. Generate C source files with the runtime library

### Android Output

Single-ROM generation can also emit an Android-ready project scaffold:

```bash
./build/bin/gbrecomp <rom.gb> -o <output_dir> --android
```

Supported Android flags:

| Flag | Description |
|------|-------------|
| `--android` | Emit an Android project under `<output_dir>/android` alongside the normal desktop project |
| `--android-package <java.package>` | Override the generated Android package name |
| `--android-app-name <label>` | Override the generated Android app label |

Notes:

- Android output is v1 single-ROM only. Multi-ROM launcher generation stays desktop-only for now.
- The generated project expects `SDL2_SOURCE_DIR` to be set as either a Gradle property or an environment variable.
- The generated Android CMake/Gradle files fail fast with a short error if `SDL2_SOURCE_DIR` is missing.
- The first Android version is controller-first and does not include touch gameplay controls.
- Face-button mapping is based on physical position, and the runtime labels it according to the connected controller profile when SDL can infer one.

Build the generated Android project from the repo root with:

```bash
SDL2_SOURCE_DIR=/path/to/SDL gradle -p <output_dir>/android :app:assembleDebug
```

For a complete Android walkthrough, see [ANDROID.md](ANDROID.md).

### Multi-ROM Recompilation

You can also point `gbrecomp` at a directory instead of a single ROM:

```bash
./build/bin/gbrecomp <rom_directory> -o <output_dir>
```

In directory mode the recompiler will:

1. Recursively find every `.gb`, `.gbc`, and `.sgb` file
2. Recompile each ROM into its own generated code module and metadata file
3. Share one runtime library across all generated ROMs
4. Emit a small launcher executable so you can choose which game to run

Directory mode parallelizes the per-ROM analyze/generate/write work by default. If you want to tune it, pass `--jobs <n>`. `--verbose` and `--trace` still force a single batch worker so the logs stay readable.

The generated launcher project includes:

- `launcher_main.cpp`: an SDL + ImGui launcher that lets you pick a generated ROM graphically
- `launcher_manifest.json`: the generated game list, ids, and source ROM paths
- one generated module per ROM, each with isolated symbol names so they can all link into the same executable

Launcher usage:

```bash
# List generated games
./output/multi_rom/build/multi_rom --list-games

# Launch a specific game by id
./output/multi_rom/build/multi_rom --game tetris
```

If you run the launcher without `--game`, it opens the graphical launcher. The CLI options still work for direct launches, scripts, and CI.

### Debugging Options

| Flag | Description |
|------|-------------|
| `--trace` | Print every instruction during analysis |
| `--limit <N>` | Stop analysis after N instructions |
| `--jobs <N>` | Override the auto-selected parallel worker count for codegen and directory-mode batch generation |
| `--add-entry-point b:addr` | Manually specified entry point (e.g. `1:4000`) |
| `--no-scan` | Disable aggressive code scanning (enabled by default) |
| `--symbols <file>` | Load a `.sym` symbol file and use those names in generated output |
| `--annotations <file>` | Load trusted function, label, and data-range guidance from a text file |
| `--verbose` | Show detailed analysis statistics |
| `--use-trace <file>` | Use runtime trace to seed entry points |
| `--android` | Also emit an Android project scaffold for single-ROM output |
| `--android-package <java.package>` | Override the generated Android package name |
| `--android-app-name <label>` | Override the generated Android app label |

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

The `.sym` importer is now conservative about which ROM symbols become trusted function entries. Direct callable targets are still seeded into the analyzer, but ambiguous global ROM labels are kept as names unless you provide stronger guidance. The analyzer also treats the Nintendo logo (`0x0104-0x0133`) and cartridge header (`0x0134-0x014f`) as built-in ROM data, which helps avoid turning those bytes into code during aggressive scanning.

If your `.sym` file carries extra comments, you can add explicit analyzer directives inline:

- `@function`
- `@label`
- `@data`
- `@size=<n>`

Example:

```text
00:0150 BootEntry ; @function
00:0153 BootEntry.loop ; @label
1f:4000 MapScriptTable ; @data @size=0x120
```

**Annotation Files for Accurate Disassemblies:**
For projects like `pret/pokecrystal`, a richer annotations file is the best way to feed trusted code/data boundaries into the analyzer. This improves correctness and often performance too, because known data regions are kept out of aggressive scanning, which reduces bogus functions and unnecessary generated code.

```bash
./build/bin/gbrecomp roms/game.gbc \
  -o output/game \
  --symbols pokecrystal.sym \
  --annotations pokecrystal.annotations
```

Annotation files are simple line-based text:

```text
# comments start with #
function 00:0150 BootEntry
label 00:0153 BootEntry.loop
data 1f:4000 0x120 MapScriptTable ; optional trailing comment
```

Rules:

- bank and address are hexadecimal `bank:addr`
- `function` and `label` optionally take a symbol name
- `data` takes a size plus an optional symbol name
- `; ...` is preserved as imported symbol metadata when a name is present

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
| `--limit <n>` | Stop the run after `n` guest instructions. If interpreter hotspot reporting is enabled, the summary is flushed before exit |
| `--debug-performance` | Enable the full slowdown-debug preset: frame logs, vsync logs, fallback logs, and periodic audio stats |
| `--trace-entries <file>` | Log all executed (Bank, PC) points to file |
| `--log-slow-frames <ms>` | Log frames whose emulation plus render time exceeds the threshold |
| `--log-slow-vsync <ms>` | Log pacing waits whose `gb_platform_vsync()` time exceeds the threshold |
| `--log-frame-fallbacks` | Log any rendered frame that hit generated->interpreter fallback |
| `--log-lcd-transitions` | Log exact LCDC on/off transitions and LCD-off span lengths in guest cycles |
| `--report-interpreter-hotspots` | Print an end-of-run interpreter summary with total fallback entries, interpreted instructions/cycles, top hotspots, and the last uncovered opcode site |
| `--interpreter-hotspot-limit <n>` | Limit how many hotspot rows are printed by `--report-interpreter-hotspots` |
| `--limit-frames <n>` | Stop the run after `n` completed guest frames |
| `--benchmark` | Run headless and uncapped for benchmarking. Host rendering, audio output, and wall-clock pacing are skipped so the runtime can execute as fast as possible |
| `--model auto|dmg|cgb` | Select hardware mode. `auto` uses the ROM header, `dmg` forces DMG, and `cgb` runs CGB hardware, using compatibility mode for DMG cartridges |
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

### Runtime Settings Menu

Press `Escape` on desktop, or on Android use `Android Back`, `Guide`, `L3`, or `R3` to open the runtime settings menu.

- The `Controls` section lets you remap both gameplay inputs (`D-pad`, `A`, `B`, `Select`, `Start`) and runtime shortcuts such as fast forward, max-speed toggle, save/load state, slot cycling, overlay toggle, mute, and menu toggle. Bindings are saved in the runtime preferences file so they persist across launches.
- The `Audio` section exposes host audio enable/disable, mute, master volume, output-device selection, reconnect/buffer reset controls, and a target latency slider. Audio output uses an SDL callback-backed ring buffer, waits for a configurable prefill before unpausing the device, and the wall-clock pacing path eases off sleeping when the audio fill drops below the low-water mark.
- Audio output is intended for real-time play. When `Speed %` is not `100`, the runtime pauses host audio output instead of letting the buffer drift badly out of sync.
- The `Savestates` section gives you 10 persistent slots per game, with quick-save on `F5`, quick-load on `F8`, and delete support from the menu. Savestates are versioned, validated against the loaded ROM, and stored next to the normal `.sav`/`.rtc` files as `.state1` through `.state10`.
- Savestates currently target the same runtime/game build family. Loading a state from the wrong ROM or an incompatible runtime version fails cleanly instead of trying to continue with mismatched memory layouts.
- The settings UI is rendered as a full-screen responsive panel so it remains usable on Android handhelds, phones, tablets, and desktop displays with very different resolutions.

Model-selection notes:

- `--model auto` is the default. It selects CGB for cartridges with header byte `0x143` set to `0x80` or `0xC0`, otherwise DMG.
- `--model cgb` runs DMG cartridges in CGB compatibility mode.
- `--model dmg` rejects CGB-only cartridges with a clear startup error.

For the current Game Boy Color support status, validated behavior, and remaining work, see [GBC.md](GBC.md).

For multi-ROM output, pass launcher options first and then the normal runtime options for the selected game:

```bash
./output/multi_rom/build/multi_rom --game tetris --log-file logs/tetris.log --limit-frames 300
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

When you are chasing generated-to-interpreter fallback, these flags are the most useful starting point:

```bash
./output/game/build/game \
  --log-file logs/interpreter.log \
  --limit 500000 \
  --log-frame-fallbacks \
  --report-interpreter-hotspots \
  --interpreter-hotspot-limit 12
```

`--log-frame-fallbacks` gives you per-frame `first=` / `last=` fallback addresses, while `--report-interpreter-hotspots` prints the aggregate fallback summary at shutdown or when `--limit` stops the run early.

To condense a large fallback log into something easier to scan, use the helper script:

```bash
python3 tools/summarize_interpreter_log.py logs/interpreter.log
```

The script summarizes:

- top fallback sites and repeated `first -> last` frame pairs
- hotspot functions resolved from generated `*_metadata.json` when it can auto-detect them
- likely triggering instructions when it can auto-detect the original ROM
- coverage-gap opcodes reported by the runtime

You can also point it at explicit metadata, symbols, or a ROM if auto-detection is ambiguous:

```bash
python3 tools/summarize_interpreter_log.py logs/interpreter.log \
  --metadata output/pokeblue/pokeblue_metadata.json \
  --sym pokeblue.sym \
  --rom roms/pokeblue.gb \
  --top 12
```

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

### Benchmark Against Emulators

Use `--benchmark` when timing a recompiled binary so the runtime does not sleep to real-time or spend time on host UI work.

The helper script below benchmarks the generated binary against PyBoy by default, repeats each run, samples peak RSS with `psutil`, and writes optional JSON for later comparison.

Generated projects now default to the faster manual-testing profile described above. The benchmark helper still auto-builds a dedicated optimized binary in `build_bench_o3`, forces `Release`, and enables IPO/LTO so benchmark runs keep the old “full optimization” path without slowing down day-to-day manual test builds:

```bash
python3 tools/benchmark_emulators.py roms/tetris.gb \
  --recompiled-binary tetris_test/build/tetris \
  --frames 1800 \
  --repeat 5 \
  --json-out logs/tetris_benchmark.json
```

The report includes:

- mean wall time per runner
- guest frames per second
- effective `x realtime`
- peak RSS in MB
- speedup relative to the emulator baseline

If you want to benchmark the existing binary exactly as-is, disable the auto-build step:

```bash
python3 tools/benchmark_emulators.py roms/tetris.gb \
  --recompiled-binary tetris_test/build/tetris \
  --no-recompiled-autobuild
```

You can also point the optimized benchmark build somewhere else or choose a different generated-source optimization level:

```bash
python3 tools/benchmark_emulators.py roms/pokeblue.gb \
  --recompiled-binary output/pokeblue_interpreter_test/build/pokeblue \
  --recompiled-build-dir output/pokeblue_interpreter_test/build_bench_o3 \
  --recompiled-opt-level 3
```

You can add other emulators with custom command templates:

```bash
python3 tools/benchmark_emulators.py roms/game.gb \
  --recompiled-binary output/game/build/game \
  --frames 1200 \
  --emulator-cmd "OtherEmu=/path/to/emulator {rom}"
```

Template placeholders available to `--emulator-cmd` are `{rom}`, `{frames}`, `{input}`, `{input_script}`, `{input_file}`, and `{recompiled_binary}`.

### Dependency Layout

The runtime now vendors a small Dear ImGui snapshot in `runtime/vendor/imgui` instead of using a git submodule. We only need the core Dear ImGui sources plus the SDL2 and SDLRenderer2 backends, so vendoring that fixed subset keeps fresh clones simpler, avoids recursive-submodule setup, and makes generated launcher builds more predictable.

If you need to update ImGui, keep the vendored copy minimal:

- `imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`
- the core headers and bundled stb headers
- `backends/imgui_impl_sdl2.*`
- `backends/imgui_impl_sdlrenderer2.*`
- `LICENSE.txt`

`mgbdis` is no longer bundled in this repo. The coverage tools work without it when you already have a trace, and the ROM-disassembly path now expects an external install passed with `--mgbdis` or available on `PATH`.

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

If you want the script to disassemble a ROM directly instead of consuming a trace, install `mgbdis` separately and point the script at it:

```bash
python3 tools/compare_ground_truth.py roms/game.gb output/game --mgbdis /path/to/mgbdis.py
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
