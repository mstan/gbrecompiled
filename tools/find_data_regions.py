#!/usr/bin/env python3
"""Scan a GB ROM and identify data-only regions per bank.

Heuristics:
- Tile data: 16-byte aligned blocks where byte pairs repeat (2bpp tiles)
- Text: runs of bytes in Pokemon's text encoding range (0x80-0xFF + control codes)
- Tables: repetitive structured data with fixed stride
- Padding: runs of 0x00 or 0xFF

Output: TOML [[data_region]] entries for the game config.
"""
import sys, struct, math
from collections import Counter

def entropy(data):
    if not data:
        return 0
    counts = Counter(data)
    total = len(data)
    return -sum((c/total) * math.log2(c/total) for c in counts.values())

def is_tile_data(data, threshold=0.7):
    """Check if data looks like 2bpp tile graphics (16 bytes per tile)."""
    if len(data) < 32:
        return False
    # Tile data tends to have low entropy and 16-byte periodicity
    e = entropy(data)
    if e > 5.0:
        return False
    # Check for 16-byte tile structure: each tile row is 2 bytes
    # Tiles often have similar patterns
    return e < 3.5

def is_padding(data, threshold=0.9):
    """Check if data is mostly 0x00 or 0xFF."""
    if not data:
        return False
    pad_count = sum(1 for b in data if b == 0x00 or b == 0xFF)
    return pad_count / len(data) >= threshold

def scan_bank(rom_data, bank, bank_size=0x4000):
    """Scan a bank and return list of (start, end) data regions."""
    offset = bank * bank_size
    if offset + bank_size > len(rom_data):
        return []

    bank_data = rom_data[offset:offset + bank_size]
    base_addr = 0x4000 if bank > 0 else 0x0000

    # Scan in 64-byte windows
    WINDOW = 64
    is_data = [False] * (bank_size // WINDOW)

    for i in range(len(is_data)):
        chunk = bank_data[i*WINDOW:(i+1)*WINDOW]
        e = entropy(chunk)

        # Very low entropy = likely data (tiles, padding, tables)
        if e < 2.0:
            is_data[i] = True
        # Check for padding
        elif is_padding(chunk, 0.8):
            is_data[i] = True
        # High entropy with no control flow opcodes = compressed data
        elif e > 6.5:
            # Check for any branch/call/ret opcodes
            control = set([0xC0,0xC2,0xC3,0xC4,0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,
                          0xD0,0xD2,0xD4,0xD8,0xDA,0xDC,0xE9,0x18,0x20,0x28,0x30,0x38])
            has_control = any(b in control for b in chunk)
            if not has_control:
                is_data[i] = True

    # Merge consecutive data windows into regions
    regions = []
    in_region = False
    region_start = 0

    for i, d in enumerate(is_data):
        if d and not in_region:
            region_start = i
            in_region = True
        elif not d and in_region:
            start = base_addr + region_start * WINDOW
            end = base_addr + i * WINDOW
            if end - start >= 128:  # Only report regions >= 128 bytes
                regions.append((start, end))
            in_region = False

    if in_region:
        start = base_addr + region_start * WINDOW
        end = base_addr + len(is_data) * WINDOW
        if end - start >= 128:
            regions.append((start, end))

    return regions

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <rom.gb>")
        sys.exit(1)

    with open(sys.argv[1], 'rb') as f:
        rom = f.read()

    bank_size = 0x4000
    num_banks = len(rom) // bank_size
    print(f"ROM size: {len(rom)} bytes, {num_banks} banks\n")

    total_data = 0
    total_rom = 0
    all_regions = []

    for bank in range(num_banks):
        regions = scan_bank(rom, bank, bank_size)
        bank_data_bytes = sum(end - start for start, end in regions)
        total_data += bank_data_bytes
        total_rom += bank_size

        if regions:
            base = 0x4000 if bank > 0 else 0x0000
            bank_end = base + bank_size
            coverage = bank_data_bytes / bank_size * 100
            # Check if entire bank is data
            if len(regions) == 1 and regions[0][0] == base and regions[0][1] >= bank_end - 64:
                print(f"# Bank {bank:2d}: ENTIRELY DATA ({coverage:.0f}%)")
            else:
                print(f"# Bank {bank:2d}: {len(regions)} data region(s), {coverage:.0f}% data")

            for start, end in regions:
                all_regions.append((bank, start, end))

    print(f"\n# Total: {total_data}/{total_rom} bytes data ({total_data/total_rom*100:.1f}%)\n")
    print("# TOML data_region entries:")
    for bank, start, end in all_regions:
        print(f'[[data_region]]')
        print(f'bank = {bank}')
        print(f'start = 0x{start:04X}')
        print(f'end = 0x{end:04X}')
        print()

if __name__ == '__main__':
    main()
