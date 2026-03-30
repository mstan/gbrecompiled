#!/usr/bin/env python3
"""Scan VRAM regions for non-zero tile data."""
import json, socket

def query(cmd):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(('127.0.0.1', 4370))
    s.settimeout(5.0)
    s.sendall((json.dumps(cmd) + '\n').encode())
    buf = b''
    while b'\n' not in buf:
        buf += s.recv(4096)
    s.close()
    return json.loads(buf.decode().strip())

# Scan tile data regions
for base in range(0x8000, 0x9800, 0x100):
    r = query({'cmd': 'read_vram', 'addr': hex(base), 'len': 256})
    h = r.get('hex', '')
    nonzero = sum(1 for i in range(0, len(h), 2) if h[i:i+2] != '00')
    if nonzero > 0:
        print(f"  0x{base:04X}-0x{base+0xFF:04X}: {nonzero:3d}/256 bytes used ({nonzero*100//256}%)")

# Scan tilemap
print("\nTilemap 0x9800 (BG):")
for row in range(18):
    addr = 0x9800 + row * 32
    r = query({'cmd': 'read_vram', 'addr': hex(addr), 'len': 20})
    h = r.get('hex', '')
    tiles = [int(h[i:i+2], 16) for i in range(0, len(h), 2)]
    unique = len(set(tiles))
    print(f"  Row {row:2d}: {unique:2d} unique tiles  [{' '.join(f'{t:02X}' for t in tiles[:20])}]")
