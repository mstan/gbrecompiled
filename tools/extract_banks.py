#!/usr/bin/env python3
"""Extract Game Boy ROM into separate bank files for Ghidra import."""
import sys
import os

def extract(rom_path, out_dir="banks"):
    with open(rom_path, "rb") as f:
        data = f.read()

    os.makedirs(out_dir, exist_ok=True)

    # GB bank size is 16KB (0x4000)
    bank_size = 0x4000
    num_banks = len(data) // bank_size

    for i in range(num_banks):
        bank_data = data[i * bank_size : (i + 1) * bank_size]
        out_path = os.path.join(out_dir, f"bank{i}.bin")
        with open(out_path, "wb") as f:
            f.write(bank_data)
        base_addr = 0x0000 if i == 0 else 0x4000
        print(f"  {out_path}: {len(bank_data)} bytes (load at 0x{base_addr:04X})")

    print(f"\nExtracted {num_banks} banks from {rom_path}")

if __name__ == "__main__":
    rom = sys.argv[1] if len(sys.argv) > 1 else "roms/tetris.gb"
    out = sys.argv[2] if len(sys.argv) > 2 else "banks"
    extract(rom, out)
