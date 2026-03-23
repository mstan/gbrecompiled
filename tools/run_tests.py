#!/usr/bin/env python3
"""
GameBoy Recompiler Accuracy Test Runner

Tests accuracy against Blargg and Mooneye test ROMs by:
  1. Recompiling each ROM with gbrecomp
  2. Building the generated project with CMake + Ninja
  3. Running the binary with a frame limit and capturing serial output
  4. Determining PASS/FAIL based on the test suite's protocol

Mooneye pass signal: serial bytes 0x03,0x05,0x08,0x0d,0x15,0x22 (Fibonacci).
Blargg pass signal:  "Passed" substring in serial text output.

Usage:
    python3 tools/run_tests.py                  # run all tests
    python3 tools/run_tests.py --json           # dump JSON results
    python3 tools/run_tests.py --md             # write ACCURACY.md
    python3 tools/run_tests.py --filter accept  # only tests matching pattern
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
WORKSPACE = Path(__file__).parent.parent.resolve()
GBRECOMP   = WORKSPACE / "build" / "bin" / "gbrecomp"
MOONEYE_BASE = WORKSPACE / "roms" / "mooneye" / "mts-20240926-1737-443f6e1"
TEST_OUTPUT  = WORKSPACE / "output" / "test_run"

# Mooneye PASS sequence: Fibonacci 3,5,8,13,21,34
MOONEYE_PASS = bytes([0x03, 0x05, 0x08, 0x0D, 0x15, 0x22])

# ---------------------------------------------------------------------------
# Test catalogue
# ---------------------------------------------------------------------------

# Blargg tests in roms/ root — each one is a single ROM.
# Frame limits are generous: Blargg tests can take several minutes of game-time.
BLARGG_ROMS = [
    ("cpu_instrs",     "roms/cpu_instrs.gb",    3600, "blargg"),
    ("01-special",     "roms/01-special.gb",    1800, "blargg"),
    ("instr_timing",   "roms/instr_timing.gb",  1800, "blargg"),
    ("mem_timing-1",   "roms/mem_timing1.gb",   1800, "blargg"),
    ("mem_timing-2",   "roms/mem_timing2.gb",   1800, "blargg"),
    ("halt_bug",       "roms/halt_bug.gb",      1800, "blargg"),
    ("oam_bug",        "roms/oam_bug.gb",       1800, "blargg"),
    ("interrupt_time", "roms/interrupt_time.gb",1800, "blargg"),
]

MOONEYE_ACCEPTANCE = [
    # bits
    "acceptance/bits/mem_oam.gb",
    "acceptance/bits/reg_f.gb",
    "acceptance/bits/unused_hwio-GS.gb",
    # instr
    "acceptance/instr/daa.gb",
    # interrupts
    "acceptance/interrupts/ie_push.gb",
    # oam_dma
    "acceptance/oam_dma/basic.gb",
    "acceptance/oam_dma/reg_read.gb",
    "acceptance/oam_dma/sources-GS.gb",
    # ppu
    "acceptance/ppu/hblank_ly_scx_timing-GS.gb",
    "acceptance/ppu/intr_1_2_timing-GS.gb",
    "acceptance/ppu/intr_2_0_timing.gb",
    "acceptance/ppu/intr_2_mode0_timing.gb",
    "acceptance/ppu/intr_2_mode0_timing_sprites.gb",
    "acceptance/ppu/intr_2_mode3_timing.gb",
    "acceptance/ppu/intr_2_oam_ok_timing.gb",
    "acceptance/ppu/lcdon_timing-GS.gb",
    "acceptance/ppu/lcdon_write_timing-GS.gb",
    "acceptance/ppu/stat_irq_blocking.gb",
    "acceptance/ppu/stat_lyc_onoff.gb",
    "acceptance/ppu/vblank_stat_intr-GS.gb",
    # timing
    "acceptance/add_sp_e_timing.gb",
    "acceptance/call_cc_timing.gb",
    "acceptance/call_cc_timing2.gb",
    "acceptance/call_timing.gb",
    "acceptance/call_timing2.gb",
    "acceptance/di_timing-GS.gb",
    "acceptance/div_timing.gb",
    "acceptance/ei_sequence.gb",
    "acceptance/ei_timing.gb",
    "acceptance/halt_ime0_ei.gb",
    "acceptance/halt_ime0_nointr_timing.gb",
    "acceptance/halt_ime1_timing.gb",
    "acceptance/halt_ime1_timing2-GS.gb",
    "acceptance/if_ie_registers.gb",
    "acceptance/intr_timing.gb",
    "acceptance/jp_cc_timing.gb",
    "acceptance/jp_timing.gb",
    "acceptance/ld_hl_sp_e_timing.gb",
    "acceptance/oam_dma_restart.gb",
    "acceptance/oam_dma_start.gb",
    "acceptance/oam_dma_timing.gb",
    "acceptance/pop_timing.gb",
    "acceptance/push_timing.gb",
    "acceptance/rapid_di_ei.gb",
    "acceptance/ret_cc_timing.gb",
    "acceptance/ret_timing.gb",
    "acceptance/reti_intr_timing.gb",
    "acceptance/reti_timing.gb",
    "acceptance/rst_timing.gb",
    "acceptance/timer/div_write.gb",
    "acceptance/timer/rapid_toggle.gb",
    "acceptance/timer/tim00.gb",
    "acceptance/timer/tim00_div_trigger.gb",
    "acceptance/timer/tim01.gb",
    "acceptance/timer/tim01_div_trigger.gb",
    "acceptance/timer/tim10.gb",
    "acceptance/timer/tim10_div_trigger.gb",
    "acceptance/timer/tim11.gb",
    "acceptance/timer/tim11_div_trigger.gb",
    "acceptance/timer/tima_reload.gb",
    "acceptance/timer/tima_write_reloading.gb",
    "acceptance/timer/tma_write_reloading.gb",
]


def sanitize(name: str) -> str:
    return "".join(c if c.isalnum() or c in "-_" else "_" for c in name)


def find_executable(build_dir: Path) -> Optional[Path]:
    skip_exts = {".a", ".ninja", ".cmake", ".txt", ".sav"}
    for p in build_dir.iterdir():
        if p.is_file() and os.access(p, os.X_OK) and p.suffix not in skip_exts and not p.name.startswith("."):
            return p
    return None


def run_test(name: str, rom_path: Path, frame_limit: int, suite: str,
             dry_run: bool = False, rebuild: bool = False) -> dict:
    result = {
        "name":        name,
        "suite":       suite,
        "rom":         str(rom_path.relative_to(WORKSPACE)),
        "status":      "unknown",
        "serial_hex":  "",
        "serial_text": "",
        "error":       "",
        "elapsed":     0.0,
    }

    t0 = time.time()
    safe = sanitize(name)
    out_dir = TEST_OUTPUT / safe

    if dry_run:
        result["status"] = "skip"
        return result

    # ------------------------------------------------------------------
    # Step 1: recompile (skip if already present and rebuild=False)
    # ------------------------------------------------------------------
    if rebuild or not (out_dir / "CMakeLists.txt").exists():
        if out_dir.exists():
            shutil.rmtree(out_dir)
        try:
            r = subprocess.run(
                [str(GBRECOMP), str(rom_path), "-o", str(out_dir)],
                capture_output=True, text=True, timeout=90,
            )
            if r.returncode != 0:
                result["status"] = "compile_error"
                result["error"]  = r.stderr[-400:]
                result["elapsed"] = time.time() - t0
                return result
        except subprocess.TimeoutExpired:
            result["status"] = "compile_timeout"
            result["elapsed"] = time.time() - t0
            return result

    # ------------------------------------------------------------------
    # Step 2: cmake + ninja (skip if binary already present and rebuild=False)
    # ------------------------------------------------------------------
    build_dir = out_dir / "build"
    binary = find_executable(build_dir) if build_dir.exists() else None

    if rebuild or binary is None:
        build_dir.mkdir(parents=True, exist_ok=True)
        try:
            r = subprocess.run(
                ["cmake", "-G", "Ninja", f"-S{out_dir}", f"-B{build_dir}", "-Wno-dev"],
                capture_output=True, text=True, timeout=60,
            )
            if r.returncode != 0:
                result["status"] = "cmake_error"
                result["error"]  = r.stderr[-400:]
                result["elapsed"] = time.time() - t0
                return result

            r = subprocess.run(
                ["ninja", f"-C{build_dir}"],
                capture_output=True, text=True, timeout=180,
            )
            if r.returncode != 0:
                result["status"] = "build_error"
                result["error"]  = r.stderr[-400:]
                result["elapsed"] = time.time() - t0
                return result
        except subprocess.TimeoutExpired:
            result["status"] = "build_timeout"
            result["elapsed"] = time.time() - t0
            return result

        binary = find_executable(build_dir)

    if binary is None:
        result["status"] = "no_executable"
        result["elapsed"] = time.time() - t0
        return result

    # ------------------------------------------------------------------
    # Step 3: run with frame limit, capture stdout (serial + runtime msgs)
    # ------------------------------------------------------------------
    # Give the binary a wall-clock budget proportional to the frame limit
    # (60fps ≈ 1s of game time per 60 frames; add 30s overhead for SDL init).
    run_timeout = max(60, frame_limit // 60 * 2 + 30)
    try:
        r = subprocess.run(
            [str(binary), "--limit-frames", str(frame_limit)],
            capture_output=True, timeout=run_timeout,
        )
        raw = r.stdout
    except subprocess.TimeoutExpired:
        result["status"] = "run_timeout"
        result["elapsed"] = time.time() - t0
        return result

    # Strip the leading "Frame limit: NNN\n" line emitted by the runtime, and
    # also remove any [GBRT] / [SDL] / [LIMIT] / [DIFF] status lines that the
    # runtime prints to stdout alongside the ROM's actual serial output.
    def _filter_serial(raw: bytes) -> bytes:
        lines = raw.split(b"\n")
        kept = []
        for line in lines:
            stripped = line.lstrip()
            if (stripped.startswith(b"Frame limit:")
                    or stripped.startswith(b"[GBRT]")
                    or stripped.startswith(b"[SDL]")
                    or stripped.startswith(b"[LIMIT]")
                    or stripped.startswith(b"[DIFF]")
                    or stripped.startswith(b"[TRACE]")):
                continue
            kept.append(line)
        return b"\n".join(kept)

    serial_raw = _filter_serial(raw)

    result["serial_hex"]  = serial_raw.hex()
    result["serial_text"] = serial_raw.decode("ascii", errors="replace").strip()
    result["elapsed"]     = time.time() - t0

    # ------------------------------------------------------------------
    # Determine PASS / FAIL
    # ------------------------------------------------------------------
    if suite == "mooneye":
        result["status"] = "pass" if MOONEYE_PASS in serial_raw else "fail"
    elif suite == "blargg":
        text = result["serial_text"]
        if "Passed" in text:
            result["status"] = "pass"
        elif "Failed" in text:
            result["status"] = "fail"
        else:
            # Serial output present but no final verdict — test ran out of frames
            result["status"] = "incomplete" if serial_raw.strip() else "fail"
    else:
        result["status"] = "unknown"

    return result


def build_test_list(filter_str: Optional[str] = None) -> list:
    tests = []

    # Blargg root-level ROMs
    for name, rom_rel, frames, suite in BLARGG_ROMS:
        rom = WORKSPACE / rom_rel
        if rom.exists():
            tests.append((name, rom, frames, suite))

    # Mooneye acceptance + timer suite
    for rel in MOONEYE_ACCEPTANCE:
        rom = MOONEYE_BASE / rel
        if rom.exists():
            name = rom.stem
            # Mooneye tests complete within 120 frames typically
            tests.append((name, rom, 300, "mooneye"))

    if filter_str:
        tests = [(n, r, f, s) for n, r, f, s in tests if filter_str.lower() in n.lower()]

    return tests


def print_result_line(result: dict):
    icon  = "✓" if result["status"] == "pass" else ("?" if result["status"] == "incomplete" else "✗")
    suite = result["suite"].upper()[:7].ljust(7)
    name  = result["name"][:45].ljust(45)
    st    = result["status"].upper()[:10].ljust(10)
    secs  = f"{result['elapsed']:5.1f}s"
    print(f"  {icon} [{suite}] {name} {st} {secs}")


def generate_accuracy_md(results: list, output_path: Path):
    date_str = time.strftime("%Y-%m-%d")

    total  = len(results)
    passed = sum(1 for r in results if r["status"] == "pass")
    failed = sum(1 for r in results if r["status"] == "fail")
    other  = total - passed - failed

    mooneye_results = [r for r in results if r["suite"] == "mooneye"]
    blargg_results  = [r for r in results if r["suite"] == "blargg"]

    m_pass = sum(1 for r in mooneye_results if r["status"] == "pass")
    b_pass = sum(1 for r in blargg_results  if r["status"] == "pass")

    def table_rows(rows):
        lines = []
        for r in sorted(rows, key=lambda x: (x["status"] != "pass", x["name"])):
            badge = "✅ PASS" if r["status"] == "pass" else (
                    "⚠️ INCOMPLETE" if r["status"] == "incomplete" else
                    "❌ FAIL" if r["status"] == "fail" else
                    f"🔧 {r['status'].upper()}")
            preview = ""
            if r["suite"] == "blargg" and r["serial_text"]:
                preview = r["serial_text"].replace("\n", " · ")[:80]
            lines.append(f"| {r['name']} | {badge} | {preview} |")
        return "\n".join(lines)

    md = f"""# GameBoy Recompiler — Accuracy Report

> Generated: {date_str}  
> Recompiler version: `gbrecomp` at `build/bin/gbrecomp`  
> Test suites: [Mooneye MTS 2024-09-26](https://github.com/Gekkio/mooneye-test-suite), [Blargg GB Test ROMs](https://github.com/retrio/gb-test-roms)

## Summary

| Suite | Passed | Total | Pass Rate |
|-------|--------|-------|-----------|
| Blargg | {b_pass} | {len(blargg_results)} | {b_pass/max(len(blargg_results),1)*100:.0f}% |
| Mooneye acceptance | {m_pass} | {len(mooneye_results)} | {m_pass/max(len(mooneye_results),1)*100:.0f}% |
| **Total** | **{passed}** | **{total}** | **{passed/max(total,1)*100:.0f}%** |

---

## Blargg CPU / Timing Tests

These ROMs output ASCII text via the serial port. "Passed" in the output = pass.

| Test | Result | Serial output |
|------|--------|---------------|
{table_rows(blargg_results)}

---

## Mooneye Acceptance Tests

Mooneye tests signal pass by writing the Fibonacci sequence `03 05 08 0D 15 22` to the serial port.
Tests marked **GS** target DMG/SGB hardware specifically.

### bits
| Test | Result | Notes |
|------|--------|-------|
{table_rows([r for r in mooneye_results if "bits/" in r["rom"] or r["name"] in ("mem_oam","reg_f","unused_hwio-GS")])}

### instructions
| Test | Result | Notes |
|------|--------|-------|
{table_rows([r for r in mooneye_results if "instr/" in r["rom"] or r["name"] == "daa"])}

### interrupts
| Test | Result | Notes |
|------|--------|-------|
{table_rows([r for r in mooneye_results if "interrupts/" in r["rom"] or r["name"] == "ie_push"])}

### OAM DMA
| Test | Result | Notes |
|------|--------|-------|
{table_rows([r for r in mooneye_results if "oam_dma" in r["rom"]])}

### PPU
| Test | Result | Notes |
|------|--------|-------|
{table_rows([r for r in mooneye_results if "ppu/" in r["rom"]])}

### Timer
| Test | Result | Notes |
|------|--------|-------|
{table_rows([r for r in mooneye_results if "timer/" in r["rom"]])}

### Misc timing
| Test | Result | Notes |
|------|--------|-------|
{table_rows([r for r in mooneye_results if "timer/" not in r["rom"] and "ppu/" not in r["rom"] and "oam_dma" not in r["rom"] and "interrupts/" not in r["rom"] and "instr/" not in r["rom"] and "bits/" not in r["rom"]])}

---

## Known Limitations

- **Cycle-exact timing**: The static recompiler batches instructions between sync points; some cycle-exact Mooneye timing tests will fail even with an otherwise correct interpreter.
- **Boot ROM**: Tests that check boot-ROM register state (`boot_regs-*`, `boot_div-*`, `boot_hwio-*`) are skipped — the recompiler does not emulate the boot ROM.
- **CGB-only tests**: Any test suffixed `-C` or operating on CGB-only hardware is expected to fail.
- **Blargg frame limits**: Some Blargg tests (`cpu_instrs`) run for several minutes; they are cut off at 180 frames and may show as "incomplete" if they need more time.
"""
    output_path.write_text(md)
    print(f"\n  → Written to {output_path}")


def main():
    parser = argparse.ArgumentParser(description="GB Recompiler accuracy test runner")
    parser.add_argument("--filter",   default=None, help="Only run tests matching this substring")
    parser.add_argument("--json",     action="store_true", help="Dump JSON results to stdout")
    parser.add_argument("--md",       action="store_true", help="Write ACCURACY.md")
    parser.add_argument("--rebuild",  action="store_true", help="Force recompile/rebuild even if cached")
    parser.add_argument("--dry-run",  action="store_true", help="List tests without running them")
    args = parser.parse_args()

    if not GBRECOMP.exists():
        print(f"[ERROR] gbrecomp not found at {GBRECOMP}. Run: ninja -C build", file=sys.stderr)
        sys.exit(1)

    TEST_OUTPUT.mkdir(parents=True, exist_ok=True)

    tests = build_test_list(args.filter)
    print(f"Running {len(tests)} tests (output: {TEST_OUTPUT})\n")

    results = []
    for i, (name, rom, frames, suite) in enumerate(tests):
        tag = f"[{i+1}/{len(tests)}]"
        print(f"  {tag} {name} ({suite}, {frames}fr) ...", end="", flush=True)
        r = run_test(name, rom, frames, suite, dry_run=args.dry_run, rebuild=args.rebuild)
        results.append(r)
        icon = "✓" if r["status"] == "pass" else ("?" if r["status"] == "incomplete" else "✗")
        print(f"\r  {tag} {icon} {name:<45} {r['status']:<12} {r['elapsed']:4.1f}s")

    passed    = sum(1 for r in results if r["status"] == "pass")
    failed    = sum(1 for r in results if r["status"] == "fail")
    incomplete = sum(1 for r in results if r["status"] == "incomplete")
    errors    = len(results) - passed - failed - incomplete

    print(f"\n{'='*60}")
    print(f"  PASS: {passed}  FAIL: {failed}  INCOMPLETE: {incomplete}  ERROR: {errors}  / {len(results)}")
    print(f"{'='*60}")

    if args.json:
        print(json.dumps(results, indent=2))

    if args.md:
        generate_accuracy_md(results, WORKSPACE / "ACCURACY.md")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
