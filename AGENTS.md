## Project Overview
This project is a static Game Boy recompiler. It turns ROMs into generated C plus a shared runtime.

Important directories:

- `recompiler/`: source for `gbrecomp`
- `runtime/`: shared runtime library (`gbrt`) used by generated projects
- `roms/`: source ROMs used for testing and local experiments
- `output/`: generated ROM projects and multi-ROM launcher outputs
- `logs/`: runtime logs, interpreter logs, benchmark results, and repro captures
- `tools/`: helper scripts for testing, benchmarking, trace capture, and log analysis

Use `README.md` for current user-facing workflows and `TODO.md` for the live project backlog. Do not rely on `ROADMAP.md` or `ARCH.md`; they are not current repo anchors anymore.

## Build Standards
- Always use CMake + Ninja.
- Run shell commands from the repo root when possible.
- Prefer absolute paths in tool output and references when useful, but keep shell commands readable.

Main project:

```bash
cmake -G Ninja -B build .
ninja -C build
```

## Required Sync Workflow
If you change recompiler logic, generated CLI/codegen behavior, runtime behavior, or runtime headers, keep the generated test project in sync.

Canonical sync flow:

```bash
ninja -C build
./build/bin/gbrecomp <checked-in test ROM> -o <checked-in generated test project>
cmake -G Ninja -S <checked-in generated test project> -B <checked-in generated test project>/build
ninja -C <checked-in generated test project>/build
```

Notes:

- Use the checked-in generated test project as the default smoke test.
- Put ad hoc generated projects under `output/`, not the repo root.
- Put logs under `logs/`, not next to binaries.
- If a generated project looks like it is ignoring a runtime change, rebuild that generated project explicitly. Stale generated builds can produce misleading results.

## Current State Worth Remembering
- Multi-ROM generation exists and emits a shared launcher project.
- The multi-ROM launcher is graphical by default and uses SDL + ImGui.
- Battery-backed save loading/saving is wired through the runtime and SDL platform layer.
- Interpreter fallback instrumentation exists and should be used before guessing about coverage/performance problems.
- Benchmarking support exists and should use the benchmark helper script, not ad hoc wall-clock timing of normal windowed builds.

## Accuracy Workflow
When working on correctness, use the runtime and tooling that already exist before inventing new debug paths.

Preferred tools:

- Differential mode for generated vs interpreter comparisons:
  ```bash
  ./output/game/build/game --differential 500000 --differential-log 100000
  ```
- Record and replay input for stable repro:
  ```bash
  ./output/game/build/game --log-file logs/session.log --record-input logs/session.input
  ./output/game/build/game --input "$(cat logs/session.input)"
  ```
- Use cycle-anchored recorded inputs when possible. `--record-input` already writes those by default.
- If you need screenshots or frame comparisons, use `--dump-frames`, `--dump-present-frames`, and the existing tools under `tools/`.

If debugging a game-specific issue, prefer keeping:

- the ROM in `roms/`
- the generated project in `output/<name>/`
- the log in `logs/<name>.log`
- the recorded input in `logs/<name>.input`

## Interpreter Fallback Workflow
Interpreter fallback is already instrumented. Use that signal.

Start here:

```bash
./output/game/build/game \
  --log-file logs/interpreter.log \
  --limit 500000 \
  --log-frame-fallbacks \
  --report-interpreter-hotspots \
  --interpreter-hotspot-limit 12
```

Then summarize with:

```bash
python3 tools/summarize_interpreter_log.py logs/interpreter.log
```

Use this workflow when:

- generated execution falls back unexpectedly
- performance is poor and you suspect interpreter use
- you need hotspot data to guide recompiler improvements

Important interpretation rule:

- if the summary says `No interpreter fallback recorded`, the remaining slowdown is not an interpreter-coverage problem

## Recompiler Debugging
If analysis hangs, misses code, or produces obviously bad output:

- trace analysis:
  ```bash
  ./build/bin/gbrecomp roms/game.gb --trace
  ```
- stop early to find where analysis goes off the rails:
  ```bash
  ./build/bin/gbrecomp roms/game.gb --limit 10000
  ```
- watch for `[ERROR] Undefined instruction` lines
- use `--add-entry-point` for known missed code entrypoints
- use `--no-scan` if aggressive scanning is misclassifying data as code

If you are working with imported symbols or generated names, remember that generated projects can emit `*_metadata.json` sidecars. Prefer using those instead of scraping generated C when a sidecar already answers the question.

## Performance Workflow
Do not benchmark normal interactive/windowed runs. Use benchmark mode.

Preferred workflow:

```bash
python3 tools/benchmark_emulators.py roms/game.gb \
  --recompiled-binary output/game/build/game \
  --frames 1800 \
  --repeat 5 \
  --warmup 1 \
  --json-out logs/game_benchmark.json
```

Important benchmark rules:

- The benchmark script auto-builds a dedicated optimized recompiled binary in `build_bench_o3` by default.
- Generated projects now default to a release-first profile: `Release`, `GBRECOMP_GENERATED_OPT_LEVEL=3`, and IPO/LTO enabled when the toolchain supports it.
- The dedicated benchmark build still matters because it gives you a clean output directory and an easy place to override optimization knobs without disturbing your day-to-day build tree.
- Use `--no-recompiled-autobuild` only if you intentionally want to benchmark the exact binary already on disk.
- The benchmark script already forces headless benchmark mode through environment variables and runtime flags.

If a benchmark result looks suspiciously close to 60 FPS or "real-time locked", suspect a stale generated binary first.

## Slowdown Investigation
If the problem is runtime speed rather than strict correctness:

- use `--debug-performance`
- use `--log-slow-frames`
- use `--log-slow-vsync`
- use `--log-lcd-transitions`
- combine with `--record-input` so you can rerun the exact same scene

This is especially useful for LCD timing, rendering artifacts, and other game-specific scene timing issues.

## Multi-ROM Workflow
The recompiler supports generating a shared multi-ROM launcher from a folder of ROMs.

General pattern:

```bash
./build/bin/gbrecomp /path/to/rom_folder -o output/my_multi_rom
cmake -G Ninja -S output/my_multi_rom -B output/my_multi_rom/build
ninja -C output/my_multi_rom/build
./output/my_multi_rom/build/my_multi_rom
```

Use `output/` for these generated projects. Do not scatter generated launcher projects around the repo.

## When Updating Docs
If you add or materially change:

- runtime CLI flags
- generated project layout
- benchmarking workflow
- interpreter analysis workflow
- launcher behavior

then update `README.md` as part of the same task.

Update `AGENTS.md` too if the change affects how future agents should work in this repo.

## Practical Defaults
- Prefer the checked-in generated test project for quick smoke validation.
- Prefer a representative larger generated project under `output/` for heavier validation and benchmark checks.
- Prefer `logs/` for anything you may want to inspect later.
- Prefer existing tools in `tools/` over one-off scripts when the repo already has the needed workflow.
