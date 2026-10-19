#include "gbrt.h"
#include "gb_snapshot.h"
#include "ppu.h"
#include "ir.h"
#include "sgb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This fixture deliberately runs its synthetic cartridge through the real
 * interpreter; production coverage is qualified separately on generated games. */
void gb_dispatch(GBContext* ctx, uint16_t addr) { gb_interpret(ctx, addr); }
void gb_dispatch_call(GBContext* ctx, uint16_t addr) { gb_interpret(ctx, addr); }
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

static GBContext* machine(void) {
    GBContext* ctx = gb_context_create(NULL);
    CHECK(ctx);
    uint8_t rom[0x8000] = {0};
    /* INC B; LD A,B; LD ($C000),A; JR $0100 */
    const uint8_t program[] = {0x04,0x78,0xea,0x00,0xc0,0x18,0xf9};
    memcpy(rom + 0x100, program, sizeof(program));
    CHECK(gb_context_load_rom(ctx, rom, sizeof(rom)));
    gb_context_set_save_id(ctx, "snapshot_fixture");
    gb_context_reset(ctx, true);
    return ctx;
}

int main(void) {
    GBContext* ctx = machine();
    uint8_t bios[0x100] = {0};
    CHECK(gb_context_load_boot_rom(ctx, bios, sizeof(bios)));
    ctx->boot_rom_active = 1;
    ctx->serial_transfer.slave_armed = 1;
    ctx->serial_transfer.slave_outgoing = 0xa5;
    ctx->serial_transfer.deferred = 1;
    ctx->serial_cycles_remaining = 99;
    gb_ir_write_rp(ctx->ir, 0xc1);
    gb_ir_receive_peer_led(ctx->ir, true);
    ctx->wram[12] = 37;
    uint8_t* saved = NULL;
    size_t saved_size = 0;
    CHECK(gb_snapshot_serialize(ctx, &saved, &saved_size));
    ctx->boot_rom_active = 0;
    ctx->serial_transfer.slave_armed = 0;
    ctx->serial_transfer.slave_outgoing = 0;
    ctx->serial_transfer.deferred = 0;
    ctx->serial_cycles_remaining = 0;
    ctx->wram[12] = 9;
    ctx->total_interpreter_instructions = 12345;
    ctx->dispatch_history_count = 123;
    gb_ir_write_rp(ctx->ir, 0);
    CHECK(gb_snapshot_deserialize(ctx, saved, saved_size));
    CHECK(ctx->boot_rom_active && ctx->serial_transfer.slave_armed);
    CHECK(ctx->serial_transfer.slave_outgoing == 0xa5 && ctx->serial_transfer.deferred);
    CHECK(ctx->serial_cycles_remaining == 99 && ctx->wram[12] == 37);
    CHECK(gb_ir_read_rp(ctx->ir) == 0xfd);
    CHECK(ctx->total_interpreter_instructions == 12345);
    CHECK(ctx->dispatch_history_count == 123);
    /* Corruption, truncation, trailing bytes, another ROM and BIOS must reject
     * without altering even the first core/memory field. */
    ctx->wram[12] = 77;
    saved[saved_size - 1] ^= 1;
    CHECK(!gb_snapshot_deserialize(ctx, saved, saved_size));
    CHECK(ctx->wram[12] == 77);
    saved[saved_size - 1] ^= 1;
    CHECK(!gb_snapshot_deserialize(ctx, saved, saved_size - 1));
    uint8_t* extra = (uint8_t*)malloc(saved_size + 1);
    CHECK(extra);
    memcpy(extra, saved, saved_size);
    extra[saved_size] = 0;
    CHECK(!gb_snapshot_deserialize(ctx, extra, saved_size + 1));
    free(extra);
    ctx->rom_sha256[0] ^= 1;
    CHECK(!gb_snapshot_deserialize(ctx, saved, saved_size));
    ctx->rom_sha256[0] ^= 1;
    ctx->boot_rom_sha256[0] ^= 1;
    CHECK(!gb_snapshot_deserialize(ctx, saved, saved_size));
    ctx->boot_rom_sha256[0] ^= 1;
    CHECK(ctx->wram[12] == 77);
    free(saved);

    ctx->boot_rom_active = 0;
    ctx->serial_transfer.deferred = 0;
    ctx->serial_transfer.slave_armed = 0;
    ctx->pc = 0x100;
    ctx->frame_done = 0;
    gb_run_cycles(ctx, 4096);
    CHECK(gb_snapshot_serialize(ctx, &saved, &saved_size));
    gb_run_cycles(ctx, 4096);
    uint8_t expected_b = ctx->b;
    uint32_t expected_cycles = ctx->cycles;
    uint8_t expected_ram = ctx->wram[0];
    CHECK(gb_snapshot_deserialize(ctx, saved, saved_size));
    gb_run_cycles(ctx, 4096);
    CHECK(ctx->b == expected_b && ctx->cycles == expected_cycles && ctx->wram[0] == expected_ram);
    free(saved);

    CHECK(gb_rewind_enable(ctx, 4, 1024 * 1024));
    for (int i = 0; i < 7; ++i) {
        ctx->wram[12] = (uint8_t)i;
        CHECK(gb_rewind_capture(ctx));
    }
    CHECK(gb_rewind_info(ctx).count == 4);
    CHECK(gb_rewind_step(ctx, 2) && ctx->wram[12] == 4);
    CHECK(gb_rewind_info(ctx).count == 2);
    ctx->wram[12] = 99;
    CHECK(gb_rewind_capture(ctx));
    CHECK(gb_rewind_step(ctx, 1) && ctx->wram[12] == 4);
    CHECK(!gb_rewind_step(ctx, 100));
    CHECK(gb_context_save_state_file(ctx, "snapshot-fixture.state"));
    ctx->wram[12] = 63;
    CHECK(gb_context_load_state_file(ctx, "snapshot-fixture.state") && ctx->wram[12] == 4);
    CHECK(gb_rewind_info(ctx).count == 0);
    CHECK(remove("snapshot-fixture.state") == 0);
    gb_context_destroy(ctx);
    puts("snapshot transaction, replay, identity, file and rb-engine branch checks passed");
    return 0;
}
