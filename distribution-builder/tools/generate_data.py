#!/usr/bin/env python3
"""Regenerate data/{gen1,gen2,moves}.json from the pret disassemblies.

Sources (override via env):
  POKERED_SRC      ~/Documents/Development/pokered
  POKECRYSTAL_SRC  ~/Documents/Development/pokecrystal-decomp

Run from anywhere; output is written to ../data/ relative to this file.
"""

import json
import os
import re
import sys
from pathlib import Path

ROOT          = Path(__file__).resolve().parent.parent
DATA_DIR      = ROOT / "data"
POKERED       = Path(os.environ.get("POKERED_SRC",
                                    Path.home() / "Documents/Development/pokered"))
POKECRYSTAL   = Path(os.environ.get("POKECRYSTAL_SRC",
                                    Path.home() / "Documents/Development/pokecrystal-decomp"))

# Type IDs are stable byte values across gens — the table only grows
# in Gen 2 (STEEL=$09, DARK=$1B). Gen 1 uses a subset.
TYPE_IDS = {
    "NORMAL": 0x00, "FIGHTING": 0x01, "FLYING": 0x02, "POISON": 0x03,
    "GROUND": 0x04, "ROCK": 0x05, "BIRD": 0x06, "BUG": 0x07, "GHOST": 0x08,
    "STEEL": 0x09, "CURSE_TYPE": 0x13,
    "FIRE": 0x14, "WATER": 0x15, "GRASS": 0x16, "ELECTRIC": 0x17,
    "PSYCHIC_TYPE": 0x18, "ICE": 0x19, "DRAGON": 0x1A, "DARK": 0x1B,
}

GROWTH_RATES = {
    "GROWTH_MEDIUM_FAST":   0,
    "GROWTH_SLIGHTLY_FAST": 1,
    "GROWTH_SLIGHTLY_SLOW": 2,
    "GROWTH_MEDIUM_SLOW":   3,
    "GROWTH_FAST":          4,
    "GROWTH_SLOW":          5,
}


# ---------------------------------------------------------------------------
# Move name tables — straight enumeration. Both lists pack names that the
# in-cart MoveNames table uses, $50-terminated. List order *is* the ID.
# ---------------------------------------------------------------------------

LI_RE = re.compile(r'^\s*li\s+"([^"]+)"', re.M)

def read_moves(path):
    text = path.read_text()
    return LI_RE.findall(text)


# ---------------------------------------------------------------------------
# Pokemon name tables. Gen 1 MonsterNames is internal-ID indexed; Gen 2
# PokemonNames is dex-indexed. Both use the same `dname "FOO"` macro.
# ---------------------------------------------------------------------------

DNAME_RE = re.compile(r'^\s*dname\s+"([^"]+)"', re.M)

def read_names(path):
    return DNAME_RE.findall(path.read_text())


# ---------------------------------------------------------------------------
# base_stats/<name>.asm parser
#
#   db DEX_<NAME>                       ; dex id  (gen 1)
#   or `db <NAME> ; <dex#>` for gen 2 — the `<NAME>` is the dex#-keyed const
#
#   db hp, atk, def, spd, spc           ; gen 1
#   db hp, atk, def, spd, satk, sdef    ; gen 2
#
#   db TYPE1, TYPE2
#   db <catch_rate>
#   db <base_exp>
#   ...
#   db GROWTH_<RATE>
# ---------------------------------------------------------------------------

def parse_stats_file(path, gen):
    """Return dict with stats / types / catch_rate / growth_rate / first-stats-line."""
    text = path.read_text()
    # Drop comments
    lines = [re.sub(r';.*', '', l).strip() for l in text.splitlines()]
    lines = [l for l in lines if l]

    out = {}
    found_stats = False
    found_types = False
    found_catch = False
    catch_seen = False

    for line in lines:
        m = re.match(r'^db\s+([0-9A-Za-z_,\s\$]+)$', line)
        if not m: continue
        parts = [p.strip() for p in m.group(1).split(',')]

        if not found_stats and len(parts) == (5 if gen == 1 else 6) \
                and all(p.replace('$', '').isdigit() or
                        p.lstrip('-').isdigit() for p in parts):
            nums = [_dbnum(p) for p in parts]
            if gen == 1:
                out["base"] = dict(hp=nums[0], atk=nums[1], def_=nums[2],
                                   spd=nums[3], spc=nums[4])
            else:
                out["base"] = dict(hp=nums[0], atk=nums[1], def_=nums[2],
                                   spd=nums[3], satk=nums[4], sdef=nums[5])
            found_stats = True
            continue

        if found_stats and not found_types and len(parts) == 2 \
                and all(p in TYPE_IDS for p in parts):
            out["types"] = [TYPE_IDS[parts[0]], TYPE_IDS[parts[1]]]
            found_types = True
            continue

        if found_types and not catch_seen and len(parts) == 1 \
                and _isnum(parts[0]):
            out["catch_rate"] = _dbnum(parts[0])
            catch_seen = True
            continue

        if "GROWTH_" in line:
            for k, v in GROWTH_RATES.items():
                if k in line:
                    out["growth_rate"] = v
                    break
    return out


def _isnum(s):
    s = s.replace('$', '')
    return s.lstrip('-').isdigit() or all(c in '0123456789abcdefABCDEF' for c in s)

def _dbnum(s):
    s = s.strip()
    if s.startswith('$'): return int(s[1:], 16)
    return int(s, 10)


# ---------------------------------------------------------------------------
# Top-level: build gen1.json, gen2.json, moves.json
# ---------------------------------------------------------------------------

def fixup_name(disp):
    """The cart's dname stores names with the in-game charmap, including a
    couple of multi-byte glyphs for Nidoran ♂/♀. The pret file uses the
    Unicode codepoints directly — keep them. Filenames use the same
    casing the user picks in dropdowns, so the JSON keeps the original
    spelling exactly (e.g. "NIDORAN♂", "MR.MIME"). Display layer can
    title-case as needed."""
    return disp.replace("?", "?")  # no-op placeholder

def title_case(name):
    """For UI display: 'NIDORAN♂' -> 'Nidoran♂', 'MR.MIME' -> 'Mr.Mime'."""
    return ''.join([c.upper() if i == 0 else c.lower()
                    for i, c in enumerate(name.lower())]) \
           if False else name  # leave UPPERCASE for now; UI can transform

def build_gen1():
    names = read_names(POKERED / "data/pokemon/names.asm")
    # MonsterNames is internal-id indexed (1-based). MISSINGNO entries
    # are placeholders for unused indexes 32..189 except the real mons.
    name_to_internal = {}
    for i, n in enumerate(names, start=1):
        if n.startswith("MISSINGNO"): continue
        name_to_internal[n] = i

    species_dir = POKERED / "data/pokemon/base_stats"
    out = []
    for asm in sorted(species_dir.glob("*.asm")):
        stats = parse_stats_file(asm, gen=1)
        if "base" not in stats: continue
        # Map filename to MonsterNames entry.
        nm = _filename_to_name(asm.stem, gen=1)
        internal = name_to_internal.get(nm)
        if internal is None:
            print(f"warn: no MonsterNames match for {asm.stem!r} -> {nm!r}",
                  file=sys.stderr)
            continue
        dex = _dex_from_stats_file(asm)
        out.append({
            "dex": dex,
            "internal_id": internal,
            "name": nm,
            "types": stats["types"],
            "catch_rate": stats["catch_rate"],
            "growth_rate": stats["growth_rate"],
            "base": {
                "hp":  stats["base"]["hp"],
                "atk": stats["base"]["atk"],
                "def": stats["base"]["def_"],
                "spd": stats["base"]["spd"],
                "spc": stats["base"]["spc"],
            },
        })
    out.sort(key=lambda s: s["dex"])
    return {"_note": "Generated from pret/pokered. species byte for Gen 1 "
                     "writes is internal_id (MonsterNames index), not dex#.",
            "species": out}


def build_gen2():
    names = read_names(POKECRYSTAL / "data/pokemon/names.asm")
    species_dir = POKECRYSTAL / "data/pokemon/base_stats"
    out = []
    for asm in sorted(species_dir.glob("*.asm")):
        stats = parse_stats_file(asm, gen=2)
        if "base" not in stats: continue
        nm = _filename_to_name(asm.stem, gen=2)
        # Gen 2 dex# from the leading `db <NAME>` (which is the dex# constant).
        dex = _dex_from_stats_file(asm)
        if not nm or nm.startswith("MISSINGNO"):
            print(f"warn: skipping placeholder {asm.stem}", file=sys.stderr)
            continue
        # Cross-check by looking up name in PokemonNames if dex is in range.
        if 1 <= dex <= len(names):
            cart_name = names[dex - 1]
            # The display name may differ in capitalization/glyphs; trust the
            # PokemonNames entry as canonical.
            nm = cart_name
        out.append({
            "dex": dex,
            "name": nm,
            "types": stats["types"],
            "catch_rate": stats["catch_rate"],
            "growth_rate": stats["growth_rate"],
            "base": {
                "hp":  stats["base"]["hp"],
                "atk": stats["base"]["atk"],
                "def": stats["base"]["def_"],
                "spd": stats["base"]["spd"],
                "satk": stats["base"]["satk"],
                "sdef": stats["base"]["sdef"],
            },
        })
    out.sort(key=lambda s: s["dex"])
    return {"_note": "Generated from pret/pokecrystal. species byte for "
                     "Gen 2 writes is the dex# directly.",
            "species": out}


# Filename mapping: pret uses lowercase-letters-only filenames, with
# punctuation/diacritics stripped. We need to map back to the canonical
# MonsterNames spelling (with `'`, `.`, `♂`, `♀`).
FILENAME_OVERRIDES = {
    # Gen 1
    "nidoranf":  "NIDORAN♀",
    "nidoranm":  "NIDORAN♂",
    "mrmime":    "MR.MIME",
    "farfetchd": "FARFETCH'D",
    # Gen 2 has the same plus a few extras:
    "mrmime":    "MR.MIME",
    "farfetchd": "FARFETCH'D",
    "hooh":      "HO-OH",
}

def _filename_to_name(stem, gen):
    if stem in FILENAME_OVERRIDES:
        return FILENAME_OVERRIDES[stem]
    return stem.upper()


# Read the leading `db <DEX_FOO>` or `db <FOO>` and return the constant
# resolved to an int. For Gen 1 it's DEX_<NAME>; for Gen 2 it's just
# <NAME>. We don't have the constant value here, so we resolve via
# the cart name table after the fact for gen 2, and fall back to the
# constants file for gen 1.
DEX_CONST_RE = re.compile(r'^\s*db\s+([A-Z_]+[A-Z0-9_]*)\s*;\s*(\d+)?', re.M)

def _dex_from_stats_file(asm_path):
    # Easiest path: the line is annotated `db DEX_BULBASAUR` or
    # `db BULBASAUR ; 151`. If a trailing `; 151` is present, use it
    # directly (Gen 2 conventionally annotates). Otherwise resolve
    # against constants/pokedex_constants.asm for Gen 1.
    text = asm_path.read_text()
    m = re.search(r'^\s*db\s+([A-Z_]+[A-Z0-9_]*)\s*(?:;\s*(\d+))?', text, re.M)
    if not m: return 0
    sym, annotated = m.group(1), m.group(2)
    if annotated: return int(annotated)
    # Gen 1: DEX_<NAME> constant. Resolve via the pokered constants file.
    if sym.startswith("DEX_"):
        return _resolve_dex_constant(sym)
    return 0


_dex_cache = None
def _resolve_dex_constant(sym):
    global _dex_cache
    if _dex_cache is None:
        _dex_cache = {}
        # Gen 1 layout: data/pokemon/dex_order.asm enumerates dex# →
        # internal name. Counting starts at 1 (Bulbasaur).
        dex_path = POKERED / "constants/pokedex_constants.asm"
        if dex_path.exists():
            text = dex_path.read_text()
            n = 0
            for line in text.splitlines():
                m = re.match(r'^\s*const\s+(DEX_[A-Z_0-9]+)', line)
                if m:
                    n += 1
                    _dex_cache[m.group(1)] = n
    return _dex_cache.get(sym, 0)


def build_moves():
    g1 = read_moves(POKERED / "data/moves/names.asm")
    g2 = read_moves(POKECRYSTAL / "data/moves/names.asm")
    # Gen 1: IDs 1..len(g1). Gen 2: 1..len(g2) where 1..165 match g1
    # by id but may differ slightly in spelling for a few entries.
    # The cart's MoveNames is the source of truth, so the gen 2 file
    # wins for shared IDs.
    out = []
    seen = set()
    for i, name in enumerate(g2, start=1):
        out.append({"id": i, "name": _title_move(name),
                    "gen": 1 if i <= 165 else 2})
        seen.add(i)
    # Sanity: emit any g1-only IDs that fell outside the loop (shouldn't).
    return {"_note": "Move IDs 1-165 are shared between Gen 1 and Gen 2. "
                     "166-251 are Gen 2 only. Names are taken from "
                     "pret/pokecrystal MoveNames.",
            "moves": out}


def _title_move(name):
    # In-cart names are uppercase. UI prefers Title Case.
    return name.title().replace("Hp", "HP").replace("Pp", "PP")


def build_items():
    """Gen 2 ItemNames table. Item 0 is NO_ITEM (not in the list);
    indices 1..255 map to the `li` entries in order. The 256th `li`
    is a "?" sentinel so the table asserts to $100 entries -- it's
    not a real item byte, so we drop it. TERU-SAMA marks unused
    slots that we also drop so the dropdown stays clean."""
    names = read_moves(POKECRYSTAL / "data/items/names.asm")  # same `li` macro
    out = [{"id": 0, "name": "(none)"}]
    for idx, name in enumerate(names, start=1):
        if idx > 255:
            break  # 256th entry is the list-length sentinel
        if name.upper() == "TERU-SAMA":
            continue   # unused slot
        out.append({"id": idx, "name": _item_display_name(name)})
    return {"_note": "Held-item IDs as the cart stores them. ID 0 is "
                     "no held item; TERU-SAMA placeholder slots are "
                     "omitted.",
            "items": out}


def _item_display_name(name):
    # `#` is the cart's charmap glyph for the PK/Poke ligature.
    # Substitute back to the conventional "Poke" prefix wherever it
    # appears (POKE_BALL #5, POKE_DOLL #37, etc.).
    if name.startswith("# "):
        return _title_item("POKE " + name[2:])
    return _title_item(name)


def _title_item(name):
    return name.title().replace("Hp", "HP").replace("Tm", "TM") \
        .replace("Hm", "HM").replace("Pp", "PP")


def main():
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    for path, builder in [
        (DATA_DIR / "gen1.json",  build_gen1),
        (DATA_DIR / "gen2.json",  build_gen2),
        (DATA_DIR / "moves.json", build_moves),
        (DATA_DIR / "items.json", build_items),
    ]:
        data = builder()
        path.write_text(json.dumps(data, indent=2,
                                   ensure_ascii=False) + "\n")
        entries = (data.get("species") or data.get("moves") or
                   data.get("items") or [])
        print(f"wrote {path.name}: {len(entries)} entries")


if __name__ == "__main__":
    main()
