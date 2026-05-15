/**
 * @file mock_gen2.c
 * @brief Runtime injectors for Gen 2 carts.
 *
 * Two categories of functionality:
 *
 *   1. Generic Pokemon builder (gb_mock_gen2_*) — works on Gold,
 *      Silver, and Crystal. Each cart has its own WRAM party-data
 *      offsets and its own BaseData / PokemonNames / EvosAttacks
 *      ROM addresses; the cart-info table below holds them all and
 *      the dispatch picks the right entry based on title-byte
 *      matching.
 *
 *   2. Crystal-only Mobile-Adapter event injectors
 *      (gb_mock_crystal_*) — the GS Ball / Celebi chain and the
 *      Odd Egg distribution. These hit cart-specific SRAM offsets
 *      and ROM data tables that only exist in Crystal, so they
 *      live behind a separate gb_mock_crystal_active() gate.
 */

#include "pokemon/mock_gen2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Gen 2 cart info — addresses for the three mainline Gen 2 carts.
 * Verified against pret/{pokegold,pokesilver,pokecrystal} sym files.
 * ------------------------------------------------------------------------- */

typedef struct {
    GBGen2Game id;
    const char* title_prefix;       /* matched at cart[0x134] */
    int         title_prefix_len;
    /* ROM addresses (banked) */
    uint8_t  rom_bank_base_stats;
    uint16_t rom_off_base_stats;    /* BaseData */
    uint8_t  rom_bank_names;
    uint16_t rom_off_names;         /* PokemonNames */
    uint8_t  rom_bank_evos;
    uint16_t rom_off_evos;          /* EvosAttacksPointers */
    uint8_t  rom_bank_moves;
    uint16_t rom_off_moves;         /* MoveNames — variable-length names */
    uint8_t  rom_bank_items;
    uint16_t rom_off_items;         /* ItemNames — variable-length names */
    uint8_t  rom_bank_item_attrs;
    uint16_t rom_off_item_attrs;    /* ItemAttributes — 7 bytes per item */
    /* WRAM bank 1 offsets ($D000-$DFFF range) */
    uint16_t wram_party_count;
    uint16_t wram_party_species;
    uint16_t wram_party_mons;
    uint16_t wram_party_ots;
    uint16_t wram_party_nicks;
    uint16_t wram_player_id;
    uint16_t wram_player_name;
    uint16_t wram_items_count;      /* wNumItems; items follow at +1 */
    uint16_t wram_balls_count;      /* wNumBalls; balls follow at +1 */
    uint16_t wram_money;            /* 3-byte BCD; max $99 $99 $99 */
} GBGen2Info;

static const GBGen2Info GEN2_CARTS[] = {
    {
        GB_MOCK_GEN2_GOLD,    "POKEMON_GLD", 11,
        0x14, 0x5B0B,
        0x6C, 0x4B74,
        0x10, 0x67BD,
        0x6C, 0x5574,
        0x6C, 0x4000,
        0x01, 0x68A0,
        0xDA22, 0xDA23, 0xDA2A, 0xDB4A, 0xDB8C, 0xD1A1, 0xD1A3,
        0xD5B7, 0xD5FC,
        0xD573,
    },
    {
        GB_MOCK_GEN2_SILVER,  "POKEMON_SLV", 11,
        0x14, 0x5B0B,
        0x6C, 0x4B74,
        0x10, 0x67BD,
        0x6C, 0x5574,
        0x6C, 0x4000,
        0x01, 0x6866,
        0xDA22, 0xDA23, 0xDA2A, 0xDB4A, 0xDB8C, 0xD1A1, 0xD1A3,
        0xD5B7, 0xD5FC,
        0xD573,
    },
    {
        GB_MOCK_GEN2_CRYSTAL, "PM_CRYSTAL",  10,
        0x14, 0x5424,
        0x14, 0x7384,
        0x10, 0x65B1,
        0x72, 0x5F29,
        0x72, 0x4000,
        0x01, 0x67C1,
        0xDCD7, 0xDCD8, 0xDCDF, 0xDDFF, 0xDE41, 0xD47B, 0xD47D,
        0xD892, 0xD8D7,
        0xD84E,
    },
};
#define GEN2_CART_COUNT ((int)(sizeof(GEN2_CARTS) / sizeof(GEN2_CARTS[0])))

#define BASE_DATA_SIZE       32
#define PARTYMON_STRUCT_LEN  48
#define NICKNAMED_MON_LEN    (PARTYMON_STRUCT_LEN + 11)
#define PARTY_LENGTH         6
#define MON_NAME_LEN         11
#define EGG_SPECIES_MARKER   0xFD

static const GBGen2Info* gen2_info(const GBContext* ctx) {
    if (!ctx || !ctx->rom || ctx->rom_size < 0x144) return NULL;
    for (int i = 0; i < GEN2_CART_COUNT; i++) {
        const GBGen2Info* c = &GEN2_CARTS[i];
        if (memcmp((const char*)ctx->rom + 0x134,
                   c->title_prefix, c->title_prefix_len) == 0) {
            return c;
        }
    }
    return NULL;
}

GBGen2Game gb_mock_gen2_detect(const GBContext* ctx) {
    const GBGen2Info* c = gen2_info(ctx);
    return c ? c->id : GB_MOCK_GEN2_NONE;
}

bool gb_mock_crystal_active(const GBContext* ctx) {
    return gb_mock_gen2_detect(ctx) == GB_MOCK_GEN2_CRYSTAL;
}

/* -------------------------------------------------------------------------
 * Shared helpers
 * ------------------------------------------------------------------------- */

static uint8_t* wram_b1_ptr(GBContext* ctx, uint16_t addr) {
    return &ctx->wram[(size_t)0x1000 + (size_t)(addr - 0xD000)];
}
static const uint8_t* wram_b1_cptr(const GBContext* ctx, uint16_t addr) {
    return &ctx->wram[(size_t)0x1000 + (size_t)(addr - 0xD000)];
}

static size_t rom_addr(uint8_t bank, uint16_t addr) {
    return (size_t)bank * 0x4000u + (size_t)(addr - 0x4000);
}

static void ensure_rng_seeded(void) {
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = true;
    }
}

/* -------------------------------------------------------------------------
 * Generic Gen 2 builder
 * ------------------------------------------------------------------------- */

uint8_t gb_mock_gen2_party_count(const GBContext* ctx) {
    const GBGen2Info* c = gen2_info(ctx);
    if (!c || !ctx->wram) return 0xFF;
    return ctx->wram[0x1000 + (c->wram_party_count - 0xD000)];
}

uint8_t* gb_mock_gen2_nick_slot(GBContext* ctx, int slot) {
    const GBGen2Info* c = gen2_info(ctx);
    if (!c || !ctx->wram || slot < 0 || slot >= 6) return NULL;
    return wram_b1_ptr(ctx, c->wram_party_nicks) + (size_t)slot * 11;
}

uint8_t* gb_mock_gen2_party_mons_slot(GBContext* ctx, int slot) {
    const GBGen2Info* c = gen2_info(ctx);
    if (!c || !ctx->wram || slot < 0 || slot >= 6) return NULL;
    return wram_b1_ptr(ctx, c->wram_party_mons) + (size_t)slot * 48;
}

uint8_t* gb_mock_gen2_party_ots_slot(GBContext* ctx, int slot) {
    const GBGen2Info* c = gen2_info(ctx);
    if (!c || !ctx->wram || slot < 0 || slot >= 6) return NULL;
    return wram_b1_ptr(ctx, c->wram_party_ots) + (size_t)slot * 11;
}

uint8_t* gb_mock_gen2_party_species_slot(GBContext* ctx, int slot) {
    const GBGen2Info* c = gen2_info(ctx);
    if (!c || !ctx->wram || slot < 0 || slot >= 6) return NULL;
    return wram_b1_ptr(ctx, c->wram_party_species) + (size_t)slot;
}

uint8_t gb_mock_gen2_party_count_inc(GBContext* ctx) {
    const GBGen2Info* c = gen2_info(ctx);
    if (!c || !ctx->wram) return 0xFF;
    uint8_t* p = &ctx->wram[0x1000 + (c->wram_party_count - 0xD000)];
    *p = (uint8_t)(*p + 1);
    return *p;
}

/* Gen 2 stat formulas with stat_exp == 0. Clean-room from spec
 * (Bulbapedia); PKSM-Core's PK2 has the same formula but is GPL-3. */
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

static bool read_base_stats(const GBContext* ctx, const GBGen2Info* c,
                            int species, uint8_t out[6]) {
    if (species < 1 || species > GB_MOCK_GEN2_SPECIES_COUNT) return false;
    /* Skip leading dex byte → land on BASE_HP. */
    size_t off = rom_addr(c->rom_bank_base_stats, c->rom_off_base_stats) +
                 (size_t)(species - 1) * BASE_DATA_SIZE + 1;
    if (off + 6 > ctx->rom_size) return false;
    memcpy(out, &ctx->rom[off], 6);
    return true;
}

static uint8_t read_growth_rate(const GBContext* ctx, const GBGen2Info* c,
                                int species) {
    size_t off = rom_addr(c->rom_bank_base_stats, c->rom_off_base_stats) +
                 (size_t)(species - 1) * BASE_DATA_SIZE + 23;
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

static int read_learnset(const GBContext* ctx, const GBGen2Info* c,
                         int species, int level, uint8_t moves[4]) {
    memset(moves, 0, 4);
    if (species < 1 || species > GB_MOCK_GEN2_SPECIES_COUNT) return 0;

    size_t ptr_off = rom_addr(c->rom_bank_evos, c->rom_off_evos) +
                     (size_t)(species - 1) * 2;
    if (ptr_off + 2 > ctx->rom_size) return 0;
    uint16_t data_addr = (uint16_t)(ctx->rom[ptr_off] |
                                    (ctx->rom[ptr_off + 1] << 8));
    size_t off = rom_addr(c->rom_bank_evos, data_addr);

    /* Skip evolution records (variable-length, terminated by 0). */
    while (off < ctx->rom_size && ctx->rom[off] != 0) off++;
    if (off >= ctx->rom_size) return 0;
    off++;

    /* (level, move) pairs, last-4 sliding window. */
    int count = 0;
    while (off + 1 < ctx->rom_size) {
        uint8_t lvl = ctx->rom[off];
        if (lvl == 0) break;
        uint8_t mv = ctx->rom[off + 1];
        off += 2;
        if (lvl > level) continue;
        moves[0] = moves[1]; moves[1] = moves[2];
        moves[2] = moves[3]; moves[3] = mv;
        if (count < 4) count++;
    }
    if (count > 0 && count < 4) {
        uint8_t tmp[4] = {0};
        for (int i = 0; i < count; i++) tmp[i] = moves[4 - count + i];
        memcpy(moves, tmp, 4);
    }
    return count;
}

static char decode_charmap_byte(uint8_t c) {
    if (c >= 0x80 && c <= 0x99) return (char)('A' + (c - 0x80));
    if (c >= 0xA0 && c <= 0xB9) return (char)('a' + (c - 0xA0));
    if (c == 0x7F)              return ' ';
    if (c == 0xE0)              return '\'';
    if (c == 0xE3)              return '-';
    if (c == 0xE8)              return '.';
    if (c == 0xEA)              return 'e';  /* e-acute -- fallback */
    if (c == 0xF1)              return '.';
    if (c == 0xF3)              return '/';
    if (c >= 0xF6 && c <= 0xFF) return (char)('0' + (c - 0xF6));
    return '?';
}

/* Scan MoveNames for a case-insensitive match. Gen 2 has 251 move
 * IDs; cap at 256 as safety. */
int gb_mock_gen2_move_id_for_name(const GBContext* ctx, const char* name) {
    if (!name || !*name) return -1;
    const GBGen2Info* c = gen2_info(ctx);
    if (!c || !ctx->rom) return -1;
    size_t off = rom_addr(c->rom_bank_moves, c->rom_off_moves);
    char buf[24];
    for (int move_id = 1; move_id <= 256; move_id++) {
        size_t i = 0;
        while (off < ctx->rom_size && ctx->rom[off] != 0x50 &&
               i < sizeof(buf) - 1) {
            buf[i++] = decode_charmap_byte(ctx->rom[off]);
            off++;
        }
        buf[i] = '\0';
        if (off < ctx->rom_size && ctx->rom[off] == 0x50) off++;
        int j = 0;
        while (buf[j] && name[j]) {
            int a = (unsigned char)buf[j], b = (unsigned char)name[j];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) break;
            j++;
        }
        if (buf[j] == '\0' && name[j] == '\0') return move_id;
        if (i == 0) break;
    }
    return -1;
}

/* Reverse: walk the variable-length MoveNames table to the Nth entry
 * and decode it to ASCII. Returns false on out-of-range or non-Gen-2
 * cart. Empty entries (Gen 2 has gaps in the move list) yield "-". */
bool gb_mock_gen2_move_name(const GBContext* ctx, int move_id,
                            char* out, size_t out_size) {
    if (!out || out_size < 2) return false;
    out[0] = '\0';
    const GBGen2Info* c = gen2_info(ctx);
    if (!c || !ctx->rom) return false;
    if (move_id < 1 || move_id > 256) return false;
    size_t off = rom_addr(c->rom_bank_moves, c->rom_off_moves);
    for (int id = 1; id <= move_id; id++) {
        size_t start = off;
        while (off < ctx->rom_size && ctx->rom[off] != 0x50) off++;
        if (off >= ctx->rom_size) return false;
        if (id == move_id) {
            size_t i = 0;
            for (size_t p = start; p < off && i + 1 < out_size; p++) {
                out[i++] = decode_charmap_byte(ctx->rom[p]);
            }
            out[i] = '\0';
            return true;
        }
        off++;
    }
    return false;
}

int gb_mock_gen2_dex_for_name(const GBContext* ctx, const char* name) {
    if (!name || !*name) return -1;
    char buf[16];
    for (int dex = 1; dex <= GB_MOCK_GEN2_SPECIES_COUNT; dex++) {
        if (!gb_mock_gen2_species_name(ctx, dex, buf, sizeof(buf))) continue;
        int i = 0;
        while (buf[i] && name[i]) {
            int a = (unsigned char)buf[i],  b = (unsigned char)name[i];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) break;
            i++;
        }
        if (buf[i] == '\0' && name[i] == '\0') return dex;
    }
    return -1;
}

bool gb_mock_gen2_item_name(const GBContext* ctx, int item_id,
                            char* out, size_t out_size) {
    if (!out || out_size < 2) return false;
    out[0] = '\0';
    const GBGen2Info* c = gen2_info(ctx);
    if (!c || !ctx->rom) return false;
    if (item_id < 1 || item_id > 255) return false;
    size_t off = rom_addr(c->rom_bank_items, c->rom_off_items);
    for (int id = 1; id <= item_id; id++) {
        size_t start = off;
        while (off < ctx->rom_size && ctx->rom[off] != 0x50) off++;
        if (off >= ctx->rom_size) return false;
        if (id == item_id) {
            size_t i = 0;
            for (size_t p = start; p < off && i + 1 < out_size; p++) {
                out[i++] = decode_charmap_byte(ctx->rom[p]);
            }
            out[i] = '\0';
            /* Replace cart's PK ligature ('#') with the conventional
             * "POKE" spelling so the dropdown shows "POKE BALL" /
             * "POKE DOLL" rather than the unrenderable glyph. */
            if (i >= 2 && out[0] == '#' && out[1] == ' ') {
                char tmp[24];
                snprintf(tmp, sizeof(tmp), "POKE%s", out + 1);
                snprintf(out, out_size, "%s", tmp);
            }
            /* Skip placeholders. */
            if (strcmp(out, "TERU-SAMA") == 0 || strcmp(out, "?") == 0)
                return false;
            return out[0] != '\0';
        }
        off++;
    }
    return false;
}

GBGen2ItemPocket gb_mock_gen2_item_pocket(const GBContext* ctx, int item_id) {
    const GBGen2Info* c = gen2_info(ctx);
    if (!c || !ctx->rom) return GB_GEN2_POCKET_NONE;
    if (item_id < 1 || item_id > 255) return GB_GEN2_POCKET_NONE;
    /* Each ItemAttributes entry is 7 bytes: 2 price, 4 (held,
     * param, property, pocket), 1 menus. Pocket byte is at +5. */
    size_t off = rom_addr(c->rom_bank_item_attrs, c->rom_off_item_attrs) +
                 (size_t)(item_id - 1) * 7 + 5;
    if (off >= ctx->rom_size) return GB_GEN2_POCKET_NONE;
    uint8_t p = ctx->rom[off];
    if (p > 4) return GB_GEN2_POCKET_NONE;
    return (GBGen2ItemPocket)p;
}

/* Gen 2 stores money as a 3-byte BIG-ENDIAN integer (not BCD like
 * Gen 1). Range 0..999999 -- max value packs to bytes $0F $42 $3F.
 * Verified against pokecrystal's GetMoney / AddMoney routines, which
 * read/write three sequential bytes treating them as a 24-bit MSB-
 * first integer. The Gen 1 BCD assumption made the reader produce
 * weird values like 154245 for the actual max because $0F's high
 * nibble decodes to BCD-invalid digit 15. */
int gb_mock_gen2_get_money(const GBContext* ctx) {
    const GBGen2Info* c = gen2_info(ctx);
    if (!c || !ctx->wram) return -1;
    const uint8_t* p = &ctx->wram[0x1000 + (c->wram_money - 0xD000)];
    return ((int)p[0] << 16) | ((int)p[1] << 8) | (int)p[2];
}

bool gb_mock_gen2_set_money(GBContext* ctx, int amount) {
    const GBGen2Info* c = gen2_info(ctx);
    if (!c || !ctx->wram) return false;
    if (amount < 0) amount = 0;
    if (amount > 999999) amount = 999999;
    uint8_t* p = wram_b1_ptr(ctx, c->wram_money);
    p[0] = (uint8_t)((amount >> 16) & 0xFF);
    p[1] = (uint8_t)((amount >> 8)  & 0xFF);
    p[2] = (uint8_t)( amount        & 0xFF);
    return true;
}

bool gb_mock_gen2_give_item(GBContext* ctx, int item_id, int qty) {
    const GBGen2Info* c = gen2_info(ctx);
    if (!c || !ctx->wram) return false;
    if (item_id < 1 || item_id > 255) return false;
    if (qty < 1) return false;
    if (qty > 99) qty = 99;

    GBGen2ItemPocket pocket = gb_mock_gen2_item_pocket(ctx, item_id);
    uint16_t count_addr;
    int max_slots;
    switch (pocket) {
    case GB_GEN2_POCKET_ITEM:
        count_addr = c->wram_items_count;
        max_slots = 20;
        break;
    case GB_GEN2_POCKET_BALL:
        count_addr = c->wram_balls_count;
        max_slots = 12;
        break;
    default:
        /* KEY_ITEM and TM_HM intentionally unsupported for now. */
        return false;
    }

    uint8_t* count_p = wram_b1_ptr(ctx, count_addr);
    uint8_t* items_p = count_p + 1;
    int slots = *count_p;
    if (slots > max_slots) {
        fprintf(stderr, "[give_item/gen2] count=$%02X (max %d) looks "
                "uninitialized -- start the game / load a save first\n",
                slots, max_slots);
        return false;
    }

    for (int i = 0; i < slots; i++) {
        if (items_p[i * 2] == (uint8_t)item_id) {
            int cur = items_p[i * 2 + 1];
            int next = cur + qty;
            if (next > 99) next = 99;
            items_p[i * 2 + 1] = (uint8_t)next;
            return true;
        }
    }
    if (slots >= max_slots) {
        fprintf(stderr, "[give_item/gen2] pocket full (%d items); toss "
                "one in-game to make room\n", max_slots);
        return false;
    }
    items_p[slots * 2]     = (uint8_t)item_id;
    items_p[slots * 2 + 1] = (uint8_t)qty;
    items_p[slots * 2 + 2] = 0xFF;
    *count_p = (uint8_t)(slots + 1);
    return true;
}

size_t gb_mock_gen2_evos_record_offset(const GBContext* ctx, int dex) {
    const GBGen2Info* c = gen2_info(ctx);
    if (!c || dex < 1 || dex > GB_MOCK_GEN2_SPECIES_COUNT) return 0;
    size_t ptr_off = rom_addr(c->rom_bank_evos, c->rom_off_evos) +
                     (size_t)(dex - 1) * 2;
    if (ptr_off + 2 > ctx->rom_size) return 0;
    uint16_t data_addr = (uint16_t)(ctx->rom[ptr_off] |
                                    (ctx->rom[ptr_off + 1] << 8));
    size_t off = rom_addr(c->rom_bank_evos, data_addr);
    if (off >= ctx->rom_size) return 0;
    return off;
}

bool gb_mock_gen2_species_name(const GBContext* ctx, int species,
                               char* out, size_t out_size) {
    if (!out || out_size < 11) return false;
    out[0] = '\0';
    const GBGen2Info* c = gen2_info(ctx);
    if (!c || !ctx->rom) return false;
    if (species < 1 || species > GB_MOCK_GEN2_SPECIES_COUNT) return false;
    size_t off = rom_addr(c->rom_bank_names, c->rom_off_names) +
                 (size_t)(species - 1) * 10;
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

bool gb_mock_gen2_inject_builder(GBContext* ctx, int species,
                                 int level, bool shiny) {
    const GBGen2Info* c = gen2_info(ctx);
    if (!c || !ctx->wram || !ctx->rom) return false;
    if (species < 1 || species > GB_MOCK_GEN2_SPECIES_COUNT) return false;
    if (level < 2 || level > 100) return false;

    uint8_t party_count = gb_mock_gen2_party_count(ctx);
    if (party_count >= PARTY_LENGTH) return false;

    uint8_t base[6];
    if (!read_base_stats(ctx, c, species, base)) return false;

    int atk_dv, def_dv, spd_dv, spc_dv;
    pick_dvs(shiny, &atk_dv, &def_dv, &spd_dv, &spc_dv);
    int hp_dv = derive_hp_dv(atk_dv, def_dv, spd_dv, spc_dv);

    uint16_t hp = (uint16_t)stat_hp   (base[0], hp_dv,  level);
    uint16_t at = (uint16_t)stat_other(base[1], atk_dv, level);
    uint16_t df = (uint16_t)stat_other(base[2], def_dv, level);
    uint16_t sp = (uint16_t)stat_other(base[3], spd_dv, level);
    uint16_t sa = (uint16_t)stat_other(base[4], spc_dv, level);
    uint16_t sd = (uint16_t)stat_other(base[5], spc_dv, level);

    uint8_t moves[4];
    read_learnset(ctx, c, species, level, moves);

    uint32_t exp = exp_for_level(read_growth_rate(ctx, c, species), level);

    uint8_t mon[PARTYMON_STRUCT_LEN] = {0};
    mon[0]  = (uint8_t)species;
    mon[1]  = 0;
    memcpy(&mon[2], moves, 4);
    memcpy(&mon[6], wram_b1_cptr(ctx, c->wram_player_id), 2);
    mon[8]  = (uint8_t)((exp >> 16) & 0xFF);
    mon[9]  = (uint8_t)((exp >>  8) & 0xFF);
    mon[10] = (uint8_t)( exp        & 0xFF);
    mon[21] = (uint8_t)((atk_dv << 4) | (def_dv & 0xF));
    mon[22] = (uint8_t)((spd_dv << 4) | (spc_dv & 0xF));
    mon[23] = mon[24] = mon[25] = mon[26] = 20;
    mon[27] = 70;
    mon[31] = (uint8_t)level;
    mon[34] = (uint8_t)(hp >> 8); mon[35] = (uint8_t)(hp & 0xFF);
    mon[36] = mon[34];           mon[37] = mon[35];
    mon[38] = (uint8_t)(at >> 8); mon[39] = (uint8_t)(at & 0xFF);
    mon[40] = (uint8_t)(df >> 8); mon[41] = (uint8_t)(df & 0xFF);
    mon[42] = (uint8_t)(sp >> 8); mon[43] = (uint8_t)(sp & 0xFF);
    mon[44] = (uint8_t)(sa >> 8); mon[45] = (uint8_t)(sa & 0xFF);
    mon[46] = (uint8_t)(sd >> 8); mon[47] = (uint8_t)(sd & 0xFF);

    uint8_t* mon_dst = wram_b1_ptr(ctx, c->wram_party_mons) +
                       (size_t)party_count * PARTYMON_STRUCT_LEN;
    memcpy(mon_dst, mon, PARTYMON_STRUCT_LEN);

    size_t name_off = rom_addr(c->rom_bank_names, c->rom_off_names) +
                      (size_t)(species - 1) * 10;
    uint8_t* nick_dst = wram_b1_ptr(ctx, c->wram_party_nicks) +
                        (size_t)party_count * MON_NAME_LEN;
    memset(nick_dst, 0x50, MON_NAME_LEN);
    for (int i = 0; i < 10 && name_off + i < ctx->rom_size; i++) {
        uint8_t b = ctx->rom[name_off + i];
        nick_dst[i] = b;
        if (b == 0x50) break;
    }

    memcpy(wram_b1_ptr(ctx, c->wram_party_ots) +
               (size_t)party_count * MON_NAME_LEN,
           wram_b1_cptr(ctx, c->wram_player_name), MON_NAME_LEN);

    uint8_t* species_arr = wram_b1_ptr(ctx, c->wram_party_species);
    species_arr[party_count] = (uint8_t)species;
    species_arr[party_count + 1] = 0xFF;
    ctx->wram[0x1000 + (c->wram_party_count - 0xD000)] = party_count + 1;

    fprintf(stderr,
            "[gen2/%s] Built species=%d level=%d shiny=%d → slot %d "
            "(H/A/D/S/SA/SD=%d/%d/%d/%d/%d/%d)\n",
            c->title_prefix, species, level, shiny ? 1 : 0, party_count,
            hp, at, df, sp, sa, sd);
    return true;
}

/* -------------------------------------------------------------------------
 * Crystal-only Mobile-Adapter event injectors
 * ------------------------------------------------------------------------- */

/* WRAM bank 1 Pokedex caught bitfield (Crystal only; offsets differ
 * from Gold/Silver). Celebi = dex #251 → byte 31 of wPokedexCaught,
 * bit 2. */
#define CRYSTAL_WRAM_POKEDEX_CAUGHT  0xDEB9
#define CRYSTAL_WRAM_CELEBI_BYTE     0xDED8
#define CRYSTAL_WRAM_CELEBI_BIT      0x04

/* SRAM bank 1 GS Ball flags (Crystal-specific). */
#define CRYSTAL_SRAM_BANK_GS         1
#define CRYSTAL_SRAM_GS_BALL_FLAG    0xBE3C
#define CRYSTAL_SRAM_GS_BALL_BACKUP  0xBE44
#define CRYSTAL_GS_BALL_AVAILABLE    0x0B

/* Odd Egg distribution tables (Crystal-only — Gold/Silver don't have
 * the Mobile Egg distribution at all). */
#define CRYSTAL_ROM_BANK_ODD_EGG     0x7E
#define CRYSTAL_ROM_OFF_ODD_EGG_PROB 0x7552
#define CRYSTAL_ROM_OFF_ODD_EGG_DATA 0x756E
#define CRYSTAL_ODD_EGG_COUNT        14

bool gb_mock_crystal_celebi_caught(const GBContext* ctx) {
    if (!gb_mock_crystal_active(ctx) || !ctx->wram) return false;
    size_t off = (size_t)0x1000 + (size_t)(CRYSTAL_WRAM_CELEBI_BYTE - 0xD000);
    return (ctx->wram[off] & CRYSTAL_WRAM_CELEBI_BIT) != 0;
}

uint8_t gb_mock_crystal_gs_ball_flag(const GBContext* ctx) {
    if (!gb_mock_crystal_active(ctx) || !ctx->eram) return 0xFF;
    size_t off = (size_t)CRYSTAL_SRAM_BANK_GS * 0x2000u +
                 (size_t)(CRYSTAL_SRAM_GS_BALL_FLAG - 0xA000);
    if (off >= ctx->eram_size) return 0xFF;
    return ctx->eram[off];
}

const char* gb_mock_crystal_gs_ball_state_label(const GBContext* ctx) {
    uint8_t flag = gb_mock_crystal_gs_ball_flag(ctx);
    switch (flag) {
        case 0x00: return "Not set";
        case 0x0B: return "Armed (talk to the Goldenrod clerk)";
        case 0xFF: return "(SRAM unavailable)";
        default:   return "In progress";
    }
}

bool gb_mock_crystal_apply_gs_ball(GBContext* ctx) {
    if (!gb_mock_crystal_active(ctx) || !ctx->eram) return false;
    size_t bank_off = (size_t)CRYSTAL_SRAM_BANK_GS * 0x2000u;
    size_t flag_off   = bank_off + (size_t)(CRYSTAL_SRAM_GS_BALL_FLAG   - 0xA000);
    size_t backup_off = bank_off + (size_t)(CRYSTAL_SRAM_GS_BALL_BACKUP - 0xA000);
    if (backup_off >= ctx->eram_size) {
        fprintf(stderr, "[crystal] SRAM too small for GS Ball flag\n");
        return false;
    }
    ctx->eram[flag_off]   = CRYSTAL_GS_BALL_AVAILABLE;
    ctx->eram[backup_off] = CRYSTAL_GS_BALL_AVAILABLE;
    fprintf(stderr, "[crystal] GS Ball event armed (sGSBallFlag <- 0x0B)\n");
    return true;
}

bool gb_mock_crystal_apply_odd_egg(GBContext* ctx) {
    const GBGen2Info* c = gen2_info(ctx);
    if (!c || c->id != GB_MOCK_GEN2_CRYSTAL) return false;
    if (!ctx->wram || !ctx->rom) return false;

    uint8_t party_count = gb_mock_gen2_party_count(ctx);
    if (party_count >= PARTY_LENGTH) {
        fprintf(stderr, "[crystal] cannot give Odd Egg: party full (%d/%d)\n",
                party_count, PARTY_LENGTH);
        return false;
    }

    /* Roll cumulative probability table → entry index. */
    ensure_rng_seeded();
    uint16_t roll = (uint16_t)rand();
    size_t prob_base = (size_t)CRYSTAL_ROM_BANK_ODD_EGG * 0x4000u +
                       (size_t)(CRYSTAL_ROM_OFF_ODD_EGG_PROB - 0x4000);
    int entry_idx = CRYSTAL_ODD_EGG_COUNT - 1;
    for (int i = 0; i < CRYSTAL_ODD_EGG_COUNT; i++) {
        size_t off = prob_base + i * 2;
        if (off + 1 >= ctx->rom_size) break;
        uint16_t prob = (uint16_t)(ctx->rom[off] | (ctx->rom[off + 1] << 8));
        if (roll < prob) { entry_idx = i; break; }
    }

    size_t entry_off = (size_t)CRYSTAL_ROM_BANK_ODD_EGG * 0x4000u +
                       (size_t)(CRYSTAL_ROM_OFF_ODD_EGG_DATA - 0x4000) +
                       (size_t)entry_idx * NICKNAMED_MON_LEN;
    if (entry_off + NICKNAMED_MON_LEN > ctx->rom_size) return false;

    /* Copy the chosen prebuilt entry into the party slot using Crystal
     * WRAM offsets from the cart-info struct. */
    uint8_t* mon_dst = wram_b1_ptr(ctx, c->wram_party_mons) +
                       (size_t)party_count * PARTYMON_STRUCT_LEN;
    memcpy(mon_dst, &ctx->rom[entry_off], PARTYMON_STRUCT_LEN);
    memcpy(mon_dst + 6, wram_b1_cptr(ctx, c->wram_player_id), 2);

    uint8_t* nick_dst = wram_b1_ptr(ctx, c->wram_party_nicks) +
                        (size_t)party_count * 11;
    memcpy(nick_dst, &ctx->rom[entry_off + PARTYMON_STRUCT_LEN], 11);

    memcpy(wram_b1_ptr(ctx, c->wram_party_ots) + (size_t)party_count * 11,
           wram_b1_cptr(ctx, c->wram_player_name), 11);

    uint8_t* species = wram_b1_ptr(ctx, c->wram_party_species);
    species[party_count] = EGG_SPECIES_MARKER;
    species[party_count + 1] = 0xFF;

    ctx->wram[0x1000 + (c->wram_party_count - 0xD000)] = party_count + 1;

    fprintf(stderr, "[crystal] Odd Egg added (entry %d, slot %d)\n",
            entry_idx, party_count);
    return true;
}
