/**
 * @file mock_gen1.c
 * @brief Runtime injectors for Gen 1 carts (Red/Blue/Yellow).
 *
 * Mirror of mock_gen2.c for the original trilogy. Per-cart addresses
 * (Red/Blue share, Yellow is one byte off in WRAM + a different
 * MonsterNames bank) live in the GBGen1Info table below. Public API
 * deliberately matches `mock_gen2.h` so the Esc-menu UI can branch
 * on whichever generation is active.
 */

#include "pokemon/mock_gen1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Gen 1 party_struct (see pokered/macros/ram.asm). Box struct is
 * 33 bytes; party adds Level + 5×2-byte stats at the end → 44. */
#define PARTYMON_STRUCT_LEN  44
#define BASE_DATA_SIZE       28      /* Gen 1 BaseStats entry */
#define PARTY_LENGTH         6
#define MON_NAME_LEN         11

typedef struct {
    GBGen1Game id;
    const char* title_prefix;
    int         title_prefix_len;
    /* ROM addresses (banked) */
    uint8_t  rom_bank_base_stats;
    uint16_t rom_off_base_stats;    /* BaseStats — keyed by pokedex# */
    uint8_t  rom_bank_names;
    uint16_t rom_off_names;         /* MonsterNames — keyed by pokedex# */
    uint8_t  rom_bank_evos;
    uint16_t rom_off_evos;          /* EvosMovesPointerTable — by pokedex# */
    uint8_t  rom_bank_dex_order;
    uint16_t rom_off_dex_order;     /* PokedexOrder — internal_id → dex# */
    /* WRAM offsets (Gen 1 doesn't use CGB WRAM banking — all in
     * the fixed $D000-$DFFF region, which lives at offset 0x1000
     * in our flat 8-bank buffer). */
    uint16_t wram_party_count;
    uint16_t wram_party_species;
    uint16_t wram_party_mons;
    uint16_t wram_party_ot;
    uint16_t wram_party_nicks;
    uint16_t wram_player_id;
    uint16_t wram_player_name;
} GBGen1Info;

static const GBGen1Info GEN1_CARTS[] = {
    {
        GB_MOCK_GEN1_RED,    "POKEMON RED",   11,
        0x0E, 0x43DE,
        0x07, 0x421E,
        0x0E, 0x705C,
        0x10, 0x5024,
        0xD163, 0xD164, 0xD16B, 0xD273, 0xD2B5, 0xD359, 0xD158,
    },
    {
        GB_MOCK_GEN1_BLUE,   "POKEMON BLUE",  12,
        0x0E, 0x43DE,
        0x07, 0x421E,
        0x0E, 0x705C,
        0x10, 0x5024,
        0xD163, 0xD164, 0xD16B, 0xD273, 0xD2B5, 0xD359, 0xD158,
    },
    {
        GB_MOCK_GEN1_YELLOW, "POKEMON YELLO", 13,
        0x0E, 0x43DE,
        0x3A, 0x4000,
        0x0E, 0x71E5,
        0x10, 0x50B1,
        0xD162, 0xD163, 0xD16A, 0xD272, 0xD2B4, 0xD358, 0xD157,
    },
};
#define GEN1_CART_COUNT ((int)(sizeof(GEN1_CARTS) / sizeof(GEN1_CARTS[0])))

static const GBGen1Info* gen1_info(const GBContext* ctx) {
    if (!ctx || !ctx->rom || ctx->rom_size < 0x144) return NULL;
    for (int i = 0; i < GEN1_CART_COUNT; i++) {
        const GBGen1Info* c = &GEN1_CARTS[i];
        if (memcmp((const char*)ctx->rom + 0x134,
                   c->title_prefix, c->title_prefix_len) == 0) {
            return c;
        }
    }
    return NULL;
}

GBGen1Game gb_mock_gen1_detect(const GBContext* ctx) {
    const GBGen1Info* c = gen1_info(ctx);
    return c ? c->id : GB_MOCK_GEN1_NONE;
}

/* -------------------------------------------------------------------------
 * Shared helpers (mirror mock_gen2)
 * ------------------------------------------------------------------------- */

static uint8_t* wram_ptr(GBContext* ctx, uint16_t addr) {
    /* Gen 1 doesn't switch WRAM banks — $D000-$DFFF maps to flat
     * offset 0x1000..0x1FFF. */
    return &ctx->wram[(size_t)0x1000 + (size_t)(addr - 0xD000)];
}
static const uint8_t* wram_cptr(const GBContext* ctx, uint16_t addr) {
    return &ctx->wram[(size_t)0x1000 + (size_t)(addr - 0xD000)];
}

static size_t rom_addr(uint8_t bank, uint16_t addr) {
    return (size_t)bank * 0x4000u + (size_t)(addr - 0x4000);
}

static void ensure_rng_seeded(void) {
    static bool seeded = false;
    if (!seeded) { srand((unsigned int)time(NULL)); seeded = true; }
}

/* Gen 1 stores Pokemon by INTERNAL hex ID (e.g. Rhydon=$01, Gastly=$19,
 * Mew=$15), not by pokedex number. The user-facing builder picks by dex
 * number (1..151), and the cart's BaseStats / EvosMoves / MonsterNames
 * tables are all dex-keyed, so dex is the right form for ROM reads.
 * Party-slot bytes (wPartySpecies, MON_SPECIES) need the internal ID
 * instead — without this translation the cart looks up the wrong row
 * in every per-species table at runtime (moves, base stats, name).
 *
 * PokedexOrder is a table mapping internal_id (1-based) → dex#. To
 * find the internal ID for a given dex#, scan the table for the first
 * row whose value equals dex#. The table includes ~190 entries
 * (MissingNo slots) so we cap the scan at a generous 200. */
static int dex_to_internal(const GBContext* ctx, const GBGen1Info* c, int dex) {
    if (dex < 1 || dex > GB_MOCK_GEN1_SPECIES_COUNT) return -1;
    size_t base = rom_addr(c->rom_bank_dex_order, c->rom_off_dex_order);
    for (int i = 0; i < 200; i++) {
        if (base + i >= ctx->rom_size) break;
        if (ctx->rom[base + i] == (uint8_t)dex) return i + 1; /* 1-based */
    }
    return -1;
}

uint8_t gb_mock_gen1_party_count(const GBContext* ctx) {
    const GBGen1Info* c = gen1_info(ctx);
    if (!c || !ctx->wram) return 0xFF;
    return ctx->wram[0x1000 + (c->wram_party_count - 0xD000)];
}

/* Gen 1 stat formula — identical math to Gen 2 (Gen 2 inherited it). */
static int stat_hp(int base, int dv, int level) {
    return ((base + dv) * 2 * level) / 100 + level + 10;
}
static int stat_other(int base, int dv, int level) {
    return ((base + dv) * 2 * level) / 100 + 5;
}

static int derive_hp_dv(int atk, int def, int spd, int spc) {
    return ((spd & 1) << 0) | ((def & 1) << 1) |
           ((atk & 1) << 2) | ((spc & 1) << 3);
}

/* "Shiny" in Gen 1 means nothing in-cart; this sets the DV pattern
 * that, after a Time Capsule trade, displays as shiny in Gen 2. */
static void pick_dvs(bool shiny, int* atk, int* def, int* spd, int* spc) {
    if (shiny) {
        *atk = 10; *def = 10; *spd = 10; *spc = 10;
    } else {
        ensure_rng_seeded();
        *atk = rand() & 0xF;
        *def = rand() & 0xF;
        *spd = rand() & 0xF;
        *spc = rand() & 0xF;
    }
}

/* Returns base[0..4] = HP/Atk/Def/Spd/Spc and out_type[0..1] = Type1/Type2. */
static bool read_base_data(const GBContext* ctx, const GBGen1Info* c,
                           int species, uint8_t base[5], uint8_t out_type[2]) {
    if (species < 1 || species > GB_MOCK_GEN1_SPECIES_COUNT) return false;
    size_t off = rom_addr(c->rom_bank_base_stats, c->rom_off_base_stats) +
                 (size_t)(species - 1) * BASE_DATA_SIZE;
    if (off + 8 > ctx->rom_size) return false;
    /* +1 skips the dex-number byte; HP/Atk/Def/Spd/Spc at +1..+5. */
    memcpy(base, &ctx->rom[off + 1], 5);
    /* Type1, Type2 at +6, +7. */
    out_type[0] = ctx->rom[off + 6];
    out_type[1] = ctx->rom[off + 7];
    return true;
}

/* Catch rate (offset +8 in BaseStats) — used as the cart-side "held
 * item" in Gen 2 trades, but in Gen 1 it's the actual capture rate. */
static uint8_t read_catch_rate(const GBContext* ctx, const GBGen1Info* c,
                               int species) {
    size_t off = rom_addr(c->rom_bank_base_stats, c->rom_off_base_stats) +
                 (size_t)(species - 1) * BASE_DATA_SIZE + 8;
    if (off >= ctx->rom_size) return 0;
    return ctx->rom[off];
}

static uint8_t read_growth_rate(const GBContext* ctx, const GBGen1Info* c,
                                int species) {
    /* +19 in BaseStats per the struct (dex+stats+types+catch+exp+pic_size
     * +frontpic+backpic+moves = 1+5+2+1+1+1+2+2+4 = 19). */
    size_t off = rom_addr(c->rom_bank_base_stats, c->rom_off_base_stats) +
                 (size_t)(species - 1) * BASE_DATA_SIZE + 19;
    if (off >= ctx->rom_size) return 0;
    return ctx->rom[off];
}

static uint32_t exp_for_level(uint8_t growth_rate, int level) {
    uint32_t n = (uint32_t)level;
    uint32_t n3 = n * n * n;
    switch (growth_rate) {
        case 0: return n3;
        case 1: return (n3 * 4) / 5 + 1;
        case 2: return (n3 * 5) / 4 + 1;
        case 3:
            if (level <= 1) return 0;
            return (6 * n3) / 5 + 100 * n -
                   (n > 0 ? 15 * n * n + 140 : 0);
        case 4: return (n3 * 4) / 5;
        case 5: return (n3 * 5) / 4;
        default: return n3;
    }
}

static int read_learnset(const GBContext* ctx, const GBGen1Info* c,
                         int species, int level, uint8_t moves[4]) {
    /* Gen 1 each species's "starting moves" live in BaseStats at
     * offset +15..+18 (4 moves). Level-up additions are in
     * EvosMovesPointerTable. BaseStats is dex-indexed but the
     * EvosMoves pointer table is INTERNAL-indexed (matching
     * MonsterNames). Without the dex→internal translation we'd
     * end up with the wrong species's learnset. */
    memset(moves, 0, 4);
    if (species < 1 || species > GB_MOCK_GEN1_SPECIES_COUNT) return 0;

    size_t base_off = rom_addr(c->rom_bank_base_stats, c->rom_off_base_stats) +
                      (size_t)(species - 1) * BASE_DATA_SIZE + 15;
    if (base_off + 4 > ctx->rom_size) return 0;
    memcpy(moves, &ctx->rom[base_off], 4);

    int internal = dex_to_internal(ctx, c, species);
    if (internal < 1) return 4;
    /* EvosMovesPointerTable — internal-indexed, 2 bytes/entry. */
    size_t ptr_off = rom_addr(c->rom_bank_evos, c->rom_off_evos) +
                     (size_t)(internal - 1) * 2;
    if (ptr_off + 2 > ctx->rom_size) return 0;
    uint16_t data_addr = (uint16_t)(ctx->rom[ptr_off] |
                                    (ctx->rom[ptr_off + 1] << 8));
    size_t off = rom_addr(c->rom_bank_evos, data_addr);

    /* Skip evolution records (variable-length, terminated by 0). */
    while (off < ctx->rom_size && ctx->rom[off] != 0) off++;
    if (off >= ctx->rom_size) return 4;
    off++;

    while (off + 1 < ctx->rom_size) {
        uint8_t lvl = ctx->rom[off];
        if (lvl == 0) break;
        uint8_t mv = ctx->rom[off + 1];
        off += 2;
        if (lvl > level) continue;
        moves[0] = moves[1]; moves[1] = moves[2];
        moves[2] = moves[3]; moves[3] = mv;
    }
    return 4;
}

static char decode_charmap_byte(uint8_t b) {
    if (b >= 0x80 && b <= 0x99) return (char)('A' + (b - 0x80));
    if (b >= 0xA0 && b <= 0xB9) return (char)('a' + (b - 0xA0));
    if (b == 0x7F)              return ' ';
    if (b == 0xE0)              return '\'';
    if (b == 0xE8)              return '.';
    if (b >= 0xF6 && b <= 0xFF) return (char)('0' + (b - 0xF6));
    return '?';
}

bool gb_mock_gen1_species_name(const GBContext* ctx, int species,
                               char* out, size_t out_size) {
    if (!out || out_size < 11) return false;
    out[0] = '\0';
    const GBGen1Info* c = gen1_info(ctx);
    if (!c || !ctx->rom) return false;
    if (species < 1 || species > GB_MOCK_GEN1_SPECIES_COUNT) return false;
    /* MonsterNames is INTERNAL-indexed in Gen 1, not dex-indexed.
     * Same trap as EvosMovesPointerTable. Translate dex → internal
     * before computing the offset, otherwise the dropdown labels
     * disagree with the species the cart actually receives. */
    int internal = dex_to_internal(ctx, c, species);
    if (internal < 1) return false;
    size_t off = rom_addr(c->rom_bank_names, c->rom_off_names) +
                 (size_t)(internal - 1) * 10;
    if (off + 10 > ctx->rom_size) return false;
    size_t i;
    for (i = 0; i < 10; i++) {
        uint8_t b = ctx->rom[off + i];
        if (b == 0x50) break;
        out[i] = decode_charmap_byte(b);
    }
    out[i] = '\0';
    return true;
}

bool gb_mock_gen1_inject_builder(GBContext* ctx, int species,
                                 int level, bool shiny) {
    const GBGen1Info* c = gen1_info(ctx);
    if (!c || !ctx->wram || !ctx->rom) return false;
    if (species < 1 || species > GB_MOCK_GEN1_SPECIES_COUNT) return false;
    if (level < 2 || level > 100) return false;

    uint8_t party_count = gb_mock_gen1_party_count(ctx);
    if (party_count >= PARTY_LENGTH) return false;

    int internal = dex_to_internal(ctx, c, species);
    if (internal < 1) return false;  /* dex_id not in PokedexOrder */

    uint8_t base[5];      /* HP, Atk, Def, Spd, Spc */
    uint8_t types[2];
    if (!read_base_data(ctx, c, species, base, types)) return false;

    int atk_dv, def_dv, spd_dv, spc_dv;
    pick_dvs(shiny, &atk_dv, &def_dv, &spd_dv, &spc_dv);
    int hp_dv = derive_hp_dv(atk_dv, def_dv, spd_dv, spc_dv);

    uint16_t hp = (uint16_t)stat_hp   (base[0], hp_dv,  level);
    uint16_t at = (uint16_t)stat_other(base[1], atk_dv, level);
    uint16_t df = (uint16_t)stat_other(base[2], def_dv, level);
    uint16_t sp = (uint16_t)stat_other(base[3], spd_dv, level);
    uint16_t sc = (uint16_t)stat_other(base[4], spc_dv, level);

    uint8_t moves[4];
    read_learnset(ctx, c, species, level, moves);

    uint32_t exp = exp_for_level(read_growth_rate(ctx, c, species), level);
    uint8_t catch_rate = read_catch_rate(ctx, c, species);

    /* Build the 44-byte party struct. Field offsets per
     * pokered/macros/ram.asm — box_struct + party-only tail. */
    uint8_t mon[PARTYMON_STRUCT_LEN] = {0};
    mon[0]  = (uint8_t)internal;        /* MON_SPECIES (internal hex ID) */
    mon[1]  = (uint8_t)(hp >> 8);       /* MON_HP (current) */
    mon[2]  = (uint8_t)(hp & 0xFF);
    mon[3]  = (uint8_t)level;           /* MON_BOX_LEVEL — kept in sync */
    mon[4]  = 0;                        /* MON_STATUS */
    mon[5]  = types[0];                 /* MON_TYPE1 */
    mon[6]  = types[1];                 /* MON_TYPE2 */
    mon[7]  = catch_rate;               /* MON_CATCH_RATE */
    memcpy(&mon[8], moves, 4);          /* MON_MOVES */
    memcpy(&mon[12], wram_cptr(ctx, c->wram_player_id), 2); /* MON_OT_ID */
    mon[14] = (uint8_t)((exp >> 16) & 0xFF);
    mon[15] = (uint8_t)((exp >>  8) & 0xFF);
    mon[16] = (uint8_t)( exp        & 0xFF);
    /* MON_*Exp (offsets 17-26) stay zero — fresh injected mon. */
    mon[27] = (uint8_t)((atk_dv << 4) | (def_dv & 0xF));
    mon[28] = (uint8_t)((spd_dv << 4) | (spc_dv & 0xF));
    mon[29] = mon[30] = mon[31] = mon[32] = 20; /* PP — default 20 */
    /* Party-only tail */
    mon[33] = (uint8_t)level;           /* MON_LEVEL */
    mon[34] = (uint8_t)(hp >> 8);       /* MAX_HP */
    mon[35] = (uint8_t)(hp & 0xFF);
    mon[36] = (uint8_t)(at >> 8); mon[37] = (uint8_t)(at & 0xFF);
    mon[38] = (uint8_t)(df >> 8); mon[39] = (uint8_t)(df & 0xFF);
    mon[40] = (uint8_t)(sp >> 8); mon[41] = (uint8_t)(sp & 0xFF);
    mon[42] = (uint8_t)(sc >> 8); mon[43] = (uint8_t)(sc & 0xFF);

    uint8_t* mon_dst = wram_ptr(ctx, c->wram_party_mons) +
                       (size_t)party_count * PARTYMON_STRUCT_LEN;
    memcpy(mon_dst, mon, PARTYMON_STRUCT_LEN);

    /* Nickname: species name. MonsterNames is internal-indexed
     * (see species_name above), so use the already-computed
     * `internal` value rather than dex `species`. */
    size_t name_off = rom_addr(c->rom_bank_names, c->rom_off_names) +
                      (size_t)(internal - 1) * 10;
    uint8_t* nick_dst = wram_ptr(ctx, c->wram_party_nicks) +
                        (size_t)party_count * MON_NAME_LEN;
    memset(nick_dst, 0x50, MON_NAME_LEN);
    for (int i = 0; i < 10 && name_off + i < ctx->rom_size; i++) {
        uint8_t b = ctx->rom[name_off + i];
        nick_dst[i] = b;
        if (b == 0x50) break;
    }

    /* OT name: copy player's. */
    memcpy(wram_ptr(ctx, c->wram_party_ot) +
               (size_t)party_count * MON_NAME_LEN,
           wram_cptr(ctx, c->wram_player_name), MON_NAME_LEN);

    /* wPartySpecies[party_count] = internal hex ID; terminator next. */
    uint8_t* species_arr = wram_ptr(ctx, c->wram_party_species);
    species_arr[party_count] = (uint8_t)internal;
    species_arr[party_count + 1] = 0xFF;

    /* Increment wPartyCount last. */
    ctx->wram[0x1000 + (c->wram_party_count - 0xD000)] = party_count + 1;

    fprintf(stderr,
            "[gen1/%s] Built dex=%d internal=$%02X level=%d shiny=%d "
            "→ slot %d (H/A/D/S/Sp=%d/%d/%d/%d/%d)\n",
            c->title_prefix, species, internal, level, shiny ? 1 : 0,
            party_count, hp, at, df, sp, sc);
    return true;
}
