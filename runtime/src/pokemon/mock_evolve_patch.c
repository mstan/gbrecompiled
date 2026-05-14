/**
 * @file mock_evolve_patch.c
 * @brief Trade-evolution -> level-evolution ROM patch.
 *
 * See header. The opcode for EVOLVE_TRADE and EVOLVE_LEVEL is 0x03
 * vs 0x01 respectively. Both rows are 3 bytes total in both gens
 * (opcode, param, target). We change opcode + param in place and
 * leave the target byte alone since it's already correct.
 */

#include "pokemon/mock_evolve_patch.h"
#include "pokemon/mock_gen1.h"
#include "pokemon/mock_gen2.h"

#include <stdio.h>
#include <string.h>

#define OP_EVOLVE_LEVEL  0x01
#define OP_EVOLVE_ITEM   0x02
#define OP_EVOLVE_TRADE  0x03

static const GBEvolvePatchEntry GEN1_ENTRIES[] = {
    {  64,  65, 35, "Kadabra @ Lv35 -> Alakazam" },
    {  67,  68, 35, "Machoke @ Lv35 -> Machamp"  },
    {  93,  94, 35, "Haunter @ Lv35 -> Gengar"   },
    {  75,  76, 35, "Graveler @ Lv35 -> Golem"   },
};
#define GEN1_ENTRY_COUNT \
    ((int)(sizeof(GEN1_ENTRIES) / sizeof(GEN1_ENTRIES[0])))

static const GBEvolvePatchEntry GEN2_ENTRIES[] = {
    {  64,  65, 35, "Kadabra @ Lv35 -> Alakazam"   },
    {  67,  68, 35, "Machoke @ Lv35 -> Machamp"    },
    {  93,  94, 35, "Haunter @ Lv35 -> Gengar"     },
    {  75,  76, 35, "Graveler @ Lv35 -> Golem"     },
    {  95, 208, 35, "Onix @ Lv35 -> Steelix"       },
    { 123, 212, 35, "Scyther @ Lv35 -> Scizor"     },
    { 117, 230, 35, "Seadra @ Lv35 -> Kingdra"     },
    {  61, 186, 35, "Poliwhirl @ Lv35 -> Politoed (use Water Stone before Lv35 for Poliwrath)" },
    {  79, 199, 30, "Slowpoke @ Lv30 -> Slowking (catch wild Slowbro for the other branch)"   },
    { 137, 233, 35, "Porygon @ Lv35 -> Porygon2"   },
};
#define GEN2_ENTRY_COUNT \
    ((int)(sizeof(GEN2_ENTRIES) / sizeof(GEN2_ENTRIES[0])))

/* One backup record per patched ROM row. Sized to fit the larger
 * (Gen 2) table; Gen 1 only fills the first 4. */
typedef struct {
    size_t  rom_off;
    uint8_t saved[3];
    bool    active;
} PatchSlot;

static PatchSlot g_slots[GEN2_ENTRY_COUNT];
static int       g_slot_count = 0;
static bool      g_enabled = false;


/* Walk forward from `start` looking for an EVOLVE_TRADE row whose
 * target byte equals `target_byte`. The walker has to know that
 * Gen 1's EVOLVE_ITEM is 4 bytes wide (all other rows are 3) so it
 * doesn't desync. Returns the row's first-byte offset, or 0 on
 * end-of-evos / overflow. */
static size_t find_trade_row(const GBContext* ctx, size_t start,
                             int gen, uint8_t target_byte) {
    size_t off = start;
    int safety = 16;  /* species have <= 4 evo rows in vanilla */
    while (safety-- > 0 && off + 3 <= ctx->rom_size) {
        uint8_t op = ctx->rom[off];
        if (op == 0) return 0;          /* terminator -- no trade row here */
        if (op == OP_EVOLVE_TRADE && ctx->rom[off + 2] == target_byte) {
            return off;
        }
        if (gen == 1 && op == OP_EVOLVE_ITEM) off += 4;
        else                                  off += 3;
    }
    return 0;
}


bool gb_evolve_patch_is_supported(const GBContext* ctx) {
    return gb_mock_gen1_detect(ctx) != GB_MOCK_GEN1_NONE ||
           gb_mock_gen2_detect(ctx) != GB_MOCK_GEN2_NONE;
}

bool gb_evolve_patch_is_enabled(void) { return g_enabled; }

int gb_evolve_patch_list(const GBContext* ctx,
                         const GBEvolvePatchEntry** out_entries) {
    if (gb_mock_gen2_detect(ctx) != GB_MOCK_GEN2_NONE) {
        if (out_entries) *out_entries = GEN2_ENTRIES;
        return GEN2_ENTRY_COUNT;
    }
    if (gb_mock_gen1_detect(ctx) != GB_MOCK_GEN1_NONE) {
        if (out_entries) *out_entries = GEN1_ENTRIES;
        return GEN1_ENTRY_COUNT;
    }
    if (out_entries) *out_entries = NULL;
    return 0;
}

void gb_evolve_patch_set_enabled(GBContext* ctx, bool enable) {
    if (g_enabled == enable || !ctx || !ctx->rom) return;

    const GBEvolvePatchEntry* entries = NULL;
    int n = gb_evolve_patch_list(ctx, &entries);
    if (n == 0) return;
    int gen = (gb_mock_gen2_detect(ctx) != GB_MOCK_GEN2_NONE) ? 2 : 1;

    if (enable) {
        g_slot_count = 0;
        for (int i = 0; i < n; i++) {
            size_t record;
            int target_byte;
            if (gen == 1) {
                int internal = gb_mock_gen1_internal_id_for_dex(ctx,
                                       entries[i].from_dex);
                int target_internal = gb_mock_gen1_internal_id_for_dex(ctx,
                                       entries[i].to_dex);
                if (internal < 1 || target_internal < 1) continue;
                record = gb_mock_gen1_evos_record_offset(ctx, internal);
                target_byte = target_internal;
            } else {
                record = gb_mock_gen2_evos_record_offset(ctx,
                                       entries[i].from_dex);
                target_byte = entries[i].to_dex;
            }
            if (record == 0) continue;

            size_t row = find_trade_row(ctx, record, gen,
                                        (uint8_t)target_byte);
            if (row == 0) {
                fprintf(stderr, "[evolve_patch] no trade row for %s\n",
                        entries[i].label);
                continue;
            }

            PatchSlot* s = &g_slots[g_slot_count++];
            s->rom_off = row;
            memcpy(s->saved, &ctx->rom[row], 3);
            s->active = true;

            ctx->rom[row]     = OP_EVOLVE_LEVEL;
            ctx->rom[row + 1] = (uint8_t)entries[i].level;
            /* target byte at row+2 stays untouched - same byte
             * meaning in EVOLVE_LEVEL (internal_id Gen 1 / dex Gen 2). */
            fprintf(stderr, "[evolve_patch] %s patched at ROM $%06zX\n",
                    entries[i].label, row);
        }
        g_enabled = true;
    } else {
        for (int i = 0; i < g_slot_count; i++) {
            if (!g_slots[i].active) continue;
            memcpy(&ctx->rom[g_slots[i].rom_off], g_slots[i].saved, 3);
        }
        g_slot_count = 0;
        g_enabled = false;
        fprintf(stderr, "[evolve_patch] restored original bytes\n");
    }
}
