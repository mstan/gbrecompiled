/*
 * gb_fb_oracle.c — SameBoy framebuffer reference for the GB PPU axis (5a).
 * Renders (skip-boot DMG) and dumps a P6 PPM, to pixel-diff against the recomp.
 * Usage:
 *   gb_fb_oracle <rom> <out.ppm> <frame> [dmg|cgb] [ldbb]
 *
 * Capture timing:
 *   - default: after exactly <frame> GB_run_frame() calls (good for normal games
 *     whose screen is stable, e.g. the Pokemon Red oracle frames).
 *   - "ldbb":  at the `LD B,B` (opcode 0x40) software breakpoint, which is how
 *     the Mealybug Tearoom tests signal "capture now" (their reference PNGs are
 *     taken at that instruction). Several m3_* tests render the pattern, hit
 *     LD B,B, then keep running and overwrite the screen — so a fixed-frame
 *     capture sees a blank/stale screen. <frame> becomes a max-frames safety cap.
 */
#include "gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define DMG_W 160
#define DMG_H 144

static uint32_t g_fb[256 * 224];        /* live SameBoy output buffer */
static uint32_t g_capture[256 * 224];   /* snapshot taken at the LD B,B breakpoint */
static int g_vblank = 0;
static int g_captured = 0;              /* LD B,B seen + framebuffer snapshotted */

static uint32_t rgb_encode(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b)
{
    (void)gb;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
static void on_vblank(GB_gameboy_t *gb, GB_vblank_type_t type)
{
    (void)gb;
    if (type == GB_VBLANK_TYPE_NORMAL_FRAME) g_vblank = 1;
}
/* Snapshot the framebuffer at the first `LD B,B` (0x40) — the Mealybug capture
 * breakpoint. The callback fires before the opcode executes; the test has just
 * finished rendering the frame, so g_fb holds the result to be captured. */
static void on_execution(GB_gameboy_t *gb, uint16_t address, uint8_t opcode)
{
    (void)gb; (void)address;
    if (opcode == 0x40 && !g_captured) {        /* LD B,B — Mealybug capture breakpoint */
        memcpy(g_capture, g_fb, sizeof(g_capture));
        g_captured = 1;
    }
}
/* Replicate the VRAM tile data the DMG boot ROM leaves behind: the Nintendo
 * logo (cart header $104-$133) expanded into tiles 1-24, plus the fixed ®
 * trademark symbol in tile 25. Several Mealybug sprite tests rely on these
 * tiles existing in VRAM and never load them themselves (m3_obp0_change's
 * sprites all use tile $19 = the ®). Algorithm transcribed from SameBoy's
 * dmg_boot.asm (DoubleBitsAndWriteRow + TrademarkSymbol): each logo byte's two
 * nibbles are each bit-doubled to an 8-pixel row and written to two consecutive
 * rows' low bit-plane (high plane left 0). */
static void load_boot_logo_vram(const uint8_t *rom, uint8_t *vram)
{
    static const uint8_t trademark[8] = {0x3C,0x42,0xB9,0xA5,0xB9,0xA5,0x42,0x3C};
    int hl = 0x10;  /* VRAM offset of tile 1 ($8010) */
    for (int i = 0; i < 48; i++) {
        uint8_t b = rom[0x104 + i];
        for (int half = 0; half < 2; half++) {   /* top nibble, then low nibble */
            uint8_t c = 0;
            for (int k = 0; k < 4; k++) {
                uint8_t bit = (uint8_t)((b >> 7) & 1);  /* sla b: MSB out */
                b = (uint8_t)(b << 1);
                c = (uint8_t)((c << 1) | bit);          /* rl c twice = double */
                c = (uint8_t)((c << 1) | bit);
            }
            vram[hl] = c; hl += 2;   /* low plane of row N   (high plane = 0) */
            vram[hl] = c; hl += 2;   /* low plane of row N+1 (vertical double) */
        }
    }
    for (int i = 0; i < 8; i++) {    /* ® trademark into tile 25 even offsets */
        vram[hl] = trademark[i]; hl += 2;
    }
}

/* Replicate the DMG state left by the boot ROM. Setting just the CPU registers
 * is NOT enough: the boot ROM leaves the LCD ON (LCDC=$91). Several test ROMs
 * (e.g. the Mealybug framework's reset_registers) `call wait_vblank` FIRST,
 * which spins until rLY==$90 — with the LCD off the PPU never runs, LY stays 0,
 * and the ROM hangs forever (observed as: stuck in reset_registers, all IO
 * zeroed, no interrupts). Write the IO via GB_write_memory so SameBoy's PPU
 * enable path runs. Values per Pan Docs "Power-Up Sequence" (DMG). */
static void skip_boot_dmg(GB_gameboy_t *gb)
{
    gb->af = 0x01B0; gb->bc = 0x0013; gb->de = 0x00D8; gb->hl = 0x014D;
    gb->sp = 0xFFFE; gb->pc = 0x0100; gb->boot_rom_finished = true;

    load_boot_logo_vram(gb->rom, gb->vram);

    GB_write_memory(gb, 0xFF05, 0x00);  /* TIMA */
    GB_write_memory(gb, 0xFF06, 0x00);  /* TMA  */
    GB_write_memory(gb, 0xFF07, 0x00);  /* TAC  */
    GB_write_memory(gb, 0xFF0F, 0xE1);  /* IF   */
    GB_write_memory(gb, 0xFF40, 0x91);  /* LCDC — LCD on (critical) */
    GB_write_memory(gb, 0xFF42, 0x00);  /* SCY  */
    GB_write_memory(gb, 0xFF43, 0x00);  /* SCX  */
    GB_write_memory(gb, 0xFF45, 0x00);  /* LYC  */
    GB_write_memory(gb, 0xFF47, 0xFC);  /* BGP  */
    GB_write_memory(gb, 0xFF48, 0xFF);  /* OBP0 */
    GB_write_memory(gb, 0xFF49, 0xFF);  /* OBP1 */
    GB_write_memory(gb, 0xFF4A, 0x00);  /* WY   */
    GB_write_memory(gb, 0xFF4B, 0x00);  /* WX   */
    GB_write_memory(gb, 0xFFFF, 0x00);  /* IE   */
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s <rom> <out.ppm> <frame> [dmg|cgb] [ldbb]\n", argv[0]); return 2; }
    const char *rom = argv[1], *out = argv[2];
    long target = atol(argv[3]);
    int use_cgb = (argc >= 5 && strcmp(argv[4], "cgb") == 0);
    int use_ldbb = 0;
    for (int i = 4; i < argc; i++) if (strcmp(argv[i], "ldbb") == 0) use_ldbb = 1;

    GB_gameboy_t gb;
    GB_init(&gb, use_cgb ? GB_MODEL_CGB_E : GB_MODEL_DMG_B);
    if (GB_load_rom(&gb, rom)) { fprintf(stderr, "load fail %s\n", rom); return 1; }
    GB_set_pixels_output(&gb, g_fb);
    GB_set_rgb_encode_callback(&gb, rgb_encode);
    GB_set_vblank_callback(&gb, on_vblank);
    if (use_ldbb) GB_set_execution_callback(&gb, on_execution);
    skip_boot_dmg(&gb);

    /* Drive frames via GB_run_frame — robust (each call advances one frame's
     * worth of cycles regardless of the ROM's vblank pattern, so it can't hang
     * on test ROMs that suppress NORMAL_FRAME vblanks). In ldbb mode <frame> is
     * a max-frames safety cap; we stop once the LD B,B snapshot is taken. */
    (void)on_vblank; (void)g_vblank;
    for (long frame = 0; frame < target; frame++) {
        GB_run_frame(&gb);
        if (use_ldbb && g_captured) break;
    }

    /* In ldbb mode use the snapshot taken at the breakpoint; otherwise the live
     * buffer after <frame> frames. Fall back to the live buffer if LD B,B was
     * never reached (so non-Mealybug ROMs still produce a frame-N capture). */
    if (use_ldbb && g_captured) {
        memcpy(g_fb, g_capture, sizeof(g_fb));
    } else if (use_ldbb) {
        fprintf(stderr, "[fb-oracle] WARNING: LD B,B never hit in %ld frames; using live buffer\n", target);
    }

    unsigned w = GB_get_screen_width(&gb), h = GB_get_screen_height(&gb);
    if (w == 0 || w > 256) w = DMG_W;
    if (h == 0 || h > 224) h = DMG_H;
    FILE *f = fopen(out, "wb");
    if (!f) { perror("out"); return 1; }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (unsigned i = 0; i < w * h; i++) {
        uint32_t p = g_fb[i];
        uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "[fb-oracle] %s frame %ld -> %s (%ux%u)\n", rom, target, out, w, h);
    return 0;
}
