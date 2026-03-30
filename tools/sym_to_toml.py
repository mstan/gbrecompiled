#!/usr/bin/env python3
"""Convert a .sym file to TOML [entry_points] for gb-recompiled.

Usage: python sym_to_toml.py pokered.sym [--filter-data] > entry_points.toml

The .sym format is: BB:AAAA SymbolName
where BB is the bank (hex) and AAAA is the address (hex).

We filter out:
- Labels that are clearly data (contain words like Table, Data, Text, Tiles, etc.)
- Labels with '.' in them (sub-labels like .loop, .done)
- RAM addresses (banks >= 0x80 or addresses in WRAM/HRAM ranges)
"""
import sys, re
from collections import defaultdict

DATA_KEYWORDS = {
    'table', 'data', 'text', 'tiles', 'tilemap', 'gfx', 'pic',
    'string', 'pointer', 'pointers', 'list', 'map', 'maps',
    'sprite', 'sprites', 'font', 'palette', 'palettes',
    'bytes', 'words', 'incbin', 'charmap', 'attr',
    'evolution', 'learnset', 'moveset', 'basestat',
    'name', 'names', 'header', 'block', 'blocks',
    'collision', 'object', 'objects', 'sign', 'signs',
    'warp', 'warps', 'connection', 'border',
}

def is_likely_code(name):
    """Heuristic: is this symbol name likely a code label vs data?"""
    lower = name.lower()
    # Sub-labels are internal — skip them
    if '.' in name:
        return False
    # Check for data keywords
    for kw in DATA_KEYWORDS:
        if kw in lower:
            return False
    return True

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <file.sym>", file=sys.stderr)
        sys.exit(1)

    sym_path = sys.argv[1]
    filter_data = '--filter-data' in sys.argv

    banks = defaultdict(list)
    total = 0
    skipped_data = 0
    skipped_sublabel = 0
    skipped_ram = 0

    with open(sym_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith(';'):
                continue

            match = re.match(r'^([0-9a-fA-F]+):([0-9a-fA-F]+)\s+(.+)$', line)
            if not match:
                continue

            bank = int(match.group(1), 16)
            addr = int(match.group(2), 16)
            name = match.group(3)
            total += 1

            # Skip RAM (WRAM, HRAM, etc.)
            if bank >= 0x80 or addr >= 0x8000:
                skipped_ram += 1
                continue

            if filter_data and not is_likely_code(name):
                if '.' in name:
                    skipped_sublabel += 1
                else:
                    skipped_data += 1
                continue

            banks[bank].append((addr, name))

    # Deduplicate addresses per bank
    for bank in banks:
        seen = set()
        unique = []
        for addr, name in sorted(banks[bank]):
            if addr not in seen:
                seen.add(addr)
                unique.append((addr, name))
        banks[bank] = unique

    # Print stats
    code_count = sum(len(v) for v in banks.values())
    print(f"# Generated from {sym_path}", file=sys.stderr)
    print(f"# Total symbols: {total}", file=sys.stderr)
    print(f"# Skipped RAM: {skipped_ram}", file=sys.stderr)
    if filter_data:
        print(f"# Skipped data labels: {skipped_data}", file=sys.stderr)
        print(f"# Skipped sub-labels: {skipped_sublabel}", file=sys.stderr)
    print(f"# Code entry points: {code_count} across {len(banks)} banks", file=sys.stderr)

    # Output TOML
    print("[entry_points]")
    for bank in sorted(banks.keys()):
        entries = banks[bank]
        if not entries:
            continue
        addrs = ", ".join(f"0x{addr:04X}" for addr, name in entries)
        print(f"bank_{bank} = [{addrs}]")

if __name__ == '__main__':
    main()
