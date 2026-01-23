# Expert Workflow: Ground Truth Guided Recompilation

This guide describes the "Expert" workflow for achieving near-100% code coverage in complex Game Boy ROMs (like Pokémon, Zelda, or games with large jump tables) using dynamic execution data from PyBoy and the recompiler's trace-guided analysis.

## The Strategy
Static analysis alone can struggle with "computed control flow" (e.g., jump tables where the destination is loaded from a data table). By using a mature emulator (PyBoy) to record an execution trace, we can "seed" our static recompiler with a list of proven entry points, effectively bridging the gap between static and dynamic analysis.

---

## Quick Start: Automated Workflow (Recommended)

For most cases, use the automated workflow script:

```bash
# Full automated workflow (capture trace, compile, verify coverage)
python3 tools/run_ground_truth.py roms/pokeblue.gb
```

This single command will:
1. Capture a 5-minute execution trace with random input
2. Recompile the ROM with trace-guided analysis
3. Verify the coverage of the recompiled code
4. Optionally refine and merge traces for better coverage

## Manual Workflow Steps

### Step 1: Capture Ground Truth (Dynamic)
First, we run the original ROM in a headless PyBoy instance to record every unique instruction address executed.

```bash
# Run for 5 minutes (18000 frames) with random input to explore the ROM
python3 tools/capture_ground_truth.py roms/pokeblue.gb -o pokeblue_ground.trace --frames 18000 --random
```

- **Output**: `pokeblue_ground.trace` containing `Bank:Address` pairs.
- **Benefit**: This trace contains the "Ground Truth"—code that is guaranteed to be executable.

### Step 2: Trace-Guided Recompilation
Now, we feed this trace into the recompiler. The recompiler will use these addresses as "roots" for its recursive descent analysis.

```bash
# Recompile using the ground truth trace
./build/bin/gbrecomp roms/pokeblue.gb -o pokeblue_output --use-trace pokeblue_ground.trace
```

- **What happens**: The recompiler loads the trace and immediately marks those addresses as function entry points. It then follows every subsequent branch from those points, discovering much more code than a blind scan would.

### Step 3: Verify Coverage
Use the comparison tool to see how much of the dynamic execution was successfully recompiled.

```bash
# Compare the recompiled C code against the ground truth trace
python3 tools/compare_ground_truth.py --trace pokeblue_ground.trace pokeblue_output
```

- **Success Metric**: For a well-seeded trace, you should see **>99% coverage**.
- **Missing Instructions**: Any missing instructions are likely specific to RAM-resident code (which should be handled by the interpreter fallback).

## Step 4: Refine the Trace (Optional)
If you find the game crashes in a specific section that wasn't covered by the random trace, you can generate a more specific trace using the recompiled binary itself.

1. **Run recompiled game with profiling**:
   ```bash
   ./pokeblue_output/build/pokeblue --trace-entries pokeblue_refined.trace --limit 2000000
   ```
2. **Merge or combine traces**:
   You can append traces together to broaden the analysis.
   ```bash
   cat pokeblue_refined.trace >> pokeblue_ground.trace
   sort -u pokeblue_ground.trace -o pokeblue_ground.trace
   ```
3. **Re-recompile**:
   Repeat Step 2 with the expanded trace.

---

## Summary of Tools

| Tool | Purpose |
|------|---------|
| `tools/run_ground_truth.py` | **Automated workflow** - captures trace, recompiles, and verifies coverage in one command. |
| `tools/capture_ground_truth.py` | Runs original ROM in PyBoy to generate a `.trace` file. |
| `gbrecomp --use-trace <file>` | Loads a `.trace` file to seed function discovery. |
| `tools/compare_ground_truth.py` | Measures coverage of a recompiled project against a `.trace`. |
| `runtime --trace-entries <file>` | Logs execution from the *recompiled* binary for further refinement. |
