/**
 * @file mock_crystal.c
 * @brief Crystal-only runtime event injectors. See header for design.
 */

#include "mock_crystal.h"
#include <stdio.h>
#include <string.h>

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
