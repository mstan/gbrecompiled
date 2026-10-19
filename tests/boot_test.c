#include "gbrt.h"
#include "gb_boot.h"
#include "gb_snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)
/* The test must stop at FF50 handoff, without ever entering a cart body. */
void gb_dispatch(GBContext* ctx, uint16_t addr) { (void)ctx; (void)addr; CHECK(0 && "entered cartridge before handoff test completed"); }
void gb_dispatch_call(GBContext* ctx, uint16_t addr) { gb_dispatch(ctx, addr); }

int main(int argc, char** argv) {
    CHECK(argc == 4);
    bool cgb = !strcmp(argv[1], "cgb");
    CHECK(gb_boot_verify_file(argv[2], cgb));
    CHECK(!gb_boot_verify_file(argv[2], !cgb));
    CHECK(!gb_boot_verify_file("missing-firmware.bin", cgb));
    FILE* f = fopen(argv[3], "rb");
    CHECK(f && !fseek(f, 0, SEEK_END));
    long size = ftell(f);
    CHECK(size > 0x150 && !fseek(f, 0, SEEK_SET));
    uint8_t* rom = (uint8_t*)malloc((size_t)size);
    CHECK(rom && fread(rom, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    GBContext* ctx = gb_context_create(NULL);
    CHECK(ctx);
    ctx->config.model = cgb ? GB_MODEL_CGB : GB_MODEL_DMG;
    ctx->hardware_mode_pref = cgb ? GB_HARDWARE_MODE_CGB : GB_HARDWARE_MODE_DMG;
    ctx->config.cartridge_supports_cgb = (rom[0x143] & 0x80) != 0;
    ctx->config.cartridge_requires_cgb = rom[0x143] == 0xc0;
    CHECK(gb_context_load_rom(ctx, rom, (size_t)size));
    free(rom);
    CHECK(gb_boot_start(ctx, argv[2]));
    CHECK(ctx->boot_rom_active && ctx->pc == 0);
    unsigned slices = 0;
    uint8_t* checkpoint = NULL;
    size_t checkpoint_size = 0;
    while (ctx->boot_rom_active && slices++ < 2000) {
        ctx->stopped = 0;
        gbrt_dispatch(ctx, ctx->pc);
        if (ctx->boot_rom_active) gb_reset_frame(ctx);
        if (slices == 50) CHECK(gb_snapshot_serialize(ctx, &checkpoint, &checkpoint_size));
    }
    if (ctx->boot_rom_active || ctx->pc != 0x100) {
        fprintf(stderr, "handoff failure pc=%04X cycles=%u ly=%u lcdc=%02X halted=%u stop=%u slices=%u fallbacks=%llu\n",
                ctx->pc, ctx->cycles, ctx->io[0x44], ctx->io[0x40], ctx->halted,
                ctx->stop_mode_active, slices, (unsigned long long)ctx->total_dispatch_fallbacks);
    }
    CHECK(!ctx->boot_rom_active && ctx->pc == 0x100);
    CHECK(ctx->total_interpreter_instructions == 0 && ctx->total_dispatch_fallbacks == 0);
    uint32_t handoff_cycles = ctx->cycles;
    CHECK(checkpoint && gb_snapshot_deserialize(ctx, checkpoint, checkpoint_size));
    CHECK(ctx->boot_rom_active);
    unsigned replay_slices = 0;
    while (ctx->boot_rom_active && replay_slices++ < 2000) {
        ctx->stopped = 0;
        gbrt_dispatch(ctx, ctx->pc);
        if (ctx->boot_rom_active) gb_reset_frame(ctx);
    }
    CHECK(!ctx->boot_rom_active && ctx->pc == 0x100 && ctx->cycles == handoff_cycles);
    CHECK(ctx->total_interpreter_instructions == 0 && ctx->total_dispatch_fallbacks == 0);
    free(checkpoint);
    printf("%s native BIOS handoff pc=%04X cycles=%u slices=%u zero interpreter/fallbacks\n",
           cgb ? "CGB" : "DMG", ctx->pc, ctx->cycles, slices);
    gb_context_destroy(ctx);
    return 0;
}
