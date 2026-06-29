"""Confirm the disable mechanism: among samples where the recomp channel is silent &
disabled while the oracle sounds, was length_counter==0 (LENGTH expiry) or dac=0 (DAC)?
state file = 8 int16/sample: [4x enabled<<8|vol][4x len|dac<<12|len_en<<13].
Usage: _pch_state2.py rec.pch ora.pch rec.state8 [lag_ms]"""
import sys, os, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audio_drift_diff as A
RATE = 44100
def load(p, nch):
    raw = np.fromfile(p, dtype="<i2"); n = (raw.size // nch) * nch
    return raw[:n].reshape(-1, nch)
rec = np.abs(load(sys.argv[1], 4).astype(np.float32))
ora = np.abs(load(sys.argv[2], 4).astype(np.float32))
st  = load(sys.argv[3], 8).astype(np.int32)
lag = int(float(sys.argv[4]) * RATE / 1000) if len(sys.argv) > 4 else 512
ora = ora[lag:] if lag >= 0 else ora
m = min(len(rec), len(ora), len(st)); rec, ora, st = rec[:m], ora[:m], st[:m]
enabled = (st[:, :4] & 0x100) != 0
length  = st[:, 4:] & 0xFF
dac     = (st[:, 4:] >> 12) & 1
len_en  = (st[:, 4:] >> 13) & 1
names = ["CH1 square", "CH2 square", "CH3 wave", "CH4 noise"]
def win_active(x, thr=0.5):
    # DC-robust: a disabled-but-DAC-on channel holds a constant DC level (~16) that
    # the old (x>0) test counted as "sounding". AC energy distinguishes a real note.
    return A.win_active_ac(x, RATE, win_s=0.05, thr=thr)
print("among recomp-DISABLED-while-oracle-sounding samples: cause breakdown")
print("%-11s | n disabled | len==0%% | dac==0%% | len_en%% | mean len" % "")
print("-" * 72)
for c in [0, 1, 3]:
    oa = win_active(ora[:, c]); k = min(len(oa), m)
    dis = (rec[:k, c] == 0) & oa[:k] & (~enabled[:k, c])
    n = int(dis.sum())
    if not n:
        print("%-11s | %10d | (none)" % (names[c], 0)); continue
    L, D, LE = length[:k, c][dis], dac[:k, c][dis], len_en[:k, c][dis]
    print("%-11s | %10d | %6.1f | %6.1f | %6.1f | %.1f"
          % (names[c], n, 100*(L == 0).mean(), 100*(D == 0).mean(),
             100*LE.mean(), L.mean()))
print("-" * 72)
print("len==0 high + len_en high => LENGTH COUNTER expired (note cut). dac==0 high => DAC disabled.")
