/**
 * @file mock_crystal.c
 * @brief Crystal-only runtime event injectors. See header for design.
 */

#include "mock_crystal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* SRAM bank 1 offsets — pulled from pret/pokecrystal symbols.
 * sGSBallFlag and sGSBallFlagBackup live at $A000+$BE3C and +$BE44
 * when SRAM bank 1 is mapped. The 0x0B value is GS_BALL_AVAILABLE,
 * the magic the Goldenrod NPC checks before launching the Kurt
 * → Ilex → Celebi script chain. */
#define SRAM_BANK_GS         1
#define SRAM_GS_BALL_FLAG    0xBE3C
#define SRAM_GS_BALL_BACKUP  0xBE44
#define GS_BALL_AVAILABLE    0x0B

/* WRAM bank 1 offset for the Pokedex caught bitfield. Celebi is
 * dex #251 → byte 31 of wPokedexCaught, bit 2. */
#define WRAM_POKEDEX_CAUGHT  0xDEB9
#define WRAM_CELEBI_BYTE     0xDED8  /* DEB9 + 31 */
#define WRAM_CELEBI_BIT      0x04

/* Party + player-identity offsets (all WRAM bank 1, indexed from
 * the start of the bank-1 region at offset 0x1000 in our flat
 * wram buffer). All values verified against pret/pokecrystal's
 * pokecrystal11.sym. */
#define WRAM_PARTY_COUNT     0xDCD7
#define WRAM_PARTY_SPECIES   0xDCD8       /* 6 species bytes + terminator */
#define WRAM_PARTY_MONS      0xDCDF       /* 6 × 48-byte party_struct entries */
#define WRAM_PARTY_OTS       0xDDFF       /* 6 × 11-byte trainer names */
#define WRAM_PARTY_NICKS     0xDE41       /* 6 × 11-byte nicknames */
#define WRAM_PLAYER_ID       0xD47B       /* 2 bytes — trainer ID */
#define WRAM_PLAYER_NAME     0xD47D       /* 11 bytes */

#define PARTYMON_STRUCT_LEN  48
#define NICKNAMED_MON_LEN    (PARTYMON_STRUCT_LEN + 11)  /* + nickname */
#define PARTY_LENGTH         6

/* The OddEggs distribution table is in vanilla US Crystal ROM at
 * bank 0x7E $7552 (probabilities, 14 × uint16_t cumulative) and
 * $756E (data, 14 × NICKNAMED_MON_LEN entries). Even though the
 * Mobile distribution that referenced these was stripped from the
 * US release, the data tables themselves stayed in the ROM. */
#define ROM_BANK_ODD_EGG     0x7E
#define ROM_OFF_ODD_EGG_PROB 0x7552
#define ROM_OFF_ODD_EGG_DATA 0x756E
#define ODD_EGG_COUNT        14

#define EGG_SPECIES_MARKER   0xFD  /* wPartySpecies[i] == this means "egg" */

/* Cart ROM data tables (Crystal US Rev 1, locations from pokecrystal11.sym). */
#define ROM_BANK_BASE_STATS      0x14
#define ROM_OFF_BASE_STATS       0x5424   /* BaseData label */
#define BASE_DATA_SIZE           32
#define ROM_OFF_POKEMON_NAMES    0x7384   /* PokemonNames, same bank, 10 chars/entry */
#define ROM_BANK_EVOS_ATTACKS    0x10
#define ROM_OFF_EVOS_ATTACKS     0x65B1   /* EvosAttacksPointers */

#define MON_NAME_LEN             11

bool gb_mock_crystal_active(const GBContext* ctx) {
    if (!ctx || !ctx->rom || ctx->rom_size < 0x144) return false;
    /* Title bytes 0x134-0x142 (15 chars) — "PM_CRYSTAL\0..." for
     * both US revisions. Match the first 10 chars to cover the
     * underscore variants. */
    return memcmp((const char*)ctx->rom + 0x134, "PM_CRYSTAL", 10) == 0;
}

bool gb_mock_crystal_celebi_caught(const GBContext* ctx) {
    if (!gb_mock_crystal_active(ctx)) return false;
    if (!ctx->wram) return false;
    /* WRAM bank 1 is always at offset 0x1000 in our flat 8-bank buffer;
     * the Pokedex flags live there regardless of which bank the cart
     * currently has mapped. */
    size_t off = (size_t)0x1000 + (size_t)(WRAM_CELEBI_BYTE - 0xD000);
    return (ctx->wram[off] & WRAM_CELEBI_BIT) != 0;
}

uint8_t gb_mock_crystal_gs_ball_flag(const GBContext* ctx) {
    if (!gb_mock_crystal_active(ctx) || !ctx->eram) return 0xFF;
    size_t off = (size_t)SRAM_BANK_GS * 0x2000u +
                 (size_t)(SRAM_GS_BALL_FLAG - 0xA000);
    if (off >= ctx->eram_size) return 0xFF;
    return ctx->eram[off];
}

const char* gb_mock_crystal_gs_ball_state_label(const GBContext* ctx) {
    /* Flag values per pret/pokecrystal: only $0B (AVAILABLE) is the
     * one we set; the rest of the chain runs through cart-side
     * script state. We surface the friendly label for $00 and $0B
     * since those are what the user will actually see between
     * pressing the button and saving in-game. */
    uint8_t flag = gb_mock_crystal_gs_ball_flag(ctx);
    switch (flag) {
        case 0x00: return "Not set";
        case 0x0B: return "Armed (talk to the Goldenrod clerk)";
        case 0xFF: return "(SRAM unavailable)";
        default:   return "In progress";
    }
}

static uint8_t* wram_b1_ptr(GBContext* ctx, uint16_t addr) {
    /* Translate a $D000-$DFFF address into our flat wram buffer's
     * bank-1 region (offset 0x1000..0x1FFF). */
    return &ctx->wram[(size_t)0x1000 + (size_t)(addr - 0xD000)];
}

uint8_t gb_mock_crystal_party_count(const GBContext* ctx) {
    if (!gb_mock_crystal_active(ctx) || !ctx->wram) return 0xFF;
    return ctx->wram[0x1000 + (WRAM_PARTY_COUNT - 0xD000)];
}

static void ensure_rng_seeded(void) {
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = true;
    }
}

bool gb_mock_crystal_apply_odd_egg(GBContext* ctx) {
    if (!gb_mock_crystal_active(ctx)) return false;
    if (!ctx->wram || !ctx->rom) return false;

    uint8_t party_count = gb_mock_crystal_party_count(ctx);
    if (party_count >= PARTY_LENGTH) {
        fprintf(stderr, "[crystal] cannot give Odd Egg: party full (%d/%d)\n",
                party_count, PARTY_LENGTH);
        return false;
    }

    /* Roll the cumulative-probability table the same way the cart's
     * own RandomOddEggIndex routine would: pick a uniform 16-bit
     * value and find the first prob threshold that exceeds it. */
    ensure_rng_seeded();
    uint16_t roll = (uint16_t)rand();
    size_t prob_base = (size_t)ROM_BANK_ODD_EGG * 0x4000u +
                       (size_t)(ROM_OFF_ODD_EGG_PROB - 0x4000);
    int entry_idx = ODD_EGG_COUNT - 1;
    for (int i = 0; i < ODD_EGG_COUNT; i++) {
        size_t off = prob_base + i * 2;
        if (off + 1 >= ctx->rom_size) break;
        uint16_t prob = (uint16_t)(ctx->rom[off] | (ctx->rom[off + 1] << 8));
        if (roll < prob) { entry_idx = i; break; }
    }

    /* Copy the chosen entry into the player's next party slot. The
     * 14 entries pair up: each species has a "regular DV" slot
     * (even index) and a "shiny DV" slot (odd index), so 14% / 100%
     * of rolls land on shiny variants per the original Odd Egg's
     * elevated shiny rate. */
    size_t entry_off = (size_t)ROM_BANK_ODD_EGG * 0x4000u +
                       (size_t)(ROM_OFF_ODD_EGG_DATA - 0x4000) +
                       (size_t)entry_idx * NICKNAMED_MON_LEN;
    if (entry_off + NICKNAMED_MON_LEN > ctx->rom_size) return false;

    /* PARTYMON struct (48 bytes) → wPartyMons + party_count*48 */
    uint8_t* mon_dst = wram_b1_ptr(ctx, WRAM_PARTY_MONS) +
                       (size_t)party_count * PARTYMON_STRUCT_LEN;
    memcpy(mon_dst, &ctx->rom[entry_off], PARTYMON_STRUCT_LEN);

    /* Override OT_ID (offset 6 in party_struct, 2 bytes) with the
     * player's trainer ID so the egg counts as the player's own
     * (otherwise traded-mon experience penalties would apply). */
    memcpy(mon_dst + 6, wram_b1_ptr(ctx, WRAM_PLAYER_ID), 2);

    /* Nickname "EGG\0..." (11 bytes from offset 48 in the table
     * entry) → wPartyMonNicknames + party_count*11 */
    uint8_t* nick_dst = wram_b1_ptr(ctx, WRAM_PARTY_NICKS) +
                        (size_t)party_count * 11;
    memcpy(nick_dst, &ctx->rom[entry_off + PARTYMON_STRUCT_LEN], 11);

    /* OT name → wPartyMonOTs + party_count*11, copied from the
     * player's name so the egg shows as un-traded. */
    uint8_t* ot_dst = wram_b1_ptr(ctx, WRAM_PARTY_OTS) +
                      (size_t)party_count * 11;
    memcpy(ot_dst, wram_b1_ptr(ctx, WRAM_PLAYER_NAME), 11);

    /* wPartySpecies[party_count] = EGG marker; terminator at next. */
    uint8_t* species = wram_b1_ptr(ctx, WRAM_PARTY_SPECIES);
    species[party_count] = EGG_SPECIES_MARKER;
    species[party_count + 1] = 0xFF;

    /* Increment wPartyCount last so the cart never sees a half-
     * initialized slot if it happens to read mid-write. */
    ctx->wram[0x1000 + (WRAM_PARTY_COUNT - 0xD000)] = party_count + 1;

    fprintf(stderr, "[crystal] Odd Egg added (table entry %d, party slot %d)\n",
            entry_idx, party_count);
    return true;
}

/* ---------------------------------------------------------------------------
 * Generic Pokemon builder
 * --------------------------------------------------------------------------- */

/* Translate a CGB-banked address into a flat ROM offset. */
static size_t rom_addr(uint8_t bank, uint16_t addr) {
    return (size_t)bank * 0x4000u + (size_t)(addr - 0x4000);
}

/* Gen 2 stat formulas with stat_exp == 0 (the cart's standard
 * level-up uses these; our injected mons are fresh so this is the
 * right starting state):
 *   HP   = ((base + DV) * 2 * level) / 100 + level + 10
 *   Stat = ((base + DV) * 2 * level) / 100 + 5
 * Reimplemented clean-room from the Bulbapedia stat-mechanics
 * article; PKSM-Core's PK2 has the same formula but is GPL.        */
static int stat_hp(int base, int dv, int level) {
    return ((base + dv) * 2 * level) / 100 + level + 10;
}
static int stat_other(int base, int dv, int level) {
    return ((base + dv) * 2 * level) / 100 + 5;
}

/* HP DV is derived from the low bits of the other 4 DVs: bit0=Spd,
 * bit1=Def, bit2=Atk, bit3=Spc. */
static int derive_hp_dv(int atk, int def, int spd, int spc) {
    return ((spd & 1) << 0) | ((def & 1) << 1) |
           ((atk & 1) << 2) | ((spc & 1) << 3);
}

/* Canonical Gen 2 shiny condition: Def=Spd=Spc=10 AND Atk has bit 1
 * set with bits 2-3 set (= 2,3,6,7,10,11,14,15). We use Atk=10. */
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

/* Read the 6 base stats (HP, Atk, Def, Spd, SAt, SDf) into out. */
static bool read_base_stats(const GBContext* ctx, int species, uint8_t out[6]) {
    if (species < 1 || species > GB_MOCK_CRYSTAL_SPECIES_COUNT) return false;
    /* Skip the leading dex-number byte to land on BASE_HP. */
    size_t off = rom_addr(ROM_BANK_BASE_STATS, ROM_OFF_BASE_STATS) +
                 (size_t)(species - 1) * BASE_DATA_SIZE + 1;
    if (off + 6 > ctx->rom_size) return false;
    memcpy(out, &ctx->rom[off], 6);
    return true;
}

/* Read the species's growth rate (offset 23 in BaseData per the
 * struct: dex(1) + stats(6) + types(2) + catch(1) + exp(1) + items(2)
 * + gender(1) + skip(1) + hatch(1) + skip(1) + pic_size(1) + frontpic(2)
 * + backpic(2) + growth_rate at +23). */
static uint8_t read_growth_rate(const GBContext* ctx, int species) {
    size_t off = rom_addr(ROM_BANK_BASE_STATS, ROM_OFF_BASE_STATS) +
                 (size_t)(species - 1) * BASE_DATA_SIZE + 23;
    if (off >= ctx->rom_size) return 0; /* default to MEDIUM_FAST */
    return ctx->rom[off];
}

/* Exp needed to be EXACTLY at level N for a given growth rate.
 * Constants match data/growth_rates.asm. */
static uint32_t exp_for_level(uint8_t growth_rate, int level) {
    uint32_t n = (uint32_t)level;
    uint32_t n3 = n * n * n;
    switch (growth_rate) {
        case 0: /* MEDIUM_FAST */  return n3;
        case 1: /* SLIGHTLY_FAST */ return (n3 * 4) / 5 + 1; /* approx */
        case 2: /* SLIGHTLY_SLOW */ return (n3 * 5) / 4 + 1;
        case 3: /* MEDIUM_SLOW  */
            /* 6/5 n^3 − 15 n^2 + 100 n − 140 */
            if (level <= 1) return 0;
            return (6 * n3) / 5 + 100 * n -
                   (n > 0 ? 15 * n * n + 140 : 0);
        case 4: /* FAST */         return (n3 * 4) / 5;
        case 5: /* SLOW */         return (n3 * 5) / 4;
        default:                   return n3;
    }
}

/* Read the last-4-learned moves at or below the chosen level from
 * the cart's EvosAttacks table. Returns the count of moves filled. */
static int read_learnset(const GBContext* ctx, int species,
                         int level, uint8_t moves[4]) {
    memset(moves, 0, 4);
    if (species < 1 || species > GB_MOCK_CRYSTAL_SPECIES_COUNT) return 0;

    /* Pointer table: 251 entries × 2 bytes, little-endian, all within
     * bank 0x10's $4000-$7FFF window. */
    size_t ptr_off = rom_addr(ROM_BANK_EVOS_ATTACKS, ROM_OFF_EVOS_ATTACKS) +
                     (size_t)(species - 1) * 2;
    if (ptr_off + 2 > ctx->rom_size) return 0;
    uint16_t data_addr = (uint16_t)(ctx->rom[ptr_off] |
                                    (ctx->rom[ptr_off + 1] << 8));
    size_t off = rom_addr(ROM_BANK_EVOS_ATTACKS, data_addr);

    /* Evolutions are variable-length records terminated by a 0
     * byte. The cart parses them by leading method byte, but for
     * our purposes we just need to find the trailing 0 so we land
     * on the (level, move) pair list. Evolution records are 4 or
     * 5 bytes — easier to scan for 0. */
    while (off < ctx->rom_size && ctx->rom[off] != 0) off++;
    if (off >= ctx->rom_size) return 0;
    off++; /* skip terminator */

    /* (level, move) pairs until level byte == 0. Maintain a 4-slot
     * sliding window of the most-recent learns at <= chosen level. */
    int count = 0;
    while (off + 1 < ctx->rom_size) {
        uint8_t lvl = ctx->rom[off];
        if (lvl == 0) break;
        uint8_t mv = ctx->rom[off + 1];
        off += 2;
        if (lvl > level) continue;
        /* shift-and-append */
        moves[0] = moves[1]; moves[1] = moves[2];
        moves[2] = moves[3]; moves[3] = mv;
        if (count < 4) count++;
    }
    /* Left-align if fewer than 4 moves were learned. */
    if (count > 0 && count < 4) {
        uint8_t tmp[4] = {0};
        for (int i = 0; i < count; i++) tmp[i] = moves[4 - count + i];
        memcpy(moves, tmp, 4);
    }
    return count;
}

/* Decode one Gen 2 charmap byte to ASCII. Best effort — unknown
 * codes become '?'. */
static char decode_charmap_byte(uint8_t c) {
    if (c >= 0x80 && c <= 0x99) return (char)('A' + (c - 0x80));
    if (c >= 0xA0 && c <= 0xB9) return (char)('a' + (c - 0xA0));
    if (c == 0x7F)              return ' ';
    if (c == 0xE0)              return '\''; /* apostrophe */
    if (c == 0xE8)              return '.';
    if (c == 0xF1)              return '.';  /* mid-dot variant */
    if (c == 0xF6)              return '0';  /* digit 0 in some fonts */
    if (c >= 0xF6 && c <= 0xFF) return (char)('0' + (c - 0xF6));
    return '?';
}

bool gb_mock_crystal_species_name(const GBContext* ctx, int species,
                                  char* out, size_t out_size) {
    if (!out || out_size < 11) return false;
    out[0] = '\0';
    if (!gb_mock_crystal_active(ctx) || !ctx->rom) return false;
    if (species < 1 || species > GB_MOCK_CRYSTAL_SPECIES_COUNT) return false;
    size_t off = rom_addr(ROM_BANK_BASE_STATS, ROM_OFF_POKEMON_NAMES) +
                 (size_t)(species - 1) * 10;
    if (off + 10 > ctx->rom_size) return false;
    size_t i;
    for (i = 0; i < 10; i++) {
        uint8_t c = ctx->rom[off + i];
        if (c == 0x50) break;
        out[i] = decode_charmap_byte(c);
    }
    out[i] = '\0';
    return true;
}

bool gb_mock_crystal_inject_builder(GBContext* ctx, int species,
                                    int level, bool shiny) {
    if (!gb_mock_crystal_active(ctx) || !ctx->wram || !ctx->rom) return false;
    if (species < 1 || species > GB_MOCK_CRYSTAL_SPECIES_COUNT) return false;
    if (level < 2 || level > 100) return false;

    uint8_t party_count = gb_mock_crystal_party_count(ctx);
    if (party_count >= PARTY_LENGTH) return false;

    uint8_t base[6];
    if (!read_base_stats(ctx, species, base)) return false;

    int atk_dv, def_dv, spd_dv, spc_dv;
    pick_dvs(shiny, &atk_dv, &def_dv, &spd_dv, &spc_dv);
    int hp_dv = derive_hp_dv(atk_dv, def_dv, spd_dv, spc_dv);

    uint16_t hp = (uint16_t)stat_hp(base[0], hp_dv,  level);
    uint16_t at = (uint16_t)stat_other(base[1], atk_dv, level);
    uint16_t df = (uint16_t)stat_other(base[2], def_dv, level);
    uint16_t sp = (uint16_t)stat_other(base[3], spd_dv, level);
    uint16_t sa = (uint16_t)stat_other(base[4], spc_dv, level);
    uint16_t sd = (uint16_t)stat_other(base[5], spc_dv, level);

    uint8_t moves[4];
    read_learnset(ctx, species, level, moves);

    uint8_t growth = read_growth_rate(ctx, species);
    uint32_t exp = exp_for_level(growth, level);

    /* Build the 48-byte party_mon struct. All multi-byte fields are
     * big-endian in cart RAM, except the DVs (single packed uint16
     * with ATK in high nibble of byte 0 and DEF in low; same pattern
     * for byte 1 with SPD/SPC). */
    uint8_t mon[48] = {0};
    mon[0]  = (uint8_t)species;
    mon[1]  = 0;  /* MON_ITEM — none */
    memcpy(&mon[2], moves, 4);
    /* OT_ID — copy player's (2 bytes, native byte order in WRAM) */
    memcpy(&mon[6], wram_b1_ptr(ctx, WRAM_PLAYER_ID), 2);
    /* MON_EXP (3 bytes big-endian) */
    mon[8]  = (uint8_t)((exp >> 16) & 0xFF);
    mon[9]  = (uint8_t)((exp >>  8) & 0xFF);
    mon[10] = (uint8_t)( exp        & 0xFF);
    /* MON_STAT_EXP (offsets 11-20) stays zero */
    /* MON_DVS (offsets 21-22) */
    mon[21] = (uint8_t)((atk_dv << 4) | (def_dv & 0xF));
    mon[22] = (uint8_t)((spd_dv << 4) | (spc_dv & 0xF));
    /* MON_PP (offsets 23-26) — default 20 each */
    mon[23] = mon[24] = mon[25] = mon[26] = 20;
    /* MON_HAPPINESS = 70 (Gen 2 default tame value) */
    mon[27] = 70;
    /* MON_POKERUS = 0, MON_CAUGHTDATA = 0 (offsets 28-30) */
    mon[31] = (uint8_t)level;
    /* MON_STATUS = 0 (offset 32), rb_skip (33) */
    /* MON_HP (34-35), MAX (36-37), stats (38-47) — all big-endian */
    mon[34] = (uint8_t)(hp >> 8); mon[35] = (uint8_t)(hp & 0xFF);
    mon[36] = mon[34];           mon[37] = mon[35];
    mon[38] = (uint8_t)(at >> 8); mon[39] = (uint8_t)(at & 0xFF);
    mon[40] = (uint8_t)(df >> 8); mon[41] = (uint8_t)(df & 0xFF);
    mon[42] = (uint8_t)(sp >> 8); mon[43] = (uint8_t)(sp & 0xFF);
    mon[44] = (uint8_t)(sa >> 8); mon[45] = (uint8_t)(sa & 0xFF);
    mon[46] = (uint8_t)(sd >> 8); mon[47] = (uint8_t)(sd & 0xFF);

    /* Commit the writes (in safe order — count last). */
    uint8_t* mon_dst = wram_b1_ptr(ctx, WRAM_PARTY_MONS) +
                       (size_t)party_count * PARTYMON_STRUCT_LEN;
    memcpy(mon_dst, mon, PARTYMON_STRUCT_LEN);

    /* Nickname: copy the species name from PokemonNames so the
     * nickname matches the species when uncustomized. */
    size_t name_off = rom_addr(ROM_BANK_BASE_STATS, ROM_OFF_POKEMON_NAMES) +
                      (size_t)(species - 1) * 10;
    uint8_t* nick_dst = wram_b1_ptr(ctx, WRAM_PARTY_NICKS) +
                        (size_t)party_count * MON_NAME_LEN;
    memset(nick_dst, 0x50, MON_NAME_LEN);  /* fill with terminators */
    for (int i = 0; i < 10 && name_off + i < ctx->rom_size; i++) {
        uint8_t c = ctx->rom[name_off + i];
        nick_dst[i] = c;
        if (c == 0x50) break;
    }

    /* OT name: copy player's. */
    memcpy(wram_b1_ptr(ctx, WRAM_PARTY_OTS) + (size_t)party_count * MON_NAME_LEN,
           wram_b1_ptr(ctx, WRAM_PLAYER_NAME), MON_NAME_LEN);

    /* Update species list + party count last so a mid-write read
     * never sees a half-initialized slot. */
    uint8_t* species_arr = wram_b1_ptr(ctx, WRAM_PARTY_SPECIES);
    species_arr[party_count] = (uint8_t)species;
    species_arr[party_count + 1] = 0xFF;
    ctx->wram[0x1000 + (WRAM_PARTY_COUNT - 0xD000)] = party_count + 1;

    fprintf(stderr,
            "[crystal] Built species=%d level=%d shiny=%d → slot %d "
            "(stats H/A/D/S/SA/SD = %d/%d/%d/%d/%d/%d, "
            "DVs A/D/S/Sp = %d/%d/%d/%d, growth=%d, exp=%u)\n",
            species, level, shiny ? 1 : 0, party_count,
            hp, at, df, sp, sa, sd,
            atk_dv, def_dv, spd_dv, spc_dv,
            growth, exp);
    return true;
}

bool gb_mock_crystal_apply_gs_ball(GBContext* ctx) {
    if (!gb_mock_crystal_active(ctx)) return false;
    if (!ctx->eram) return false;

    /* SRAM is a flat ram_bank * 0x2000 + (addr - 0xA000) buffer,
     * regardless of which bank the cart currently has mapped. We
     * write directly to bank 1's region so the change survives the
     * cart's next save+load cycle. */
    size_t bank_offset = (size_t)SRAM_BANK_GS * 0x2000u;
    size_t flag_off    = bank_offset + (size_t)(SRAM_GS_BALL_FLAG    - 0xA000);
    size_t backup_off  = bank_offset + (size_t)(SRAM_GS_BALL_BACKUP  - 0xA000);
    if (flag_off >= ctx->eram_size || backup_off >= ctx->eram_size) {
        fprintf(stderr, "[crystal] SRAM too small for GS Ball flag (have %zu, need %zu)\n",
                ctx->eram_size, backup_off + 1);
        return false;
    }
    ctx->eram[flag_off]   = GS_BALL_AVAILABLE;
    ctx->eram[backup_off] = GS_BALL_AVAILABLE;
    fprintf(stderr, "[crystal] GS Ball event armed (sGSBallFlag <- 0x0B)\n");
    return true;
}
