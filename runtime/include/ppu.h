/**
 * @file ppu.h
 * @brief GameBoy PPU (Pixel Processing Unit) for graphics rendering
 */

#ifndef GB_PPU_H
#define GB_PPU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ========================================================================== */

#define GB_SCREEN_WIDTH    160
#define GB_SCREEN_HEIGHT   144
#define GB_FRAMEBUFFER_SIZE (GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT)

/* Scanline timing (in cycles) */
#define CYCLES_OAM_SCAN    80   /* Mode 2: OAM search */
#define CYCLES_PIXEL_DRAW  172  /* Mode 3: Pixel transfer (variable) */
#define CYCLES_HBLANK      204  /* Mode 0: H-Blank (variable) */
#define CYCLES_SCANLINE    456  /* Total cycles per scanline */
#define CYCLES_VBLANK      4560 /* 10 scanlines * 456 */

/* Number of scanlines */
#define VISIBLE_SCANLINES  144
#define VBLANK_SCANLINES   10
#define TOTAL_SCANLINES    154

/* VRAM/OAM sizes */
#define VRAM_SIZE          0x2000
#define OAM_SIZE           0xA0
#define TILES_PER_BANK     384
#define TILE_SIZE          16   /* 8x8 pixels, 2 bits per pixel */

/* ============================================================================
 * LCD Control Register (LCDC - 0xFF40)
 * ========================================================================== */

#define LCDC_BG_ENABLE      0x01  /* Bit 0: BG/Window enable */
#define LCDC_OBJ_ENABLE     0x02  /* Bit 1: OBJ (sprite) enable */
#define LCDC_OBJ_SIZE       0x04  /* Bit 2: OBJ size (0=8x8, 1=8x16) */
#define LCDC_BG_TILEMAP     0x08  /* Bit 3: BG tile map area */
#define LCDC_TILE_DATA      0x10  /* Bit 4: BG/Window tile data area */
#define LCDC_WINDOW_ENABLE  0x20  /* Bit 5: Window enable */
#define LCDC_WINDOW_TILEMAP 0x40  /* Bit 6: Window tile map area */
#define LCDC_LCD_ENABLE     0x80  /* Bit 7: LCD enable */

/* ============================================================================
 * LCD Status Register (STAT - 0xFF41)
 * ========================================================================== */

#define STAT_MODE_MASK      0x03  /* Bits 0-1: Mode flag */
#define STAT_LYC_MATCH      0x04  /* Bit 2: LY=LYC coincidence */
#define STAT_HBLANK_INT     0x08  /* Bit 3: Mode 0 H-Blank interrupt */
#define STAT_VBLANK_INT     0x10  /* Bit 4: Mode 1 V-Blank interrupt */
#define STAT_OAM_INT        0x20  /* Bit 5: Mode 2 OAM interrupt */
#define STAT_LYC_INT        0x40  /* Bit 6: LY=LYC interrupt */

/* PPU Modes */
typedef enum {
    PPU_MODE_HBLANK = 0,  /* Horizontal blank */
    PPU_MODE_VBLANK = 1,  /* Vertical blank */
    PPU_MODE_OAM    = 2,  /* OAM search */
    PPU_MODE_DRAW   = 3,  /* Pixel transfer */
} PPUMode;

/* ============================================================================
 * OAM Entry (Sprite Attributes)
 * ========================================================================== */

typedef struct {
    uint8_t y;          /* Y position - 16 */
    uint8_t x;          /* X position - 8 */
    uint8_t tile;       /* Tile index */
    uint8_t flags;      /* Attributes */
} OAMEntry;

/* OAM Flags */
#define OAM_PALETTE     0x10  /* Bit 4: Palette (DMG only) */
#define OAM_FLIP_X      0x20  /* Bit 5: X flip */
#define OAM_FLIP_Y      0x40  /* Bit 6: Y flip */
#define OAM_PRIORITY    0x80  /* Bit 7: Priority (0=above BG, 1=behind) */

/* CGB OAM Flags */
#define OAM_CGB_BANK    0x08  /* Bit 3: VRAM bank (CGB only) */
#define OAM_CGB_PALETTE 0x07  /* Bits 0-2: Palette number (CGB only) */

/* A sprite selected during the OAM scan for the current scanline. The list is
 * built once (OAM/mode 2 is fixed), then composited per mode-3 segment using
 * LIVE obj-enable + OBP0/OBP1 so mid-mode-3 writes (Mealybug m3_obp0/obj_en)
 * affect only later dots. Tile bytes are fetched at scan time (size = latched). */
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

/* ============================================================================
 * PPU State
 * ========================================================================== */

typedef struct GBContext GBContext;

typedef struct GBPPU {
    /* LCD Registers (cached from I/O memory) */
    uint8_t lcdc;       /* 0xFF40 - LCD Control */
    uint8_t stat;       /* 0xFF41 - LCD Status */
    uint8_t scy;        /* 0xFF42 - Scroll Y */
    uint8_t scx;        /* 0xFF43 - Scroll X */
    uint8_t ly;         /* 0xFF44 - Current scanline */
    uint8_t lyc;        /* 0xFF45 - LY Compare */
    uint8_t dma;        /* 0xFF46 - DMA Transfer */
    uint8_t bgp;        /* 0xFF47 - BG Palette */
    uint8_t obp0;       /* 0xFF48 - OBJ Palette 0 */
    uint8_t obp1;       /* 0xFF49 - OBJ Palette 1 */
    uint8_t wy;         /* 0xFF4A - Window Y */
    uint8_t wx;         /* 0xFF4B - Window X */
    uint8_t bgpi;       /* 0xFF68 - BG palette index */
    uint8_t obpi;       /* 0xFF6A - OBJ palette index */
    uint8_t opri;       /* Internal object priority mode */

    /* Registers latched at the start of mode 3 for the active scanline */
    uint8_t latched_lcdc;
    uint8_t latched_scy;
    uint8_t latched_scx;
    uint8_t latched_bgp;
    uint8_t latched_obp0;
    uint8_t latched_obp1;
    uint8_t latched_wy;
    uint8_t latched_wx;
    
    /* Internal state */
    bool stat_irq_state;
    PPUMode mode;
    uint32_t mode_cycles;     /* Cycles in current mode */
    uint8_t window_line;      /* Current window internal line counter */
    bool window_triggered;    /* Window was triggered this frame */
    bool lcd_on_first_line;   /* Line 0 immediately after LCD-enable: shorter mode 2
                               * and mode 0 per hardware (SameBoy display.c:1664-1714).
                               * Consumed at that line's HBLANK end. */

    /* Per-scanline timing (computed during OAM scan) */
    uint32_t scanline_draw_cycles;   /* Variable mode 3 duration */
    uint32_t scanline_hblank_cycles; /* Adjusted mode 0 duration */
    uint8_t  scanline_sprite_count;  /* Sprites found on this line */

    /* Segmented mid-scanline (mode 3) rendering state. The BG/window scanline is
     * drawn in pixel ranges as mode-3 cycles elapse, sampling LIVE registers per
     * segment, so mid-mode-3 register writes (Mealybug m3_*) affect later dots. */
    int      render_x;               /* Next BG pixel to draw this scanline (0..160) */
    bool     win_rendered_line;      /* Window drawn on this scanline (window_line++) */
    uint8_t  bg_raw_line[GB_SCREEN_WIDTH];      /* Persistent across segments */
    uint8_t  bg_priority_line[GB_SCREEN_WIDTH];

    /* Sprite list for the current scanline: built once (OAM scan, latched OBJ
     * size), then composited per mode-3 segment with LIVE obj-enable + OBP0/OBP1.
     * Parallels render_x so sprites are drawn in the same dot ranges as the BG. */
    ScanlineSprite scanline_sprites[10];
    int      scanline_sprite_list_count;
    bool     sprite_list_built;      /* List built for this scanline yet? */
    
    /* Framebuffer (2-bit color indices) */
    uint8_t framebuffer[GB_FRAMEBUFFER_SIZE];

    /* 15-bit color framebuffer for CGB palettes */
    uint16_t color_framebuffer[GB_FRAMEBUFFER_SIZE];

    /* RGB framebuffer for display (32-bit RGBA) */
    uint32_t rgb_framebuffer[GB_FRAMEBUFFER_SIZE];

    /* CGB palette RAM */
    uint8_t bg_palette_ram[0x40];
    uint8_t obj_palette_ram[0x40];
    
    /* Frame complete flag */
    bool frame_ready;
    
} GBPPU;

/* ============================================================================
 * PPU Functions
 * ========================================================================== */

/**
 * @brief Initialize PPU
 */
void ppu_init(GBPPU* ppu);

/**
 * @brief Reset PPU to initial state
 */
void ppu_reset(GBPPU* ppu, const GBContext* ctx);

/**
 * Re-apply the CGB BIOS DMG-compat palette for the current cart.
 * Reads ctx->cgb_compat_palette_override (set by the menu) and ctx->rom
 * (for the title-hash lookup when override is -1), writes the resulting
 * BG/OBJ palette colors into CGB palette RAM. No-op outside CGB compat
 * mode. Safe to call mid-session — the next rendered frame picks up
 * the new colors. */
void ppu_reload_cgb_compat_palette(GBPPU* ppu, const GBContext* ctx);

/**
 * @brief Tick the PPU for a number of cycles
 */
void ppu_tick(GBPPU* ppu, GBContext* ctx, uint32_t cycles);

/**
 * @brief Read LCD register
 */
uint8_t ppu_read_register(GBPPU* ppu, uint16_t addr);

/**
 * @brief Write LCD register
 */
void ppu_write_register(GBPPU* ppu, GBContext* ctx, uint16_t addr, uint8_t value);

/**
 * @brief Check if frame is ready
 */
bool ppu_frame_ready(GBPPU* ppu);

/**
 * @brief Clear frame ready flag
 */
void ppu_clear_frame_ready(GBPPU* ppu);

/**
 * @brief Get the RGB framebuffer
 */
const uint32_t* ppu_get_framebuffer(GBPPU* ppu);

/**
 * @brief Render a scanline
 */
void ppu_render_scanline(GBPPU* ppu, GBContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* GB_PPU_H */
