/*
 * gb_audio_oracle.c — SameBoy-backed audio reference oracle for the GB recomp
 * accuracy burndown (Axis 5b). Links SameBoy's portable Core directly.
 *
 * Emits a deterministic S16 stereo reference sample stream + a per-sample
 * guest-cycle sidecar, the GB analog of the PSX project source-hooking Beetle
 * for guest cycles (GB_run() returns elapsed time in 8 MHz ticks).
 *
 * Skip-boot: we set post-boot DMG register/PC state and mark the boot ROM
 * finished, so the oracle starts at the game's 0x100 entry with NO boot chime —
 * matching the recomp, which also skips the boot ROM (ACCURACY.md). Determinism:
 * fixed model + fixed sample rate + no RTC dependence over the capture window.
 *
 * Usage: gb_audio_oracle <rom> <out.s16> <seconds> [dmg|cgb]
 *   out.s16  : raw interleaved S16LE stereo @ <rate> Hz
 *   out.s16.cyc : text sidecar "sample_index guest_cycles_8mhz" (sparse, 1/frame)
 */
#include "gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SAMPLE_RATE 44100   /* match the recomp's fixed 44.1 kHz output */

static FILE     *g_out;
static FILE     *g_cyc;
static FILE     *g_pch;                /* per-channel: int16 x4 (CH1,CH2,CH3,CH4) per sample */
static uint64_t  g_guest_cycles = 0;   /* accumulated 8 MHz ticks */
static uint64_t  g_sample_index = 0;
static uint64_t  g_last_logged_frame = (uint64_t)-1;

static void on_sample(GB_gameboy_t *gb, GB_sample_t *sample)
{
    int16_t buf[2] = { sample->left, sample->right };
    fwrite(buf, sizeof(int16_t), 2, g_out);
    if (g_pch) {
        /* gb->apu.samples[ch] = each channel's current 4-bit digital output (0-15,
         * 0 when DAC off). Drift diff is amplitude-normalized so the unipolar
         * (oracle) vs bipolar (recomp) encoding difference washes out. */
        int16_t p[4] = {
            (int16_t)gb->apu.samples[GB_SQUARE_1],
            (int16_t)gb->apu.samples[GB_SQUARE_2],
            (int16_t)gb->apu.samples[GB_WAVE],
            (int16_t)gb->apu.samples[GB_NOISE],
        };
        fwrite(p, sizeof(int16_t), 4, g_pch);
    }
    g_sample_index++;
}

/* Post-boot DMG hardware state (Pan Docs "Power-Up Sequence"). */
static void skip_boot_dmg(GB_gameboy_t *gb)
{
    gb->af = 0x01B0;
    gb->bc = 0x0013;
    gb->de = 0x00D8;
    gb->hl = 0x014D;
    gb->sp = 0xFFFE;
    gb->pc = 0x0100;
    gb->boot_rom_finished = true;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <rom> <out.s16> <seconds> [dmg|cgb]\n", argv[0]);
        return 2;
    }
    const char *rom_path = argv[1];
    const char *out_path = argv[2];
    double seconds = atof(argv[3]);
    int use_cgb = (argc >= 5 && strcmp(argv[4], "cgb") == 0);

    GB_gameboy_t gb;
    GB_init(&gb, use_cgb ? GB_MODEL_CGB_E : GB_MODEL_DMG_B);

    if (GB_load_rom(&gb, rom_path)) {
        fprintf(stderr, "Failed to load ROM '%s'\n", rom_path);
        return 1;
    }

    GB_set_sample_rate(&gb, SAMPLE_RATE);
    GB_apu_set_sample_callback(&gb, on_sample);
    GB_set_rendering_disabled(&gb, true);  /* PPU timing still runs; skip pixel work */

    skip_boot_dmg(&gb);

    g_out = fopen(out_path, "wb");
    if (!g_out) { perror("open out"); return 1; }
    char cyc_path[1024];
    snprintf(cyc_path, sizeof(cyc_path), "%s.cyc", out_path);
    g_cyc = fopen(cyc_path, "wb");
    if (g_cyc) fprintf(g_cyc, "# sample_index guest_cycles_8mhz\n");
    /* Per-channel stream is opt-in via env GBRT_AUDIO_PCH (matches the recomp flag). */
    {
        const char* pch = getenv("GBRT_AUDIO_PCH");
        if (pch && *pch && strcmp(pch, "0") != 0) {
            char pch_path[1024];
            snprintf(pch_path, sizeof(pch_path), "%s.pch", out_path);
            g_pch = fopen(pch_path, "wb");
        }
    }

    /* DMG: 4.194304 MHz T-cycles; SameBoy counts in 8 MHz units (2x). */
    const uint64_t units_per_sec = 8388608ULL;
    uint64_t target = (uint64_t)(seconds * (double)units_per_sec);

    uint64_t frame = 0;
    while (g_guest_cycles < target) {
        g_guest_cycles += GB_run(&gb);
        /* Log a sparse cycle anchor roughly once per emulated frame (70224 T = 140448 8MHz). */
        uint64_t f = g_guest_cycles / 140448ULL;
        if (g_cyc && f != g_last_logged_frame) {
            fprintf(g_cyc, "%llu %llu\n",
                    (unsigned long long)g_sample_index,
                    (unsigned long long)g_guest_cycles);
            g_last_logged_frame = f;
            frame = f;
        }
    }

    fflush(g_out); fclose(g_out);
    if (g_cyc) { fflush(g_cyc); fclose(g_cyc); }
    if (g_pch) { fflush(g_pch); fclose(g_pch); }
    fprintf(stderr,
            "[oracle] model=%s rom=%s\n"
            "[oracle] wrote %llu stereo samples (%.3f s @ %d Hz) to %s\n"
            "[oracle] guest_cycles=%llu (8MHz units), ~%llu frames\n",
            use_cgb ? "CGB_E" : "DMG_B", rom_path,
            (unsigned long long)g_sample_index, (double)g_sample_index / SAMPLE_RATE,
            SAMPLE_RATE, out_path,
            (unsigned long long)g_guest_cycles, (unsigned long long)frame);
    return 0;
}
