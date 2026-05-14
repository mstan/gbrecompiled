#!/usr/bin/env python3
"""Generate the Gen 1 wild-encounter-diff tables from pret/pokered
and pret/pokeyellow.

For each route's WildMons table, walk the three builds (Red, Blue,
Yellow) and at every slot position find the set of unique species
across the three. If the set has more than one entry, the slot gets a
diff record: a per-cart-family ROM offset plus the candidate list.

Output (two headers):
  runtime/include/pokemon/gen1_wild_diffs_rb.gen.h
  runtime/include/pokemon/gen1_wild_diffs_yellow.gen.h

Both share the same species candidates per slot - only the ROM
offsets differ (pokered.sym vs pokeyellow.sym).

Override the source paths via POKERED_SRC / POKEYELLOW_SRC env vars.
"""

import os
import re
import sys
from pathlib import Path

ROOT       = Path(__file__).resolve().parent.parent   # runtime/
OUT_RB     = ROOT / "include/pokemon/gen1_wild_diffs_rb.gen.h"
OUT_YELLOW = ROOT / "include/pokemon/gen1_wild_diffs_yellow.gen.h"
POKERED    = Path(os.environ.get("POKERED_SRC",
                                 Path.home() / "Documents/Development/pokered"))
POKEYELLOW = Path(os.environ.get("POKEYELLOW_SRC",
                                 Path.home() / "Documents/Development/pokeyellow"))


# ---------------------------------------------------------------------------
# Pokemon name -> cart MON_SPECIES byte. Both `const NAME` and
# `const_skip` advance the counter; the const_skip placeholders are
# unused internal-id slots (MissingNo's home in MonsterNames). Missing
# them was the source of an earlier off-by-N bug that surfaced as
# random Flareons in Route 4 grass.
# ---------------------------------------------------------------------------

def load_species_map(repo):
    path = repo / "constants/pokemon_constants.asm"
    out = {}
    idx = 0
    for line in path.read_text().splitlines():
        line = line.split(';', 1)[0].strip()
        if not line: continue
        if re.match(r'^const_skip\s*$', line):
            idx += 1
            continue
        m = re.match(r'^const\s+([A-Z_0-9]+)\s*$', line)
        if not m: continue
        out[m.group(1)] = idx
        idx += 1
    return out


# ---------------------------------------------------------------------------
# Route sym lookup. Both pokered.sym and pokeyellow.sym share the
# layout `<bank>:<addr> <label>`.
# ---------------------------------------------------------------------------

def load_sym_addrs(sym_path):
    out = {}
    for line in sym_path.read_text().splitlines():
        m = re.match(r'^([0-9a-fA-F]{2}):([0-9a-fA-F]{4})\s+(\S+)\s*$',
                     line.strip())
        if not m: continue
        bank, addr, label = int(m.group(1), 16), int(m.group(2), 16), m.group(3)
        if not label.endswith("WildMons"): continue
        if "." in label: continue
        out[label] = bank * 0x4000 + (addr - 0x4000)
    return out


# ---------------------------------------------------------------------------
# Parse a Red/Blue route file (pokered source). Yields parallel
# (red_grass, blue_grass, red_water, blue_water) slot lists.
# ---------------------------------------------------------------------------

def parse_route_rb(path):
    state = 'both'
    in_grass = False
    in_water = False
    red_g, blue_g, red_w, blue_w = [], [], [], []

    def append_slot(level, species):
        target = in_grass
        if state == 'both':
            (red_g  if target else red_w ).append((level, species))
            (blue_g if target else blue_w).append((level, species))
        elif state == 'red':
            (red_g  if target else red_w ).append((level, species))
        elif state == 'blue':
            (blue_g if target else blue_w).append((level, species))

    for raw in path.read_text().splitlines():
        line = raw.split(';', 1)[0].strip()
        if not line: continue
        if line.startswith("IF DEF(_RED)"):     state = 'red';   continue
        if line.startswith("IF DEF(_BLUE)"):    state = 'blue';  continue
        if line.startswith("ELIF DEF(_RED)"):   state = 'red';   continue
        if line.startswith("ELIF DEF(_BLUE)"):  state = 'blue';  continue
        if line.startswith("ELSE"):
            state = 'red' if state == 'blue' else 'blue'; continue
        if line.startswith("ENDC"):             state = 'both';  continue
        if "def_grass_wildmons" in line: in_grass = True;  in_water = False; continue
        if "end_grass_wildmons" in line: in_grass = False; continue
        if "def_water_wildmons" in line: in_water = True;  in_grass = False; continue
        if "end_water_wildmons" in line: in_water = False; continue
        m = re.match(r'^db\s+(-?\d+|\$[0-9A-Fa-f]+)\s*,\s*([A-Z_0-9]+)\s*$', line)
        if not m: continue
        lvl_tok, species = m.group(1), m.group(2)
        level = int(lvl_tok[1:], 16) if lvl_tok.startswith('$') else int(lvl_tok)
        append_slot(level, species)
    return red_g, blue_g, red_w, blue_w


# ---------------------------------------------------------------------------
# Parse a Yellow route file (no IF DEFs). Flat slot lists.
# ---------------------------------------------------------------------------

def parse_route_yellow(path):
    in_grass = in_water = False
    grass, water = [], []
    for raw in path.read_text().splitlines():
        line = raw.split(';', 1)[0].strip()
        if not line: continue
        if "def_grass_wildmons" in line: in_grass = True;  in_water = False; continue
        if "end_grass_wildmons" in line: in_grass = False; continue
        if "def_water_wildmons" in line: in_water = True;  in_grass = False; continue
        if "end_water_wildmons" in line: in_water = False; continue
        m = re.match(r'^db\s+(-?\d+|\$[0-9A-Fa-f]+)\s*,\s*([A-Z_0-9]+)\s*$', line)
        if not m: continue
        lvl_tok, species = m.group(1), m.group(2)
        level = int(lvl_tok[1:], 16) if lvl_tok.startswith('$') else int(lvl_tok)
        (grass if in_grass else water).append((level, species))
    return grass, water


# ---------------------------------------------------------------------------
# Compute the per-slot union of species names across the three builds.
# Returns a list of (slot_index, [unique_species_names]) for slots
# where the set has more than one entry.
# ---------------------------------------------------------------------------

def slot_diffs(red, blue, yellow):
    """yellow may be None if the route file isn't present in pokeyellow
    (e.g. routes that exist only in R/B). In that case fall back to
    a 2-way R/B comparison."""
    out = []
    n = min(len(red), len(blue))
    if yellow is not None:
        n = min(n, len(yellow))
    for i in range(n):
        # Preserve insertion order so the species list is stable.
        seen = []
        for sp in [red[i][1], blue[i][1]] + ([yellow[i][1]] if yellow else []):
            if sp not in seen: seen.append(sp)
        if len(seen) > 1:
            out.append((i, seen))
    return out


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    species_map = load_species_map(POKERED)
    rb_sym      = load_sym_addrs(POKERED / "pokered.sym")
    yellow_sym  = load_sym_addrs(POKEYELLOW / "pokeyellow.sym")

    rb_entries     = []   # (rom_offset, candidates list, comment)
    yellow_entries = []

    rb_route_dir = POKERED    / "data/wild/maps"
    yw_route_dir = POKEYELLOW / "data/wild/maps"

    for path in sorted(rb_route_dir.glob("*.asm")):
        stem  = path.stem
        label = stem + "WildMons"
        rb_base = rb_sym.get(label)
        yw_base = yellow_sym.get(label)
        if rb_base is None and yw_base is None:
            continue

        red_g, blue_g, red_w, blue_w = parse_route_rb(path)
        yellow_path = yw_route_dir / path.name
        if yellow_path.exists():
            yw_g, yw_w = parse_route_yellow(yellow_path)
        else:
            yw_g, yw_w = None, None

        for slot, cands in slot_diffs(red_g, blue_g, yw_g):
            # Filter species we can resolve to byte IDs.
            ids = [species_map[s] for s in cands if s in species_map]
            if len(ids) < 2: continue
            comment = f"{stem} grass slot {slot}: {' / '.join(cands)}"
            if rb_base is not None:
                off = rb_base + 1 + slot * 2 + 1
                rb_entries.append((off, ids, comment))
            if yw_base is not None and yw_g is not None:
                off = yw_base + 1 + slot * 2 + 1
                yellow_entries.append((off, ids, comment))

        for slot, cands in slot_diffs(red_w, blue_w, yw_w):
            ids = [species_map[s] for s in cands if s in species_map]
            if len(ids) < 2: continue
            comment = f"{stem} water slot {slot}: {' / '.join(cands)}"
            if rb_base is not None:
                off = rb_base + 21 + 1 + slot * 2 + 1
                rb_entries.append((off, ids, comment))
            if yw_base is not None and yw_w is not None:
                off = yw_base + 21 + 1 + slot * 2 + 1
                yellow_entries.append((off, ids, comment))

    # Yellow-only routes (e.g. PowerPlant has Yellow-unique tables, or
    # entire maps that only exist in Yellow). For those, we can't do a
    # 3-way diff (no R/B counterpart), so they contribute nothing here.

    write_header(OUT_RB,     rb_entries,     "GBGen1WildDiff",
                 "GEN1_WILD_DIFFS_RB", "GEN1_WILD_DIFFS_RB_COUNT",
                 "GEN1_WILD_DIFFS_RB", "pret/pokered (Red/Blue ROM)")
    write_header(OUT_YELLOW, yellow_entries, "GBGen1WildDiff",
                 "GEN1_WILD_DIFFS_YELLOW", "GEN1_WILD_DIFFS_YELLOW_COUNT",
                 "GEN1_WILD_DIFFS_YELLOW", "pret/pokeyellow (Yellow ROM)")
    print(f"wrote {OUT_RB.name}: {len(rb_entries)} slots")
    print(f"wrote {OUT_YELLOW.name}: {len(yellow_entries)} slots")


def write_header(path, entries, struct_name, guard_root, count_macro,
                 array_name, source_blurb):
    path.parent.mkdir(parents=True, exist_ok=True)
    guard = f"{guard_root}_GEN_H"
    with path.open('w') as f:
        f.write("/* AUTO-GENERATED by runtime/tools/generate_wild_diffs.py.\n"
                f" * Source: {source_blurb}.\n"
                " * Do not edit by hand. Re-run the script if pret\n"
                " * sources change. */\n\n")
        f.write(f"#ifndef {guard}\n#define {guard}\n\n")
        f.write("#include <stdint.h>\n\n")
        # Shared struct -- declare only once. The first file to define
        # it wins; the other guards itself via ifdef.
        f.write("#ifndef GBGEN1_WILDDIFF_STRUCT_DEFINED\n")
        f.write("#define GBGEN1_WILDDIFF_STRUCT_DEFINED\n")
        f.write("typedef struct {\n"
                "    uint32_t rom_offset;\n"
                "    uint8_t  count;          /* 2 or 3 candidate species */\n"
                "    uint8_t  species[3];\n"
                "} GBGen1WildDiff;\n")
        f.write("#endif\n\n")
        f.write(f"#define {count_macro}  {len(entries)}\n\n")
        f.write(f"static const GBGen1WildDiff {array_name}"
                f"[{count_macro}] = {{\n")
        for off, ids, comment in entries:
            padded = ids + [0] * (3 - len(ids))
            ids_str = ", ".join(f"{v:>3}" for v in padded)
            f.write(f"    {{ 0x{off:06X}, {len(ids)}, {{ {ids_str} }} }},"
                    f"  /* {comment} */\n")
        f.write("};\n\n")
        f.write(f"#endif /* {guard} */\n")


if __name__ == "__main__":
    main()
