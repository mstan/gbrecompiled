#!/usr/bin/env python3
"""Palette-independent framebuffer diff for the GB PPU axis. Loads two P6 PPMs
(recomp --dump-frames vs gb_fb_oracle), maps each to GB shade indices by ranking
its distinct gray levels by luminance, and reports the fraction of differing
pixels (structure, not exact palette). Optionally writes a diff PPM.
Usage: fb_diff.py recomp.ppm oracle.ppm [--out diff.ppm]"""
import sys, argparse
import numpy as np

def load_ppm(path):
    with open(path, "rb") as f:
        raw = f.read()
    assert raw[:2] == b"P6", "not P6"
    # tokenize header (magic, w, h, maxval), skipping #-comments and whitespace
    toks, i, n = [], 2, len(raw)
    while len(toks) < 3:
        while i < n and raw[i:i+1].isspace():
            i += 1
        if raw[i:i+1] == b"#":
            while i < n and raw[i:i+1] != b"\n":
                i += 1
            continue
        j = i
        while j < n and not raw[j:j+1].isspace():
            j += 1
        toks.append(raw[i:j]); i = j
    w, h, maxv = int(toks[0]), int(toks[1]), int(toks[2])
    i += 1  # single whitespace after maxval
    data = np.frombuffer(raw[i:i + w*h*3], dtype=np.uint8).reshape(h, w, 3)
    return data

def to_shades(img):
    lum = (0.299*img[:,:,0] + 0.587*img[:,:,1] + 0.114*img[:,:,2])
    levels = np.unique(lum)
    rank = {v: i for i, v in enumerate(levels)}
    sh = np.vectorize(rank.get)(lum).astype(np.int16)
    return sh, len(levels)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("recomp"); ap.add_argument("oracle")
    ap.add_argument("--out")
    a = ap.parse_args()
    r, o = load_ppm(a.recomp), load_ppm(a.oracle)
    print("recomp %s  oracle %s" % (r.shape[1::-1], o.shape[1::-1]))
    if r.shape != o.shape:
        print("DIMENSION MISMATCH"); sys.exit(1)
    rs, rn = to_shades(r); os_, on = to_shades(o)
    print("distinct gray levels: recomp %d  oracle %d" % (rn, on))
    diff = rs != os_
    n = diff.size; nd = int(diff.sum())
    print("differing pixels: %d / %d  (%.3f%%)" % (nd, n, 100*nd/n))
    # per-shade confusion if both have <=4
    if rn <= 4 and on <= 4 and nd:
        ys, xs = np.where(diff)
        rows = np.unique(ys)
        print("rows with diffs: %d (y %d..%d)" % (len(rows), rows.min(), rows.max()))
    if a.out:
        d = np.zeros_like(r); d[diff] = [255, 0, 0]; d[~diff] = o[~diff]
        with open(a.out, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (r.shape[1], r.shape[0]))
            f.write(d.astype(np.uint8).tobytes())
        print("wrote %s" % a.out)

if __name__ == "__main__":
    main()
