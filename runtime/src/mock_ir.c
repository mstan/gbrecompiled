/*
 * See mock_ir.h. Item/deco tables, queue state, partner-WRAM writer.
 *
 * The 36-item and 36-deco lists below are in the same order as the cart's
 * MysteryGiftItems / MysteryGiftDecos tables — the cart looks up these by
 * index, so we write the index (0..35) directly into wMysteryGiftPartner
 * WhichItem / WhichDeco and let the cart resolve the actual byte/flag.
 */
#include "mock_ir.h"

#include "gbrt.h"

#include <stdlib.h>
#include <string.h>

static const char* const ITEM_NAMES[GB_MOCK_IR_NUM_ITEMS] = {
    "BERRY",         "PRZCUREBERRY",  "MINT BERRY",    "ICE BERRY",
    "BURNT BERRY",   "PSNCUREBERRY",  "GUARD SPEC.",   "X DEFEND",
    "X ATTACK",      "BITTER BERRY",  "DIRE HIT",      "X SPECIAL",
    "X ACCURACY",    "EON MAIL",      "MORPH MAIL",    "MUSIC MAIL",
    "MIRACLEBERRY",  "GOLD BERRY",    "REVIVE",        "GREAT BALL",
    "SUPER REPEL",   "MAX REPEL",     "ELIXER",        "ETHER",
    "WATER STONE",   "FIRE STONE",    "LEAF STONE",    "THUNDERSTONE",
    "MAX ETHER",     "MAX ELIXER",    "MAX REVIVE",    "SCOPE LENS",
    "HP UP",         "PP UP",         "RARE CANDY",    "BLUESKY MAIL",
    "MIRAGE MAIL",
};

static const char* const DECO_NAMES[GB_MOCK_IR_NUM_DECOS] = {
    "JIGGLYPUFF DOLL",   "POLIWAG DOLL",      "DIGLETT DOLL",
    "STARYU DOLL",       "MAGIKARP DOLL",     "ODDISH DOLL",
    "GENGAR DOLL",       "SHELLDER DOLL",     "GRIMER DOLL",
    "VOLTORB DOLL",      "CLEFAIRY POSTER",   "JIGGLYPUFF POSTER",
    "SNES",              "WEEDLE DOLL",       "GEODUDE DOLL",
    "MACHOP DOLL",       "MAGNAPLANT",        "TROPICPLANT",
    "FAMICOM",           "N64",               "BULBASAUR DOLL",
    "SQUIRTLE DOLL",     "PINK BED",          "POLKADOT BED",
    "RED CARPET",        "BLUE CARPET",       "YELLOW CARPET",
    "GREEN CARPET",      "JUMBOPLANT",        "VIRTUAL BOY",
    "BIG ONIX DOLL",     "PIKACHU POSTER",    "BIG LAPRAS DOLL",
    "SURF PIKACHU DOLL", "PIKACHU BED",       "UNOWN DOLL",
    "TENTACOOL DOLL",
};

/* Queue state — protected only by single-threaded access from the platform
 * main loop. The Esc-menu UI runs on the same thread that ticks the cart. */
static GBMockIRKind g_queue_kind = GB_MOCK_IR_KIND_NONE;
static int          g_queue_index = 0;
static char         g_queue_label[32] = "";

const char* gb_mock_ir_item_name(int index) {
    if (index < 0 || index >= GB_MOCK_IR_NUM_ITEMS) return NULL;
    return ITEM_NAMES[index];
}

const char* gb_mock_ir_deco_name(int index) {
    if (index < 0 || index >= GB_MOCK_IR_NUM_DECOS) return NULL;
    return DECO_NAMES[index];
}

GBMockIRGame gb_mock_ir_detect(const struct GBContext* ctx) {
    if (!ctx || !ctx->rom || ctx->rom_size < 0x144) return GB_MOCK_IR_GAME_NONE;
    const uint8_t* title = ctx->rom + 0x134;
    /* Cart title is up to 11 ASCII bytes, NUL-padded. */
    if (memcmp(title, "POKEMON_GLD", 11) == 0) return GB_MOCK_IR_GAME_GOLD;
    if (memcmp(title, "POKEMON_SLV", 11) == 0) return GB_MOCK_IR_GAME_SILVER;
    if (memcmp(title, "PM_CRYSTAL",  10) == 0) return GB_MOCK_IR_GAME_CRYSTAL;
    return GB_MOCK_IR_GAME_NONE;
}

static void update_queue_label(void) {
    switch (g_queue_kind) {
    case GB_MOCK_IR_KIND_ITEM: {
        const char* n = gb_mock_ir_item_name(g_queue_index);
        snprintf(g_queue_label, sizeof(g_queue_label), "%s", n ? n : "?");
        break;
    }
    case GB_MOCK_IR_KIND_DECO: {
        const char* n = gb_mock_ir_deco_name(g_queue_index);
        snprintf(g_queue_label, sizeof(g_queue_label), "%s", n ? n : "?");
        break;
    }
    default:
        snprintf(g_queue_label, sizeof(g_queue_label), "(none — random)");
        break;
    }
}

void gb_mock_ir_queue(GBMockIRKind kind, int index) {
    int max =
        (kind == GB_MOCK_IR_KIND_ITEM) ? GB_MOCK_IR_NUM_ITEMS :
        (kind == GB_MOCK_IR_KIND_DECO) ? GB_MOCK_IR_NUM_DECOS : 0;
    if (index < 0 || index >= max) {
        gb_mock_ir_clear_queue();
        return;
    }
    g_queue_kind = kind;
    g_queue_index = index;
    update_queue_label();
}

void gb_mock_ir_clear_queue(void) {
    g_queue_kind = GB_MOCK_IR_KIND_NONE;
    g_queue_index = 0;
    update_queue_label();
}

void gb_mock_ir_get_queue(GBMockIRKind* kind_out, int* index_out) {
    if (kind_out) *kind_out = g_queue_kind;
    if (index_out) *index_out = g_queue_index;
}

const char* gb_mock_ir_queue_label(void) {
    if (g_queue_label[0] == '\0') update_queue_label();
    return g_queue_label;
}

/* Per-cart base address of wMysteryGiftPartnerData. Layout is identical
 * across the three Gen 2 carts; only the WRAM offset differs. */
static uint16_t partner_data_addr(GBMockIRGame game) {
    switch (game) {
    case GB_MOCK_IR_GAME_GOLD:
    case GB_MOCK_IR_GAME_SILVER:  return 0xC800;
    case GB_MOCK_IR_GAME_CRYSTAL: return 0xC900;
    default:                      return 0;
    }
}

/* Pokemon character set: A..Z = $80..$99, space = $7F, terminator = $50. */
static uint8_t poketext_byte(char c) {
    if (c >= 'A' && c <= 'Z') return (uint8_t)(0x80 + (c - 'A'));
    if (c >= 'a' && c <= 'z') return (uint8_t)(0x80 + (c - 'a'));
    if (c == ' ')             return 0x7F;
    return 0x7F;
}

static void write_poketext_name(struct GBContext* ctx, uint16_t addr, const char* name) {
    /* NAME_LENGTH = 11: up to 10 chars then 0x50 terminator (matches cart's
     * fixed-width layout). */
    int i = 0;
    while (i < 10 && name[i]) {
        gb_write8(ctx, (uint16_t)(addr + i), poketext_byte(name[i]));
        i++;
    }
    /* Pad rest with $50 (terminator) — cart treats first $50 as end-of-string. */
    while (i < 11) {
        gb_write8(ctx, (uint16_t)(addr + i), 0x50);
        i++;
    }
}

#define MG_PARTNER_VERSION_RESERVED 4   /* RESERVED_GAME_VERSION — skip trainer-house save */

static const char* const FAKE_PARTNERS[] = {
    "OAK", "RED", "BLUE", "GREEN", "ETHAN", "KRIS", "LYRA",
    "GOLD", "SILVER", "MISTY", "BROCK", "ERIKA", "JASMINE",
};
#define FAKE_PARTNER_COUNT ((int)(sizeof(FAKE_PARTNERS) / sizeof(FAKE_PARTNERS[0])))

bool gb_mock_ir_apply_partner(struct GBContext* ctx, GBMockIRGame game) {
    uint16_t base = partner_data_addr(game);
    if (!ctx || base == 0) return false;

    /* If nothing's queued, roll a random gift. ~80% item / ~20% deco
     * matches the real cart's RandomSample distribution roughly. */
    GBMockIRKind kind = g_queue_kind;
    int index = g_queue_index;
    if (kind == GB_MOCK_IR_KIND_NONE) {
        if ((rand() % 100) < 80) {
            kind = GB_MOCK_IR_KIND_ITEM;
            index = rand() % GB_MOCK_IR_NUM_ITEMS;
        } else {
            kind = GB_MOCK_IR_KIND_DECO;
            index = rand() % GB_MOCK_IR_NUM_DECOS;
        }
    }

    /* +0  GameVersion — RESERVED so the cart skips its trainer-house save. */
    gb_write8(ctx, (uint16_t)(base + 0), MG_PARTNER_VERSION_RESERVED);
    /* +1..+2 PartnerID — random 16-bit. */
    uint16_t fake_id = (uint16_t)((rand() & 0xFFFF));
    gb_write8(ctx, (uint16_t)(base + 1), (uint8_t)(fake_id & 0xFF));
    gb_write8(ctx, (uint16_t)(base + 2), (uint8_t)((fake_id >> 8) & 0xFF));
    /* +3..+13 PartnerName (11 bytes incl. terminator) */
    const char* name = FAKE_PARTNERS[rand() % FAKE_PARTNER_COUNT];
    write_poketext_name(ctx, (uint16_t)(base + 3), name);
    /* +14 DexCaught — believable mid-game count. */
    gb_write8(ctx, (uint16_t)(base + 14), 30);
    /* +15 SentDeco: 0=item, 1=decoration. */
    gb_write8(ctx, (uint16_t)(base + 15), (kind == GB_MOCK_IR_KIND_DECO) ? 1 : 0);
    /* +16 WhichItem (index into MysteryGiftItems). */
    gb_write8(ctx, (uint16_t)(base + 16),
              (kind == GB_MOCK_IR_KIND_ITEM) ? (uint8_t)index : 0);
    /* +17 WhichDeco (index into MysteryGiftDecos). */
    gb_write8(ctx, (uint16_t)(base + 17),
              (kind == GB_MOCK_IR_KIND_DECO) ? (uint8_t)index : 0);
    /* +18 BackupItem — 0 means partner is ready (not "FriendNotReady"). */
    gb_write8(ctx, (uint16_t)(base + 18), 0);
    /* +19 reserved padding */
    gb_write8(ctx, (uint16_t)(base + 19), 0);

    gb_mock_ir_clear_queue();
    return true;
}
