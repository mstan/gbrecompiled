"""Split the per-channel amplitude gap into ENVELOPE-VOLUME vs ACTIVITY.
Both .pch streams are 0-15 digital x4 (recomp DAC build + oracle samples[]).
- active%   = fraction of samples non-zero (note-on x duty/lfsr high)
- level NZ  = distribution of non-zero values = the channel's output level (volume)
If recomp level matches oracle but active% is lower -> activity/trigger difference.
If recomp level is ~half oracle -> envelope-volume bug.
Usage: _pch_amplitude.py recomp.pch oracle.pch"""
import sys, numpy as np

def load(p):
    raw = np.fromfile(p, dtype="<i2"); n = (raw.size // 4) * 4
    return np.abs(raw[:n].reshape(-1, 4).astype(np.float32))

rec, ora = load(sys.argv[1]), load(sys.argv[2])
names = ["CH1 square", "CH2 square", "CH3 wave", "CH4 noise"]
print("%-11s | active%%  (r/o) | meanNZ (r/o) | maxNZ (r/o) | p90NZ (r/o) | level ratio o/r"
      % "")
print("-" * 92)
for c in range(4):
    r, o = rec[:, c], ora[:, c]
    rnz, onz = r[r > 0], o[o > 0]
    ra, oa = (100 * len(rnz) / len(r)), (100 * len(onz) / len(o))
    rm, om = (rnz.mean() if len(rnz) else 0), (onz.mean() if len(onz) else 0)
    rx, ox = (r.max() if len(r) else 0), (o.max() if len(o) else 0)
    rp, op = (np.percentile(rnz, 90) if len(rnz) else 0,
              np.percentile(onz, 90) if len(onz) else 0)
    ratio = (om / rm) if rm > 0 else float("nan")
    print("%-11s | %5.1f / %5.1f | %5.2f / %5.2f | %4d / %4d | %4.0f / %4.0f | x%.2f"
          % (names[c], ra, oa, rm, om, int(rx), int(ox), rp, op, ratio))
print("-" * 92)
print("level ratio ~1 -> volume matches (gap is activity); ~2 -> envelope volume ~half")
