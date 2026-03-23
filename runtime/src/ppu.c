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
    ppu->stat = 0x01;  /* Mode 1 (VBlank) - post-bootrom state */
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
    ppu->latched_lcdc = ppu->lcdc;
    ppu->latched_scy = ppu->scy;
    ppu->latched_scx = ppu->scx;
    ppu->latched_bgp = ppu->bgp;
    ppu->latched_obp0 = ppu->obp0;
    ppu->latched_obp1 = ppu->obp1;
    ppu->latched_wy = ppu->wy;
    ppu->latched_wx = ppu->wx;
    
    /* Internal state - post-bootrom DMG state:
     * After the DMG boot ROM completes, the PPU is in VBlank at LY=145 (0x91).
     * div_counter=0xABCC, mode=VBlank, STAT mode bits=1.
     * mode_cycles starts at ~4 (just transitioned to LY=145). */
    ppu->ly = 145;
    ppu->mode = PPU_MODE_VBLANK;
    ppu->mode_cycles = 4;
    ppu->window_line = 0;
    ppu->window_triggered = false;
    ppu->frame_ready = false;
    
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
static uint16_t get_tile_data_addr(uint8_t lcdc, uint8_t tile_idx, bool is_obj) {
    if (is_obj || (lcdc & LCDC_TILE_DATA)) {
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
static uint16_t get_bg_tilemap_addr(uint8_t lcdc) {
    return (lcdc & LCDC_BG_TILEMAP) ? 0x9C00 : 0x9800;
}

static uint16_t get_window_tilemap_addr(uint8_t lcdc) {
    return (lcdc & LCDC_WINDOW_TILEMAP) ? 0x9C00 : 0x9800;
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
 * Scanline Rendering
 * ========================================================================== */

/**
 * @brief Apply palette to a 2-bit color value
 */
static uint8_t apply_palette(uint8_t color, uint8_t palette) {
    return (palette >> (color * 2)) & 0x03;
}

/**
 * @brief Render background/window for current scanline
 */
static void render_bg_scanline(GBPPU* ppu, GBContext* ctx, uint8_t* bg_prio) {
    uint8_t scanline = ppu->ly;
    uint8_t lcdc = ppu->latched_lcdc;
    
    if (!(lcdc & LCDC_LCD_ENABLE)) {
        /* LCD disabled - blank line */
        memset(&ppu->framebuffer[scanline * GB_SCREEN_WIDTH], 0, GB_SCREEN_WIDTH);
        if (bg_prio) memset(bg_prio, 0, GB_SCREEN_WIDTH);
        return;
    }
    
    bool bg_enable = (lcdc & LCDC_BG_ENABLE);
    /* Note: on DMG, LCDC_BG_ENABLE (bit 0) also controls Master Enable (BG+Window). 
       On CGB, it controls priority. Assuming DMG mostly here. */
    bool window_enable = bg_enable && (lcdc & LCDC_WINDOW_ENABLE) &&
                         (ppu->latched_wx <= 166) && (ppu->latched_wy <= scanline);
    
    /* Track if window was triggered */
    if (window_enable && !ppu->window_triggered) {
        ppu->window_triggered = true;
    }
    
    for (int x = 0; x < GB_SCREEN_WIDTH; x++) {
        uint8_t color = 0;
        
        /* Check if we're in window area */
        bool in_window = window_enable && (x >= (ppu->latched_wx - 7));
        
        if (in_window) {
            /* Render window */
            int win_x = x - (ppu->latched_wx - 7);
            int win_y = ppu->window_line;
            
            /* Get tile from window tilemap */
            uint16_t tilemap_addr = get_window_tilemap_addr(lcdc);
            uint8_t tile_x = win_x / 8;
            uint8_t tile_y = win_y / 8;
            uint8_t tile_idx = vram_read(ctx, tilemap_addr + tile_y * 32 + tile_x);
            
            /* Get pixel from tile */
            uint16_t tile_addr = get_tile_data_addr(lcdc, tile_idx, false);
            uint8_t pixel_y = win_y % 8;
            uint8_t pixel_x = win_x % 8;
            
            uint8_t lo = vram_read(ctx, tile_addr + pixel_y * 2);
            uint8_t hi = vram_read(ctx, tile_addr + pixel_y * 2 + 1);
            
            uint8_t bit = 7 - pixel_x;
            color = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
        } else if (bg_enable) {
            /* Render background */
            int bg_x = (x + ppu->latched_scx) & 0xFF;
            int bg_y = (scanline + ppu->latched_scy) & 0xFF;
            
            /* Get tile from background tilemap */
            uint16_t tilemap_addr = get_bg_tilemap_addr(lcdc);
            uint8_t tile_x = bg_x / 8;
            uint8_t tile_y = bg_y / 8;
            uint8_t tile_idx = vram_read(ctx, tilemap_addr + tile_y * 32 + tile_x);
            
            /* Get pixel from tile */
            uint16_t tile_addr = get_tile_data_addr(lcdc, tile_idx, false);
            uint8_t pixel_y = bg_y % 8;
            uint8_t pixel_x = bg_x % 8;
            
            uint8_t lo = vram_read(ctx, tile_addr + pixel_y * 2);
            uint8_t hi = vram_read(ctx, tile_addr + pixel_y * 2 + 1);
            
            uint8_t bit = 7 - pixel_x;
            color = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
        }
        
        /* Apply palette and store */
        ppu->framebuffer[scanline * GB_SCREEN_WIDTH + x] = apply_palette(color, ppu->latched_bgp);
        if (bg_prio) bg_prio[x] = color;
    }
    
    /* Increment window line counter if window was used */
    if (ppu->window_triggered && window_enable) {
        ppu->window_line++;
    }
}

/**
 * @brief Render sprites for current scanline
 */
static void render_sprites_scanline(GBPPU* ppu, GBContext* ctx, const uint8_t* bg_prio) {
    if (!(ppu->latched_lcdc & LCDC_OBJ_ENABLE)) {
        return;  /* Sprites disabled */
    }
    
    uint8_t scanline = ppu->ly;
    uint8_t sprite_height = (ppu->latched_lcdc & LCDC_OBJ_SIZE) ? 16 : 8;

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
    
    /* Find sprites on this scanline (max 10) */
    int sprite_count = 0;
    ScanlineSprite sprites[10];
    
    for (int i = 0; i < 40 && sprite_count < 10; i++) {
        OAMEntry* sprite = (OAMEntry*)(ctx->oam + i * 4);
        int sprite_y = sprite->y - 16;
        
        if (scanline >= sprite_y && scanline < sprite_y + sprite_height) {
            int line = scanline - sprite_y;
            uint8_t tile_idx = sprite->tile;
            if (sprite_height == 16) {
                tile_idx &= 0xFE;  /* Clear bit 0 for 8x16 sprites */
            }

            if (sprite->flags & OAM_FLIP_Y) {
                line = sprite_height - 1 - line;
            }

            uint16_t tile_addr = 0x8000 + tile_idx * 16 + line * 2;
            sprites[sprite_count].oam_index = i;
            sprites[sprite_count].screen_x = sprite->x - 8;
            sprites[sprite_count].x_pos = sprite->x;
            sprites[sprite_count].flags = sprite->flags;
            sprites[sprite_count].palette = (sprite->flags & OAM_PALETTE) ? ppu->latched_obp1 : ppu->latched_obp0;
            sprites[sprite_count].behind_bg = (sprite->flags & OAM_PRIORITY) != 0;
            sprites[sprite_count].lo = vram_read(ctx, tile_addr);
            sprites[sprite_count].hi = vram_read(ctx, tile_addr + 1);
            sprite_count++;
        }
    }

    /* On DMG, smaller X has higher priority; ties are resolved by OAM order. */
    for (int i = 1; i < sprite_count; i++) {
        ScanlineSprite sprite = sprites[i];
        int j = i - 1;

        while (j >= 0) {
            bool sprite_has_higher_priority = (sprite.x_pos < sprites[j].x_pos) ||
                                              (sprite.x_pos == sprites[j].x_pos &&
                                               sprite.oam_index < sprites[j].oam_index);
            if (!sprite_has_higher_priority) {
                break;
            }
            sprites[j + 1] = sprites[j];
            j--;
        }

        sprites[j + 1] = sprite;
    }

    for (int screen_x = 0; screen_x < GB_SCREEN_WIDTH; screen_x++) {
        const ScanlineSprite* chosen_sprite = NULL;
        uint8_t chosen_color = 0;

        for (int i = 0; i < sprite_count; i++) {
            const ScanlineSprite* sprite = &sprites[i];
            int sprite_px = screen_x - sprite->screen_x;
            if (sprite_px < 0 || sprite_px >= 8) {
                continue;
            }

            int bit_pos = (sprite->flags & OAM_FLIP_X) ? sprite_px : (7 - sprite_px);
            uint8_t color = ((sprite->lo >> bit_pos) & 1) | (((sprite->hi >> bit_pos) & 1) << 1);
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

        uint8_t bg_raw_color = bg_prio ? bg_prio[screen_x] : 0;
        if (chosen_sprite->behind_bg && bg_raw_color != 0) {
            continue;
        }

        ppu->framebuffer[scanline * GB_SCREEN_WIDTH + screen_x] =
            apply_palette(chosen_color, chosen_sprite->palette);
    }
}

void ppu_render_scanline(GBPPU* ppu, GBContext* ctx) {
    if (ppu->ly == 0 && (ctx->cycles % 6000 == 0)) {  // Log occasionally
         // printf("[PPU] Frame debug: LCDC=%02X BGP=%02X\n", ppu->lcdc, ppu->bgp);
    }

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
    /* Debug: check if framebuffer has any non-zero pixels */
    static int convert_count = 0;
    bool has_content = dbg_has_nonzero_pixels(ppu->framebuffer, GB_FRAMEBUFFER_SIZE);
    
    for (int i = 0; i < GB_FRAMEBUFFER_SIZE; i++) {
        ppu->rgb_framebuffer[i] = dmg_palette[ppu->framebuffer[i] & 0x03];
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
static void check_stat_interrupt(GBPPU* ppu, GBContext* ctx, const char* reason) {
    uint8_t source_state_mask = 0;
    uint8_t source_enable_mask = 0;
    uint8_t active_source_mask = 0;
    bool previous_state = ppu->stat_irq_state;
    bool current_state = false;

    if (ppu->mode == PPU_MODE_HBLANK) {
        source_state_mask |= 0x1;
    }
    if (ppu->mode == PPU_MODE_VBLANK) {
        source_state_mask |= 0x2;
    }
    if (ppu->mode == PPU_MODE_OAM) {
        source_state_mask |= 0x4;
    }
    if (ppu->stat & STAT_LYC_MATCH) {
        source_state_mask |= 0x8;
    }

    if (ppu->stat & STAT_HBLANK_INT) {
        source_enable_mask |= 0x1;
    }
    if (ppu->stat & STAT_VBLANK_INT) {
        source_enable_mask |= 0x2;
    }
    if (ppu->stat & STAT_OAM_INT) {
        source_enable_mask |= 0x4;
    }
    if (ppu->stat & STAT_LYC_INT) {
        source_enable_mask |= 0x8;
    }

    active_source_mask = source_state_mask & source_enable_mask;
    current_state = (active_source_mask != 0);

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

    /* Edge detection: only fire on rising edge */
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
        return;  /* LCD disabled */
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
                
                /* Render the scanline once the pixel-transfer phase completes. */
                ppu_render_scanline(ppu, ctx);
                
                ppu->mode = PPU_MODE_HBLANK;
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx, "draw->hblank");
                break;
                
            case PPU_MODE_HBLANK:
                if (ppu->mode_cycles < CYCLES_HBLANK) {
                    return;
                }
                ppu->mode_cycles -= CYCLES_HBLANK;
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
                check_stat_interrupt(ppu, ctx, "hblank->next");
                break;
                
            case PPU_MODE_VBLANK:
                if (ppu->mode_cycles < CYCLES_SCANLINE) {
                    return;
                }
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
    uint8_t old_value = ppu_read_register(ppu, addr);
    
    /* Only log first 100 and special values */
    if (ppu_write_count <= 100 || (addr == 0xFF40 && (value == 0x91 || value == 0x00))) {
        DBG_REGS("PPU write #%d: addr=0x%04X value=0x%02X (A=0x%02X)", 
                 ppu_write_count, addr, value, ctx->a);
    }
    
    switch (addr) {
        case 0xFF40:
            {
            uint8_t old_lcdc = ppu->lcdc;
            DBG_REGS("LCDC: 0x%02X -> 0x%02X (LCD=%s, BG=%s, OBJ=%s)", 
                     old_lcdc, value,
                     (value & 0x80) ? "ON" : "OFF",
                     (value & 0x01) ? "ON" : "OFF", 
                     (value & 0x02) ? "ON" : "OFF");
            ppu->lcdc = value;
            /* Check if LCD is being turned off */
            if ((old_lcdc & LCDC_LCD_ENABLE) && !(value & LCDC_LCD_ENABLE)) {
                /* LCD turned off - reset to line 0 */
                ppu->ly = 0;
                ppu->window_line = 0;
                ppu->window_triggered = false;
                ppu->mode = PPU_MODE_HBLANK; /* Mode 0 when LCD is off (Pan Docs) */
                ppu->mode_cycles = 0;
                ctx->io[0x44] = 0;
                /* Update STAT mode bits to reflect mode 0 immediately */
                update_stat(ppu, ctx);
                /* Clear frame ready to avoid stale frame rendering */
                ppu->frame_ready = false;
                gbrt_note_lcd_transition(ctx, false, old_lcdc, value, ppu->ly, ppu->mode);
                DBG_REGS("LCD turned OFF - reset LY to 0");
            } else if (!(old_lcdc & LCDC_LCD_ENABLE) && (value & LCDC_LCD_ENABLE)) {
                /* LCD turned on - restart from line 0, mode 2 */
                ppu->ly = 0;
                ppu->window_line = 0;
                ppu->window_triggered = false;
                ppu->mode = PPU_MODE_OAM;
                ppu->mode_cycles = 0;
                ppu->frame_ready = false;
                ctx->io[0x44] = 0;
                update_stat(ppu, ctx);
                gbrt_note_lcd_transition(ctx, true, old_lcdc, value, ppu->ly, ppu->mode);
                DBG_REGS("LCD turned ON - restart at LY 0 OAM");
            }
            break;
            }
        case 0xFF41:
            /* Bits 0-2 are read-only */
            ppu->stat = (ppu->stat & 0x07) | (value & 0x78);
            if (ppu->lcdc & LCDC_LCD_ENABLE) {
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx, "stat-write");
            }
            break;
        case 0xFF42: ppu->scy = value; break;
        case 0xFF43: ppu->scx = value; break;
        case 0xFF45: 
            ppu->lyc = value;
            /* Immediately check for LYC match */
            if (ppu->lcdc & LCDC_LCD_ENABLE) {
                update_stat(ppu, ctx);
                check_stat_interrupt(ppu, ctx, "lyc-write");
            }
            break;
        case 0xFF46:
            ppu->dma = value;
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

    gbrt_log_ppu_register_write(ctx, addr, old_value, ppu_read_register(ppu, addr), ppu->ly, ppu->mode);
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
