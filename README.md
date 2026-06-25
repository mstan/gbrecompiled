# GB Recompiled — Development Fork

A **static recompiler** for original Game Boy ROMs that translates SM83 assembly into portable C code. Run classic Game Boy games as native executables — no traditional emulator required.

> ### ⚠ This is a development fork. The canonical project is [arcanite24/gb-recompiled](https://github.com/arcanite24/gb-recompiled) — go there for stable use.
>
> This repository (`mstan/gbrecompiled`) is a **personal development fork** by [@mstan](https://github.com/mstan), built on two upstreams: the original recompiler by [arcanite24](https://github.com/arcanite24) and the runtime/hardware work from the [GB-Recomp/gb-recompiled](https://github.com/GB-Recomp/gb-recompiled) fork (merged 2026-06), plus mstan's PPU fixes, TCP debug server, launcher hooks, and game-specific work (Megaman Xtreme 2, Pokemon Red/Blue). It may diverge from either upstream at any time.

This fork inherits the GB-Recomp feature set — see [Fork Additions](#fork-additions) below — and adds the runtime changes documented under [What This Fork Adds](#what-this-fork-adds).

<p align="center">
  <img src="dino.png" alt="GB Recompiled Screenshot" width="400">
</p>

---

## Fork Additions

Everything below is built on top of upstream `arcanite24/gb-recompiled`. Most are runtime / launcher features that any recompiled cart picks up automatically by linking against this fork's `gbrt`.

### Networking & link play
- **BGB-compatible link cable over TCP** — peer-to-peer between two recompiled instances; verified Pokemon trade between Gen 1 carts.
- **LAN peer auto-discovery** — Esc menu's Network section finds other instances on the same LAN.
- **Internet relay** — companion `gb-link-rendezvous` Flask server + `relay_client.c` for NAT-traversal-style introductions. Gameplay still runs P2P over BGB once paired.

### Pokemon Gen 2 Mystery Gift mock
- Scripted Mystery Gift for Pokemon Gold / Silver / Crystal — pick an item or decoration from an Esc-menu carousel, the next in-game IR exchange delivers it. Uses a runtime `gb_dispatch` override that intercepts each cart's `ExchangeMysteryGiftData` entry point and synthesizes a successful exchange.
- Cart-side flow runs unmodified afterward: items go to the Goldenrod counter, decorations into your bedroom PC list (after save + Continue).

### Hardware emulation
- **Super Game Boy** support — palettes, cart-supplied border, MASK_EN. Auto-enables from cart header; can be toggled mid-game.
- **Game Boy Camera** (MBC `$FC`) — exposes a host webcam to the cart via per-platform native APIs (Linux v4l2, macOS AVFoundation, Windows Media Foundation; zero runtime deps). Opt-in via CMake `-DGBRT_ENABLE_GBCAM=ON` (default off) so non-Camera carts don't compile or link it.
- **Game Boy Printer** — virtual paper printer that writes received prints to `<cwd>/prints/*.png`, with smart concatenation across multi-page jobs.
- **GBC IR port (`$FF56`)** — generic state machine for the CGB IR LED + photodiode; used by the Mystery Gift mock above.

### Display pipeline
- **GLES 2.0 rendering backend** with a built-in post-process shader pipeline. One binary covers desktop Mesa, Mali, Adreno — same shaders run everywhere.
- **Shipped shader presets** (sharp / scanlines / CRT-like effects) selectable from the Esc menu's Look section. Per-game shader preference stays sticky once set.

### Per-game UX
- **Per-game preferences** — palette, shader, SGB toggles, custom border, hardware mode (DMG/SGB/CGB/AUTO). Globals act as defaults; overrides save per game ID.
- **Custom SGB borders** — drop 256x224 PNGs into `borders/` next to the binary; cycle from the Esc menu.
- **Cart-border cache** — SGB border decoded once and cached to disk so non-SGB modes can still display it.
- **Pocket / Light palette presets**, **Show FPS overlay**, **audio settings**, **input remapping** with controller support and per-profile labels (Xbox / PlayStation / Nintendo / Generic).
- **Savestates** with multi-slot UI.

### Launcher infrastructure
- **Multi-ROM launcher** with a graphical picker, missing-ROM tagging, and `--game <id>` headless launch.
- **Single-cart auto-start** — when a launcher has exactly one game registered, the picker is skipped and the cart boots straight up. Esc menu's "Return to Launcher" is replaced with "Restart Game" so you can reboot the cart from inside the menu.
- **`--prefix-symbols`** flag on `gbrecomp` so multiple carts can be linked into one binary without symbol collisions.
- **Native asset-loader integration** — ROM data is bundled into the binary as compressed sections that get extracted into `assets/<id>/` on first boot.
- **`roms/` subfolder convention** — user-supplied ROMs live at `roms/<id>.<ext>` next to the binary.

### Recompiler workflow
- **Directory mode** with auto-discovery of per-ROM `.sym` / `.annotations` files.
- **Parallel code generation**, **annotation handling** (skips non-address sym lines), and various analyzer fixes.

### Ecosystem
- [GB-Recomp/pgbcomp](https://github.com/GB-Recomp/pgbcomp) — Pokemon Gen 1 + Gen 2 compilation umbrella that FetchContents six per-game repos plus this fork.
- Per-game repos under [github.com/GB-Recomp](https://github.com/GB-Recomp): `pokered`, `pokeblue`, `pokeyellow`, `pokegold`, `pokesilver`, `pokecrystal` — each builds standalone or links into a compilation.

---

## Downloads
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

- **[arcanite24](https://github.com/arcanite24)** — Original author of gb-recompiled. All recompiler architecture, static analysis engine, code generation, and initial runtime implementation. This project is built on that foundation.
- **[GB-Recomp / christopher-roelofs](https://github.com/GB-Recomp/gb-recompiled)** — The runtime/hardware fork merged in as the base for this development fork (SGB, GB Camera/Printer, IR, GLES2 shaders, savestates, MBC1/2/3/5, netplay, multi-ROM launcher, Android, PyBoy oracle, interpreter-fallback tier).
- **[Matthew Stanley (@mstan)](https://github.com/mstan)** — This development fork: the 3-way merge of the above, PPU latching fixes, audio underrun fade, TCP debug server, pointer/launcher ROM delivery + SHA-256 verification, ANGLE GLES path, the agnostic `game_extras` overlay/hook model, the Tier-0 `dispatch_misses.toml` seed manifest, and the per-game ports (Tetris, Megaman Xtreme 2, Pokémon Red/Blue). Developed with Claude Code.
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
