import sys

for fname in sys.argv[1:]:
    with open(fname, 'rb') as f:
        magic = f.readline()
        dims = f.readline()
        maxval = f.readline()
        data = f.read()
        px = [data[i:i+3] for i in range(0, len(data), 3)]
        unique = len(set(px))
        w, h = 160, 144
        print(f"{fname}: {unique} unique colors, {len(px)} pixels")
        # Sample center pixels
        for y in [36, 72, 108]:
            row = []
            for x in [20, 40, 60, 80, 100, 120, 140]:
                idx = (y * w + x) * 3
                r, g, b = data[idx], data[idx+1], data[idx+2]
                row.append(f"({r},{g},{b})")
            print(f"  y={y}: {' '.join(row)}")
