#include "gbrt.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%d: %s\n",__LINE__,#x); exit(1); } } while (0)
static const uint8_t helper[] = {0xf3,0x3e,0xc2,0xe0,0x46,0x3e,0x28,0x3d,0x20,0xfd,0xd9};
static GBContext* machine(unsigned bank, unsigned start, bool modified, bool double_speed) {
    GBContext* ctx = gb_context_create(NULL);
    CHECK(ctx);
    uint8_t rom[0x8000] = {0}; rom[0x100]=0x76;
    CHECK(gb_context_load_rom(ctx,rom,sizeof(rom)));
    gb_context_reset(ctx,true);
    ctx->config.model=GB_MODEL_CGB;
    ctx->cgb_double_speed=double_speed;
    ctx->rom_bank=(uint16_t)bank;
    memcpy(ctx->hram,helper,sizeof(helper));
    if (modified) ctx->hram[0]=0;
    ctx->sp=0xcff0; /* A WRAM stack is blocked by DMA; HRAM would hide early reads. */
    gb_push16(ctx,0x100);
    ctx->a=40;
    ctx->pc=(uint16_t)start;
    return ctx;
}
int main(void) {
    for (unsigned speed=0;speed<2;++speed)
    for (unsigned bank=1;bank<64;bank+=7) for (unsigned entry=0;entry<2;++entry) {
        GBContext* ctx=machine(bank,entry ? 0xff87 : 0xff80,false,speed);
        gb_dispatch(ctx,ctx->pc);
        CHECK(ctx->halted && ctx->pc==0x101 && ctx->rom_bank==bank);
        CHECK(ctx->total_dispatch_fallbacks==0 && ctx->total_interpreter_instructions==0);
        gb_context_destroy(ctx);
    }
    for (unsigned speed=0;speed<2;++speed) for (unsigned conditional=0;conditional<2;++conditional)
    for (unsigned remaining=4;remaining<=12;remaining+=4) {
        GBContext* ctx=machine(1,0xff80,false,speed);
        gb_write16(ctx,ctx->sp,0x1234);
        ctx->dma.active=1;ctx->dma.pending=0;ctx->dma.startup_delay=0;
        ctx->dma.source_high=0xc2;ctx->dma.cycles_remaining=remaining;
        ctx->dma.progress=(uint8_t)(160-remaining/4);
        ctx->ime=0;ctx->ime_pending=2;
        uint32_t before=ctx->cycles;
        gb_ret_timed(ctx,conditional,false);
        unsigned first=conditional ? 8 : 4;
        uint16_t expected=remaining<=first ? 0x1234 : remaining<=first+4 ? 0x12ff : 0xffff;
        CHECK(ctx->pc==expected && ctx->sp==0xcff0);
        CHECK(ctx->cycles-before==(conditional ? 20u : 16u)/(speed ? 2 : 1));
        CHECK(ctx->ime_pending==1 && !ctx->ime); /* EI advances once per instruction. */
        gb_context_destroy(ctx);
    }
    GBContext* changed=machine(60,0xff80,true,false);
    gb_dispatch(changed,changed->pc);
    fprintf(stderr,"changed bytes: fallbacks=%llu interpreter=%llu\n",
        (unsigned long long)changed->total_dispatch_fallbacks,
        (unsigned long long)changed->total_interpreter_instructions);
    CHECK(changed->total_dispatch_fallbacks>0 && changed->total_interpreter_entries>0);
    if (!GBRT_RAM_HLE) CHECK(changed->total_interpreter_instructions>0);
    gb_context_destroy(changed);
    GBContext* variant=machine(60,0xff80,false,false);
    variant->hram[2]=0xc3; /* A second byte image cannot authorize the first IR body. */
    gb_dispatch(variant,variant->pc);
    CHECK(variant->total_dispatch_fallbacks>0 && variant->halted && variant->pc==0x101);
    gb_context_destroy(variant);
    GBContext* truncated=machine(1,0xff90,false,false);
    truncated->hram[0x10]=0x3e;truncated->hram[0x11]=0x23;truncated->hram[0x12]=0x76;
    gb_dispatch(truncated,truncated->pc);
    CHECK(truncated->total_dispatch_fallbacks>0 && truncated->halted && truncated->a==0x23);
    gb_context_destroy(truncated);
    puts("native copied RAM entry/resume works across caller banks; changed bytes reject AOT body");
    return 0;
}
