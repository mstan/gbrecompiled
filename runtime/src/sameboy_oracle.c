/* sameboy_oracle.c — embedded SameBoy Core driver for the cross-oracle co-sim.
 * The ONLY TU that includes SameBoy's Core headers (Beetle-driver isolation).
 * Compiled with GB_INTERNAL (like Core) so it can read gb->ime and
 * gb->boot_rom_finished directly; everything else uses the public GB_ API.
 *
 * See COSIM_ORACLE.md. GB_run returns "8 MHz units" (2x our 4.19 MHz T-cycle in
 * single speed); we accumulate and expose the count divided by 2 as T-cycles.
 */
#include "gb.h"   /* SameBoy Core */

#include "sameboy_oracle.h"
#include "cosim_neutral.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>   /* ssize_t */

/* mingw-w64's libmingwex here doesn't export getline, but SameBoy's debugger /
 * interactive-input paths reference it (never reached by the headless oracle).
 * Provide a minimal POSIX getline so the archive links. */
ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    if (!lineptr || !n || !stream) return -1;
    if (!*lineptr || *n == 0) {
        *n = 128;
        *lineptr = (char*)malloc(*n);
        if (!*lineptr) return -1;
    }
    size_t pos = 0;
    int ch;
    while ((ch = fgetc(stream)) != EOF) {
        if (pos + 1 >= *n) {
            size_t nn = *n * 2;
            char *p = (char*)realloc(*lineptr, nn);
            if (!p) return -1;
            *lineptr = p; *n = nn;
        }
        (*lineptr)[pos++] = (char)ch;
        if (ch == '\n') break;
    }
    if (pos == 0 && ch == EOF) return -1;
    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
}

#define SB_FIFO_CAP 8192
typedef struct {
    uint16_t pc; uint8_t div; uint8_t ly; uint32_t cyc;
    uint8_t ime;   /* interrupt master enable (0/1) at the fetch boundary */
    uint8_t iflag; /* IF (0xFF0F) masked to bits 0-4 */
    uint8_t ie;    /* IE (0xFFFF) masked to bits 0-4 */
    uint16_t div16;/* SameBoy internal 16-bit divider (gb->div_counter) */
} SBTraceEntry;

struct SBOracle {
    GB_gameboy_t gb;              /* MUST be first: the execution callback recovers
                                   * the SBOracle by casting the gb pointer. */
    uint64_t ticks_8mhz;          /* accumulated GB_run() return (8 MHz units) */
    uint64_t icount;              /* instructions executed (execution-callback count) */
    SBTraceEntry fifo[SB_FIFO_CAP];
    uint32_t fifo_head, fifo_tail;
    uint8_t  fifo_overflow;
    SBTraceEntry last_popped;     /* the entry most recently returned by next_instruction */
    uint32_t pixels[160 * 144];   /* headless pixel sink */
};

/* Per-instruction execution callback: push the fetch PC (+ DIV/LY sampled at the
 * instruction boundary) into the FIFO. gb is the first member of SBOracle. */
static void sb_exec_cb(GB_gameboy_t *gb, uint16_t address, uint8_t opcode) {
    (void)opcode;
    SBOracle *o = (SBOracle *)gb;
    o->icount++;
    uint32_t next = (o->fifo_tail + 1u) % SB_FIFO_CAP;
    if (next == o->fifo_head) { o->fifo_overflow = 1; return; }
    o->fifo[o->fifo_tail].pc  = address;
    o->fifo[o->fifo_tail].div = GB_read_memory(gb, 0xFF04);
    o->fifo[o->fifo_tail].ly  = GB_read_memory(gb, 0xFF44);
    o->fifo[o->fifo_tail].cyc = (uint32_t)(gb->absolute_debugger_ticks / 2);
    o->fifo[o->fifo_tail].ime = (uint8_t)(gb->ime ? 1 : 0);
    o->fifo[o->fifo_tail].iflag = (uint8_t)(GB_read_memory(gb, 0xFF0F) & 0x1F);
    o->fifo[o->fifo_tail].ie  = (uint8_t)(GB_read_memory(gb, 0xFFFF) & 0x1F);
    o->fifo[o->fifo_tail].div16 = gb->div_counter;
    o->fifo_tail = next;
}

static void sb_vblank_cb(GB_gameboy_t *gb, GB_vblank_type_t type) {
    (void)gb; (void)type;   /* headless: no-op */
}
static uint32_t sb_rgb_encode_cb(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b) {
    (void)gb;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

SBOracle* sb_oracle_create(SBOracleModel model,
                           const uint8_t* boot_rom, size_t boot_rom_size,
                           const uint8_t* rom, size_t rom_size) {
    if (!boot_rom || !rom) return NULL;
    SBOracle* o = (SBOracle*)calloc(1, sizeof(*o));
    if (!o) return NULL;

    GB_model_t m = (model == SB_MODEL_CGB) ? GB_MODEL_CGB_E : GB_MODEL_DMG_B;
    GB_init(&o->gb, m);

    /* Headless: disable rendering but still provide a pixel sink + callbacks so
     * GB_run never dereferences a null output. */
    GB_set_vblank_callback(&o->gb, sb_vblank_cb);
    GB_set_rgb_encode_callback(&o->gb, sb_rgb_encode_cb);
    GB_set_pixels_output(&o->gb, o->pixels);
    GB_set_rendering_disabled(&o->gb, true);

    /* Load the SAME BIOS the recomp uses, then the cartridge, then reset so the
     * real boot ROM executes from power-on. No boot_rom_load_callback is set, so
     * reset uses the buffer we loaded. */
    GB_load_boot_rom_from_buffer(&o->gb, boot_rom, boot_rom_size);
    GB_load_rom_from_buffer(&o->gb, rom, rom_size);
    GB_reset(&o->gb);
    o->ticks_8mhz = 0;
    return o;
}

void sb_oracle_destroy(SBOracle* o) {
    if (!o) return;
    GB_free(&o->gb);
    free(o);
}

uint64_t sb_oracle_tcycles(const SBOracle* o) {
    return o->ticks_8mhz / 2;
}

uint64_t sb_oracle_run_to_tcycle(SBOracle* o, uint64_t target_tcycles) {
    uint64_t target_8mhz = target_tcycles * 2;
    while (o->ticks_8mhz < target_8mhz) {
        o->ticks_8mhz += GB_run(&o->gb);
    }
    return o->ticks_8mhz / 2;
}

uint16_t sb_oracle_pc(SBOracle* o) {
    return GB_get_registers(&o->gb)->pc;
}

uint8_t sb_oracle_read(SBOracle* o, uint16_t addr) {
    return GB_read_memory(&o->gb, addr);
}

bool sb_oracle_boot_done(const SBOracle* o) {
    return o->gb.boot_rom_finished;
}

void sb_oracle_enable_execution_trace(SBOracle* o) {
    o->fifo_head = o->fifo_tail = 0;
    o->fifo_overflow = 0;
    GB_set_execution_callback(&o->gb, sb_exec_cb);
}

uint64_t sb_oracle_instruction_count(const SBOracle* o) {
    return o->icount;
}

void sb_oracle_last_intr(SBOracle* o, uint8_t* ime, uint8_t* iflag, uint8_t* ie) {
    if (ime)   *ime   = o->last_popped.ime;
    if (iflag) *iflag = o->last_popped.iflag;
    if (ie)    *ie    = o->last_popped.ie;
}

uint16_t sb_oracle_last_div16(SBOracle* o) {
    return o->last_popped.div16;
}

bool sb_oracle_next_instruction(SBOracle* o, uint16_t* pc, uint8_t* div, uint8_t* ly, uint32_t* cyc) {
    int guard = 0;
    while (o->fifo_head == o->fifo_tail) {
        if (o->fifo_overflow) return false;
        o->ticks_8mhz += GB_run(&o->gb);   /* callback pushes entries */
        if (++guard > 2000000) return false;
    }
    SBTraceEntry e = o->fifo[o->fifo_head];
    o->fifo_head = (o->fifo_head + 1u) % SB_FIFO_CAP;
    o->last_popped = e;
    if (pc)  *pc  = e.pc;
    if (div) *div = e.div;
    if (ly)  *ly  = e.ly;
    if (cyc) *cyc = e.cyc;
    return true;
}

uint64_t sb_oracle_neutral_hash(SBOracle* o, GBNeutralSubHashes* sub) {
    GBNeutralSubHashes s;
    GB_registers_t* r = GB_get_registers(&o->gb);

    uint8_t a = (uint8_t)(r->af >> 8), f = (uint8_t)(r->af & 0xFF);
    uint8_t b = (uint8_t)(r->bc >> 8), c = (uint8_t)(r->bc & 0xFF);
    uint8_t d = (uint8_t)(r->de >> 8), e = (uint8_t)(r->de & 0xFF);
    uint8_t h = (uint8_t)(r->hl >> 8), l = (uint8_t)(r->hl & 0xFF);
    s.cpu = gb_neutral_cpu_hash(a, f, b, c, d, e, h, l, r->sp, r->pc, o->gb.ime);

    size_t sz; uint16_t bank;
    const uint8_t* p;
    p = (const uint8_t*)GB_get_direct_access(&o->gb, GB_DIRECT_ACCESS_RAM, &sz, &bank);
    s.wram = gb_neutral_fnv_bytes(GB_NEUTRAL_FNV_OFFSET, p, (uint32_t)sz);
    p = (const uint8_t*)GB_get_direct_access(&o->gb, GB_DIRECT_ACCESS_VRAM, &sz, &bank);
    s.vram = gb_neutral_fnv_bytes(GB_NEUTRAL_FNV_OFFSET, p, (uint32_t)sz);
    p = (const uint8_t*)GB_get_direct_access(&o->gb, GB_DIRECT_ACCESS_OAM, &sz, &bank);
    s.oam = gb_neutral_fnv_bytes(GB_NEUTRAL_FNV_OFFSET, p, (uint32_t)sz);
    p = (const uint8_t*)GB_get_direct_access(&o->gb, GB_DIRECT_ACCESS_HRAM, &sz, &bank);
    s.hram = gb_neutral_fnv_bytes(GB_NEUTRAL_FNV_OFFSET, p, (uint32_t)sz);
    p = (const uint8_t*)GB_get_direct_access(&o->gb, GB_DIRECT_ACCESS_CART_RAM, &sz, &bank);
    s.cart_ram = (p && sz) ? gb_neutral_fnv_bytes(GB_NEUTRAL_FNV_OFFSET, p, (uint32_t)sz)
                           : GB_NEUTRAL_FNV_OFFSET;

    if (sub) *sub = s;
    uint64_t top = GB_NEUTRAL_FNV_OFFSET;
    for (int i = 0; i < GB_NEUTRAL_SUBHASH_COUNT; i++) {
        uint64_t v = gb_neutral_subhash_by_index(&s, i);
        for (int by = 0; by < 8; by++)
            top = gb_neutral_fnv_u8(top, (uint8_t)((v >> (8 * by)) & 0xFF));
    }
    return top;
}
