#!/usr/bin/env python3
"""gbc_cosim.py — Phase-A co-simulation gate coordinator.

Drives the generated game executable's in-process co-sim (`--cosim`) through the
four validation gates from COSIM_ORACLE.md, then runs the actual recomp-vs-interp
(A-vs-B) measurement. Trust NOTHING until gates 1-4 pass.

  Gate 1  recomp-vs-recomp  == 0    (determinism + hashing + host-only excluded)
  Gate 2  interp-vs-interp  == 0    (interpreter determinism)
  Gate 3  injected fault halts at the right subsystem, per target
                                    (proves the tool is not silently blind;
                                     exercises the newly-wired APU/timer/etc.)
  Gate 4  hash-vs-byte audit finds no disagreement

Usage:
  python tools/gbc_cosim.py --exe tetris_test/build/tetris.exe
  python tools/gbc_cosim.py --exe .../tetris.exe --checkpoints 800 --inject-at 100
"""
import argparse
import os
import re
import subprocess
import sys

RESULT_RE = re.compile(r"\[COSIM\] result matched=(\d+) checkpoints=(\d+) frames=(\d+) chain=([0-9A-Fa-f]+)")
DIVERGE_RE = re.compile(r"FIRST DIVERGENCE at checkpoint (\d+) cyc=(\d+) subsystem=(\w+)")
AUDIT_RE = re.compile(r"AUDIT FAILURE at checkpoint (\d+)")


class Run:
    def __init__(self, rc, matched, checkpoints, frames, chain, subsystem, div_cp, audit_fail, raw):
        self.rc = rc
        self.matched = matched
        self.checkpoints = checkpoints
        self.frames = frames
        self.chain = chain
        self.subsystem = subsystem
        self.div_cp = div_cp
        self.audit_fail = audit_fail
        self.raw = raw


def run_cosim(exe, extra, timeout):
    cmd = [exe, "--cosim"] + extra
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        print(f"  ! TIMEOUT after {timeout}s: {' '.join(cmd)}")
        return Run(None, None, None, None, None, None, None, False, (e.stderr or ""))
    err = p.stderr or ""
    matched = checkpoints = frames = chain = None
    subsystem = div_cp = None
    audit_fail = bool(AUDIT_RE.search(err))
    m = RESULT_RE.search(err)
    if m:
        matched = int(m.group(1)) == 1
        checkpoints = int(m.group(2))
        frames = int(m.group(3))
        chain = m.group(4)
    d = DIVERGE_RE.search(err)
    if d:
        div_cp = int(d.group(1))
        subsystem = d.group(3)
    return Run(p.returncode, matched, checkpoints, frames, chain, subsystem, div_cp, audit_fail, err)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True, help="path to the generated game executable")
    ap.add_argument("--checkpoints", type=int, default=800, help="bound each run to N checkpoints")
    ap.add_argument("--stride", type=int, default=456, help="T-cycles per checkpoint")
    ap.add_argument("--inject-at", type=int, default=100, help="checkpoint index to inject the Gate-3 fault")
    ap.add_argument("--timeout", type=int, default=180, help="per-run timeout seconds")
    ap.add_argument("--verbose", action="store_true", help="dump raw stderr for each run")
    args = ap.parse_args()

    # subprocess/CreateProcess on Windows won't resolve a bare relative path the
    # way a shell does — normalize to absolute so `--exe foo/bar.exe` just works.
    args.exe = os.path.abspath(args.exe)
    if not os.path.isfile(args.exe):
        print(f"error: exe not found: {args.exe}")
        return 2

    base = ["--cosim-stride", str(args.stride), "--cosim-checkpoints", str(args.checkpoints)]
    results = []  # (name, passed, detail)

    def record(name, passed, detail, run):
        results.append((name, passed, detail))
        tag = "PASS" if passed else "FAIL"
        print(f"[{tag}] {name}: {detail}")
        if args.verbose and run is not None:
            for line in run.raw.splitlines():
                if "[COSIM]" in line:
                    print("        " + line)

    # Gate 1 — recomp vs recomp == 0
    r = run_cosim(args.exe, base + ["--cosim-pair", "aa"], args.timeout)
    record("Gate1 recomp-vs-recomp", r.matched is True,
           f"matched={r.matched} checkpoints={r.checkpoints} chain={r.chain}", r)
    gate1_chain = r.chain

    # Gate 2 — interp vs interp == 0
    r = run_cosim(args.exe, base + ["--cosim-pair", "bb"], args.timeout)
    record("Gate2 interp-vs-interp", r.matched is True,
           f"matched={r.matched} checkpoints={r.checkpoints} chain={r.chain}", r)

    # Gate 3 — injected fault halts at the right subsystem, one per target.
    # Run on the aa pair so ONLY the injection can cause a divergence.
    for target, expect_sub in [("wram", "wram"), ("ppu", "ppu"), ("apu", "apu"),
                               ("cpu", "cpu"), ("timer", "timer")]:
        r = run_cosim(args.exe, base + ["--cosim-pair", "aa",
                                        "--cosim-inject", target,
                                        "--cosim-inject-at", str(args.inject_at)], args.timeout)
        halted = r.matched is False and r.subsystem is not None
        named_right = r.subsystem == expect_sub
        near = r.div_cp is not None and abs(r.div_cp - args.inject_at) <= 2
        passed = halted and named_right and near
        record(f"Gate3 inject:{target}", passed,
               f"halted={halted} subsystem={r.subsystem} (want {expect_sub}) "
               f"cp={r.div_cp} (want ~{args.inject_at})", r)

    # Gate 4 — hash-vs-byte audit every checkpoint, no disagreement (aa pair).
    r = run_cosim(args.exe, base + ["--cosim-pair", "aa", "--cosim-audit", "1"], args.timeout)
    record("Gate4 hash-vs-byte-audit", r.matched is True and not r.audit_fail,
           f"matched={r.matched} audit_fail={r.audit_fail}", r)

    gates_passed = all(p for _, p, _ in results)
    print("-" * 72)
    print(f"GATES: {'ALL PASS' if gates_passed else 'FAILURES PRESENT'} "
          f"({sum(1 for _,p,_ in results if p)}/{len(results)})")

    if not gates_passed:
        print("Refusing to run A-vs-B measurement until all gates pass.")
        return 1

    # The actual measurement: recomp (A) vs interpreter (B).
    print("-" * 72)
    print("A-vs-B measurement (recomp vs interpreter):")
    r = run_cosim(args.exe, base + ["--cosim-pair", "ab", "--cosim-audit", "64"], args.timeout)
    if r.matched:
        print(f"  MATCHED all {r.checkpoints} checkpoints / {r.frames} frames. "
              f"chain={r.chain}  <-- pinned baseline")
    else:
        print(f"  FIRST DIVERGENCE at checkpoint {r.div_cp} subsystem={r.subsystem}")
        for line in r.raw.splitlines():
            if "[COSIM]" in line:
                print("    " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
