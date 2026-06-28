/*
 * gb_fb_oracle.c — SameBoy framebuffer reference for the GB PPU axis (5a).
 * Renders to frame N (skip-boot DMG) and dumps a P6 PPM, to pixel-diff against
 * the recomp's --dump-frames PPM. Usage:
 *   gb_fb_oracle <rom> <out.ppm> <frame> [dmg|cgb]
 */
#include "gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define DMG_W 160
#define DMG_H 144

static uint32_t g_fb[256 * 224];   /* big enough for any model */
static int g_vblank = 0;

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
static void skip_boot_dmg(GB_gameboy_t *gb)
{
    gb->af = 0x01B0; gb->bc = 0x0013; gb->de = 0x00D8; gb->hl = 0x014D;
    gb->sp = 0xFFFE; gb->pc = 0x0100; gb->boot_rom_finished = true;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s <rom> <out.ppm> <frame> [dmg|cgb]\n", argv[0]); return 2; }
    const char *rom = argv[1], *out = argv[2];
    long target = atol(argv[3]);
    int use_cgb = (argc >= 5 && strcmp(argv[4], "cgb") == 0);

    GB_gameboy_t gb;
    GB_init(&gb, use_cgb ? GB_MODEL_CGB_E : GB_MODEL_DMG_B);
    if (GB_load_rom(&gb, rom)) { fprintf(stderr, "load fail %s\n", rom); return 1; }
    GB_set_pixels_output(&gb, g_fb);
    GB_set_rgb_encode_callback(&gb, rgb_encode);
    GB_set_vblank_callback(&gb, on_vblank);
    skip_boot_dmg(&gb);

    long frame = 0;
    while (frame < target) {
        GB_run(&gb);
        if (g_vblank) { g_vblank = 0; frame++; }
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
