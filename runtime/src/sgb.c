/*
 * See sgb.h. M1: packet bit state machine + MLT_REQ handshake so Pokemon
 * (and any other SGB-aware DMG game) flips its `wOnSGB` flag.
 */
#include "sgb.h"
#include "gbrt.h"   /* for GBContext layout (ctx->sgb opaque pointer) */
#include "ppu.h"    /* for GBPPU framebuffer layout */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SGB_PACKET_BYTES   16
#define SGB_MAX_PACKETS    7

/* Visible SGB attribute grid spans 20x18 8x8 tiles (= 160x144 / 8). */
#define SGB_ATTR_W   20
#define SGB_ATTR_H   18
#define SGB_ATTR_COUNT (SGB_ATTR_W * SGB_ATTR_H)

/* SGB command IDs we care about for the full feature set. M1 only uses
 * MLT_REQ, but the dispatch table is laid out so later milestones can
 * fill the rest in. */
enum {
    SGB_CMD_PAL01     = 0x00,
    SGB_CMD_PAL23     = 0x01,
    SGB_CMD_PAL03     = 0x02,
    SGB_CMD_PAL12     = 0x03,
    SGB_CMD_ATTR_BLK  = 0x04,
    SGB_CMD_ATTR_LIN  = 0x05,
    SGB_CMD_ATTR_DIV  = 0x06,
    SGB_CMD_ATTR_CHR  = 0x07,
    SGB_CMD_PAL_SET   = 0x0A,
    SGB_CMD_PAL_TRN   = 0x0B,
    SGB_CMD_MLT_REQ   = 0x11,
    SGB_CMD_CHR_TRN   = 0x13,
    SGB_CMD_PCT_TRN   = 0x14,
    SGB_CMD_ATTR_TRN  = 0x15,
    SGB_CMD_ATTR_SET  = 0x16,
    SGB_CMD_MASK_EN   = 0x17,
};

typedef enum {
    SGB_RX_IDLE,        /* waiting for a reset pulse */
    SGB_RX_RECEIVING,   /* in the middle of a 128-bit packet */
} GBSgbRxState;

struct GBSgbState {
    bool enabled;

    /* Last value written to JOYP (top two bits, the SGB-relevant nibble).
     * Used to detect bit-pulse transitions: each "active" pulse (0x10 or
     * 0x20) is clocked in when the *next* write returns to 0x30. */
    uint8_t  prev_joyp;

    GBSgbRxState rx_state;
    uint8_t  rx_packet[SGB_PACKET_BYTES];
    uint8_t  rx_bit_index;          /* 0..127 */

    /* Multi-packet sequence buffering. The first byte of the first packet
     * holds the command (top 5 bits) and the number of packets in the
     * sequence (low 3 bits, 1..7). */
    uint8_t  sequence_packets[SGB_MAX_PACKETS][SGB_PACKET_BYTES];
    uint8_t  sequence_total;        /* expected packet count from packet 0 */
    uint8_t  sequence_received;     /* packets received so far */

    /* MLT_REQ bookkeeping: number of controllers requested (1, 2, or 4)
     * and a small read-side counter so JOYP reads cycle the controller
     * index nibble like a real SGB. M1 only needs `players >= 2` for the
     * detection check; full rotation will get refined in M3. */
    uint8_t  mlt_req_players;       /* 1, 2, or 4 — 1 means MLT_REQ inactive */
    uint8_t  mlt_req_index;         /* current controller index, low 2 bits */

    /* Active palettes (4 palettes × 4 colors, RGB555 little-endian). The
     * SGB shares color 0 across all four palettes — we mirror that on
     * every install so games that rely on a unified backdrop don't see
     * stripes between attribute regions. */
    uint16_t palette[4][4];

    /* SGB system palette table. PAL_TRN copies 4096 bytes from VRAM
     * 0x8000-0x8FFF here; that's 512 palettes × 4 colors × 2 bytes
     * RGB555. PAL_SET later picks 4 of those into the active set. */
    uint8_t  sys_palettes[4096];
    bool     sys_palettes_loaded;

    /* Per-tile attribute grid: which palette (0-3) each visible 8x8
     * cell uses. Default = 0 so an un-styled game still renders. */
    uint8_t  attribute_grid[SGB_ATTR_COUNT];

    /* True once any PAL/ATTR command has arrived. Suppresses the recolor
     * pass before the game has had a chance to set up its palettes,
     * leaving the PPU's RGB output untouched for those early frames. */
    bool     palettes_active;

    /* Cart border (CHR_TRN tile graphics + PCT_TRN tilemap+palettes).
     * Tiles are 4bpp, 32 bytes each; CHR_TRN delivers 4 KiB = 128 tiles
     * (tiles 0-127 with byte1 bit 0 == 0; pokered only sends this set).
     * The tilemap is 32x32 16-bit entries and the four border palettes
     * follow at offset 0x800 in the 4 KiB PCT_TRN payload. */
    uint8_t  border_chr[4096];
    uint8_t  border_tilemap[2048];
    uint16_t border_palette[4][16];
    bool     border_chr_loaded;
    bool     border_pct_loaded;
    uint32_t border_revision;     /* bumps on every CHR_TRN/PCT_TRN */

    /* MASK_EN: 0 off, 1 freeze on last frame, 2 black, 3 backdrop. */
    uint8_t  mask_mode;
    /* Snapshot of the last live color_framebuffer, used when mask_mode
     * is 1 (freeze). Allocated lazily on first non-zero mask. */
    uint16_t* freeze_buffer;

    /* Diagnostics. */
    uint32_t packet_count;
    uint8_t  last_command;
};

GBSgbState* gb_sgb_create(void) {
    GBSgbState* sgb = (GBSgbState*)calloc(1, sizeof(GBSgbState));
    if (!sgb) return NULL;
    gb_sgb_reset(sgb);
    return sgb;
}

void gb_sgb_destroy(GBSgbState* sgb) {
    if (!sgb) return;
    free(sgb->freeze_buffer);
    free(sgb);
}

void gb_sgb_reset(GBSgbState* sgb) {
    if (!sgb) return;
    bool was_enabled = sgb->enabled;
    free(sgb->freeze_buffer);
    memset(sgb, 0, sizeof(*sgb));
    sgb->enabled = was_enabled;
    sgb->rx_state = SGB_RX_IDLE;
    sgb->prev_joyp = 0x30;
    sgb->mlt_req_players = 1;
    sgb->mlt_req_index = 0;
}

bool gb_sgb_cart_supports(const uint8_t* rom, size_t rom_size) {
    /* Cart header byte 0x146 == 0x03 → game uses SGB functions. */
    if (!rom || rom_size <= 0x146) return false;
    return rom[0x146] == 0x03;
}

void gb_sgb_set_enabled(GBSgbState* sgb, bool enabled) {
    if (!sgb) return;
    if (sgb->enabled == enabled) return;
    sgb->enabled = enabled;
    if (enabled) {
        fprintf(stderr, "[SGB] enabled\n");
    }
    sgb->rx_state = SGB_RX_IDLE;
    sgb->prev_joyp = 0x30;
    sgb->rx_bit_index = 0;
    sgb->sequence_received = 0;
    sgb->sequence_total = 0;
    sgb->mlt_req_players = 1;
    sgb->mlt_req_index = 0;
}

bool gb_sgb_is_enabled(const GBSgbState* sgb) {
    return sgb && sgb->enabled;
}

uint32_t gb_sgb_packet_count(const GBSgbState* sgb) {
    return sgb ? sgb->packet_count : 0;
}

uint8_t gb_sgb_last_command(const GBSgbState* sgb) {
    return sgb ? sgb->last_command : 0;
}

/* ---------------- packet dispatch ---------------- */

static void sgb_handle_mlt_req(GBSgbState* sgb, const uint8_t* packet) {
    /* Byte 1 low 2 bits encode the controller count: 0=>1, 1=>2, 3=>4.
     * A value of 2 is undefined; treat as 1. */
    uint8_t code = (uint8_t)(packet[1] & 0x03);
    uint8_t players = 1;
    if (code == 0x01) players = 2;
    else if (code == 0x03) players = 4;
    sgb->mlt_req_players = players;
    sgb->mlt_req_index = 0;
}

/* ---------------- palette / attribute commands ---------------- */

/* PAL_TRN: copy 4 KiB of palette data the CPU staged at VRAM 0x8800
 * (vChars1 / "Tile Bank 1") into our system palette table. Per Pan Docs
 * the SGB reads from $8800-$97FF (the second tile data block); pokered's
 * CopyGfxToSuperNintendoVRAM writes its source bytes to vChars1 = $8800
 * before sending the packet. */
static void sgb_handle_pal_trn(GBSgbState* sgb, GBContext* ctx) {
    if (!ctx || !ctx->vram) return;
    memcpy(sgb->sys_palettes, ctx->vram + 0x0800, sizeof(sgb->sys_palettes));
    sgb->sys_palettes_loaded = true;
}

static void sgb_install_system_palette(GBSgbState* sgb, int active_slot,
                                       uint16_t sys_index) {
    if (!sgb->sys_palettes_loaded) return;
    if (active_slot < 0 || active_slot > 3) return;
    if (sys_index >= 512) sys_index = 0;
    const uint8_t* src = &sgb->sys_palettes[sys_index * 8];
    for (int c = 0; c < 4; c++) {
        sgb->palette[active_slot][c] =
            (uint16_t)(src[c * 2] | (src[c * 2 + 1] << 8));
    }
}

/* PAL_SET: 1 packet selects 4 of the 512 system palettes. Bytes 1-2
 * are the index for active palette 0, 3-4 for palette 1, etc. (16-bit
 * little-endian). Byte 9 has flags (use ATTR_FILE etc.) that we ignore
 * for now. */
static void sgb_handle_pal_set(GBSgbState* sgb, const uint8_t* p) {
    for (int i = 0; i < 4; i++) {
        uint16_t idx = (uint16_t)(p[1 + i * 2] | (p[2 + i * 2] << 8));
        sgb_install_system_palette(sgb, i, idx);
    }
    /* Color 0 is shared across all four active palettes on real SGB. */
    uint16_t shared = sgb->palette[0][0];
    for (int i = 1; i < 4; i++) sgb->palette[i][0] = shared;
    sgb->palettes_active = true;
}

/* Helper for ATTR_BLK: clamp tile coords to the visible grid. */
static int sgb_clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void sgb_attr_set(GBSgbState* sgb, int tx, int ty, uint8_t pal) {
    if (tx < 0 || tx >= SGB_ATTR_W || ty < 0 || ty >= SGB_ATTR_H) return;
    sgb->attribute_grid[ty * SGB_ATTR_W + tx] = (uint8_t)(pal & 0x03);
}

/* ATTR_BLK: 1-7 packets, each with up to 18 data sets. byte[1] of
 * packet 0 is the data set count; sets pack 6 bytes each starting at
 * byte 2 and continue into subsequent packets. Each set: control bits
 * (which regions to recolor), palette nibble (inside/border/outside),
 * x1, y1, x2, y2. */
static void sgb_handle_attr_blk(GBSgbState* sgb, const uint8_t (*packets)[SGB_PACKET_BYTES],
                                uint8_t packet_count) {
    if (packet_count == 0) return;
    uint8_t set_count = packets[0][1];
    if (set_count == 0) return;

    /* Walk packed bytes starting at packet 0 byte 2, then continuing
     * straight through to packet N byte 0. Each set is 6 bytes. */
    uint8_t flat[SGB_MAX_PACKETS * SGB_PACKET_BYTES];
    size_t flat_size = 0;
    /* skip command byte and set count byte */
    for (int i = 2; i < SGB_PACKET_BYTES; i++) flat[flat_size++] = packets[0][i];
    for (int p = 1; p < packet_count; p++) {
        for (int i = 0; i < SGB_PACKET_BYTES; i++) flat[flat_size++] = packets[p][i];
    }

    for (int s = 0; s < set_count; s++) {
        size_t off = (size_t)s * 6;
        if (off + 6 > flat_size) break;
        uint8_t ctrl = (uint8_t)(flat[off] & 0x07);
        uint8_t pals = flat[off + 1];
        int x1 = sgb_clamp(flat[off + 2], 0, SGB_ATTR_W - 1);
        int y1 = sgb_clamp(flat[off + 3], 0, SGB_ATTR_H - 1);
        int x2 = sgb_clamp(flat[off + 4], 0, SGB_ATTR_W - 1);
        int y2 = sgb_clamp(flat[off + 5], 0, SGB_ATTR_H - 1);
        if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
        if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }

        uint8_t pal_inside  = (uint8_t)((pals >> 0) & 0x03);
        uint8_t pal_border  = (uint8_t)((pals >> 2) & 0x03);
        uint8_t pal_outside = (uint8_t)((pals >> 4) & 0x03);

        bool change_inside  = (ctrl & 0x01) != 0;
        bool change_border  = (ctrl & 0x02) != 0;
        bool change_outside = (ctrl & 0x04) != 0;

        /* When the docs say "inside", they include any cell that's
         * strictly inside the rectangle. "Border" is the outline of
         * the rectangle. With both bits set the whole rect is one
         * palette — the common case in pokered's BlkPacket_*. */
        for (int y = 0; y < SGB_ATTR_H; y++) {
            for (int x = 0; x < SGB_ATTR_W; x++) {
                bool in_rect = (x >= x1 && x <= x2 && y >= y1 && y <= y2);
                bool on_border = in_rect && (x == x1 || x == x2 ||
                                             y == y1 || y == y2);
                if (in_rect && !on_border) {
                    if (change_inside) sgb_attr_set(sgb, x, y, pal_inside);
                } else if (on_border) {
                    if (change_border) sgb_attr_set(sgb, x, y, pal_border);
                } else {
                    if (change_outside) sgb_attr_set(sgb, x, y, pal_outside);
                }
            }
        }
    }
    sgb->palettes_active = true;
}

/* CHR_TRN: copies 4 KiB of border tile graphics from VRAM 0x8800.
 * byte1 bit 0 selects 0..127 vs 128..255 and bit 1 selects bg vs obj
 * tiles. pokered only sends bg tiles 0..127, so 4 KiB is enough. */
static void sgb_handle_chr_trn(GBSgbState* sgb, GBContext* ctx, const uint8_t* p) {
    if (!ctx || !ctx->vram) return;
    if ((p[1] & 0x03) != 0) return;
    memcpy(sgb->border_chr, ctx->vram + 0x0800, sizeof(sgb->border_chr));
    sgb->border_chr_loaded = true;
    sgb->border_revision++;
}

/* PCT_TRN: 4 KiB from VRAM 0x8800. First 2 KiB is the 32x32 tilemap;
 * 128 bytes at offset 0x800 hold the four border palettes (4 × 16
 * colors × 2 bytes RGB555). Anything past 0x880 is padding. */
static void sgb_handle_pct_trn(GBSgbState* sgb, GBContext* ctx) {
    if (!ctx || !ctx->vram) return;
    memcpy(sgb->border_tilemap, ctx->vram + 0x0800, sizeof(sgb->border_tilemap));
    const uint8_t* pal_src = ctx->vram + 0x0800 + 0x800;
    /* Pokemon Blue leaves PAL_SGB1[0] as (0,0,0) and notes in its source
     * "the first color is not defined, but if used, turns up as 30,29,29"
     * — i.e. real SGB falls back to a default backdrop when the cart
     * didn't bother to set color 0. Pokemon Yellow on the other hand
     * uses color 0 as a real fill (e.g. (24,6,6) red on its title-screen
     * border), so we can't unconditionally override. Apply the default
     * only when the cart actually wrote zeros. */
    const uint16_t border_default_color0 =
        (uint16_t)(30u | (29u << 5) | (29u << 10));
    for (int pi = 0; pi < 4; pi++) {
        for (int c = 0; c < 16; c++) {
            int off = pi * 32 + c * 2;
            sgb->border_palette[pi][c] =
                (uint16_t)(pal_src[off] | (pal_src[off + 1] << 8));
        }
        if (sgb->border_palette[pi][0] == 0) {
            sgb->border_palette[pi][0] = border_default_color0;
        }
    }
    sgb->border_pct_loaded = true;
    sgb->border_revision++;
}

static void sgb_handle_mask_en(GBSgbState* sgb, const uint8_t* p) {
    sgb->mask_mode = (uint8_t)(p[1] & 0x03);
}

static void sgb_dispatch_sequence(GBSgbState* sgb, GBContext* ctx) {
    const uint8_t* p0 = sgb->sequence_packets[0];
    uint8_t cmd = (uint8_t)(p0[0] >> 3);
    sgb->last_command = cmd;
    sgb->packet_count++;

    switch (cmd) {
        case SGB_CMD_MLT_REQ:
            sgb_handle_mlt_req(sgb, p0);
            break;
        case SGB_CMD_PAL_TRN:
            sgb_handle_pal_trn(sgb, ctx);
            break;
        case SGB_CMD_PAL_SET:
            sgb_handle_pal_set(sgb, p0);
            break;
        case SGB_CMD_ATTR_BLK:
            sgb_handle_attr_blk(sgb, sgb->sequence_packets,
                                sgb->sequence_total);
            break;
        case SGB_CMD_CHR_TRN:
            sgb_handle_chr_trn(sgb, ctx, p0);
            break;
        case SGB_CMD_PCT_TRN:
            sgb_handle_pct_trn(sgb, ctx);
            break;
        case SGB_CMD_MASK_EN:
            sgb_handle_mask_en(sgb, p0);
            break;
        /* PAL01/23/03/12 inline palette commands and the rarer ATTR_*
         * variants (LIN/DIV/CHR/SET/TRN) stay unhandled — pokered does
         * not reach for any of them. */
        default:
            break;
    }
}

static void sgb_record_bit(GBSgbState* sgb, GBContext* ctx, uint8_t bit) {
    if (sgb->rx_bit_index >= 128) return;
    /* Pokemon's SendSGBPacket sends each byte LSB-first via "rr d" after
     * testing bit 0, so the first bit on the wire is byte 0 bit 0. */
    uint8_t byte = (uint8_t)(sgb->rx_bit_index >> 3);
    uint8_t shift = (uint8_t)(sgb->rx_bit_index & 7);
    if (bit) {
        sgb->rx_packet[byte] |= (uint8_t)(1u << shift);
    }
    sgb->rx_bit_index++;

    if (sgb->rx_bit_index == 128) {
        /* Packet complete. Stash it; if it's the first of a sequence, the
         * top-byte's low 3 bits tell us how many packets to expect. */
        if (sgb->sequence_received < SGB_MAX_PACKETS) {
            memcpy(sgb->sequence_packets[sgb->sequence_received],
                   sgb->rx_packet, SGB_PACKET_BYTES);
        }
        if (sgb->sequence_received == 0) {
            uint8_t total = (uint8_t)(sgb->rx_packet[0] & 0x07);
            if (total == 0) total = 1;
            sgb->sequence_total = total;
        }
        sgb->sequence_received++;

        if (sgb->sequence_received >= sgb->sequence_total) {
            sgb_dispatch_sequence(sgb, ctx);
            sgb->sequence_received = 0;
            sgb->sequence_total = 0;
        }

        sgb->rx_state = SGB_RX_IDLE;
        sgb->rx_bit_index = 0;
        memset(sgb->rx_packet, 0, sizeof(sgb->rx_packet));
    }
}

/* ---------------- JOYP write hook ---------------- */

void gb_sgb_on_joyp_write(GBContext* ctx, uint8_t value) {
    if (!ctx) return;
    GBSgbState* sgb = (GBSgbState*)ctx->sgb;
    if (!gb_sgb_is_enabled(sgb)) return;

    uint8_t hi = (uint8_t)(value & 0x30);

    /* Reset pulse: both lines low. Always begins a new packet, even mid-bit. */
    if (hi == 0x00) {
        sgb->rx_state = SGB_RX_RECEIVING;
        sgb->rx_bit_index = 0;
        memset(sgb->rx_packet, 0, sizeof(sgb->rx_packet));
        sgb->prev_joyp = hi;
        return;
    }

    /* A bit is clocked in when the line returns from a "data" pulse back
     * to 0x30 (idle). Per the SGB JOYP protocol (cf. pokered's
     * hardware.inc): JOYP_SGB_ONE = 0x10 (P15 low, sending bit 1),
     * JOYP_SGB_ZERO = 0x20 (P14 low, sending bit 0). The pulse direction
     * is the opposite of what the bit names alone might suggest. */
    if (sgb->rx_state == SGB_RX_RECEIVING && hi == 0x30) {
        if (sgb->prev_joyp == 0x10) {
            sgb_record_bit(sgb, ctx, 1);
        } else if (sgb->prev_joyp == 0x20) {
            sgb_record_bit(sgb, ctx, 0);
        }
    }

    sgb->prev_joyp = hi;
}

/* ---------------- recolor pass ---------------- */

static uint16_t* sgb_freeze_buffer(GBSgbState* sgb) {
    if (!sgb->freeze_buffer) {
        sgb->freeze_buffer = (uint16_t*)calloc(GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT,
                                               sizeof(uint16_t));
    }
    return sgb->freeze_buffer;
}

void gb_sgb_apply_to_frame(GBContext* ctx) {
    if (!ctx) return;
    GBSgbState* sgb = (GBSgbState*)ctx->sgb;
    if (!gb_sgb_is_enabled(sgb)) return;
    if (!sgb->palettes_active) return;

    GBPPU* ppu = (GBPPU*)ctx->ppu;
    if (!ppu) return;

    /* MASK_EN handling. Mode 1 (freeze) replays the last live frame so the
     * player doesn't see the LCD-off white flash that the GB shows during
     * the cart's VRAM transfer windows. */
    if (sgb->mask_mode == 1) {
        uint16_t* fz = sgb_freeze_buffer(sgb);
        if (fz) {
            memcpy(ppu->color_framebuffer, fz,
                   sizeof(uint16_t) * GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT);
            return;
        }
        /* fallthrough to recolor if alloc failed */
    } else if (sgb->mask_mode == 2) {
        memset(ppu->color_framebuffer, 0,
               sizeof(uint16_t) * GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT);
        return;
    } else if (sgb->mask_mode == 3) {
        uint16_t color0 = sgb->palette[0][0];
        for (int i = 0; i < GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT; i++) {
            ppu->color_framebuffer[i] = color0;
        }
        return;
    }

    for (int y = 0; y < GB_SCREEN_HEIGHT; y++) {
        int ty = y >> 3;
        if (ty >= SGB_ATTR_H) ty = SGB_ATTR_H - 1;
        const uint8_t* row_attr = &sgb->attribute_grid[ty * SGB_ATTR_W];
        const uint8_t* fb_row = &ppu->framebuffer[y * GB_SCREEN_WIDTH];
        uint16_t* out_row = &ppu->color_framebuffer[y * GB_SCREEN_WIDTH];

        for (int x = 0; x < GB_SCREEN_WIDTH; x++) {
            int tx = x >> 3;
            if (tx >= SGB_ATTR_W) tx = SGB_ATTR_W - 1;
            uint8_t pal_idx = row_attr[tx];
            uint8_t shade = (uint8_t)(fb_row[x] & 0x03);
            out_row[x] = sgb->palette[pal_idx][shade];
        }
    }

    /* Snapshot for the next freeze. */
    uint16_t* fz = sgb_freeze_buffer(sgb);
    if (fz) {
        memcpy(fz, ppu->color_framebuffer,
               sizeof(uint16_t) * GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT);
    }
}

/* ---------------- border decode ---------------- */

bool gb_sgb_border_ready(const GBSgbState* sgb) {
    return sgb && sgb->border_chr_loaded && sgb->border_pct_loaded;
}

uint32_t gb_sgb_border_revision(const GBSgbState* sgb) {
    return sgb ? sgb->border_revision : 0;
}

uint8_t gb_sgb_mask_mode(const GBSgbState* sgb) {
    return sgb ? sgb->mask_mode : 0;
}

static uint32_t sgb_rgb555_to_rgba(uint16_t c) {
    /* RGB555 little-endian → RGBA8888. 5-bit channels expanded to 8 bits
     * via the standard "duplicate top 3 bits into low 3" trick. */
    uint8_t r5 = (uint8_t)(c & 0x1F);
    uint8_t g5 = (uint8_t)((c >> 5) & 0x1F);
    uint8_t b5 = (uint8_t)((c >> 10) & 0x1F);
    uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
    uint8_t g = (uint8_t)((g5 << 3) | (g5 >> 2));
    uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));
    return (uint32_t)0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

bool gb_sgb_render_border(const GBSgbState* sgb, uint32_t* out_rgba) {
    if (!gb_sgb_border_ready(sgb) || !out_rgba) return false;

    /* The visible 160x144 GB screen sits in the middle of the 256x224
     * border, at offset (48, 40). Tiles in that region get alpha=0 so
     * the platform can blit the live GB frame underneath. */
    const int center_x0 = 48;
    const int center_y0 = 40;
    const int center_x1 = center_x0 + GB_SCREEN_WIDTH;
    const int center_y1 = center_y0 + GB_SCREEN_HEIGHT;

    for (int ty = 0; ty < 28; ty++) {
        for (int tx = 0; tx < 32; tx++) {
            int map_idx = (ty * 32 + tx) * 2;
            uint16_t entry = (uint16_t)(sgb->border_tilemap[map_idx] |
                                        (sgb->border_tilemap[map_idx + 1] << 8));
            uint16_t tile_no = (uint16_t)(entry & 0x00FF);  /* pokered only fills 0..127 */
            uint8_t pal_no   = (uint8_t)((entry >> 10) & 0x03);
            bool flip_x = (entry & 0x4000) != 0;
            bool flip_y = (entry & 0x8000) != 0;
            const uint8_t* tile = &sgb->border_chr[tile_no * 32];
            const uint16_t* pal = sgb->border_palette[pal_no];

            for (int row = 0; row < 8; row++) {
                int src_row = flip_y ? (7 - row) : row;
                uint8_t b0 = tile[src_row * 2 + 0];     /* plane 0 */
                uint8_t b1 = tile[src_row * 2 + 1];     /* plane 1 */
                uint8_t b2 = tile[16 + src_row * 2 + 0]; /* plane 2 */
                uint8_t b3 = tile[16 + src_row * 2 + 1]; /* plane 3 */

                int py = ty * 8 + row;
                if (py >= GB_SGB_BORDER_H) continue;

                for (int col = 0; col < 8; col++) {
                    int src_col = flip_x ? col : (7 - col);
                    int px = tx * 8 + col;
                    if (px >= GB_SGB_BORDER_W) continue;

                    int idx = ((b3 >> src_col) & 1) << 3 |
                              ((b2 >> src_col) & 1) << 2 |
                              ((b1 >> src_col) & 1) << 1 |
                              ((b0 >> src_col) & 1);

                    /* Clear the GB-screen window so the live frame shows
                     * through. Outside that, render every color index —
                     * including 0 — using the real palette entry. (SGB
                     * specs claim color 0 is transparent, but in
                     * practice cart border art uses index 0 as a normal
                     * fill colour and treating it as transparent leaves
                     * silhouettes hollow / inside-out.) */
                    bool in_screen = (px >= center_x0 && px < center_x1 &&
                                      py >= center_y0 && py < center_y1);
                    uint32_t* dst = &out_rgba[py * GB_SGB_BORDER_W + px];
                    if (in_screen) {
                        *dst = 0;  /* transparent under the GB screen */
                    } else {
                        *dst = sgb_rgb555_to_rgba(pal[idx]);
                    }
                }
            }
        }
    }
    return true;
}

/* ---------------- JOYP read shim ---------------- */

uint8_t gb_sgb_modify_joyp_read(GBContext* ctx, uint8_t base) {
    if (!ctx) return base;
    GBSgbState* sgb = (GBSgbState*)ctx->sgb;
    if (!gb_sgb_is_enabled(sgb)) return base;
    if (sgb->mlt_req_players < 2) return base;

    /* MLT_REQ active. On a real SGB the JOYP read's low 4 bits are the
     * inverse of the active controller index, so for an N-player setup
     * the value cycles through (0xF, 0xE, ..., 0x10-N) as the CPU
     * advances controllers. Pokemon's CheckSGB only inspects the very
     * first read for `(joyp & 0x03) != 0x03`, and any non-3 value here
     * makes the carry-set "isSGB" branch fall through. We start the
     * cycle at index 1 (low=0xE) so that first read passes, then rotate
     * on every read so multi-controller code sees a fresh value each
     * call. */
    uint8_t idx = (uint8_t)((sgb->mlt_req_index + 1) & 0x03);
    sgb->mlt_req_index = (uint8_t)((sgb->mlt_req_index + 1) % sgb->mlt_req_players);
    uint8_t low = (uint8_t)(0x0F ^ idx);
    uint8_t high = (uint8_t)(base & 0xF0);
    return (uint8_t)(high | low);
}
