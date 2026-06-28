#!/usr/bin/env python3
"""
perchannel_diff.py — per-channel (CH1-4) drift-tolerant diff, to localize which GB
APU channel diverges (the mixed-stream cosine can't). Inputs are the interleaved
int16 x4 per-channel streams: the recomp's debug_audio_pch.raw (GBRT_AUDIO_PCH=1)
and the oracle's <out>.pch (GBRT_AUDIO_PCH=1). Reuses audio_drift_diff metrics.

Encodings differ (recomp bipolar +/-vol, oracle unipolar 0-15 digital) — all metrics
are DC+amplitude-normalized so that washes out; we compare per-channel WAVEFORM
structure (onset/spectral) and, for pitched channels, pitch. CH4 (noise) has no
pitch, so it uses the energy-envelope correlation instead.

Usage: perchannel_diff.py --recomp debug_audio_pch.raw --oracle oracle.s16.pch [--rate 44100]
"""
import argparse, os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audio_drift_diff as A   # normalize, onset_envelope, best_lag, spectral_similarity, dominant_pitch_track, pitch_cents_error

NAMES = ["CH1 square", "CH2 square", "CH3 wave", "CH4 noise"]


def load_pch(path, nch=4):
    raw = np.fromfile(path, dtype="<i2")
    n = (raw.size // nch) * nch
    return raw[:n].reshape(-1, nch).astype(np.float32)


def raw_rms(x):
    # per-channel values are unscaled digital (0-15 oracle / +/-15 recomp), so
    # measure activity in raw units, not int16-full-scale dBFS.
    return float(np.sqrt(np.mean(x * x)))


def align(a, b, lag_samples):
    if lag_samples >= 0:
        a2, b2 = a[lag_samples:], b
    else:
        a2, b2 = a, b[-lag_samples:]
    m = min(len(a2), len(b2))
    return a2[:m], b2[:m]


def energy_env_corr(a, b, rate, win=1024, hop=512):
    def env(x):
        from scipy.signal import stft
        f, t, Z = stft(x, fs=rate, nperseg=win, noverlap=win - hop, boundary=None)
        return np.abs(Z).sum(axis=0)
    ea, eb = env(a), env(b)
    k = min(len(ea), len(eb)); ea, eb = ea[:k], eb[:k]
    da, db = ea - ea.mean(), eb - eb.mean()
    return float(np.dot(da, db) / ((np.linalg.norm(da) * np.linalg.norm(db)) + 1e-12))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--recomp", required=True)
    ap.add_argument("--oracle", required=True)
    ap.add_argument("--rate", type=int, default=44100)
    ap.add_argument("--active-rms", type=float, default=0.5, help="channel-active threshold (raw RMS)")
    args = ap.parse_args()

    rec = load_pch(args.recomp)
    ora = load_pch(args.oracle)
    print("=" * 78)
    print("Per-channel drift diff   recomp %d smp | oracle %d smp  @ %d Hz"
          % (rec.shape[0], ora.shape[0], args.rate))
    print("=" * 78)
    print("%-11s %7s %7s  %7s %7s  %7s  %s"
          % ("channel", "rec rms", "ora rms", "lag ms", "onsetX", "specCos", "pitch/energy"))
    print("-" * 78)

    for c in range(4):
        r, o = rec[:, c], ora[:, c]
        rrms, orms = raw_rms(r), raw_rms(o)
        r_act, o_act = rrms > args.active_rms, orms > args.active_rms
        if not r_act and not o_act:
            print("%-11s %7.2f %7.2f  %s" % (NAMES[c], rrms, orms, "(silent both)"))
            continue
        if r_act != o_act:
            who = "recomp only" if r_act else "ORACLE only"
            print("%-11s %7.2f %7.2f  >> ACTIVITY MISMATCH: %s plays this channel"
                  % (NAMES[c], rrms, orms, who))
            continue
        rn, on = A.normalize(r), A.normalize(o)
        env_r, hop = A.onset_envelope(rn, args.rate)
        env_o, _ = A.onset_envelope(on, args.rate)
        lag_frames, onsetx = A.best_lag(env_o, env_r)
        lag_samples = lag_frames * hop
        a, b = align(on, rn, lag_samples)            # oracle, recomp aligned
        spec = A.spectral_similarity(a, b, args.rate)
        spec_cos = float(np.median(spec)) if len(spec) else float("nan")
        if c == 3:  # noise: no pitch, use energy-envelope correlation
            tail = "energy-env corr %.3f" % energy_env_corr(a, b, args.rate)
        else:
            p_o = A.dominant_pitch_track(a, args.rate)
            p_r = A.dominant_pitch_track(b, args.rate)
            cents = A.pitch_cents_error(p_o, p_r)
            if cents is None or len(cents) == 0:
                tail = "pitch n/a"
            else:
                tail = "pitch |med| %.0fc  <50c %.0f%%" % (
                    np.median(np.abs(cents)), 100 * np.mean(np.abs(cents) <= 50))
        print("%-11s %7.2f %7.2f  %+7.1f %7.3f  %7.3f  %s"
              % (NAMES[c], rrms, orms, 1000.0 * lag_samples / args.rate, onsetx, spec_cos, tail))
    print("=" * 78)


if __name__ == "__main__":
    main()
