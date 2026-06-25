import json, socket, sys

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

# Scan WRAM for non-zero regions
for base in range(0xC000, 0xD000, 0x100):
    r = query({'cmd': 'read_ram', 'addr': hex(base), 'len': 256})
    h = r.get('hex', '')
    if h != '00' * 256:
        nz = []
        for i in range(0, len(h), 2):
            byte = int(h[i:i+2], 16)
            if byte != 0:
                nz.append(f'{base+i//2:04X}={byte:02X}')
        if nz:
            print(f'  {nz[:20]}')
            if len(nz) > 20:
                print(f'  ... and {len(nz)-20} more')

# Also scan HRAM
r = query({'cmd': 'read_io', 'addr': '0xFF80', 'len': 127})
h = r.get('hex', '')
nz = []
for i in range(0, len(h), 2):
    byte = int(h[i:i+2], 16)
    if byte != 0:
        nz.append(f'{0xFF80+i//2:04X}={byte:02X}')
if nz:
    print(f'HRAM: {nz}')
else:
    print('HRAM: all zeros')
