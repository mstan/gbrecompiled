/*
 * gb_state_oracle.c — SameBoy per-frame state+cycle ring for the GB cycle/state
 * comparator (Axis 2). Records, at each VBlank, the guest T-cycle count and CPU/
 * PPU state, so it can be diffed against the recomp's always-on frame ring
 * (debug-server `frame_timeseries`). The GB analog of the PSX cycle_compare anchor.
 *
 * Output CSV columns: frame,tcyc,pc,a,sp,bank,lcdc,ly
 *   tcyc = cumulative guest T-cycles (SameBoy 8 MHz units / 2).
 *
 * Skip-boot to 0x100 (matches the recomp). Usage:
 *   gb_state_oracle <rom> <out.csv> <frames> [dmg|cgb]
 */
#include "gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint64_t g_cycles8 = 0;     /* cumulative 8 MHz ticks */
static int      g_vblank_pending = 0;

static void on_vblank(GB_gameboy_t *gb, GB_vblank_type_t type)
{
    (void)gb;
    if (type == GB_VBLANK_TYPE_NORMAL_FRAME)   /* rendered frames only, matches recomp ring */
        g_vblank_pending = 1;
}

static void skip_boot_dmg(GB_gameboy_t *gb)
{
    gb->af = 0x01B0; gb->bc = 0x0013; gb->de = 0x00D8; gb->hl = 0x014D;
    gb->sp = 0xFFFE; gb->pc = 0x0100; gb->boot_rom_finished = true;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <rom> <out.csv> <frames> [dmg|cgb]\n", argv[0]);
        return 2;
    }
    const char *rom = argv[1], *out_path = argv[2];
    long frames = atol(argv[3]);
    int use_cgb = (argc >= 5 && strcmp(argv[4], "cgb") == 0);

    GB_gameboy_t gb;
    GB_init(&gb, use_cgb ? GB_MODEL_CGB_E : GB_MODEL_DMG_B);
    if (GB_load_rom(&gb, rom)) { fprintf(stderr, "load fail %s\n", rom); return 1; }
    GB_set_rendering_disabled(&gb, true);
    GB_set_vblank_callback(&gb, on_vblank);
    skip_boot_dmg(&gb);

    FILE *f = fopen(out_path, "wb");
    if (!f) { perror("out"); return 1; }
    fprintf(f, "frame,tcyc,pc,a,sp,bank,lcdc,ly\n");

    /* Sample on every rendered VBlank, but ALSO force a sample if ~1 frame of
     * cycles elapses without one (i.e. during LCD-off, when NORMAL_FRAME does not
     * fire) so the ring is not blind to LCD-off periods -- symmetric with the
     * recomp's frame ring, which records across LCD-off. */
    const uint64_t FRAME_8MHZ = 140448;   /* 70224 T-cycles * 2 */
    uint64_t last_rec = 0;
    long frame = 0;
    while (frame < frames) {
        g_cycles8 += GB_run(&gb);
        if (g_vblank_pending || (g_cycles8 - last_rec) >= FRAME_8MHZ) {
            g_vblank_pending = 0;
            last_rec = g_cycles8;
            fprintf(f, "%ld,%llu,%u,%u,%u,%u,%u,%u\n",
                    frame,
                    (unsigned long long)(g_cycles8 / 2),     /* 8MHz -> T-cycles */
                    gb.pc, gb.a, gb.sp, gb.mbc_rom_bank,
                    gb.io_registers[GB_IO_LCDC], gb.io_registers[GB_IO_LY]);
            frame++;
        }
    }
    fclose(f);
    fprintf(stderr, "[state-oracle] %s: %ld frames, %llu T-cycles (~%.1f/frame)\n",
            rom, frame, (unsigned long long)(g_cycles8/2),
            frame ? (double)(g_cycles8/2)/frame : 0.0);
    return 0;
}
