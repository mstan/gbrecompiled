#!/usr/bin/env python3
"""Fetch pokered layout.link to extract section addresses and generate entry points."""
import json, urllib.request, sys

# The pokered repo uses layout.link to define ROM sections
# Each SECTION has a bank and address range
# We need the .asm files to find function labels

# First, get the repo tree to find relevant files
url = "https://api.github.com/repos/pret/pokered/git/trees/master?recursive=1"
req = urllib.request.Request(url, headers={"User-Agent": "gbcrecomp"})
with urllib.request.urlopen(req) as resp:
    tree = json.loads(resp.read().decode())

# Find all .asm files
asm_files = [item['path'] for item in tree['tree'] if item['path'].endswith('.asm')]
print(f"Found {len(asm_files)} .asm files")

# Look for layout/linker script
link_files = [item['path'] for item in tree['tree']
              if item['path'].endswith('.link') or 'layout' in item['path'].lower()
              or item['path'].endswith('.sym')]
print(f"Link/layout files: {link_files}")

# Print home/ and engine/ asm files (likely code, not data)
code_dirs = ['home/', 'engine/', 'scripts/', 'data/']
for d in code_dirs:
    matches = [f for f in asm_files if f.startswith(d)]
    print(f"\n{d}: {len(matches)} files")
    for f in matches[:10]:
        print(f"  {f}")
    if len(matches) > 10:
        print(f"  ... and {len(matches)-10} more")
