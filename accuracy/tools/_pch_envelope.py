"""Decisive split: does the recomp's volume ENVELOPE decay faster (per-note fade),
or are notes just triggered LESS (gaps at full volume)? For one channel, over
aligned time windows, report peak level (= envelope volume when sounding) and
active%. If recomp peak tracks oracle but in fewer windows -> activity/trigger.
If recomp peak is consistently lower within sounding windows -> envelope decay.
Usage: _pch_envelope.py recomp.pch oracle.pch <chan 0-3> [lag_ms]"""
import sys, numpy as np

RATE = 44100
def load(p):
    raw = np.fromfile(p, dtype="<i2"); n = (raw.size // 4) * 4
    return np.abs(raw[:n].reshape(-1, 4).astype(np.float32))

rec = load(sys.argv[1]); ora = load(sys.argv[2]); ch = int(sys.argv[3])
lag = int(float(sys.argv[4]) * RATE / 1000) if len(sys.argv) > 4 else 512  # +11.6ms default
r = rec[:, ch]; o = ora[:, ch]
# align: recomp lags oracle by +lag samples
if lag >= 0: o = o[lag:]
else: r = r[-lag:]
m = min(len(r), len(o)); r, o = r[:m], o[:m]

W = int(0.05 * RATE)  # 50 ms windows
rows = []
for i in range(0, m - W, W):
    rw, ow = r[i:i+W], o[i:i+W]
    rnz, onz = rw[rw > 0], ow[ow > 0]
    rows.append((i / RATE,
                 100*len(rnz)/W, 100*len(onz)/W,                      # active%
                 (np.percentile(rnz,95) if len(rnz) else 0),         # peak level when sounding
                 (np.percentile(onz,95) if len(onz) else 0)))
rows = np.array(rows)
# windows where BOTH are sounding (active>10%): compare peak levels
both = (rows[:,1] > 10) & (rows[:,2] > 10)
ro = rows[both]
print("channel %d  windows=%d  both-sounding=%d" % (ch, len(rows), both.sum()))
if both.sum():
    print("  when BOTH sounding: peak level recomp %.1f vs oracle %.1f  (ratio o/r %.2f)"
          % (ro[:,3].mean(), ro[:,4].mean(), ro[:,4].mean()/max(0.1, ro[:,3].mean())))
# windows oracle sounds but recomp silent (note dropped / decayed early)
ora_only = (rows[:,2] > 10) & (rows[:,1] < 2)
print("  oracle-sounds-but-recomp-silent windows: %d / %d oracle-active (%.0f%%)"
      % (ora_only.sum(), (rows[:,2] > 10).sum(),
         100*ora_only.sum()/max(1,(rows[:,2] > 10).sum())))
print("  mean active%%: recomp %.1f  oracle %.1f" % (rows[:,1].mean(), rows[:,2].mean()))
print("INTERPRETATION: ratio~1 + many oracle-only windows => activity/trigger (notes dropped/cut);")
print("                ratio>>1 in both-sounding windows  => envelope decays too fast / lower volume")
