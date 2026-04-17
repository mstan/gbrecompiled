/**
 * @file ppu.c
 * @brief GameBoy PPU (Pixel Processing Unit) implementation
 */

#include "ppu.h"
#include "gbrt.h"
#include "gbrt_debug.h"

#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Default DMG palette
 * ========================================================================== */

static const uint32_t dmg_palette_rgba[4] = {
    0xFFE0F8D0,
    0xFF88C070,
    0xFF346856,
    0xFF081820,
};

static const uint16_t dmg_palette_rgb555[4] = {
    0x67DC,
    0x32EE,
    0x2A66,
    0x0841,
};

/* ============================================================================
 * CGB compatibility palette tables (ported from SameBoy's cgb_boot.asm)
 * ========================================================================== */

static const uint8_t compat_title_checksums[] = {
    0x00, 0x88, 0x16, 0x36, 0xD1, 0xDB, 0xF2, 0x3C, 0x8C, 0x92, 0x3D, 0x5C,
    0x58, 0xC9, 0x3E, 0x70, 0x1D, 0x59, 0x69, 0x19, 0x35, 0xA8, 0x14, 0xAA,
    0x75, 0x95, 0x99, 0x34, 0x6F, 0x15, 0xFF, 0x97, 0x4B, 0x90, 0x17, 0x10,
    0x39, 0xF7, 0xF6, 0xA2, 0x49, 0x4E, 0x43, 0x68, 0xE0, 0x8B, 0xF0, 0xCE,
    0x0C, 0x29, 0xE8, 0xB7, 0x86, 0x9A, 0x52, 0x01, 0x9D, 0x71, 0x9C, 0xBD,
    0x5D, 0x6D, 0x67, 0x3F, 0x6B, 0xB3, 0x46, 0x28, 0xA5, 0xC6, 0xD3, 0x27,
    0x61, 0x18, 0x66, 0x6A, 0xBF, 0x0D, 0xF4, 0xB3, 0x46, 0x28, 0xA5, 0xC6,
    0xD3, 0x27, 0x61, 0x18, 0x66, 0x6A, 0xBF, 0x0D, 0xF4, 0xB3,
};

static const uint8_t compat_palette_per_checksum[] = {
    0, 4, 5, 35, 34, 3, 31, 15, 10, 5, 19, 36,
    135, 37, 30, 44, 21, 32, 31, 20, 5, 33, 13, 14,
    5, 29, 5, 18, 9, 3, 2, 26, 25, 25, 41, 42,
    26, 45, 42, 45, 36, 38, 154, 42, 30, 41, 34, 34,
    5, 42, 6, 5, 33, 25, 42, 42, 40, 2, 16, 25,
    42, 42, 5, 0, 39, 36, 22, 25, 6, 32, 12, 36,
    11, 39, 18, 39, 24, 31, 50, 17, 46, 6, 27, 0,
    47, 41, 41, 0, 0, 19, 34, 23, 18, 29,
};

static const char compat_dup_fourth_letters[] = "BEFAARBEKEK R-URAR INAILICE R";
static const size_t compat_first_duplicate_index = 65;

/* Each value is a 16-bit-color offset into compat_palette_words. */
static const uint8_t compat_palette_combo_offsets[] = {
    16, 16, 116, 72, 72, 72, 80, 80, 80, 96, 96, 96, 36, 36, 36,
    0, 0, 0, 108, 108, 108, 20, 20, 20, 48, 48, 48, 104, 104, 104,
    64, 32, 32, 16, 112, 112, 16, 8, 8, 12, 16, 16, 16, 116, 116,
    112, 16, 112, 8, 68, 8, 64, 64, 32, 16, 16, 28, 16, 16,
    72, 16, 16, 80, 76, 76, 36, 15, 15, 44, 68, 68, 8, 16, 16,
    8, 16, 16, 12, 112, 112, 0, 12, 12, 0, 0, 0, 4, 72, 88,
    72, 80, 88, 80, 96, 88, 96, 64, 88, 32, 68, 16, 52, 111, 0,
    56, 111, 16, 60, 76, 91, 36, 64, 112, 40, 16, 92, 112, 68, 88,
    8, 16, 0, 8, 16, 112, 12, 112, 12, 0, 12, 112, 16, 84, 112,
    16, 12, 112, 0, 100, 12, 112, 0, 112, 32, 16, 12, 112, 112, 12,
    24, 16, 112, 116, 120, 120, 120, 124, 124, 124, 112, 16, 4, 0, 0, 8,
};

static const uint16_t compat_palette_words[] = {
    0x7FFF, 0x32BF, 0x00D0, 0x0000,
    0x639F, 0x4279, 0x15B0, 0x04CB,
    0x7FFF, 0x6E31, 0x454A, 0x0000,
    0x7FFF, 0x1BEF, 0x0200, 0x0000,
    0x7FFF, 0x421F, 0x1CF2, 0x0000,
    0x7FFF, 0x5294, 0x294A, 0x0000,
    0x7FFF, 0x03FF, 0x012F, 0x0000,
    0x7FFF, 0x03EF, 0x01D6, 0x0000,
    0x7FFF, 0x42B5, 0x3DC8, 0x0000,
    0x7E74, 0x03FF, 0x0180, 0x0000,
    0x67FF, 0x77AC, 0x1A13, 0x2D6B,
    0x7ED6, 0x4BFF, 0x2175, 0x0000,
    0x53FF, 0x4A5F, 0x7E52, 0x0000,
    0x4FFF, 0x7ED2, 0x3A4C, 0x1CE0,
    0x03ED, 0x7FFF, 0x255F, 0x0000,
    0x036A, 0x021F, 0x03FF, 0x7FFF,
    0x7FFF, 0x01DF, 0x0112, 0x0000,
    0x231F, 0x035F, 0x00F2, 0x0009,
    0x7FFF, 0x03EA, 0x011F, 0x0000,
    0x299F, 0x001A, 0x000C, 0x0000,
    0x7FFF, 0x027F, 0x001F, 0x0000,
    0x7FFF, 0x03E0, 0x0206, 0x0120,
    0x7FFF, 0x7EEB, 0x001F, 0x7C00,
    0x7FFF, 0x3FFF, 0x7E00, 0x001F,
    0x7FFF, 0x03FF, 0x001F, 0x0000,
    0x03FF, 0x001F, 0x000C, 0x0000,
    0x7FFF, 0x033F, 0x0193, 0x0000,
    0x0000, 0x4200, 0x037F, 0x7FFF,
    0x7FFF, 0x7E8C, 0x7C00, 0x0000,
    0x7FFF, 0x1BEF, 0x6180, 0x0000,
    0x7FFF, 0x7FEA, 0x7D5F, 0x0000,
    0x4778, 0x3290, 0x1D87, 0x0861,
};

/* ============================================================================
 * Helpers
 * ========================================================================== */

static bool ppu_is_cgb_mode(const GBContext* ctx) {
    return ctx && ctx->config.model == GB_MODEL_CGB && !ctx->config.cgb_compatibility_mode;
}

static bool ppu_is_cgb_compat_mode(const GBContext* ctx) {
    return ctx && ctx->config.model == GB_MODEL_CGB && ctx->config.cgb_compatibility_mode;
}

static uint32_t rgb555_to_rgba(uint16_t color) {
    uint8_t r = (uint8_t)(((color >> 0) & 0x1F) * 255 / 31);
    uint8_t g = (uint8_t)(((color >> 5) & 0x1F) * 255 / 31);
    uint8_t b = (uint8_t)(((color >> 10) & 0x1F) * 255 / 31);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static uint8_t apply_palette(uint8_t color, uint8_t palette) {
    return (uint8_t)((palette >> (color * 2)) & 0x03);
}

static uint16_t read_palette_color(const uint8_t* palette_ram, uint8_t palette_number, uint8_t color) {
    size_t index = (size_t)palette_number * 8u + (size_t)color * 2u;
    return (uint16_t)(palette_ram[index] | (palette_ram[index + 1] << 8));
}

static uint8_t vram_read_bank(const GBContext* ctx, uint8_t bank, uint16_t addr) {
    if (!ctx || !ctx->vram || addr < 0x8000 || addr > 0x9FFF) {
        return 0xFF;
    }
    return ctx->vram[(bank * VRAM_SIZE) + (addr - 0x8000)];
}

static uint16_t get_tile_data_addr(uint8_t lcdc, uint8_t tile_idx, bool is_obj) {
    if (is_obj || (lcdc & LCDC_TILE_DATA)) {
        return (uint16_t)(0x8000 + (tile_idx * 16));
    }
    return (uint16_t)(0x9000 + ((int8_t)tile_idx * 16));
}

static uint16_t get_bg_tilemap_addr(uint8_t lcdc) {
    return (lcdc & LCDC_BG_TILEMAP) ? 0x9C00 : 0x9800;
}

static uint16_t get_window_tilemap_addr(uint8_t lcdc) {
    return (lcdc & LCDC_WINDOW_TILEMAP) ? 0x9C00 : 0x9800;
}

static bool compat_license_is_nintendo(const GBContext* ctx) {
    if (!ctx || !ctx->rom || ctx->rom_size <= 0x145) {
        return false;
    }
    if (ctx->rom[0x14B] == 0x01) {
        return true;
    }
    return ctx->rom[0x14B] == 0x33 &&
           ctx->rom[0x144] == '0' &&
           ctx->rom[0x145] == '1';
}

static uint8_t compat_title_checksum(const GBContext* ctx) {
    uint8_t checksum = 0;

    if (!ctx || !ctx->rom || ctx->rom_size <= 0x143) {
        return 0;
    }

    for (size_t i = 0; i < 16; i++) {
        checksum = (uint8_t)(checksum + ctx->rom[0x134 + i]);
    }

    return checksum;
}

static uint8_t compat_palette_index(const GBContext* ctx) {
    uint8_t checksum;

    if (!compat_license_is_nintendo(ctx)) {
        return 0;
    }

    checksum = compat_title_checksum(ctx);
    for (size_t i = 0; i < sizeof(compat_title_checksums); i++) {
        if (compat_title_checksums[i] != checksum) {
            continue;
        }

        if (i >= compat_first_duplicate_index) {
            uint8_t fourth_letter = (ctx && ctx->rom && ctx->rom_size > 0x137) ? ctx->rom[0x137] : 0;
            if ((char)fourth_letter != compat_dup_fourth_letters[i - compat_first_duplicate_index]) {
                continue;
            }
        }

        return compat_palette_per_checksum[i];
    }

    return 0;
}

static void compat_copy_palette_bytes(uint8_t* dst, uint8_t word_offset) {
    for (size_t i = 0; i < 4; i++) {
        uint16_t color = compat_palette_words[word_offset + i];
        dst[i * 2] = (uint8_t)(color & 0xFF);
        dst[i * 2 + 1] = (uint8_t)(color >> 8);
    }
}

static void ppu_fill_palette_ram(uint8_t* palette_ram, uint16_t color) {
    for (size_t palette = 0; palette < 8; palette++) {
        for (size_t entry = 0; entry < 4; entry++) {
            size_t index = palette * 8 + entry * 2;
            palette_ram[index] = (uint8_t)(color & 0xFF);
            palette_ram[index + 1] = (uint8_t)(color >> 8);
        }
    }
}

static void ppu_load_compatibility_palettes(GBPPU* ppu, const GBContext* ctx) {
    uint8_t palette_index = compat_palette_index(ctx) & 0x7F;
    size_t combo_index = (size_t)palette_index * 3u;

    memset(ppu->bg_palette_ram, 0, sizeof(ppu->bg_palette_ram));
    memset(ppu->obj_palette_ram, 0, sizeof(ppu->obj_palette_ram));

    compat_copy_palette_bytes(&ppu->obj_palette_ram[0], compat_palette_combo_offsets[combo_index + 0]);
    compat_copy_palette_bytes(&ppu->obj_palette_ram[8], compat_palette_combo_offsets[combo_index + 1]);
    compat_copy_palette_bytes(&ppu->bg_palette_ram[0], compat_palette_combo_offsets[combo_index + 2]);
}

static void latch_scanline_registers(GBPPU* ppu) {
    ppu->latched_lcdc = ppu->lcdc;
    ppu->latched_scy = ppu->scy;
    ppu->latched_scx = ppu->scx;
    ppu->latched_bgp = ppu->bgp;
    ppu->latched_obp0 = ppu->obp0;
    ppu->latched_obp1 = ppu->obp1;
    ppu->latched_wy = ppu->wy;
    ppu->latched_wx = ppu->wx;
}

/* ============================================================================
 * PPU Initialization
 * ========================================================================== */

void ppu_init(GBPPU* ppu) {
    memset(ppu, 0, sizeof(*ppu));
    ppu_reset(ppu, NULL);
    DBG_PPU("PPU initialized");
}

void ppu_reset(GBPPU* ppu, const GBContext* ctx) {
    bool cgb_mode = ppu_is_cgb_mode(ctx);
    bool cgb_compat_mode = ppu_is_cgb_compat_mode(ctx);
    uint16_t default_color = cgb_mode ? 0x7FFF : dmg_palette_rgb555[0];

    memset(ppu->framebuffer, 0, sizeof(ppu->framebuffer));
    for (size_t i = 0; i < GB_FRAMEBUFFER_SIZE; i++) {
        ppu->color_framebuffer[i] = default_color;
        ppu->rgb_framebuffer[i] = cgb_mode ? rgb555_to_rgba(default_color) : dmg_palette_rgba[0];
    }

    ppu->lcdc = 0x91;
    ppu->stat = 0x81;
    ppu->scy = 0x00;
    ppu->scx = 0x00;
    ppu->ly = 145;
    ppu->lyc = 0x00;
    ppu->dma = (ctx && ctx->config.model == GB_MODEL_CGB) ? 0x00 : 0xFF;
    ppu->bgp = 0xFC;
    ppu->obp0 = 0xFF;
    ppu->obp1 = 0xFF;
    ppu->wy = 0x00;
    ppu->wx = 0x00;
    ppu->bgpi = 0xC0;
    ppu->obpi = 0xC0;
    ppu->opri = cgb_mode ? 0 : 1;

    latch_scanline_registers(ppu);

    ppu->stat_irq_state = false;
    ppu->mode = PPU_MODE_VBLANK;
    ppu->mode_cycles = 4;
    ppu->window_line = 0;
    ppu->window_triggered = false;
    ppu->frame_ready = false;

    if (cgb_mode) {
        ppu_fill_palette_ram(ppu->bg_palette_ram, 0x7FFF);
        ppu_fill_palette_ram(ppu->obj_palette_ram, 0x7FFF);
    } else if (cgb_compat_mode) {
        ppu_load_compatibility_palettes(ppu, ctx);
    } else {
        memset(ppu->bg_palette_ram, 0, sizeof(ppu->bg_palette_ram));
        memset(ppu->obj_palette_ram, 0, sizeof(ppu->obj_palette_ram));
    }

    if (ctx) {
        GBContext* mutable_ctx = (GBContext*)ctx;
        mutable_ctx->io[0x40] = ppu->lcdc;
        mutable_ctx->io[0x41] = ppu->stat;
        mutable_ctx->io[0x42] = ppu->scy;
        mutable_ctx->io[0x43] = ppu->scx;
        mutable_ctx->io[0x44] = ppu->ly;
        mutable_ctx->io[0x45] = ppu->lyc;
        mutable_ctx->io[0x46] = ppu->dma;
        mutable_ctx->io[0x47] = ppu->bgp;
        mutable_ctx->io[0x48] = ppu->obp0;
        mutable_ctx->io[0x49] = ppu->obp1;
        mutable_ctx->io[0x4A] = ppu->wy;
        mutable_ctx->io[0x4B] = ppu->wx;
        mutable_ctx->io[0x68] = (uint8_t)(ppu->bgpi | 0x40);
        mutable_ctx->io[0x6A] = (uint8_t)(ppu->obpi | 0x40);
        mutable_ctx->io[0x6C] = (uint8_t)(0xFE | (ppu->opri & 0x01));
    }

    DBG_PPU("PPU reset - LCDC=0x%02X mode=%s cgb=%d compat=%d",
            ppu->lcdc, ppu_mode_name(ppu->mode), cgb_mode ? 1 : 0, cgb_compat_mode ? 1 : 0);
}

/* ============================================================================
 * Rendering
 * ========================================================================== */

static uint16_t resolve_bg_color(const GBPPU* ppu,
                                 const GBContext* ctx,
                                 uint8_t palette_number,
                                 uint8_t raw_color,
                                 uint8_t dmg_palette_reg) {
    if (ppu_is_cgb_mode(ctx)) {
        return read_palette_color(ppu->bg_palette_ram, palette_number, raw_color);
    }

    if (ppu_is_cgb_compat_mode(ctx)) {
        uint8_t shade = apply_palette(raw_color, dmg_palette_reg);
        return read_palette_color(ppu->bg_palette_ram, 0, shade);
    }

    return dmg_palette_rgb555[apply_palette(raw_color, dmg_palette_reg)];
}

static uint16_t resolve_obj_color(const GBPPU* ppu,
                                  const GBContext* ctx,
                                  uint8_t palette_number,
                                  uint8_t raw_color,
                                  uint8_t dmg_palette_reg) {
    if (ppu_is_cgb_mode(ctx)) {
        return read_palette_color(ppu->obj_palette_ram, palette_number, raw_color);
    }

    if (ppu_is_cgb_compat_mode(ctx)) {
        uint8_t shade = apply_palette(raw_color, dmg_palette_reg);
        return read_palette_color(ppu->obj_palette_ram, palette_number, shade);
    }

    return dmg_palette_rgb555[apply_palette(raw_color, dmg_palette_reg)];
}

static void render_bg_scanline(GBPPU* ppu,
                               GBContext* ctx,
                               uint8_t* bg_raw,
                               uint8_t* bg_priority) {
    uint8_t scanline = ppu->ly;
    uint8_t lcdc = ppu->latched_lcdc;
    bool cgb_mode = ppu_is_cgb_mode(ctx);
    bool cgb_compat_mode = ppu_is_cgb_compat_mode(ctx);
    bool bg_visible = cgb_mode ? true : ((lcdc & LCDC_BG_ENABLE) != 0);
    /* Window becomes active only on the first scanline where LY == WY (and
     * the other preconditions are met). Once activated, it stays active for
     * the rest of the frame even if WY is rewritten to a different value.
     * Using (WY <= scanline) instead would let a mid-frame WY decrease
     * retroactively trigger the window, which is not how the hardware latches. */
    bool window_hw_enable = (lcdc & LCDC_WINDOW_ENABLE) &&
                            (ppu->latched_wx <= 166) &&
                            (cgb_mode || bg_visible);
    if (window_hw_enable && !ppu->window_triggered &&
        ppu->latched_wy == scanline) {
        ppu->window_triggered = true;
    }
    bool window_enable = window_hw_enable && ppu->window_triggered;

    if (!(lcdc & LCDC_LCD_ENABLE)) {
        memset(&ppu->framebuffer[scanline * GB_SCREEN_WIDTH], 0, GB_SCREEN_WIDTH);
        memset(bg_raw, 0, GB_SCREEN_WIDTH);
        memset(bg_priority, 0, GB_SCREEN_WIDTH);
        for (size_t x = 0; x < GB_SCREEN_WIDTH; x++) {
            ppu->color_framebuffer[scanline * GB_SCREEN_WIDTH + x] = dmg_palette_rgb555[0];
        }
        return;
    }

    for (int x = 0; x < GB_SCREEN_WIDTH; x++) {
        uint8_t raw_color = 0;
        uint8_t palette_number = 0;
        uint8_t tile_bank = 0;
        bool priority = false;
        bool in_window = window_enable && (x >= (int)ppu->latched_wx - 7);
        uint16_t tilemap_addr;
        uint8_t tile_x;
        uint8_t tile_y;
        uint8_t tile_idx;
        uint8_t attr = 0;
        int pixel_x;
        int pixel_y;
        uint16_t tile_addr;
        uint8_t lo;
        uint8_t hi;
        int bit;

        if (!bg_visible && !in_window) {
            ppu->framebuffer[scanline * GB_SCREEN_WIDTH + x] = 0;
            ppu->color_framebuffer[scanline * GB_SCREEN_WIDTH + x] = dmg_palette_rgb555[0];
            bg_raw[x] = 0;
            bg_priority[x] = 0;
            continue;
        }

        if (in_window) {
            int win_x = x - ((int)ppu->latched_wx - 7);
            int win_y = ppu->window_line;
            tilemap_addr = get_window_tilemap_addr(lcdc);
            tile_x = (uint8_t)(win_x / 8);
            tile_y = (uint8_t)(win_y / 8);
            pixel_x = win_x % 8;
            pixel_y = win_y % 8;
        } else {
            int bg_x = (x + ppu->latched_scx) & 0xFF;
            int bg_y = (scanline + ppu->latched_scy) & 0xFF;
            tilemap_addr = get_bg_tilemap_addr(lcdc);
            tile_x = (uint8_t)(bg_x / 8);
            tile_y = (uint8_t)(bg_y / 8);
            pixel_x = bg_x % 8;
            pixel_y = bg_y % 8;
        }

        tile_idx = vram_read_bank(ctx, 0, (uint16_t)(tilemap_addr + tile_y * 32 + tile_x));
        if (cgb_mode) {
            attr = vram_read_bank(ctx, 1, (uint16_t)(tilemap_addr + tile_y * 32 + tile_x));
            palette_number = attr & 0x07;
            tile_bank = (attr & 0x08) ? 1 : 0;
            priority = (attr & 0x80) != 0;
            if (attr & 0x20) pixel_x = 7 - pixel_x;
            if (attr & 0x40) pixel_y = 7 - pixel_y;
        }

        tile_addr = get_tile_data_addr(lcdc, tile_idx, false);
        lo = vram_read_bank(ctx, tile_bank, (uint16_t)(tile_addr + pixel_y * 2));
        hi = vram_read_bank(ctx, tile_bank, (uint16_t)(tile_addr + pixel_y * 2 + 1));
        bit = 7 - pixel_x;
        raw_color = (uint8_t)(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1));

        ppu->framebuffer[scanline * GB_SCREEN_WIDTH + x] = cgb_mode
            ? raw_color
            : apply_palette(raw_color, ppu->latched_bgp);
        ppu->color_framebuffer[scanline * GB_SCREEN_WIDTH + x] =
            resolve_bg_color(ppu, ctx, palette_number, raw_color, ppu->latched_bgp);
        bg_raw[x] = raw_color;
        bg_priority[x] = priority ? 1 : 0;
    }

    if (ppu->window_triggered && window_enable) {
        ppu->window_line++;
    }

    (void)cgb_compat_mode;
}

typedef struct {
    int oam_index;
    int screen_x;
    uint8_t x_pos;
    uint8_t flags;
    uint8_t palette;
    bool behind_bg;
    uint8_t lo;
    uint8_t hi;
} ScanlineSprite;

static void sort_scanline_sprites(ScanlineSprite* sprites, int sprite_count) {
    for (int i = 1; i < sprite_count; i++) {
        ScanlineSprite sprite = sprites[i];
        int j = i - 1;

        while (j >= 0) {
            bool higher_priority = (sprite.x_pos < sprites[j].x_pos) ||
                                   (sprite.x_pos == sprites[j].x_pos &&
                                    sprite.oam_index < sprites[j].oam_index);
            if (!higher_priority) {
                break;
            }
            sprites[j + 1] = sprites[j];
            j--;
        }

        sprites[j + 1] = sprite;
    }
}

static void render_sprites_scanline(GBPPU* ppu,
                                    GBContext* ctx,
                                    const uint8_t* bg_raw,
                                    const uint8_t* bg_priority) {
    bool cgb_mode = ppu_is_cgb_mode(ctx);
    bool dmg_priority_mode = !cgb_mode || ppu->opri != 0;
    uint8_t scanline = ppu->ly;
    uint8_t sprite_height;
    int sprite_count = 0;
    ScanlineSprite sprites[10];

    if (!(ppu->latched_lcdc & LCDC_OBJ_ENABLE)) {
        return;
    }

    sprite_height = (ppu->latched_lcdc & LCDC_OBJ_SIZE) ? 16 : 8;

    for (int i = 0; i < 40 && sprite_count < 10; i++) {
        const OAMEntry* sprite = (const OAMEntry*)(ctx->oam + i * 4);
        int sprite_y = (int)sprite->y - 16;

        if (scanline < sprite_y || scanline >= sprite_y + sprite_height) {
            continue;
        }

        {
            int line = (int)scanline - sprite_y;
            uint8_t tile_idx = sprite->tile;
            uint8_t tile_bank = 0;

            if (sprite_height == 16) {
                tile_idx &= 0xFE;
            }

            if (sprite->flags & OAM_FLIP_Y) {
                line = sprite_height - 1 - line;
            }

            if (cgb_mode && (sprite->flags & OAM_CGB_BANK)) {
                tile_bank = 1;
            }

            sprites[sprite_count].oam_index = i;
            sprites[sprite_count].screen_x = (int)sprite->x - 8;
            sprites[sprite_count].x_pos = sprite->x;
            sprites[sprite_count].flags = sprite->flags;
            sprites[sprite_count].palette = cgb_mode
                ? (sprite->flags & OAM_CGB_PALETTE)
                : ((sprite->flags & OAM_PALETTE) ? 1 : 0);
            sprites[sprite_count].behind_bg = (sprite->flags & OAM_PRIORITY) != 0;
            sprites[sprite_count].lo = vram_read_bank(ctx, tile_bank, (uint16_t)(0x8000 + tile_idx * 16 + line * 2));
            sprites[sprite_count].hi = vram_read_bank(ctx, tile_bank, (uint16_t)(0x8000 + tile_idx * 16 + line * 2 + 1));
            sprite_count++;
        }
    }

    if (dmg_priority_mode) {
        sort_scanline_sprites(sprites, sprite_count);
    }

    for (int screen_x = 0; screen_x < GB_SCREEN_WIDTH; screen_x++) {
        const ScanlineSprite* chosen_sprite = NULL;
        uint8_t chosen_color = 0;

        for (int i = 0; i < sprite_count; i++) {
            const ScanlineSprite* sprite = &sprites[i];
            int sprite_px = screen_x - sprite->screen_x;
            int bit_pos;
            uint8_t color;

            if (sprite_px < 0 || sprite_px >= 8) {
                continue;
            }

            bit_pos = (sprite->flags & OAM_FLIP_X) ? sprite_px : (7 - sprite_px);
            color = (uint8_t)(((sprite->lo >> bit_pos) & 1) | (((sprite->hi >> bit_pos) & 1) << 1));
            if (color == 0) {
                continue;
            }

            chosen_sprite = sprite;
            chosen_color = color;
            break;
        }

        if (!chosen_sprite) {
            continue;
        }

        if (cgb_mode) {
            uint8_t bg_color = bg_raw ? bg_raw[screen_x] : 0;
            uint8_t bg_attr_priority = bg_priority ? bg_priority[screen_x] : 0;

            if (bg_color != 0) {
                if (!(ppu->latched_lcdc & LCDC_BG_ENABLE)) {
                    /* LCDC bit 0 clear gives OBJ priority in CGB mode. */
                } else if (bg_attr_priority || chosen_sprite->behind_bg) {
                    continue;
                }
            }

            ppu->framebuffer[scanline * GB_SCREEN_WIDTH + screen_x] = chosen_color;
            ppu->color_framebuffer[scanline * GB_SCREEN_WIDTH + screen_x] =
                resolve_obj_color(ppu, ctx, chosen_sprite->palette, chosen_color,
                                  chosen_sprite->palette ? ppu->latched_obp1 : ppu->latched_obp0);
        } else {
            uint8_t bg_color = bg_raw ? bg_raw[screen_x] : 0;
            uint8_t dmg_palette_reg = chosen_sprite->palette ? ppu->latched_obp1 : ppu->latched_obp0;
            uint8_t shade = apply_palette(chosen_color, dmg_palette_reg);

            if (chosen_sprite->behind_bg && bg_color != 0) {
                continue;
            }

            ppu->framebuffer[scanline * GB_SCREEN_WIDTH + screen_x] = shade;
            ppu->color_framebuffer[scanline * GB_SCREEN_WIDTH + screen_x] =
                resolve_obj_color(ppu, ctx, chosen_sprite->palette, chosen_color, dmg_palette_reg);
        }
    }
}

void ppu_render_scanline(GBPPU* ppu, GBContext* ctx) {
    uint8_t bg_raw[GB_SCREEN_WIDTH];
    uint8_t bg_priority[GB_SCREEN_WIDTH];

    if (ppu->ly == 0) {
        gbrt_log_oam_snapshot(ctx, "scanline-0");
    }

    gbrt_log_ppu_scanline(ctx,
                          ppu->ly,
                          ppu->mode,
                          ppu->lcdc,
                          ppu->stat,
                          ppu->scx,
                          ppu->scy,
                          ppu->wx,
                          ppu->wy,
                          ppu->bgp,
                          ppu->obp0,
                          ppu->obp1,
                          ppu->window_line,
                          ppu->window_triggered);

    memset(bg_raw, 0, sizeof(bg_raw));
    memset(bg_priority, 0, sizeof(bg_priority));

    render_bg_scanline(ppu, ctx, bg_raw, bg_priority);
    render_sprites_scanline(ppu, ctx, bg_raw, bg_priority);
}

static void convert_to_rgb(GBPPU* ppu) {
    static int convert_count = 0;
    bool has_content = dbg_has_nonzero_pixels(ppu->framebuffer, GB_FRAMEBUFFER_SIZE);

    for (int i = 0; i < GB_FRAMEBUFFER_SIZE; i++) {
        ppu->rgb_framebuffer[i] = rgb555_to_rgba(ppu->color_framebuffer[i]);
    }

    convert_count++;
    if (convert_count <= 5 || (convert_count % 60 == 0)) {
        DBG_FRAME("Frame %d converted to RGB - has_content=%d", convert_count, has_content);
        dbg_dump_framebuffer(ppu->framebuffer, GB_SCREEN_WIDTH);
    }
}

/* ============================================================================
 * PPU Mode State Machine
 * ========================================================================== */

static void update_stat(GBPPU* ppu, GBContext* ctx) {
    ppu->stat = (uint8_t)((ppu->stat & ~STAT_MODE_MASK) | ppu->mode);

    if (ppu->ly == ppu->lyc) {
        ppu->stat |= STAT_LYC_MATCH;
    } else {
        ppu->stat &= (uint8_t)~STAT_LYC_MATCH;
    }

    ctx->io[0x41] = ppu->stat;
    ctx->io[0x44] = ppu->ly;
}

static void check_stat_interrupt(GBPPU* ppu, GBContext* ctx, const char* reason) {
    uint8_t source_state_mask = 0;
    uint8_t source_enable_mask = 0;
    uint8_t active_source_mask = 0;
    bool previous_state = ppu->stat_irq_state;
    bool current_state = false;

    if (ppu->mode == PPU_MODE_HBLANK) source_state_mask |= 0x1;
    if (ppu->mode == PPU_MODE_VBLANK) source_state_mask |= 0x2;
    if (ppu->mode == PPU_MODE_OAM) source_state_mask |= 0x4;
    if (ppu->stat & STAT_LYC_MATCH) source_state_mask |= 0x8;

    if (ppu->stat & STAT_HBLANK_INT) source_enable_mask |= 0x1;
    if (ppu->stat & STAT_VBLANK_INT) source_enable_mask |= 0x2;
    if (ppu->stat & STAT_OAM_INT) source_enable_mask |= 0x4;
    if (ppu->stat & STAT_LYC_INT) source_enable_mask |= 0x8;

    active_source_mask = source_state_mask & source_enable_mask;
    current_state = active_source_mask != 0;

    gbrt_log_stat_irq_check(ctx,
                            reason,
                            ppu->ly,
                            ppu->mode,
                            ppu->stat,
                            source_state_mask,
                            source_enable_mask,
                            active_source_mask,
                            previous_state,
                            current_state);

    if (current_state && !previous_state) {
        uint8_t if_before = ctx->io[0x0F];
        ctx->io[0x0F] |= 0x02;
        gbrt_log_stat_irq_request(ctx,
                                  reason,
                                  ppu->ly,
                                  ppu->mode,
                                  ppu->stat,
                                  active_source_mask,
                                  if_before,
                                  ctx->io[0x0F]);
    }

    ppu->stat_irq_state = current_state;
}

void ppu_tick(GBPPU* ppu, GBContext* ctx, uint32_t cycles) {
    if (!(ppu->lcdc & LCDC_LCD_ENABLE)) {
        return;
    }

    ppu->mode_cycles += cycles;

    for (;;) {
        switch (ppu->mode) {
            case PPU_MODE_OAM:
                if (ppu->mode_cycles < CYCLES_OAM_SCAN) {
                    return;
                }
                ppu->mode_cycles -= CYCLES_OAM_SCAN;
                ppu->mode = PPU_MODE_DRAW;
                latch_scanline_registers(ppu);
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx, "oam->draw");
                break;

            case PPU_MODE_DRAW:
                if (ppu->mode_cycles < CYCLES_PIXEL_DRAW) {
                    return;
                }
                ppu->mode_cycles -= CYCLES_PIXEL_DRAW;
                ppu_render_scanline(ppu, ctx);
                ppu->mode = PPU_MODE_HBLANK;
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx, "draw->hblank");
                gbrt_hdma_hblank(ctx);
                break;

            case PPU_MODE_HBLANK:
                if (ppu->mode_cycles < CYCLES_HBLANK) {
                    return;
                }
                ppu->mode_cycles -= CYCLES_HBLANK;
                ppu->ly++;

                if (ppu->ly >= VISIBLE_SCANLINES) {
                    ppu->mode = PPU_MODE_VBLANK;
                    if (!ppu->frame_ready) {
                        convert_to_rgb(ppu);
                        ppu->frame_ready = true;
                        ctx->frame_done = 1;
                    }
                    ctx->io[0x0F] |= 0x01;
                } else {
                    ppu->mode = PPU_MODE_OAM;
                }

                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx, "hblank->next");
                break;

            case PPU_MODE_VBLANK:
                if (ppu->mode_cycles < CYCLES_SCANLINE) {
                    return;
                }
                ppu->mode_cycles -= CYCLES_SCANLINE;
                ppu->ly++;

                if (ppu->ly >= TOTAL_SCANLINES) {
                    ppu->ly = 0;
                    ppu->window_line = 0;
                    ppu->window_triggered = false;
                    ppu->mode = PPU_MODE_OAM;
                }

                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx, "vblank->next");
                break;
        }
    }
}

/* ============================================================================
 * Register Access
 * ========================================================================== */

uint8_t ppu_read_register(GBPPU* ppu, uint16_t addr) {
    switch (addr) {
        case 0xFF40: return ppu->lcdc;
        case 0xFF41: return (uint8_t)(ppu->stat | 0x80);
        case 0xFF42: return ppu->scy;
        case 0xFF43: return ppu->scx;
        case 0xFF44: return ppu->ly;
        case 0xFF45: return ppu->lyc;
        case 0xFF46: return ppu->dma;
        case 0xFF47: return ppu->bgp;
        case 0xFF48: return ppu->obp0;
        case 0xFF49: return ppu->obp1;
        case 0xFF4A: return ppu->wy;
        case 0xFF4B: return ppu->wx;
        case 0xFF68: return (uint8_t)(ppu->bgpi | 0x40);
        case 0xFF69:
            if (ppu->mode == PPU_MODE_DRAW) return 0xFF;
            return ppu->bg_palette_ram[ppu->bgpi & 0x3F];
        case 0xFF6A: return (uint8_t)(ppu->obpi | 0x40);
        case 0xFF6B:
            if (ppu->mode == PPU_MODE_DRAW) return 0xFF;
            return ppu->obj_palette_ram[ppu->obpi & 0x3F];
        default: return 0xFF;
    }
}

void ppu_write_register(GBPPU* ppu, GBContext* ctx, uint16_t addr, uint8_t value) {
    static int ppu_write_count = 0;
    uint8_t old_value;

    ppu_write_count++;
    old_value = ppu_read_register(ppu, addr);

    if (ppu_write_count <= 100 || (addr == 0xFF40 && (value == 0x91 || value == 0x00))) {
        DBG_REGS("PPU write #%d: addr=0x%04X value=0x%02X (A=0x%02X)",
                 ppu_write_count, addr, value, ctx ? ctx->a : 0);
    }

    switch (addr) {
        case 0xFF40:
        {
            uint8_t old_lcdc = ppu->lcdc;
            ppu->lcdc = value;
            if ((old_lcdc & LCDC_LCD_ENABLE) && !(value & LCDC_LCD_ENABLE)) {
                ppu->ly = 0;
                ppu->window_line = 0;
                ppu->window_triggered = false;
                ppu->mode = PPU_MODE_HBLANK;
                ppu->mode_cycles = 0;
                ppu->frame_ready = false;
                update_stat(ppu, ctx);
                gbrt_note_lcd_transition(ctx, false, old_lcdc, value, ppu->ly, ppu->mode);
            } else if (!(old_lcdc & LCDC_LCD_ENABLE) && (value & LCDC_LCD_ENABLE)) {
                ppu->ly = 0;
                ppu->window_line = 0;
                ppu->window_triggered = false;
                ppu->mode = PPU_MODE_OAM;
                ppu->mode_cycles = 0;
                ppu->frame_ready = false;
                update_stat(ppu, ctx);
                gbrt_note_lcd_transition(ctx, true, old_lcdc, value, ppu->ly, ppu->mode);
            }
            break;
        }

        case 0xFF41:
            ppu->stat = (uint8_t)((ppu->stat & 0x07) | (value & 0x78));
            if (ppu->lcdc & LCDC_LCD_ENABLE) {
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx, "stat-write");
            }
            break;

        case 0xFF42: ppu->scy = value; break;
        case 0xFF43: ppu->scx = value; break;

        case 0xFF45:
            ppu->lyc = value;
            if (ppu->lcdc & LCDC_LCD_ENABLE) {
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx, "lyc-write");
            }
            break;

        case 0xFF46: ppu->dma = value; break;
        case 0xFF47: ppu->bgp = value; break;
        case 0xFF48: ppu->obp0 = value; break;
        case 0xFF49: ppu->obp1 = value; break;
        case 0xFF4A: ppu->wy = value; break;
        case 0xFF4B: ppu->wx = value; break;

        case 0xFF68:
            ppu->bgpi = value & 0xBF;
            break;

        case 0xFF69:
        {
            uint8_t index = ppu->bgpi & 0x3F;
            if (ppu->mode != PPU_MODE_DRAW) {
                ppu->bg_palette_ram[index] = value;
            }
            if (ppu->bgpi & 0x80) {
                ppu->bgpi = (uint8_t)((ppu->bgpi & 0x80) | ((index + 1) & 0x3F));
            }
            break;
        }

        case 0xFF6A:
            ppu->obpi = value & 0xBF;
            break;

        case 0xFF6B:
        {
            uint8_t index = ppu->obpi & 0x3F;
            if (ppu->mode != PPU_MODE_DRAW) {
                ppu->obj_palette_ram[index] = value;
            }
            if (ppu->obpi & 0x80) {
                ppu->obpi = (uint8_t)((ppu->obpi & 0x80) | ((index + 1) & 0x3F));
            }
            break;
        }

        default:
            break;
    }

    if (ctx) {
        if (addr >= 0xFF40 && addr <= 0xFF4B) {
            ctx->io[addr - 0xFF00] = ppu_read_register(ppu, addr);
        } else if (addr == 0xFF68 || addr == 0xFF6A) {
            ctx->io[addr - 0xFF00] = ppu_read_register(ppu, addr);
        } else if (addr == 0xFF69) {
            ctx->io[0x68] = ppu_read_register(ppu, 0xFF68);
        } else if (addr == 0xFF6B) {
            ctx->io[0x6A] = ppu_read_register(ppu, 0xFF6A);
        }
    }

    gbrt_log_ppu_register_write(ctx,
                                addr,
                                old_value,
                                ppu_read_register(ppu, addr),
                                ppu->ly,
                                ppu->mode);
}

/* ============================================================================
 * Frame Handling
 * ========================================================================== */

bool ppu_frame_ready(GBPPU* ppu) {
    return ppu->frame_ready;
}

void ppu_clear_frame_ready(GBPPU* ppu) {
    ppu->frame_ready = false;
}

const uint32_t* ppu_get_framebuffer(GBPPU* ppu) {
    return ppu->rgb_framebuffer;
}
