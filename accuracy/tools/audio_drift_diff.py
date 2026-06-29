#!/usr/bin/env python3
"""
audio_drift_diff.py — drift-tolerant differential comparison of two GB audio
sample streams (recomp vs SameBoy oracle) for the GB accuracy burndown (Axis 5b).

Bit-exact is the aspirational GREEN gate; until the recomp gains sample-accurate
register-write timing + band-limited resampling it will phase-drift and alias, so
the working metric is drift-tolerant:

  1. DC + amplitude normalize    (recomp uses a +/-1 x vol mixer; absolute level
                                  and DC offset are not comparable to SameBoy's DAC)
  2. global cross-correlation lag alignment (onset-envelope based, robust to timbre)
  3. post-alignment similarity:  segmental Pearson correlation over time (shows drift)
  4. onset-timing histogram:     matched onset deltas in ms (rhythm fidelity)
  5. per-note pitch error:       framewise dominant-pitch error in cents (tuning)

Inputs are raw interleaved S16LE stereo @ --rate (default 44100), e.g. the recomp's
debug_audio.raw and the oracle's out.s16.

Usage:
  audio_drift_diff.py --recomp debug_audio.raw --oracle oracle.s16 \
      [--rate 44100] [--json report.json] [--label pokered]
"""
import argparse, json, sys
import numpy as np

try:
    from scipy.signal import stft, find_peaks, fftconvolve, butter, sosfiltfilt
except Exception as e:                                   # pragma: no cover
    print("ERROR: scipy required (%s)" % e, file=sys.stderr); sys.exit(2)


# --------------------------------------------------------------- DC-robust activity
def highpass(x, rate, cut=40.0, order=4):
    """Remove DC + sub-audio low frequencies. A disabled-but-DAC-on GB channel holds
    a constant DC level (digital ~16) in the raw per-channel capture — inaudible (the
    output high-pass removes it) but it inflates raw RMS and masquerades as a sounding
    note. High-passing drops that DC hold (and the slow DAC on/off block switching)
    while keeping the audio-band oscillation of a real note, so activity tests compare
    apples-to-apples vs the recomp (which represents the same state as 0)."""
    x = np.asarray(x, dtype=np.float64)
    if len(x) < 3 * order + 1:
        return (x - np.mean(x)).astype(np.float32)
    sos = butter(order, cut / (rate * 0.5), btype="highpass", output="sos")
    return sosfiltfilt(sos, x).astype(np.float32)


def ac_rms(x, rate, cut=40.0):
    """RMS of the audio-band (DC-removed) signal."""
    return float(np.sqrt(np.mean(highpass(x, rate, cut) ** 2)))


def win_active_ac(x, rate, win_s=0.05, cut=40.0, thr=0.5):
    """Per-sample boolean (repeated per window): window has audio-band AC energy above
    thr. DC holds -> ~0 -> inactive; real notes -> high -> active."""
    h = highpass(x, rate, cut)
    W = max(1, int(win_s * rate)); n = (len(h) // W) * W
    if n == 0:
        return np.zeros(len(x), dtype=bool)
    w = h[:n].reshape(-1, W)
    rms = np.sqrt((w * w).mean(axis=1))
    act = np.repeat(rms > thr, W)
    if len(act) < len(x):                                # pad tail
        act = np.concatenate([act, np.zeros(len(x) - len(act), dtype=bool)])
    return act


# ----------------------------------------------------------------------------- io
def load_s16_stereo(path, rate):
    raw = np.fromfile(path, dtype="<i2")
    if raw.size == 0:
        raise SystemExit("empty stream: %s" % path)
    if raw.size % 2:                                     # tolerate odd tail
        raw = raw[:-1]
    st = raw.reshape(-1, 2).astype(np.float32)
    mono = st.mean(axis=1)
    return mono, st.shape[0]


def normalize(x):
    """DC-remove + unit-RMS. Makes the +/-1xvol recomp comparable to SameBoy."""
    x = x - np.mean(x)
    rms = np.sqrt(np.mean(x * x)) + 1e-12
    return x / rms


# ------------------------------------------------------------------- onset envelope
def onset_envelope(x, rate, hop=512, win=1024):
    """Spectral-flux onset strength envelope (half-wave rectified)."""
    f, t, Z = stft(x, fs=rate, nperseg=win, noverlap=win - hop, boundary=None)
    mag = np.abs(Z)
    flux = np.diff(mag, axis=1)
    flux[flux < 0] = 0.0
    env = flux.sum(axis=0)
    env = np.concatenate([[0.0], env])                  # realign to frame count
    if env.max() > 0:
        env = env / env.max()
    return env, hop


# ------------------------------------------------------------------------ alignment
def best_lag(env_a, env_b):
    """Lag (in env frames) of b relative to a that maximizes cross-correlation.
    Positive lag => b is delayed relative to a."""
    a = env_a - env_a.mean()
    b = env_b - env_b.mean()
    corr = fftconvolve(a, b[::-1], mode="full")
    norm = (np.linalg.norm(a) * np.linalg.norm(b)) + 1e-12
    corr = corr / norm
    idx = int(np.argmax(corr))
    lag = idx - (len(b) - 1)
    return lag, float(corr[idx])


# --------------------------------------------------------------------------- onsets
def onset_times(env, hop, rate, prominence=0.08, min_gap_s=0.04):
    distance = max(1, int((min_gap_s * rate) / hop))
    peaks, _ = find_peaks(env, prominence=prominence, distance=distance)
    return peaks * hop / rate


def onset_histogram(t_ref, t_test, tol_ms=60.0):
    """For each ref onset, nearest test onset within tol; return matched deltas (ms)."""
    if len(t_ref) == 0 or len(t_test) == 0:
        return np.array([]), 0.0
    deltas = []
    tt = np.asarray(t_test)
    for tr in t_ref:
        j = int(np.argmin(np.abs(tt - tr)))
        d = (tt[j] - tr) * 1000.0
        if abs(d) <= tol_ms:
            deltas.append(d)
    matched = len(deltas) / len(t_ref)
    return np.asarray(deltas), matched


# ----------------------------------------------------------------------- pitch error
def dominant_pitch_track(x, rate, hop=512, win=4096, fmin=65.0, fmax=2000.0,
                         n_harm=5):
    """Harmonic-Product-Spectrum f0 tracker. Multiplying the spectrum by its
    1/2,1/3,..1/n decimations reinforces the true fundamental and suppresses the
    octave/harmonic confusion that a single dominant-bin picker suffers from
    (critical here: GB square waves are harmonic-rich). Sub-bin accuracy via
    parabolic interpolation on the log-HPS peak."""
    f, t, Z = stft(x, fs=rate, nperseg=win, noverlap=win - hop, boundary=None)
    mag = np.abs(Z).astype(np.float64)
    nb = mag.shape[0]
    df = rate / win
    kmin = max(1, int(np.floor(fmin / df)))
    kmax = int(np.ceil(fmax / df))
    nframes = mag.shape[1]
    pitch = np.zeros(nframes)
    energy = mag.sum(axis=0)
    voiced = energy > (np.median(energy) * 0.5 + 1e-9)
    logmag = np.log1p(mag)
    for i in range(nframes):
        if not voiced[i]:
            continue
        hps = logmag[:, i].copy()
        for h in range(2, n_harm + 1):
            dec = logmag[::h, i]
            hps[:len(dec)] += dec
        klo, khi = kmin, min(kmax, len(hps) - 2)
        if khi <= klo:
            continue
        k = klo + int(np.argmax(hps[klo:khi]))
        # parabolic interpolation around the peak bin
        if 1 <= k < nb - 1:
            a, b, c = hps[k-1], hps[k], hps[k+1]
            denom = (a - 2*b + c)
            delta = 0.5 * (a - c) / denom if abs(denom) > 1e-12 else 0.0
        else:
            delta = 0.0
        pitch[i] = (k + delta) * df
    return pitch


def pitch_cents_error(p_ref, p_test):
    n = min(len(p_ref), len(p_test))
    p_ref, p_test = p_ref[:n], p_test[:n]
    both = (p_ref > 0) & (p_test > 0)
    if both.sum() == 0:
        return None
    cents = 1200.0 * np.log2(p_test[both] / p_ref[both])
    return cents


# -------------------------------------------------------------------- segmental corr
def segmental_corr(a, b, rate, win_s=1.0):
    """Raw-waveform Pearson per window. Secondary 'phase' indicator only — it is
    near-zero/negative whenever timbres differ (recomp +/-1 square vs DAC) even
    when the music is the same, so it is NOT the similarity headline."""
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    w = int(win_s * rate)
    cs = []
    for i in range(0, n - w, w):
        sa, sb = a[i:i+w], b[i:i+w]
        da, db = sa - sa.mean(), sb - sb.mean()
        denom = (np.linalg.norm(da) * np.linalg.norm(db)) + 1e-12
        cs.append(float(np.dot(da, db) / denom))
    return np.asarray(cs)


# ----------------------------------------------------------------- spectral similarity
def _mel_filterbank(sr, n_fft, n_mels=40, fmin=40.0, fmax=None):
    fmax = fmax or sr / 2
    def hz2mel(f): return 2595.0 * np.log10(1.0 + f / 700.0)
    def mel2hz(m): return 700.0 * (10.0 ** (m / 2595.0) - 1.0)
    mpts = np.linspace(hz2mel(fmin), hz2mel(fmax), n_mels + 2)
    hzpts = mel2hz(mpts)
    bins = np.floor((n_fft + 1) * hzpts / sr).astype(int)
    fb = np.zeros((n_mels, n_fft // 2 + 1), dtype=np.float32)
    for m in range(1, n_mels + 1):
        l, c, r = bins[m-1], bins[m], bins[m+1]
        if c == l: c += 1
        if r == c: r += 1
        for k in range(l, c):
            if 0 <= k < fb.shape[1]: fb[m-1, k] = (k - l) / max(1, (c - l))
        for k in range(c, r):
            if 0 <= k < fb.shape[1]: fb[m-1, k] = (r - k) / max(1, (r - c))
    return fb


def spectral_similarity(a, b, rate, win=1024, hop=512, n_mels=40):
    """Drift-tolerant similarity: per-frame cosine similarity of log-mel spectra.
    Timbre-aware and phase-invariant — the correct 'how close is the music' number.
    Returns array of per-frame cosine sims over voiced (energetic) frames."""
    n = min(len(a), len(b)); a, b = a[:n], b[:n]
    fb = _mel_filterbank(rate, win, n_mels=n_mels)
    def logmel(x):
        f, t, Z = stft(x, fs=rate, nperseg=win, noverlap=win-hop, boundary=None)
        m = fb @ np.abs(Z)
        return np.log1p(m), np.abs(Z).sum(axis=0)
    ma, ea = logmel(a); mb, eb = logmel(b)
    k = min(ma.shape[1], mb.shape[1])
    ma, mb, ea, eb = ma[:, :k], mb[:, :k], ea[:k], eb[:k]
    voiced = (ea > np.median(ea)*0.5) & (eb > np.median(eb)*0.5)
    sims = []
    for i in range(k):
        if not voiced[i]:
            continue
        u, v = ma[:, i], mb[:, i]
        sims.append(float(np.dot(u, v) / ((np.linalg.norm(u)*np.linalg.norm(v)) + 1e-12)))
    return np.asarray(sims)


def pct(a, q):
    return float(np.percentile(a, q)) if len(a) else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--recomp", required=True, help="recomp S16LE stereo stream")
    ap.add_argument("--oracle", required=True, help="SameBoy S16LE stereo stream")
    ap.add_argument("--rate", type=int, default=44100)
    ap.add_argument("--label", default="run")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    rec, nrec = load_s16_stereo(args.recomp, args.rate)
    ora, nora = load_s16_stereo(args.oracle, args.rate)
    rec_n, ora_n = normalize(rec), normalize(ora)

    env_r, hop = onset_envelope(rec_n, args.rate)
    env_o, _   = onset_envelope(ora_n, args.rate)

    lag_frames, env_corr = best_lag(env_o, env_r)        # rec relative to oracle
    lag_samples = lag_frames * hop
    lag_ms = 1000.0 * lag_samples / args.rate

    # Align rec to oracle by shifting.
    if lag_samples >= 0:
        a = ora_n[lag_samples:]; b = rec_n
    else:
        a = ora_n; b = rec_n[-lag_samples:]
    m = min(len(a), len(b))
    a, b = a[:m], b[:m]

    seg = segmental_corr(a, b, args.rate, win_s=1.0)
    spec = spectral_similarity(a, b, args.rate)

    t_o = onset_times(env_o, hop, args.rate)
    t_r = onset_times(env_r, hop, args.rate)
    t_r_aligned = t_r - (lag_samples / args.rate)
    deltas, matched = onset_histogram(t_o, t_r_aligned)

    p_o = dominant_pitch_track(a, args.rate)
    p_r = dominant_pitch_track(b, args.rate)
    cents = pitch_cents_error(p_o, p_r)

    report = {
        "label": args.label,
        "rate": args.rate,
        "recomp_samples": int(nrec), "oracle_samples": int(nora),
        "recomp_seconds": nrec / args.rate, "oracle_seconds": nora / args.rate,
        "alignment": {
            "lag_samples": int(lag_samples), "lag_ms": round(lag_ms, 2),
            "onset_env_xcorr_peak": round(env_corr, 4),
        },
        "spectral_similarity": {
            "logmel_cosine_mean": round(float(np.mean(spec)), 4) if len(spec) else None,
            "logmel_cosine_p10": round(pct(spec, 10), 4) if len(spec) else None,
            "logmel_cosine_p50": round(pct(spec, 50), 4) if len(spec) else None,
            "voiced_frames": int(len(spec)),
        },
        "post_align_similarity_raw_waveform": {
            "segmental_corr_mean": round(float(np.mean(seg)), 4) if len(seg) else None,
            "segmental_corr_min": round(float(np.min(seg)), 4) if len(seg) else None,
            "segmental_corr_p10": round(pct(seg, 10), 4) if len(seg) else None,
            "n_windows": int(len(seg)),
            "drift_trend_first_vs_last": (
                round(float(np.mean(seg[:max(1,len(seg)//5)]) -
                            np.mean(seg[-max(1,len(seg)//5):])), 4)
                if len(seg) >= 5 else None),
        },
        "onset_timing": {
            "ref_onsets": int(len(t_o)), "test_onsets": int(len(t_r)),
            "matched_fraction": round(matched, 3),
            "delta_ms_median": round(pct(deltas, 50), 2),
            "delta_ms_p10": round(pct(deltas, 10), 2),
            "delta_ms_p90": round(pct(deltas, 90), 2),
            "delta_ms_abs_mean": round(float(np.mean(np.abs(deltas))), 2) if len(deltas) else None,
        },
        "pitch_error_cents": None if cents is None else {
            "voiced_frames": int(len(cents)),
            "median_abs": round(float(np.median(np.abs(cents))), 2),
            "p90_abs": round(float(np.percentile(np.abs(cents), 90)), 2),
            "within_50c_fraction": round(float(np.mean(np.abs(cents) <= 50)), 3),
        },
    }

    # ---- human-readable summary
    print("=" * 64)
    print("GB audio drift-tolerant diff  [%s]  @ %d Hz" % (args.label, args.rate))
    print("=" * 64)
    print("streams      : recomp %.2fs (%d) | oracle %.2fs (%d)"
          % (report["recomp_seconds"], nrec, report["oracle_seconds"], nora))
    al = report["alignment"]
    print("alignment    : lag %+d samples (%+.1f ms)  onset-env xcorr peak=%.3f"
          % (al["lag_samples"], al["lag_ms"], al["onset_env_xcorr_peak"]))
    sp = report["spectral_similarity"]
    print("spectral sim : log-mel cosine mean=%s p50=%s p10=%s  (%d voiced frames)  "
          "<- drift-tolerant headline"
          % (sp["logmel_cosine_mean"], sp["logmel_cosine_p50"],
             sp["logmel_cosine_p10"], sp["voiced_frames"]))
    ps = report["post_align_similarity_raw_waveform"]
    print("raw-wave corr: seg mean=%s min=%s p10=%s  (%d x 1s win; ~0 expected: timbre differs)"
          % (ps["segmental_corr_mean"], ps["segmental_corr_min"],
             ps["segmental_corr_p10"], ps["n_windows"]))
    if ps["drift_trend_first_vs_last"] is not None:
        print("drift        : first-fifth minus last-fifth seg-corr = %+.4f "
              "(>0 => correlation decays over time)" % ps["drift_trend_first_vs_last"])
    ot = report["onset_timing"]
    print("onset timing : ref=%d test=%d matched=%.0f%%  delta_ms median=%s "
          "[p10=%s p90=%s] |mean|=%s"
          % (ot["ref_onsets"], ot["test_onsets"], 100*ot["matched_fraction"],
             ot["delta_ms_median"], ot["delta_ms_p10"], ot["delta_ms_p90"],
             ot["delta_ms_abs_mean"]))
    pe = report["pitch_error_cents"]
    if pe:
        print("pitch error  : |median|=%.1f cents  p90=%.1f  within-50c=%.0f%%  (%d voiced)"
              % (pe["median_abs"], pe["p90_abs"], 100*pe["within_50c_fraction"],
                 pe["voiced_frames"]))
    print("=" * 64)

    if args.json:
        with open(args.json, "w") as f:
            json.dump(report, f, indent=2)
        print("wrote %s" % args.json)


if __name__ == "__main__":
    main()
