/**
 * @file mock_ir.h
 * @brief Scripted Mystery Gift mock for Pokemon Gen 2 (Gold / Silver / Crystal).
 *
 * The carts' IR-driven Mystery Gift exchange is not emulated bit-for-bit — we
 * intercept ExchangeMysteryGiftData at the recompiled-symbol level, fill the
 * partner's WRAM block with a chosen (or random) gift, and let the cart's
 * existing post-exchange flow deliver it as if a real partner had IR'd one
 * over.
 *
 * The host UI (Esc menu) chooses what gets delivered. Two carousels (item
 * list, decoration list) each have their own Send button. Pressing Send
 * queues the selection for the next cart-initiated Mystery Gift; if nothing
 * is queued when the cart asks, a uniform-random pick is rolled.
 *
 * 36 items + 36 decorations match the gift tables shared by Gold/Silver/
 * Crystal (data/items/mystery_gift_items.asm + data/decorations/
 * mystery_gift_decos.asm in the pret disassembly).
 *
 * The mock pretends to be a "reserved" partner (PartnerGameVersion = 4),
 * which makes the cart skip its trainer-house save path. That keeps the
 * mock simple: we only have to populate the 20-byte PartnerData block, not
 * the full trainer summary.
 */
#ifndef GBRT_MOCK_IR_H
#define GBRT_MOCK_IR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GBContext;

typedef enum {
    GB_MOCK_IR_GAME_NONE = 0,
    GB_MOCK_IR_GAME_GOLD,
    GB_MOCK_IR_GAME_SILVER,
    GB_MOCK_IR_GAME_CRYSTAL,
} GBMockIRGame;

typedef enum {
    GB_MOCK_IR_KIND_NONE = 0,  /* nothing queued — apply rolls a random gift */
    GB_MOCK_IR_KIND_ITEM,
    GB_MOCK_IR_KIND_DECO,
} GBMockIRKind;

#define GB_MOCK_IR_NUM_ITEMS 37
#define GB_MOCK_IR_NUM_DECOS 37

/* Display names for the carousel UI. Returns NULL if index out of range.
 * Indexes 0..36 match the cart's MysteryGiftItems / MysteryGiftDecos order. */
const char* gb_mock_ir_item_name(int index);
const char* gb_mock_ir_deco_name(int index);

/* Identify Gen 2 carts by 11-byte cart title at $0134. Returns
 * GB_MOCK_IR_GAME_NONE for everything else. */
GBMockIRGame gb_mock_ir_detect(const struct GBContext* ctx);

/* Esc-menu side: queue the next gift, or clear, or read what's queued. */
void        gb_mock_ir_queue(GBMockIRKind kind, int index);
void        gb_mock_ir_clear_queue(void);
void        gb_mock_ir_get_queue(GBMockIRKind* kind_out, int* index_out);
const char* gb_mock_ir_queue_label(void);  /* for the "Queued: ..." line */

/* Mystery Gift hook side: fill the partner's PartnerData block in WRAM. If
 * nothing is queued, rolls a random gift (~80% item, ~20% decoration to
 * mirror the real cart's RandomSample distribution). After applying, the
 * queue is cleared. Returns true on success. */
bool gb_mock_ir_apply_partner(struct GBContext* ctx, GBMockIRGame game);

#ifdef __cplusplus
}
#endif

#endif /* GBRT_MOCK_IR_H */
