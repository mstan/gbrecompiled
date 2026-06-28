"""Root-cause the square/noise note-drop: among samples where the recomp's channel
output is SILENT but the oracle's channel is sounding, what is the recomp's internal
state? enabled=0 => channel DISABLED (length/sweep/DAC dropped the note); enabled=1 &
vol=0 => envelope decayed; enabled=1 & vol>0 => duty-low (benign).
Inputs: recomp output .pch, oracle output .pch, recomp state (pchstate, int16x4 =
(enabled<<8)|volume). Usage: _pch_state.py rec.pch ora.pch rec_state.raw [lag_ms]"""
import sys, numpy as np
RATE = 44100
def load(p, nch=4):
    raw = np.fromfile(p, dtype="<i2"); n = (raw.size // nch) * nch
    return raw[:n].reshape(-1, nch)
rec = np.abs(load(sys.argv[1]).astype(np.float32))
ora = np.abs(load(sys.argv[2]).astype(np.float32))
st  = load(sys.argv[3]).astype(np.int32)
lag = int(float(sys.argv[4]) * RATE / 1000) if len(sys.argv) > 4 else 512
# align oracle to recomp (recomp lags oracle by +lag)
ora = ora[lag:] if lag >= 0 else ora
m = min(len(rec), len(ora), len(st)); rec, ora, st = rec[:m], ora[:m], st[:m]
enabled = (st & 0x100) != 0
volume  = (st & 0xF).astype(np.float32)
names = ["CH1 square", "CH2 square", "CH3 wave", "CH4 noise"]

# oracle "sounding" = active in a 50ms window around the sample
W = int(0.05 * RATE)
def windowed_active(x, thr=0.10):
    # fraction non-zero per window, broadcast back to samples
    n = (len(x) // W) * W
    w = x[:n].reshape(-1, W)
    act = (w > 0).mean(axis=1) > thr
    return np.repeat(act, W)

print("among samples where recomp-silent BUT oracle-sounding, recomp channel state:")
print("%-11s | n samples | disabled%% | env-zero%% | duty(vol>0)%%" % "")
print("-" * 66)
for c in range(4):
    o_act = windowed_active(ora[:, c])
    k = min(len(o_act), m)
    sil = (rec[:k, c] == 0) & o_act[:k]          # recomp silent while oracle sounding
    n = int(sil.sum())
    if n == 0:
        print("%-11s | %9d | %s" % (names[c], 0, "(no such samples)")); continue
    en = enabled[:k][sil]; vo = volume[:k][sil]
    disabled = float((~en).mean())
    envzero  = float((en & (vo == 0)).mean())
    dutylow  = float((en & (vo > 0)).mean())
    print("%-11s | %9d | %8.1f | %8.1f | %8.1f"
          % (names[c], n, 100*disabled, 100*envzero, 100*dutylow))
print("-" * 66)
print("disabled%% high => NOTE DROPPED (channel disabled: length/sweep/DAC).")
print("env-zero%% high => envelope decayed to 0. duty%% => benign duty-low (window too coarse).")
