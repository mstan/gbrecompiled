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
 * Default Color Palette (DMG green shades)
 * ========================================================================== */

static const uint32_t dmg_palette[4] = {
    0xFFE0F8D0,  /* Lightest (white) - RGBA */
    0xFF88C070,  /* Light */
    0xFF346856,  /* Dark */
    0xFF081820,  /* Darkest (black) */
};

/* ============================================================================
 * PPU Initialization
 * ========================================================================== */

void ppu_init(GBPPU* ppu) {
    memset(ppu, 0, sizeof(GBPPU));
    ppu_reset(ppu);
    DBG_PPU("PPU initialized");
}

void ppu_reset(GBPPU* ppu) {
    /* LCD Registers - post-bootrom state */
    ppu->lcdc = 0x91;  /* LCD on, BG on, tiles at 0x8000 */
    ppu->stat = 0x00;
    ppu->scy = 0;
    ppu->scx = 0;
    ppu->ly = 0;
    ppu->lyc = 0;
    ppu->dma = 0;
    ppu->bgp = 0xFC;   /* 11 11 11 00 */
    ppu->obp0 = 0xFF;
    ppu->obp1 = 0xFF;
    ppu->wy = 0;
    ppu->wx = 0;
    
    /* Internal state */
    ppu->mode = PPU_MODE_OAM;
    ppu->mode_cycles = 0;
    ppu->window_line = 0;
    ppu->window_triggered = false;
    ppu->frame_ready = false;
    ppu->scanline_draw_cycles = CYCLES_PIXEL_DRAW;
    ppu->scanline_hblank_cycles = CYCLES_HBLANK;
    ppu->scanline_sprite_count = 0;
    
    /* Clear framebuffers */
    memset(ppu->framebuffer, 0, sizeof(ppu->framebuffer));
    for (int i = 0; i < GB_FRAMEBUFFER_SIZE; i++) {
        ppu->rgb_framebuffer[i] = dmg_palette[0];
    }
    
    DBG_PPU("PPU reset - LCDC=0x%02X, BGP=0x%02X, mode=%s", 
            ppu->lcdc, ppu->bgp, ppu_mode_name(ppu->mode));
}

/* ============================================================================
 * Tile Fetching
 * ========================================================================== */

/**
 * @brief Get tile data address for a tile index
 */
static uint16_t get_tile_data_addr(GBPPU* ppu, uint8_t tile_idx, bool is_obj) {
    if (is_obj || (ppu->lcdc & LCDC_TILE_DATA)) {
        /* 8000 addressing mode - unsigned indexing */
        return 0x8000 + (tile_idx * 16);
    } else {
        /* 8800 addressing mode - signed indexing from 0x9000 */
        return 0x9000 + ((int8_t)tile_idx * 16);
    }
}

/**
 * @brief Get tile map address
 */
static uint16_t get_bg_tilemap_addr(GBPPU* ppu) {
    return (ppu->lcdc & LCDC_BG_TILEMAP) ? 0x9C00 : 0x9800;
}

static uint16_t get_window_tilemap_addr(GBPPU* ppu) {
    return (ppu->lcdc & LCDC_WINDOW_TILEMAP) ? 0x9C00 : 0x9800;
}

/**
 * @brief Read a byte from VRAM
 */
static uint8_t vram_read(GBContext* ctx, uint16_t addr) {
    if (addr >= 0x8000 && addr <= 0x9FFF) {
        return ctx->vram[addr - 0x8000];
    }
    return 0xFF;
}

/* ============================================================================
 * Scanline Rendering
 * ========================================================================== */

/**
 * @brief Apply palette to a 2-bit color value
 */
static uint8_t apply_palette(uint8_t color, uint8_t palette) {
    return (palette >> (color * 2)) & 0x03;
}

/**
 * @brief Render background/window for current scanline (tile-optimized)
 *
 * Instead of per-pixel tile lookups, fetch each tile row once and decode
 * all 8 pixels from it. This reduces VRAM reads from ~640/scanline to ~40.
 */
static void render_bg_scanline(GBPPU* ppu, GBContext* ctx, uint8_t* bg_prio) {
    uint8_t scanline = ppu->ly;
    uint8_t *fb_row = &ppu->framebuffer[scanline * GB_SCREEN_WIDTH];

    if (!(ppu->lcdc & LCDC_LCD_ENABLE)) {
        memset(fb_row, 0, GB_SCREEN_WIDTH);
        if (bg_prio) memset(bg_prio, 0, GB_SCREEN_WIDTH);
        return;
    }

    bool bg_enable = (ppu->lcdc & LCDC_BG_ENABLE);
    bool window_hw_enable = bg_enable && (ppu->lcdc & LCDC_WINDOW_ENABLE) && (ppu->wx <= 166);
    if (window_hw_enable && !ppu->window_triggered && ppu->wy == scanline) {
        ppu->window_triggered = true;
    }
    bool window_enable = window_hw_enable && ppu->window_triggered;

    /* Pre-compute values constant across the scanline */
    uint8_t bgp = ppu->bgp;
    uint8_t *vram = ctx->vram;  /* Direct VRAM pointer (no bounds checks) */
    bool use_8000 = (ppu->lcdc & LCDC_TILE_DATA) != 0;

    /* Background constants */
    uint16_t bg_tilemap = ((ppu->lcdc & LCDC_BG_TILEMAP) ? 0x9C00 : 0x9800) - 0x8000;
    int bg_y = (scanline + ppu->latched_scy) & 0xFF;
    int bg_tile_row = (bg_y / 8) * 32;  /* Row offset in tilemap */
    int bg_pixel_y = bg_y & 7;          /* Y within tile */

    /* Window constants */
    int win_start_x = window_enable ? (ppu->wx - 7) : 256; /* off-screen if disabled */
    uint16_t win_tilemap = ((ppu->lcdc & LCDC_WINDOW_TILEMAP) ? 0x9C00 : 0x9800) - 0x8000;
    int win_y = ppu->window_line;
    int win_tile_row = (win_y / 8) * 32;
    int win_pixel_y = win_y & 7;

    int x = 0;

    /* === Render background portion (before window) === */
    if (bg_enable && win_start_x > 0) {
        int bg_end = (win_start_x < GB_SCREEN_WIDTH) ? win_start_x : GB_SCREEN_WIDTH;
        int bg_x = (ppu->latched_scx) & 0xFF;

        while (x < bg_end) {
            /* Fetch tile */
            uint8_t tile_idx = vram[bg_tilemap + bg_tile_row + (bg_x >> 3)];
            uint16_t tile_base;
            if (use_8000) {
                tile_base = tile_idx * 16;
            } else {
                tile_base = 0x1000 + (int8_t)tile_idx * 16;
            }
            uint8_t lo = vram[tile_base + bg_pixel_y * 2];
            uint8_t hi = vram[tile_base + bg_pixel_y * 2 + 1];

            /* Decode pixels from this tile */
            int start_bit = bg_x & 7;
            for (int bit = start_bit; bit < 8 && x < bg_end; bit++, x++) {
                uint8_t shift = 7 - bit;
                uint8_t color = ((lo >> shift) & 1) | (((hi >> shift) & 1) << 1);
                fb_row[x] = (bgp >> (color * 2)) & 0x03;
                if (bg_prio) bg_prio[x] = color;
            }
            bg_x = (bg_x + (8 - start_bit)) & 0xFF;
        }
    } else if (!bg_enable) {
        /* BG disabled — fill with color 0 */
        int bg_end = (win_start_x < GB_SCREEN_WIDTH) ? win_start_x : GB_SCREEN_WIDTH;
        for (; x < bg_end; x++) {
            fb_row[x] = bgp & 0x03;
            if (bg_prio) bg_prio[x] = 0;
        }
    }

    /* === Render window portion === */
    if (window_enable && x < GB_SCREEN_WIDTH) {
        if (x < win_start_x) x = win_start_x;
        int win_x = x - win_start_x;

        while (x < GB_SCREEN_WIDTH) {
            uint8_t tile_idx = vram[win_tilemap + win_tile_row + (win_x >> 3)];
            uint16_t tile_base;
            if (use_8000) {
                tile_base = tile_idx * 16;
            } else {
                tile_base = 0x1000 + (int8_t)tile_idx * 16;
            }
            uint8_t lo = vram[tile_base + win_pixel_y * 2];
            uint8_t hi = vram[tile_base + win_pixel_y * 2 + 1];

            int start_bit = win_x & 7;
            for (int bit = start_bit; bit < 8 && x < GB_SCREEN_WIDTH; bit++, x++, win_x++) {
                uint8_t shift = 7 - bit;
                uint8_t color = ((lo >> shift) & 1) | (((hi >> shift) & 1) << 1);
                fb_row[x] = (bgp >> (color * 2)) & 0x03;
                if (bg_prio) bg_prio[x] = color;
            }
        }
        ppu->window_line++;
    } else if (ppu->window_triggered && window_enable) {
        ppu->window_line++;
    }
}

/**
 * @brief Render sprites for current scanline
 */
static void render_sprites_scanline(GBPPU* ppu, GBContext* ctx, const uint8_t* bg_prio) {
    if (!(ppu->lcdc & LCDC_OBJ_ENABLE)) {
        return;  /* Sprites disabled */
    }
    
    uint8_t scanline = ppu->ly;
    uint8_t sprite_height = (ppu->lcdc & LCDC_OBJ_SIZE) ? 16 : 8;
    
    /* Find sprites on this scanline (max 10) */
    int sprite_count = 0;
    int sprites[10];
    
    for (int i = 0; i < 40 && sprite_count < 10; i++) {
        OAMEntry* sprite = (OAMEntry*)(ctx->oam + i * 4);
        int sprite_y = sprite->y - 16;
        
        if (scanline >= sprite_y && scanline < sprite_y + sprite_height) {
            sprites[sprite_count++] = i;
        }
    }
    
    /* Render sprites in reverse order (priority - lower index = higher priority) */
    uint8_t *vram = ctx->vram;
    uint8_t *fb_row = &ppu->framebuffer[scanline * GB_SCREEN_WIDTH];

    for (int i = sprite_count - 1; i >= 0; i--) {
        OAMEntry* sprite = (OAMEntry*)(ctx->oam + sprites[i] * 4);
        int sprite_x = sprite->x - 8;

        /* Skip sprites entirely off-screen */
        if (sprite_x <= -8 || sprite_x >= GB_SCREEN_WIDTH) continue;

        uint8_t tile_idx = sprite->tile;
        if (sprite_height == 16) tile_idx &= 0xFE;

        int line = scanline - (sprite->y - 16);
        if (sprite->flags & OAM_FLIP_Y) line = sprite_height - 1 - line;

        /* Direct VRAM access (sprites always use 0x8000 addressing) */
        uint16_t tile_off = tile_idx * 16 + line * 2;
        uint8_t lo = vram[tile_off];
        uint8_t hi = vram[tile_off + 1];

        uint8_t palette = (sprite->flags & OAM_PALETTE) ? ppu->obp1 : ppu->obp0;
        bool behind_bg = (sprite->flags & OAM_PRIORITY);
        bool flip_x = (sprite->flags & OAM_FLIP_X);

        /* Clamp pixel loop to visible range */
        int px_start = (sprite_x < 0) ? -sprite_x : 0;
        int px_end = (sprite_x + 8 > GB_SCREEN_WIDTH) ? (GB_SCREEN_WIDTH - sprite_x) : 8;

        for (int px = px_start; px < px_end; px++) {
            int bit_pos = flip_x ? px : (7 - px);
            uint8_t color = ((lo >> bit_pos) & 1) | (((hi >> bit_pos) & 1) << 1);
            if (color == 0) continue;

            int screen_x = sprite_x + px;
            if (behind_bg && bg_prio && bg_prio[screen_x] != 0) continue;

            fb_row[screen_x] = (palette >> (color * 2)) & 0x03;
        }
    }
}

void ppu_render_scanline(GBPPU* ppu, GBContext* ctx) {
    if (ppu->ly == 0 && (ctx->cycles % 6000 == 0)) {  // Log occasionally
         // printf("[PPU] Frame debug: LCDC=%02X BGP=%02X\n", ppu->lcdc, ppu->bgp);
    }

    uint8_t bg_prio[GB_SCREEN_WIDTH];
    memset(bg_prio, 0, sizeof(bg_prio));

    render_bg_scanline(ppu, ctx, bg_prio);
    render_sprites_scanline(ppu, ctx, bg_prio);
    
    /* Debug: log first scanline render details */
    if (ppu->ly == 0) {
        // DBG_PPU("Rendered scanline 0 - LCDC=0x%02X, BGP=0x%02X, SCX=%d, SCY=%d",
        //        ppu->lcdc, ppu->bgp, ppu->scx, ppu->scy);
    }
}

/**
 * @brief Convert framebuffer to RGB
 */
static void convert_to_rgb(GBPPU* ppu) {
    for (int i = 0; i < GB_FRAMEBUFFER_SIZE; i++) {
        ppu->rgb_framebuffer[i] = dmg_palette[ppu->framebuffer[i] & 0x03];
    }
}

/* ============================================================================
 * PPU Mode State Machine
 * ========================================================================== */

/**
 * @brief Update STAT register mode bits
 */
static void update_stat(GBPPU* ppu, GBContext* ctx) {
    ppu->stat = (ppu->stat & ~STAT_MODE_MASK) | ppu->mode;
    
    /* Update LY=LYC flag */
    if (ppu->ly == ppu->lyc) {
        ppu->stat |= STAT_LYC_MATCH;
    } else {
        ppu->stat &= ~STAT_LYC_MATCH;
    }
    
    /* Write back to I/O */
    ctx->io[0x41] = ppu->stat;
    ctx->io[0x44] = ppu->ly;
}

/**
 * @brief Request LCD STAT interrupt if conditions met
 */
static void check_stat_interrupt(GBPPU* ppu, GBContext* ctx) {
    bool current_state = false;
    
    if ((ppu->stat & STAT_HBLANK_INT) && ppu->mode == PPU_MODE_HBLANK) {
        current_state = true;
    }
    if ((ppu->stat & STAT_VBLANK_INT) && ppu->mode == PPU_MODE_VBLANK) {
        current_state = true;
    }
    if ((ppu->stat & STAT_OAM_INT) && ppu->mode == PPU_MODE_OAM) {
        current_state = true;
    }
    if ((ppu->stat & STAT_LYC_INT) && (ppu->stat & STAT_LYC_MATCH)) {
        current_state = true;
    }
    
    /* Edge detection: only fire on rising edge */
    if (current_state && !ppu->stat_irq_state) {
        /* Request LCD STAT interrupt (IF bit 1) */
        /* DISABLE STAT INTERRUPTS FOR DEBUGGING TETRIS */
        ctx->io[0x0F] |= 0x02;
        // fprintf(stderr, "[PPU] STAT Interrupt Fired! Mode=%d STAT=0x%02X LY=%d LYC=%d\n", ppu->mode, ppu->stat, ppu->ly, ppu->lyc);
    }
    
    ppu->stat_irq_state = current_state;
}



void ppu_tick(GBPPU* ppu, GBContext* ctx, uint32_t cycles) {
    if (!(ppu->lcdc & LCDC_LCD_ENABLE)) {
        return;  /* LCD disabled */
    }
    
    ppu->mode_cycles += cycles;
    
    switch (ppu->mode) {
        case PPU_MODE_OAM:
            if (ppu->mode_cycles >= CYCLES_OAM_SCAN) {
                ppu->mode_cycles -= CYCLES_OAM_SCAN;
                /* Latch scroll registers for this scanline */
                ppu->latched_scx = ppu->scx;
                ppu->latched_scy = ppu->scy;

                /* Count sprites on this scanline for variable mode 3 timing */
                {
                    uint8_t sprite_height = (ppu->lcdc & LCDC_OBJ_SIZE) ? 16 : 8;
                    uint8_t count = 0;
                    for (int i = 0; i < 40 && count < 10; i++) {
                        uint8_t sy = ctx->oam[i * 4] - 16;
                        if (ppu->ly >= sy && ppu->ly < sy + sprite_height)
                            count++;
                    }
                    ppu->scanline_sprite_count = count;

                    /* Mode 3 base = 172, +SCX fine scroll penalty, +6 per sprite */
                    uint32_t draw = 172 + (ppu->latched_scx & 7) + count * 6;
                    /* Mode 3 + Mode 0 = 376 cycles (456 - 80 OAM) */
                    uint32_t hblank = (draw < 376) ? (376 - draw) : 0;
                    ppu->scanline_draw_cycles = draw;
                    ppu->scanline_hblank_cycles = hblank;
                }

                ppu->mode = PPU_MODE_DRAW;
                update_stat(ppu, ctx);
            }
            break;
            
        case PPU_MODE_DRAW:
            if (ppu->mode_cycles >= ppu->scanline_draw_cycles) {
                ppu->mode_cycles -= ppu->scanline_draw_cycles;
                
                /* Render the scanline */
                ppu_render_scanline(ppu, ctx);
                
                ppu->mode = PPU_MODE_HBLANK;
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx);
            }
            break;
            
        case PPU_MODE_HBLANK:
            if (ppu->mode_cycles >= ppu->scanline_hblank_cycles) {
                ppu->mode_cycles -= ppu->scanline_hblank_cycles;
                ppu->ly++;
                
                if (ppu->ly >= VISIBLE_SCANLINES) {
                    /* Enter VBlank */
                    ppu->mode = PPU_MODE_VBLANK;
                    
                    /* Convert framebuffer to RGB - only if not already ready */
                    if (!ppu->frame_ready) {
                        convert_to_rgb(ppu);
                        ppu->frame_ready = true;
                        ctx->frame_done = 1;
                    }
                    
                    /* Request VBlank interrupt (IF bit 0) */
                    ctx->io[0x0F] |= 0x01;
                } else {
                    ppu->mode = PPU_MODE_OAM;
                }
                
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx);
            }
            break;
            
        case PPU_MODE_VBLANK:
            if (ppu->mode_cycles >= CYCLES_SCANLINE) {
                ppu->mode_cycles -= CYCLES_SCANLINE;
                ppu->ly++;
                
                if (ppu->ly >= TOTAL_SCANLINES) {
                    /* Frame complete - start new frame */
                    ppu->ly = 0;
                    ppu->window_line = 0;
                    ppu->window_triggered = false;
                    ppu->mode = PPU_MODE_OAM;
                }
                
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx);
            }
            break;
    }
}

/* ============================================================================
 * Register Access
 * ========================================================================== */

uint8_t ppu_read_register(GBPPU* ppu, uint16_t addr) {
    switch (addr) {
        case 0xFF40: return ppu->lcdc;
        case 0xFF41: return ppu->stat | 0x80;  /* Bit 7 always 1 */
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
        default: return 0xFF;
    }
}

void ppu_write_register(GBPPU* ppu, GBContext* ctx, uint16_t addr, uint8_t value) {
    static int ppu_write_count = 0;
    ppu_write_count++;
    
    /* Only log first 100 and special values */
    if (ppu_write_count <= 100 || (addr == 0xFF40 && (value == 0x91 || value == 0x00))) {
        DBG_REGS("PPU write #%d: addr=0x%04X value=0x%02X (A=0x%02X)", 
                 ppu_write_count, addr, value, ctx->a);
    }
    
    switch (addr) {
        case 0xFF40:
            DBG_REGS("LCDC: 0x%02X -> 0x%02X (LCD=%s, BG=%s, OBJ=%s)",
                     ppu->lcdc, value,
                     (value & 0x80) ? "ON" : "OFF",
                     (value & 0x01) ? "ON" : "OFF",
                     (value & 0x02) ? "ON" : "OFF");
            /* Check if LCD is being turned off */
            if ((ppu->lcdc & LCDC_LCD_ENABLE) && !(value & LCDC_LCD_ENABLE)) {
                /* LCD turned off - reset to line 0 */
                ppu->ly = 0;
                ppu->window_line = 0;
                ppu->window_triggered = false;
                ppu->mode = PPU_MODE_HBLANK; /* Mode 0 */
                ppu->mode_cycles = 0;
                ctx->io[0x44] = 0;
                /* Clear frame ready to avoid stale frame rendering */
                ppu->frame_ready = false;
                fprintf(stderr, "[LCD] OFF — LY reset to 0, mode=HBLANK\n");
            }
            /* Check if LCD is being turned on */
            if (!(ppu->lcdc & LCDC_LCD_ENABLE) && (value & LCDC_LCD_ENABLE)) {
                /* LCD re-enabled — start fresh frame from OAM mode */
                ppu->ly = 0;
                ppu->mode = PPU_MODE_OAM;
                ppu->mode_cycles = 0;
                ppu->window_line = 0;
                ppu->window_triggered = false;
                ctx->io[0x44] = 0;
                update_stat(ppu, ctx);
                fprintf(stderr, "[LCD] ON  — reset to LY=0 mode=OAM\n");
            }
            ppu->lcdc = value;
            break;
        case 0xFF41:
            /* Bits 0-2 are read-only */
            ppu->stat = (ppu->stat & 0x07) | (value & 0x78);
            break;
        case 0xFF42: ppu->scy = value; break;
        case 0xFF43: ppu->scx = value; break;
        case 0xFF45: 
            ppu->lyc = value;
            /* Immediately check for LYC match */
            if (ppu->lcdc & LCDC_LCD_ENABLE) {
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx);
            }
            break;
        case 0xFF46:
            /* OAM DMA transfer */
            DBG_REGS("DMA transfer from 0x%04X", (uint16_t)(value << 8));
            ppu->dma = value;
            {
                uint16_t src = value << 8;
                for (int i = 0; i < OAM_SIZE; i++) {
                    ctx->oam[i] = gb_read8(ctx, src + i);
                }
            }
            break;
        case 0xFF47: 
            DBG_REGS("BGP palette: 0x%02X -> 0x%02X", ppu->bgp, value);
            ppu->bgp = value; 
            break;
        case 0xFF48: ppu->obp0 = value; break;
        case 0xFF49: ppu->obp1 = value; break;
        case 0xFF4A: ppu->wy = value; break;
        case 0xFF4B: ppu->wx = value; break;
    }
}

/* ============================================================================
 * Frame Handling
 * ========================================================================== */

bool ppu_frame_ready(GBPPU* ppu) {
    return ppu->frame_ready;
}

static int clear_debug = 0;
void ppu_clear_frame_ready(GBPPU* ppu) {
    ppu->frame_ready = false;
}

const uint32_t* ppu_get_framebuffer(GBPPU* ppu) {
    return ppu->rgb_framebuffer;
}
