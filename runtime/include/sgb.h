/**
 * @file sgb.h
 * @brief Super GameBoy emulation: packet receiver and JOYP-side handshake.
 *
 * The SGB sits between the GB CPU and the SNES, watching writes to JOYP
 * (FF00) for a bit-encoded packet protocol. The CPU sends 16-byte packets
 * (up to 7 packets per command sequence) by pulsing P14/P15:
 *
 *   write 0x00 -> RESET (begin packet)
 *   write 0x10 -> bit 0 pulse
 *   write 0x20 -> bit 1 pulse
 *   write 0x30 -> idle / clock-high (between bits, and after packet)
 *
 * After a complete sequence, the SGB acts on the command. The most visible
 * commands are border, palette, and per-tile attribute control.
 *
 * M1 scope: receive packets, recognize MLT_REQ, and bend the JOYP read
 * result so the game's CheckSGB routine succeeds.
 */
#ifndef GB_SGB_H
#define GB_SGB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBContext GBContext;
typedef struct GBSgbState GBSgbState;

GBSgbState* gb_sgb_create(void);
void        gb_sgb_destroy(GBSgbState* sgb);

/** Reset all packet state. Call when the cart is (re)loaded. */
void gb_sgb_reset(GBSgbState* sgb);

/**
 * Whether the cart's header advertises SGB features (cart byte 0x146 == 0x03).
 * Used by the platform to decide whether to enable the engine.
 */
bool gb_sgb_cart_supports(const uint8_t* rom, size_t rom_size);

/** Enable / disable the SGB engine at runtime. The engine has two layers:
 *
 *   1. ENGINE — packet RX, MLT_REQ probe response, palette/border state
 *      accumulation. Driven by `enabled`. The cart's CheckSGB sees the
 *      engine via the JOYP read shim, so this must be on for `wOnSGB`
 *      to latch to 1 at boot. Once on, leave it on.
 *
 *   2. DISPLAY — split into two independent toggles so the user can pick
 *      either, both, or neither without losing the engine-side state:
 *
 *        a. display_palettes — applies captured palette + attribute
 *           packets to the framebuffer (SGB region tints).
 *        b. display_border   — renders the cart's authored SGB border
 *           around the game viewport.
 *
 *      Both default true once the engine is active. Free to flip at any
 *      time without disturbing what the cart has loaded.
 *
 * Splitting engine from display lets a user toggle SGB visuals on/off
 * mid-game without losing the per-scene palette updates Pokemon sends
 * only when wOnSGB=1. Splitting palettes from border further lets users
 * keep the cart border without the SGB color tint, or vice versa.
 */
void gb_sgb_set_enabled(GBSgbState* sgb, bool enabled);
bool gb_sgb_is_enabled(const GBSgbState* sgb);

void gb_sgb_set_display_palettes(GBSgbState* sgb, bool active);
bool gb_sgb_is_display_palettes(const GBSgbState* sgb);

void gb_sgb_set_display_border(GBSgbState* sgb, bool active);
bool gb_sgb_is_display_border(const GBSgbState* sgb);

/**
 * Hook called from gb_write8 whenever the CPU writes JOYP (FF00). Drives
 * the bit state machine and dispatches completed packets. Cheap when SGB
 * is disabled (early-out).
 */
void gb_sgb_on_joyp_write(GBContext* ctx, uint8_t value);

/**
 * Modify the result of a JOYP read in light of recent SGB packet activity.
 * `base` is what the existing JOYP-read logic would have returned. Returns
 * the value to pass back to the CPU. When SGB is disabled, returns `base`
 * unchanged.
 */
uint8_t gb_sgb_modify_joyp_read(GBContext* ctx, uint8_t base);

/** Diagnostics: number of packets seen since reset (any command). */
uint32_t gb_sgb_packet_count(const GBSgbState* sgb);

/** Diagnostics: most recently received command byte (0..0x1F). */
uint8_t gb_sgb_last_command(const GBSgbState* sgb);

/**
 * Apply the active SGB palettes + per-tile attribute grid to the PPU's
 * framebuffer. Call once per frame, after the PPU has finished rendering
 * scanlines and before the runtime converts color_framebuffer to RGBA.
 *
 * Reads ppu->framebuffer (uint8 0..3 shade per pixel) and writes
 * ppu->color_framebuffer (uint16 RGB555 per pixel). No-op when SGB is
 * disabled or no palette command has arrived yet.
 */
void gb_sgb_apply_to_frame(GBContext* ctx);

/* SGB cart border dimensions. */
#define GB_SGB_BORDER_W 256
#define GB_SGB_BORDER_H 224

/**
 * True once both CHR_TRN (border tile graphics) and PCT_TRN (border
 * tilemap + palettes) have been received, so a complete border can be
 * rendered.
 */
bool gb_sgb_border_ready(const GBSgbState* sgb);

/**
 * Render the SGB cart border into a 256x224 RGBA8888 buffer. The center
 * 160x144 region is left fully transparent (alpha=0) so callers can blit
 * the live game frame underneath. Returns false if the border isn't
 * ready yet.
 */
bool gb_sgb_render_border(const GBSgbState* sgb, uint32_t* out_rgba);

/**
 * Frame number incremented every time CHR_TRN or PCT_TRN updates the
 * border. Platform code can poll this to know when to rebuild any
 * cached border texture without keeping a stale copy around.
 */
uint32_t gb_sgb_border_revision(const GBSgbState* sgb);

/**
 * MASK_EN state from the most recent packet:
 *   0 = mask off (normal display)
 *   1 = freeze on last live frame
 *   2 = blank to black
 *   3 = blank to color 0 of palette 0
 */
uint8_t gb_sgb_mask_mode(const GBSgbState* sgb);

#ifdef __cplusplus
}
#endif

#endif /* GB_SGB_H */
