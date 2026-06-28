#!/usr/bin/env python3
"""
gb_cycle_compare.py — GB cycle/state comparator (Axis 2), the GB analog of the PSX
cycle_compare. Diffs the recomp's always-on per-frame ring (debug-server
`frame_timeseries`, port 4370) against SameBoy's per-frame ring (gb_state_oracle CSV).

Both rings are queried for the window of interest (never arm-then-time). Reports:
  * cycles-per-frame on each side (mean/std)            -> is pacing equal?
  * cumulative cycle drift (offset-removed)             -> timing divergence onset/rate
  * first sustained state divergence (pc/a/sp/bank)     -> the lever for state forks

Usage:
  # pull recomp ring live from a running recomp, compare to oracle CSV:
  gb_cycle_compare.py --oracle oracle_state.csv --port 4370 --frames 600 \
      [--save-recomp recomp_state.csv] [--label pokered]
  # or compare two CSVs offline:
  gb_cycle_compare.py --oracle oracle_state.csv --recomp-csv recomp_state.csv
"""
import argparse, json, socket, sys, csv


# ----------------------------------------------------------------- recomp ring client
def recomp_pull(host, port, frames, chunk=200, timeout=10.0):
    s = socket.create_connection((host, port), timeout=timeout)
    s.settimeout(timeout)
    buf = b""
    def rpc(obj):
        nonlocal buf
        s.sendall((json.dumps(obj) + "\n").encode())
        while b"\n" not in buf:
            d = s.recv(65536)
            if not d:
                raise IOError("recomp closed")
            buf += d
        line, buf = buf.split(b"\n", 1)
        return json.loads(line.decode())
    out = {}
    f = 0
    while f < frames:
        e = min(f + chunk - 1, frames - 1)
        r = rpc({"cmd": "frame_timeseries", "start": f, "end": e})
        if not r.get("ok"):
            raise IOError("frame_timeseries err: %s" % r)
        for rec in r["ts"]:
            if rec is None:
                continue
            out[int(rec["f"])] = {
                "pc": int(rec["pc"]) & 0xFFFF, "a": int(rec["a"]) & 0xFF,
                "sp": int(rec["sp"]) & 0xFFFF, "cyc": int(rec["cyc"]) & 0xFFFFFFFF,
                "bank": int(rec["bk"]),
                "lcdc": int(rec.get("lcdc", 0)) & 0xFF, "ly": int(rec.get("ly", 0)) & 0xFF,
            }
        f = e + 1
    try:
        s.sendall(b'{"cmd":"quit"}\n')
    except Exception:
        pass
    s.close()
    return out


def load_csv(path):
    out = {}
    with open(path, newline="") as fh:
        for row in csv.DictReader(fh):
            out[int(row["frame"])] = {
                "pc": int(row["pc"]) & 0xFFFF, "a": int(row["a"]) & 0xFF,
                "sp": int(row["sp"]) & 0xFFFF, "cyc": int(row["tcyc"]),
                "bank": int(row["bank"]),
                "lcdc": int(row.get("lcdc", 0)) & 0xFF, "ly": int(row.get("ly", 0)) & 0xFF,
            }
    return out


def save_csv(path, ring):
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["frame", "tcyc", "pc", "a", "sp", "bank", "lcdc", "ly"])
        for f in sorted(ring):
            r = ring[f]
            w.writerow([f, r["cyc"], r["pc"], r["a"], r["sp"], r["bank"],
                        r.get("lcdc", 0), r.get("ly", 0)])


def lcd_transitions(ring):
    """Frames where LCDC bit7 (LCD enable) changes, as (frame, 'on'|'off')."""
    out = []
    prev = None
    for f in sorted(ring):
        on = bool(ring[f]["lcdc"] & 0x80)
        if prev is not None and on != prev:
            out.append((f, "on" if on else "off"))
        prev = on
    return out


def mean_std(xs):
    if not xs:
        return (float("nan"), float("nan"))
    m = sum(xs) / len(xs)
    v = sum((x - m) ** 2 for x in xs) / len(xs)
    return (m, v ** 0.5)


def per_frame_cycles(ring, frames, wrap=1 << 32):
    """Δcyc between consecutive frames, handling uint32 wrap (recomp side)."""
    out = {}
    for f in frames[1:]:
        if f - 1 in ring and f in ring:
            d = ring[f]["cyc"] - ring[f - 1]["cyc"]
            if d < 0:
                d += wrap
            out[f] = d
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--oracle", required=True, help="gb_state_oracle CSV")
    ap.add_argument("--recomp-csv", help="recomp ring CSV (offline mode)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4370)
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--save-recomp", help="write the pulled recomp ring to CSV")
    ap.add_argument("--label", default="run")
    args = ap.parse_args()

    oracle = load_csv(args.oracle)
    if args.recomp_csv:
        recomp = load_csv(args.recomp_csv)
    else:
        recomp = recomp_pull(args.host, args.port, args.frames)
        if args.save_recomp:
            save_csv(args.save_recomp, recomp)

    common = sorted(set(oracle) & set(recomp))
    if not common:
        print("No overlapping frames between oracle and recomp.", file=sys.stderr)
        sys.exit(1)
    lo, hi = common[0], common[-1]

    # cycles-per-frame pacing
    o_cpf = per_frame_cycles(oracle, common)
    r_cpf = per_frame_cycles(recomp, common)
    om, osd = mean_std(list(o_cpf.values()))
    rm, rsd = mean_std(list(r_cpf.values()))

    def median(xs):
        s = sorted(xs); n = len(s)
        return float("nan") if n == 0 else (s[n//2] if n % 2 else (s[n//2-1]+s[n//2])/2)

    # steady-state = rendered frames near the DMG ideal (excludes LCD-off gap frames)
    def steady(cpf):
        return [d for d in cpf.values() if 69000 <= d <= 71500]
    o_st, r_st = steady(o_cpf), steady(r_cpf)
    o_med, r_med = median(list(o_cpf.values())), median(list(r_cpf.values()))
    o_out = sum(1 for d in o_cpf.values() if not 69000 <= d <= 71500)
    r_out = sum(1 for d in r_cpf.values() if not 69000 <= d <= 71500)

    # Odd frames = Δcyc outside the steady band (LCD-off / init multi-frame gaps).
    def odd_frames(ring, frames):
        cpf = per_frame_cycles(ring, frames)
        return [(f, cpf[f], round(cpf[f] / 70224.0, 2))
                for f in sorted(cpf) if not 69000 <= cpf[f] <= 71500]
    o_odd, r_odd = odd_frames(oracle, common), odd_frames(recomp, common)

    # Cycle-aligned state comparison (robust to frame-index offset): for each
    # oracle frame, find the recomp frame with the nearest cumulative cycle; the
    # difference in their FRAME INDICES is the game-progress lag in frames.
    import bisect
    r_frames = sorted(recomp)
    r_cycs = [recomp[f]["cyc"] for f in r_frames]
    def nearest_recomp(cyc):
        i = bisect.bisect_left(r_cycs, cyc)
        cands = [j for j in (i - 1, i) if 0 <= j < len(r_frames)]
        return min(cands, key=lambda j: abs(r_cycs[j] - cyc))
    # progress lag at the end of the window: at the oracle's final cycle, how many
    # frames of game-progress is the recomp ahead/behind?
    j = nearest_recomp(oracle[hi]["cyc"])
    progress_lag_frames = r_frames[j] - hi   # +ve: recomp has rendered more frames per cycle

    # cumulative drift, offset-removed at first common frame (frame-index based)
    off = recomp[lo]["cyc"] - oracle[lo]["cyc"]
    last_drift = (recomp[hi]["cyc"] - oracle[hi]["cyc"]) - off
    drift_onset = next((f for f in common if abs((recomp[f]["cyc"] - oracle[f]["cyc"]) - off) > 8), None)

    print("=" * 70)
    def fmt_odd(odd):
        return ", ".join("%d(%.1fx)" % (f, m) for f, _, m in odd[:10]) or "none"

    print("GB cycle/state comparator  [%s]   frames %d..%d (%d common)"
          % (args.label, lo, hi, len(common)))
    print("=" * 70)
    print("pacing steady: oracle %.1f +/- %.1f (%d fr)   recomp %.1f +/- %.1f (%d fr)   ideal 70224"
          % (mean_std(o_st)[0], mean_std(o_st)[1], len(o_st),
             mean_std(r_st)[0], mean_std(r_st)[1], len(r_st)))
    print("  -> steady cycle pacing %s"
          % ("MATCHES (not a clock-rate bug)"
             if abs(mean_std(o_st)[0] - mean_std(r_st)[0]) < 50 else "DIFFERS"))
    print("odd frames   : oracle %d  %s" % (len(o_odd), fmt_odd(o_odd)))
    print("               recomp %d  %s" % (len(r_odd), fmt_odd(r_odd)))
    print("               (frame(Nx) = a >1-frame LCD-off/init gap of N frames' cycles)")
    o_excess = sum(d - 70224 for _, d, _ in o_odd)
    r_excess = sum(d - 70224 for _, d, _ in r_odd)
    print("gap cycles   : oracle +%d (~%.1f fr)  recomp +%d (~%.1f fr)  -> recomp extra ~%.1f fr"
          % (o_excess, o_excess/70224, r_excess, r_excess/70224,
             (r_excess - o_excess)/70224))
    print("progress lag : at the window's end, recomp game-progress is %+d frames vs oracle"
          % progress_lag_frames)
    print("               (by nearest-cumulative-cycle match; robust to frame-index offset)")
    o_tr, r_tr = lcd_transitions(oracle), lcd_transitions(recomp)
    def fmt_tr(tr):
        return ", ".join("%d:%s" % (f, s) for f, s in tr[:12]) or "none"
    print("LCDC on/off  : oracle %s" % fmt_tr(o_tr))
    print("               recomp %s" % fmt_tr(r_tr))
    print("=" * 70)


if __name__ == "__main__":
    main()
