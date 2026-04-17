import json, socket

def q(cmd):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(('127.0.0.1', 4370))
    s.settimeout(5.0)
    s.sendall((json.dumps(cmd) + '\n').encode())
    buf = b''
    while b'\n' not in buf:
        buf += s.recv(4096)
    s.close()
    return json.loads(buf.decode().strip())

print('Window tilemap 0x9C00:')
for row in range(18):
    addr = 0x9C00 + row * 32
    r = q({'cmd': 'read_vram', 'addr': hex(addr), 'len': 20})
    h = r.get('hex', '')
    tiles = [int(h[i:i+2], 16) for i in range(0, len(h), 2)]
    unique = len(set(tiles))
    if unique > 1:
        print(f'  Row {row:2d}: [{" ".join(f"{t:02X}" for t in tiles[:20])}]')
    else:
        print(f'  Row {row:2d}: blank (0x{tiles[0]:02X})')
