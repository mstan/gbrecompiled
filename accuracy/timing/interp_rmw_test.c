/* Interpreter-only proof for the RMW sub-instruction tick split (Axis 2).
 *
 * Drives gb_interpret() directly on a single (HL) RMW instruction whose read
 * target is DIV (FF04) — DIV advances 1 per T-cycle, so the read VALUE depends
 * on exactly which M-cycle the read samples. We craft div_counter so the DIV
 * byte (div_counter>>8) flips across the read window, making the read's
 * sampling cycle observable through a CPU flag.
 *
 *   A) CB SRL (HL): 16T, read on M3 (+12), write on M4 (+16). div=0xF2 ->
 *      +12 => DIV 0x00 (bit0=0 => carry 0)  [correct, new]
 *      +16 => DIV 0x01 (bit0=1 => carry 1)  [the old both-at-end bug]
 *   B) INC (HL): 12T, read on M2 (+8), write on M3 (+12). div=0xFF6 ->
 *      +8  => DIV 0x0F (INC half-carry H=1)  [correct, new]
 *      +12 => DIV 0x10 (H=0)                 [the old bug]
 */
#include "gbrt.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CK(c) do{ if(!(c)){ printf("FAIL: %s\n", #c); fails++; } }while(0)

static GBContext* fresh(void) {
    GBConfig cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.speed_percent = 100;
    GBContext* ctx = gb_context_create(&cfg);
    static uint8_t rom[0x8000];
    memset(rom, 0, sizeof(rom));
    rom[0x147] = 0; rom[0x148] = 0; rom[0x149] = 0;
    gb_context_load_rom(ctx, rom, sizeof(rom));
    return ctx;
}

static void run_one(GBContext* ctx, uint16_t at) {
    ctx->pc = at;
    ctx->single_step_mode = 1;
    ctx->stopped = 0;
    gb_interpret(ctx, at);
}

int main(void) {
    /* A: CB SRL (HL) — read must sample DIV at M3 (+12) => carry 0 */
    {
        GBContext* ctx = fresh();
        ctx->hl = 0xFF04;
        ctx->div_counter = 0xF2;
        gb_write8(ctx, 0xC000, 0xCB);
        gb_write8(ctx, 0xC001, 0x3E);   /* SRL (HL) */
        ctx->f_c = 1;
        run_one(ctx, 0xC000);
        printf("[A] SRL (HL): carry=%d (expect 0 = read at M3 +12)\n", ctx->f_c);
        CK(ctx->f_c == 0);
        gb_context_destroy(ctx);
    }
    /* B: INC (HL) — read must sample DIV at M2 (+8) => half-carry 1 */
    {
        GBContext* ctx = fresh();
        ctx->hl = 0xFF04;
        ctx->div_counter = 0xFF6;
        gb_write8(ctx, 0xC000, 0x34);   /* INC (HL) */
        ctx->f_h = 0;
        run_one(ctx, 0xC000);
        printf("[B] INC (HL): H=%d (expect 1 = read at M2 +8)\n", ctx->f_h);
        CK(ctx->f_h == 1);
        gb_context_destroy(ctx);
    }
    printf(fails ? "\nINTERP RMW: FAIL\n" : "\nINTERP RMW: PASS\n");
    return fails ? 1 : 0;
}
