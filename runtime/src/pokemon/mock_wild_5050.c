/**
 * @file mock_wild_5050.c
 * @brief Wild-encounter 50/50 patch. See header.
 *
 * Per-cart diff tables ship as generated headers in
 * include/pokemon/gen{1,2}_wild_diffs_*.gen.h. Each entry has
 * identical layout in both gens: (rom_offset, count, species[3]).
 * At toggle-on we pick one candidate uniformly at random per slot
 * and write it into the cart's ROM; at toggle-off we restore the
 * original byte from a saved backup.
 */

#include "pokemon/mock_wild_5050.h"
#include "pokemon/mock_gen1.h"
#include "pokemon/mock_gen2.h"
#include "pokemon/gen1_wild_diffs_rb.gen.h"
#include "pokemon/gen1_wild_diffs_yellow.gen.h"
#include "pokemon/gen2_wild_diffs_gs.gen.h"
#include "pokemon/gen2_wild_diffs_crystal.gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Sized to the largest table so one backup buffer fits any cart. */
#define BIGGEST(a, b) ((a) > (b) ? (a) : (b))
#define MAX_SLOTS \
    BIGGEST(BIGGEST(GEN1_WILD_DIFFS_RB_COUNT, GEN1_WILD_DIFFS_YELLOW_COUNT), \
            BIGGEST(GEN2_WILD_DIFFS_GS_COUNT, GEN2_WILD_DIFFS_CRYSTAL_COUNT))

static uint8_t g_saved[MAX_SLOTS];
static bool    g_active[MAX_SLOTS];
static bool    g_enabled = false;
static bool    g_seeded  = false;

/* The cart-family-specific table that was patched on toggle-on. We
 * keep these around so toggle-off can restore against the same array
 * regardless of any cart-detection drift. */
typedef struct {
    uint32_t rom_offset;
    uint8_t  count;
    uint8_t  species[3];
} WildDiffEntry;

static const WildDiffEntry* g_active_table = NULL;
static int                  g_active_count = 0;

/* Last-seen map key, packed as (group << 8) | number. Gen 1 uses
 * group = 0 since it doesn't have map groups. -1 means "no map
 * observed yet" so the very first tick after toggle-on re-rolls
 * unconditionally. */
static int g_last_map_key = -1;

static void ensure_seeded(void) {
    if (!g_seeded) { srand((unsigned)time(NULL)); g_seeded = true; }
}

/* Read the cart's current (map_group, map_number) into a packed int.
 * Gen 1 only has a number byte; we return (0 << 8) | number. */
static int read_map_key(const GBContext* ctx) {
    GBGen1Game g1 = gb_mock_gen1_detect(ctx);
    GBGen2Game g2 = gb_mock_gen2_detect(ctx);
    /* WRAM bank 0 ($D000-$DFFF) lives at flat offset 0x1000 for
     * Gen 1; Gen 2 carts use bank 1 which lives at 0x1000 as well
     * (the runtime's CGB WRAM bank-1 region). */
    if (g1 == GB_MOCK_GEN1_YELLOW) {
        return ctx->wram[0x1000 + (0xD35D - 0xD000)];
    }
    if (g1 == GB_MOCK_GEN1_RED || g1 == GB_MOCK_GEN1_BLUE ||
        g1 == GB_MOCK_GEN1_GREEN) {
        return ctx->wram[0x1000 + (0xD35E - 0xD000)];
    }
    if (g2 == GB_MOCK_GEN2_GOLD || g2 == GB_MOCK_GEN2_SILVER) {
        uint8_t group = ctx->wram[0x1000 + (0xDA00 - 0xD000)];
        uint8_t num   = ctx->wram[0x1000 + (0xDA01 - 0xD000)];
        return (group << 8) | num;
    }
    if (g2 == GB_MOCK_GEN2_CRYSTAL) {
        uint8_t group = ctx->wram[0x1000 + (0xDCB5 - 0xD000)];
        uint8_t num   = ctx->wram[0x1000 + (0xDCB6 - 0xD000)];
        return (group << 8) | num;
    }
    return -1;
}

/* Roll a fresh species byte for every active slot. Backup bytes
 * stay as the originals saved on toggle-on so toggle-off still
 * restores correctly. */
static void reroll_active(GBContext* ctx) {
    if (!g_active_table) return;
    int rolled = 0;
    for (int i = 0; i < g_active_count; i++) {
        if (!g_active[i]) continue;
        uint32_t off = g_active_table[i].rom_offset;
        if (off >= ctx->rom_size) continue;
        int pick = rand() % g_active_table[i].count;
        ctx->rom[off] = g_active_table[i].species[pick];
        rolled++;
    }
    fprintf(stderr, "[wild_5050] re-rolled %d slots on map change\n", rolled);
}

static void resolve_table(const GBContext* ctx,
                          const WildDiffEntry** out_table,
                          int* out_count) {
    GBGen1Game g1 = gb_mock_gen1_detect(ctx);
    GBGen2Game g2 = gb_mock_gen2_detect(ctx);
    *out_table = NULL;
    *out_count = 0;

    if (g1 == GB_MOCK_GEN1_RED || g1 == GB_MOCK_GEN1_BLUE ||
        g1 == GB_MOCK_GEN1_GREEN) {
        *out_table = (const WildDiffEntry*)GEN1_WILD_DIFFS_RB;
        *out_count = GEN1_WILD_DIFFS_RB_COUNT;
    } else if (g1 == GB_MOCK_GEN1_YELLOW) {
        *out_table = (const WildDiffEntry*)GEN1_WILD_DIFFS_YELLOW;
        *out_count = GEN1_WILD_DIFFS_YELLOW_COUNT;
    } else if (g2 == GB_MOCK_GEN2_GOLD || g2 == GB_MOCK_GEN2_SILVER) {
        *out_table = (const WildDiffEntry*)GEN2_WILD_DIFFS_GS;
        *out_count = GEN2_WILD_DIFFS_GS_COUNT;
    } else if (g2 == GB_MOCK_GEN2_CRYSTAL) {
        *out_table = (const WildDiffEntry*)GEN2_WILD_DIFFS_CRYSTAL;
        *out_count = GEN2_WILD_DIFFS_CRYSTAL_COUNT;
    }
}

bool gb_wild_5050_is_supported(const GBContext* ctx) {
    return gb_mock_gen1_detect(ctx) != GB_MOCK_GEN1_NONE ||
           gb_mock_gen2_detect(ctx) != GB_MOCK_GEN2_NONE;
}

bool gb_wild_5050_is_enabled(void) { return g_enabled; }

int gb_wild_5050_slot_count(const GBContext* ctx) {
    const WildDiffEntry* tab; int n;
    resolve_table(ctx, &tab, &n);
    return n;
}

void gb_wild_5050_set_enabled(GBContext* ctx, bool enable) {
    if (g_enabled == enable || !ctx || !ctx->rom) return;
    if (!gb_wild_5050_is_supported(ctx)) return;

    if (enable) {
        ensure_seeded();
        resolve_table(ctx, &g_active_table, &g_active_count);
        if (!g_active_table) return;
        int patched = 0;
        for (int i = 0; i < g_active_count; i++) {
            uint32_t off = g_active_table[i].rom_offset;
            if (off >= ctx->rom_size) {
                g_active[i] = false;
                continue;
            }
            g_saved[i]  = ctx->rom[off];
            g_active[i] = true;
            int pick = rand() % g_active_table[i].count;
            ctx->rom[off] = g_active_table[i].species[pick];
            patched++;
        }
        g_enabled = true;
        g_last_map_key = read_map_key(ctx);
        fprintf(stderr, "[wild_5050] enabled: %d/%d slots patched\n",
                patched, g_active_count);
    } else {
        if (!g_active_table) return;
        for (int i = 0; i < g_active_count; i++) {
            if (!g_active[i]) continue;
            uint32_t off = g_active_table[i].rom_offset;
            if (off < ctx->rom_size) ctx->rom[off] = g_saved[i];
        }
        memset(g_active, 0, sizeof(g_active));
        g_active_table = NULL;
        g_active_count = 0;
        g_enabled = false;
        g_last_map_key = -1;
        fprintf(stderr, "[wild_5050] restored\n");
    }
}

void gb_wild_5050_tick(GBContext* ctx) {
    if (!g_enabled || !ctx || !ctx->rom || !ctx->wram) return;
    int key = read_map_key(ctx);
    if (key < 0 || key == g_last_map_key) return;
    g_last_map_key = key;
    reroll_active(ctx);
}
