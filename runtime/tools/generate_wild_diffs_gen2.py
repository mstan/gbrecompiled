#!/usr/bin/env python3
"""Generate the Gen 2 wild-encounter-diff tables for Gold / Silver /
Crystal carts. Gen 2's wild data lives in 4 big files (johto_grass,
johto_water, kanto_grass, kanto_water); each file holds one entry
per map, and within an entry the slot layout is:

  grass:  2 bytes map_id + 3 bytes rate + 21 slots * 2 bytes  (47 / entry)
  water:  2 bytes map_id + 1 byte  rate +  3 slots * 2 bytes  ( 9 / entry)

Sym files have per-map sub-labels:
  JohtoGrassWildMons._def_grass_wildmons_ROUTE_29

Slot N's species byte = sub-label + 6 + N*2 (grass) / + 4 + N*2 (water).

For Gold/Silver we walk pokegold-decomp's source with both IF DEF
branches; for Crystal we walk pokecrystal-decomp's flat source.
Output: two generated headers (Gold/Silver share ROM offsets, Crystal
gets its own).
"""

import os
import re
import sys
from pathlib import Path

ROOT       = Path(__file__).resolve().parent.parent
OUT_GS     = ROOT / "include/pokemon/gen2_wild_diffs_gs.gen.h"
OUT_CRY    = ROOT / "include/pokemon/gen2_wild_diffs_crystal.gen.h"
GOLD_SRC   = Path(os.environ.get("POKEGOLD_SRC",
                                 Path.home() / "Documents/Development/pokegold-decomp"))
CRY_SRC    = Path(os.environ.get("POKECRYSTAL_SRC",
                                 Path.home() / "Documents/Development/pokecrystal-decomp"))


def load_species_map(repo):
    """Gen 2 has 251 species + const_skip placeholders. Honors the
    optional argument to `const_def` -- pokecrystal uses `const_def 1`
    so BULBASAUR = 1, not 0. Skipping that initializer was an
    off-by-one bug that made every species look like its predecessor."""
    path = repo / "constants/pokemon_constants.asm"
    out = {}
    idx = 0
    for line in path.read_text().splitlines():
        line = line.split(';', 1)[0].strip()
        if not line: continue
        m = re.match(r'^const_def(?:\s+(\d+))?\s*$', line)
        if m:
            idx = int(m.group(1)) if m.group(1) else 0
            continue
        if re.match(r'^const_skip\s*$', line):
            idx += 1
            continue
        m = re.match(r'^const\s+([A-Z_0-9]+)\s*$', line)
        if not m: continue
        out[m.group(1)] = idx
        idx += 1
    return out


def load_sym(path, table_name):
    """Returns {map_name -> flat_rom_offset} for the sub-labels under
    the given top-level wildmons table."""
    prefix = f"{table_name}._"
    out = {}
    for line in path.read_text().splitlines():
        m = re.match(r'^([0-9a-fA-F]{2}):([0-9a-fA-F]{4})\s+(\S+)\s*$',
                     line.strip())
        if not m: continue
        bank, addr, label = int(m.group(1), 16), int(m.group(2), 16), m.group(3)
        if not label.startswith(prefix): continue
        # label like: JohtoGrassWildMons._def_grass_wildmons_ROUTE_29
        suffix = label[len(prefix):]
        m2 = re.match(r'^def_(?:grass|water)_wildmons_(.+)$', suffix)
        if not m2: continue
        map_name = m2.group(1)
        flat = bank * 0x4000 + (addr - 0x4000)
        out[map_name] = flat
    return out


def parse_wild_file(path, two_way=False):
    """Walk a johto_/kanto_ grass/water file. Returns
    {map_name -> {'gold': [species_per_slot], 'silver': [...], 'crystal': [...]}}.

    For two_way=True (pokegold source), state machine handles
    `IF DEF(_GOLD) / ELIF DEF(_SILVER)`. Slots inside neither branch
    appear in both lists.

    For two_way=False (pokecrystal source), all slots go into a
    single 'crystal' list.
    """
    out = {}
    cur_map = None
    state = 'both'   # 'gold', 'silver', 'both' (or 'crystal' for non-two_way)

    def append(level, species):
        if cur_map is None: return
        entry = out.setdefault(cur_map, {'gold': [], 'silver': [], 'crystal': []})
        if two_way:
            if state == 'gold' or state == 'both':
                entry['gold'].append(species)
            if state == 'silver' or state == 'both':
                entry['silver'].append(species)
        else:
            entry['crystal'].append(species)

    for raw in path.read_text().splitlines():
        line = raw.split(';', 1)[0].strip()
        if not line: continue
        if two_way:
            if line.startswith("IF DEF(_GOLD)"):     state = 'gold';   continue
            if line.startswith("IF DEF(_SILVER)"):   state = 'silver'; continue
            if line.startswith("ELIF DEF(_GOLD)"):   state = 'gold';   continue
            if line.startswith("ELIF DEF(_SILVER)"): state = 'silver'; continue
            if line.startswith("ELSE"):
                state = 'gold' if state == 'silver' else 'silver'; continue
            if line.startswith("ENDC"):              state = 'both';   continue
        m = re.match(r'^def_(?:grass|water)_wildmons\s+([A-Z_0-9]+)', line)
        if m:
            cur_map = m.group(1)
            continue
        if 'end_grass_wildmons' in line or 'end_water_wildmons' in line:
            continue
        m = re.match(r'^db\s+(-?\d+|\$[0-9A-Fa-f]+)\s*,\s*([A-Z_0-9]+)\s*$', line)
        if not m: continue
        # Drop the leading map_id db / rate db lines -- those have
        # `<map_id>` or `percent` and don't match this regex anyway.
        # Real slots are `db <level>, <SPECIES>`.
        species = m.group(2)
        # Skip lines like `db 2 percent, 2 percent, 2 percent` (no
        # match here since 'percent' isn't an all-caps species token,
        # so the regex above already filtered them).
        if 'percent' in line: continue
        try:
            lvl = m.group(1)
            level = int(lvl[1:], 16) if lvl.startswith('$') else int(lvl)
        except ValueError:
            continue
        append(level, species)
    return out


def main():
    species_map = load_species_map(CRY_SRC)

    # Per-table sym lookups. Gold and Silver share addresses so one
    # lookup suffices for both.
    sym_pairs = []   # (cart_family_key, sym_path, table_offset_calc)

    gs_sym  = GOLD_SRC / "pokegold.sym"
    cry_sym = CRY_SRC  / "pokecrystal.sym"

    tables = [
        # (file_basename,         table_label,          slot_count, slot0_offset_in_entry)
        ("johto_grass.asm",  "JohtoGrassWildMons", 21, 6),
        ("kanto_grass.asm",  "KantoGrassWildMons", 21, 6),
        ("johto_water.asm",  "JohtoWaterWildMons",  3, 4),
        ("kanto_water.asm",  "KantoWaterWildMons",  3, 4),
    ]

    gs_entries  = []   # (rom_off, count, species_ids[3], comment)
    cry_entries = []

    for (basename, table, slot_count, slot0_off) in tables:
        gs_addrs  = load_sym(gs_sym,  table)
        cry_addrs = load_sym(cry_sym, table)

        gs_data  = parse_wild_file(GOLD_SRC / "data/wild" / basename, two_way=True)
        cry_data = parse_wild_file(CRY_SRC  / "data/wild" / basename, two_way=False)

        # Intersection of maps available across both sources.
        all_maps = sorted(set(gs_data.keys()) | set(cry_data.keys()))

        for map_name in all_maps:
            gold   = gs_data.get(map_name, {}).get('gold',   [])
            silver = gs_data.get(map_name, {}).get('silver', [])
            crystal = cry_data.get(map_name, {}).get('crystal', [])

            n = slot_count
            # Some maps may have fewer slots present in one source;
            # cap at min so we don't index past the smaller list.
            n = min(n, len(gold) if gold else n, len(silver) if silver else n,
                       len(crystal) if crystal else n)

            for slot in range(n):
                seen = []
                for sp in (gold[slot] if gold else None,
                           silver[slot] if silver else None,
                           crystal[slot] if crystal else None):
                    if sp is None: continue
                    if sp not in seen: seen.append(sp)
                if len(seen) < 2: continue

                ids = [species_map[s] for s in seen if s in species_map]
                if len(ids) < 2: continue

                comment = (f"{table} {map_name} slot {slot}: "
                           f"{' / '.join(seen)}")

                if map_name in gs_addrs and gold and silver:
                    off = gs_addrs[map_name] + slot0_off + slot * 2
                    gs_entries.append((off, ids, comment))
                if map_name in cry_addrs and crystal:
                    off = cry_addrs[map_name] + slot0_off + slot * 2
                    cry_entries.append((off, ids, comment))

    write_header(OUT_GS,  gs_entries,
                 "GEN2_WILD_DIFFS_GS", "GEN2_WILD_DIFFS_GS_COUNT",
                 "GEN2_WILD_DIFFS_GS",
                 "pret/pokegold-decomp (Gold + Silver share offsets)")
    write_header(OUT_CRY, cry_entries,
                 "GEN2_WILD_DIFFS_CRYSTAL", "GEN2_WILD_DIFFS_CRYSTAL_COUNT",
                 "GEN2_WILD_DIFFS_CRYSTAL",
                 "pret/pokecrystal-decomp")
    print(f"wrote {OUT_GS.name}: {len(gs_entries)} slots")
    print(f"wrote {OUT_CRY.name}: {len(cry_entries)} slots")


def write_header(path, entries, guard_root, count_macro, array_name, source_blurb):
    path.parent.mkdir(parents=True, exist_ok=True)
    guard = f"{guard_root}_GEN_H"
    with path.open('w') as f:
        f.write("/* AUTO-GENERATED by runtime/tools/generate_wild_diffs_gen2.py.\n"
                f" * Source: {source_blurb}.\n"
                " * Do not edit by hand. Re-run the script if pret\n"
                " * sources change. */\n\n")
        f.write(f"#ifndef {guard}\n#define {guard}\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write("#ifndef GBGEN2_WILDDIFF_STRUCT_DEFINED\n")
        f.write("#define GBGEN2_WILDDIFF_STRUCT_DEFINED\n")
        f.write("typedef struct {\n"
                "    uint32_t rom_offset;\n"
                "    uint8_t  count;\n"
                "    uint8_t  species[3];\n"
                "} GBGen2WildDiff;\n")
        f.write("#endif\n\n")
        f.write(f"#define {count_macro}  {len(entries)}\n\n")
        f.write(f"static const GBGen2WildDiff {array_name}"
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
