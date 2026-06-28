"""Diagnose the recomp-vs-oracle startup lag: where does audio actually begin in
each stream, and is the cross-correlation peak unique or one of several (loop
aliasing)? Usage: _lag_probe.py recomp.s16 oracle.s16"""
import sys, numpy as np
from scipy.signal import stft, fftconvolve

RATE = 44100

def load(path):
    raw = np.fromfile(path, dtype="<i2")
    if raw.size % 2: raw = raw[:-1]
    return raw.reshape(-1, 2).astype(np.float32).mean(axis=1)

def first_audio_time(x, win=2048, hop=512, thresh_db=-50):
    # frame RMS in dBFS relative to int16 full-scale
    n = (len(x) - win) // hop
    t0 = None
    for i in range(n):
        seg = x[i*hop:i*hop+win]
        rms = np.sqrt(np.mean(seg*seg)) + 1e-9
        db = 20*np.log10(rms/32768.0)
        if db > thresh_db:
            # require it to sustain ~100ms
            j2 = i + int(0.1*RATE/hop)
            seg2 = x[i*hop:j2*hop+win]
            if 20*np.log10(np.sqrt(np.mean(seg2*seg2)+1e-9)/32768.0) > thresh_db:
                return i*hop/RATE
    return None

def onset_env(x, win=1024, hop=512):
    f,t,Z = stft(x, fs=RATE, nperseg=win, noverlap=win-hop, boundary=None)
    flux = np.diff(np.abs(Z), axis=1); flux[flux<0]=0
    env = np.concatenate([[0], flux.sum(axis=0)])
    return env/ (env.max()+1e-12), hop

def top_lags(env_o, env_r, hop, k=6):
    a = env_o - env_o.mean(); b = env_r - env_r.mean()
    corr = fftconvolve(a, b[::-1], mode="full")
    corr /= (np.linalg.norm(a)*np.linalg.norm(b)+1e-12)
    lags = np.arange(len(corr)) - (len(b)-1)
    # find local maxima, sort by value
    idx = np.argsort(corr)[::-1]
    seen=[]; out=[]
    for i in idx:
        L = lags[i]*hop/RATE
        if all(abs(L-s)>0.3 for s in seen):
            seen.append(L); out.append((round(L*1000,1), round(float(corr[i]),3)))
        if len(out)>=k: break
    return out

rec, ora = load(sys.argv[1]), load(sys.argv[2])
print("recomp first-audio  : %.3f s" % (first_audio_time(rec) or -1))
print("oracle first-audio  : %.3f s" % (first_audio_time(ora) or -1))
er,hop = onset_env(rec); eo,_ = onset_env(ora)
print("top xcorr lags (ms, peak)  [lag = recomp relative to oracle]:")
for L,v in top_lags(eo, er, hop):
    print("   %+8.1f ms   %.3f" % (L, v))
