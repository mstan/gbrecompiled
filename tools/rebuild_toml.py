#!/usr/bin/env python3
"""Rebuild a Pokemon Red TOML by combining header, entry points, and data regions."""
import sys

old_toml = sys.argv[1]
sym_entry_points = sys.argv[2]
output = sys.argv[3]

# Read the old TOML to extract data regions
with open(old_toml) as f:
    old_content = f.read()

# Extract data_region blocks
data_regions = []
lines = old_content.split('\n')
i = 0
while i < len(lines):
    if lines[i].strip() == '[[data_region]]':
        block = [lines[i]]
        i += 1
        while i < len(lines) and lines[i].strip() and not lines[i].startswith('[[') and not lines[i].startswith('--'):
            block.append(lines[i])
            i += 1
        data_regions.append('\n'.join(block))
    else:
        i += 1

# Read entry points
with open(sym_entry_points) as f:
    entry_points = f.read()

# Write output
with open(output, 'w') as f:
    # Header
    f.write('# Pokemon Red (UE) - Game Boy Static Recompiler Configuration\n')
    f.write('# Entry points generated from pokered disassembly .sym file\n\n')
    f.write('[rom]\n')
    f.write('path = "roms/Pokemon Red (UE) [S][!].gb"\n')
    f.write('output_dir = "generated"\n')
    f.write('runtime_dir = "../../gb-recompiled/runtime"\n\n')
    f.write('[options]\n')
    f.write('aggressive_scan = false\n')
    f.write('emit_comments = true\n\n')
    # Entry points
    f.write(entry_points)
    f.write('\n')
    # Data regions
    for dr in data_regions:
        f.write(dr + '\n\n')

print(f"Written {output}: {len(data_regions)} data regions, entry points from {sym_entry_points}")
