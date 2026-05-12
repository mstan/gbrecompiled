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
