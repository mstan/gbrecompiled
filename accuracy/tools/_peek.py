import sys, numpy as np
x = np.fromfile(sys.argv[1], dtype="<i2").reshape(-1, 2).astype(np.float32)
print("file", sys.argv[1], "samples", x.shape, "peak", int(np.abs(x).max()),
      "rms", round(float(np.sqrt((x**2).mean())), 1))
r = 44100
for s in range(min(12, x.shape[0]//r)):
    seg = x[s*r:(s+1)*r]
    print("  s%d peak=%5d rms=%6.1f" % (s, int(np.abs(seg).max()),
                                        float(np.sqrt((seg**2).mean()))))
