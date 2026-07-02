/* cosim_state.c — full guest-architectural-state canonical hash. See
 * COSIM_ORACLE.md and cosim_state.h. Pure read of a GBContext; never mutates.
 *
 * Deliberately hashes the WHOLE machine, not a hypothesis-trimmed subset: the
 * per-subsystem sub-hash is what TELLS us which subsystem split; we do not
 * decide that in advance. Host-only state is excluded (see EXCLUDE notes) so
 * recomp-vs-recomp is exactly zero (Gate 1).
 */
#include "cosim_state.h"
#include "ppu.h"

#include <stddef.h>

/* Region sizes — kept identical to differential.c's byte-compare regions so the
 * hash and the exact-compare audit (Gate 4) cover the same surface. */
#define COSIM_WRAM_SIZE (0x1000u * 8u)
#define COSIM_VRAM_SIZE (VRAM_SIZE * 2u)
#define COSIM_OAM_SIZE  OAM_SIZE
#define COSIM_HRAM_SIZE 0x7Fu
#define COSIM_IO_SIZE   0x81u

/* ---- 64-bit FNV-1a over explicit little-endian serialization ---- */
#define FNV64_OFFSET 1469598103934665603ULL
#define FNV64_PRIME  1099511628211ULL

static inline uint64_t fnv_u8(uint64_t h, uint8_t v) {
    return (h ^ (uint64_t)v) * FNV64_PRIME;
}
static inline uint64_t fnv_bytes(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) h = fnv_u8(h, b[i]);
    return h;
}
static inline uint64_t fnv_u16(uint64_t h, uint16_t v) {
    h = fnv_u8(h, (uint8_t)(v & 0xFF));
    h = fnv_u8(h, (uint8_t)((v >> 8) & 0xFF));
    return h;
}
static inline uint64_t fnv_u32(uint64_t h, uint32_t v) {
    for (int i = 0; i < 4; i++) h = fnv_u8(h, (uint8_t)((v >> (8 * i)) & 0xFF));
    return h;
}
static inline uint64_t fnv_u64(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; i++) h = fnv_u8(h, (uint8_t)((v >> (8 * i)) & 0xFF));
    return h;
}
static inline uint64_t fnv_i32(uint64_t h, int32_t v) {
    return fnv_u32(h, (uint32_t)v);
}

/* ---- CPU: registers + flags + execution-mode latches (NOT pc) ----
 * pc is excluded per the PC-currency caveat: the recomp keeps pc current only at
 * block boundaries while the interpreter updates it every instruction, so pc has
 * different currency between backends. A real control-flow split shows as a
 * differing GPR/RAM/IO byte within one checkpoint. */
static uint64_t hash_cpu(const GBContext* ctx) {
    uint64_t h = FNV64_OFFSET;
    h = fnv_u8(h, ctx->a);
    h = fnv_u8(h, ctx->b);
    h = fnv_u8(h, ctx->c);
    h = fnv_u8(h, ctx->d);
    h = fnv_u8(h, ctx->e);
    h = fnv_u8(h, ctx->h);
    h = fnv_u8(h, ctx->l);
    h = fnv_u16(h, ctx->sp);
    /* pack flags canonically (the unpacked f_* bytes are the source of truth) */
    h = fnv_u8(h, (uint8_t)((ctx->f_z ? 0x80 : 0) | (ctx->f_n ? 0x40 : 0) |
                            (ctx->f_h ? 0x20 : 0) | (ctx->f_c ? 0x10 : 0)));
    h = fnv_u8(h, ctx->ime);
    h = fnv_u8(h, ctx->ime_pending);
    h = fnv_u8(h, ctx->halted);
    h = fnv_u8(h, ctx->stopped);
    h = fnv_u8(h, ctx->stop_mode_active);
    h = fnv_u8(h, ctx->halt_bug);
    h = fnv_u8(h, ctx->cgb_double_speed);
    return h;
}

static uint64_t hash_timer(const GBContext* ctx) {
    uint64_t h = FNV64_OFFSET;
    h = fnv_u16(h, ctx->div_counter);
    h = fnv_u8(h, ctx->tima_reload_pending);
    return h;
}

static uint64_t hash_dma(const GBContext* ctx) {
    uint64_t h = FNV64_OFFSET;
    h = fnv_u8(h, ctx->dma.active);
    h = fnv_u8(h, ctx->dma.pending);
    h = fnv_u8(h, ctx->dma.source_high);
    h = fnv_u8(h, ctx->dma.progress);
    h = fnv_u16(h, ctx->dma.cycles_remaining);
    h = fnv_u8(h, ctx->dma.startup_delay);
    h = fnv_u16(h, ctx->hdma.source);
    h = fnv_u16(h, ctx->hdma.dest);
    h = fnv_u8(h, ctx->hdma.blocks_remaining);
    h = fnv_u8(h, ctx->hdma.active);
    h = fnv_u8(h, ctx->hdma.hblank_mode);
    return h;
}

static uint64_t hash_serial(const GBContext* ctx) {
    uint64_t h = FNV64_OFFSET;
    h = fnv_i32(h, (int32_t)ctx->serial_cycles_remaining);
    h = fnv_u8(h, ctx->serial_transfer.active);
    h = fnv_u8(h, ctx->serial_transfer.fast_clock);
    h = fnv_u32(h, ctx->serial_transfer.cycles_remaining);
    h = fnv_u8(h, ctx->serial_transfer.deferred);
    h = fnv_u8(h, ctx->serial_transfer.slave_armed);
    h = fnv_u8(h, ctx->serial_transfer.slave_outgoing);
    return h;
}

/* MBC mapper regs + banks + MBC3 RTC registers. rtc.last_time is host-clock
 * seeded; for a deterministic attract run both sides seed it identically, so it
 * is included — recomp-vs-recomp (Gate 1) will flag it if that ever breaks. */
static uint64_t hash_mbc(const GBContext* ctx) {
    uint64_t h = FNV64_OFFSET;
    h = fnv_u16(h, ctx->rom_bank);
    h = fnv_u8(h, ctx->ram_bank);
    h = fnv_u8(h, ctx->wram_bank);
    h = fnv_u8(h, ctx->vram_bank);
    h = fnv_u8(h, ctx->mbc_type);
    h = fnv_u8(h, ctx->ram_enabled);
    h = fnv_u8(h, ctx->mbc_mode);
    h = fnv_u8(h, ctx->rom_bank_upper);
    h = fnv_u8(h, ctx->rtc_mode);
    h = fnv_u8(h, ctx->rtc_reg);
    h = fnv_u8(h, ctx->rtc.s);
    h = fnv_u8(h, ctx->rtc.m);
    h = fnv_u8(h, ctx->rtc.h);
    h = fnv_u8(h, ctx->rtc.dl);
    h = fnv_u8(h, ctx->rtc.dh);
    h = fnv_u8(h, ctx->rtc.latched_s);
    h = fnv_u8(h, ctx->rtc.latched_m);
    h = fnv_u8(h, ctx->rtc.latched_h);
    h = fnv_u8(h, ctx->rtc.latched_dl);
    h = fnv_u8(h, ctx->rtc.latched_dh);
    h = fnv_u8(h, ctx->rtc.latch_state);
    h = fnv_u64(h, ctx->rtc.last_time);
    h = fnv_u8(h, (uint8_t)(ctx->rtc.active ? 1 : 0));
    return h;
}

/* PPU: every register + latched set + internal mode/fetcher + palette RAM.
 * Framebuffers are EXCLUDED (write-only output; they cannot influence future
 * guest execution) — the exact-compare audit still checks them separately. */
static uint64_t hash_ppu(const GBContext* ctx) {
    uint64_t h = FNV64_OFFSET;
    const GBPPU* p = (const GBPPU*)ctx->ppu;
    if (!p) return h;
    h = fnv_u8(h, p->lcdc); h = fnv_u8(h, p->stat);
    h = fnv_u8(h, p->scy);  h = fnv_u8(h, p->scx);
    h = fnv_u8(h, p->ly);   h = fnv_u8(h, p->lyc);
    h = fnv_u8(h, p->dma);  h = fnv_u8(h, p->bgp);
    h = fnv_u8(h, p->obp0); h = fnv_u8(h, p->obp1);
    h = fnv_u8(h, p->wy);   h = fnv_u8(h, p->wx);
    h = fnv_u8(h, p->bgpi); h = fnv_u8(h, p->obpi); h = fnv_u8(h, p->opri);
    h = fnv_u8(h, p->latched_lcdc); h = fnv_u8(h, p->latched_scy);
    h = fnv_u8(h, p->latched_scx);  h = fnv_u8(h, p->latched_bgp);
    h = fnv_u8(h, p->latched_obp0); h = fnv_u8(h, p->latched_obp1);
    h = fnv_u8(h, p->latched_wy);   h = fnv_u8(h, p->latched_wx);
    h = fnv_u8(h, (uint8_t)(p->stat_irq_state ? 1 : 0));
    h = fnv_u32(h, (uint32_t)p->mode);
    h = fnv_u32(h, p->mode_cycles);
    h = fnv_u8(h, p->window_line);
    h = fnv_u8(h, (uint8_t)(p->window_triggered ? 1 : 0));
    h = fnv_u32(h, p->scanline_draw_cycles);
    h = fnv_u32(h, p->scanline_hblank_cycles);
    h = fnv_u8(h, p->scanline_sprite_count);
    h = fnv_i32(h, (int32_t)p->render_x);
    h = fnv_u8(h, (uint8_t)(p->win_rendered_line ? 1 : 0));
    h = fnv_bytes(h, p->bg_raw_line, sizeof(p->bg_raw_line));
    h = fnv_bytes(h, p->bg_priority_line, sizeof(p->bg_priority_line));
    /* sprite list: fixed-order field-by-field over all 10 slots + counts/flag */
    for (int i = 0; i < 10; i++) {
        const ScanlineSprite* s = &p->scanline_sprites[i];
        h = fnv_i32(h, (int32_t)s->oam_index);
        h = fnv_i32(h, (int32_t)s->screen_x);
        h = fnv_u8(h, s->x_pos);
        h = fnv_u8(h, s->flags);
        h = fnv_u8(h, s->palette);
        h = fnv_u8(h, (uint8_t)(s->behind_bg ? 1 : 0));
        h = fnv_u8(h, s->lo);
        h = fnv_u8(h, s->hi);
    }
    h = fnv_i32(h, (int32_t)p->scanline_sprite_list_count);
    h = fnv_u8(h, (uint8_t)(p->sprite_list_built ? 1 : 0));
    h = fnv_bytes(h, p->bg_palette_ram, sizeof(p->bg_palette_ram));
    h = fnv_bytes(h, p->obj_palette_ram, sizeof(p->obj_palette_ram));
    return h;
}

static uint64_t hash_region(const uint8_t* p, size_t n) {
    if (!p) return FNV64_OFFSET;
    return fnv_bytes(FNV64_OFFSET, p, n);
}

uint64_t gb_cosim_state_hash(const GBContext* ctx, CosimSubHashes* sub) {
    CosimSubHashes s;
    s.cpu    = hash_cpu(ctx);
    s.timer  = hash_timer(ctx);
    s.dma    = hash_dma(ctx);
    s.serial = hash_serial(ctx);
    s.mbc    = hash_mbc(ctx);
    s.ppu    = hash_ppu(ctx);
    s.apu    = ctx->apu ? gb_audio_cosim_hash(ctx->apu) : FNV64_OFFSET;
    s.wram   = hash_region(ctx->wram, COSIM_WRAM_SIZE);
    s.vram   = hash_region(ctx->vram, COSIM_VRAM_SIZE);
    s.oam    = hash_region(ctx->oam,  COSIM_OAM_SIZE);
    s.hram   = hash_region(ctx->hram, COSIM_HRAM_SIZE);
    s.io     = hash_region(ctx->io,   COSIM_IO_SIZE);
    s.eram   = (ctx->eram && ctx->eram_size) ? hash_region(ctx->eram, ctx->eram_size)
                                             : FNV64_OFFSET;
    s.clock  = fnv_u32(FNV64_OFFSET, ctx->cycles);

    if (sub) *sub = s;

    /* Fold sub-hashes into the top hash in fixed order. */
    uint64_t top = FNV64_OFFSET;
    for (int i = 0; i < GB_COSIM_SUBHASH_COUNT; i++) {
        top = fnv_u64(top, gb_cosim_subhash_by_index(&s, i));
    }
    return top;
}

uint64_t gb_cosim_subhash_by_index(const CosimSubHashes* sub, int index) {
    switch (index) {
        case 0:  return sub->cpu;
        case 1:  return sub->timer;
        case 2:  return sub->dma;
        case 3:  return sub->serial;
        case 4:  return sub->mbc;
        case 5:  return sub->ppu;
        case 6:  return sub->apu;
        case 7:  return sub->wram;
        case 8:  return sub->vram;
        case 9:  return sub->oam;
        case 10: return sub->hram;
        case 11: return sub->io;
        case 12: return sub->eram;
        case 13: return sub->clock;
        default: return 0;
    }
}

/* ---- Implementation-neutral architectural hash (recomp side) ----
 * Serializes only architecturally-defined state, in the exact order the SameBoy
 * extractor (sameboy_oracle.c) uses, so the two are directly comparable. */
uint64_t gb_cosim_neutral_hash(const GBContext* ctx, GBNeutralSubHashes* sub) {
    GBNeutralSubHashes s;
    /* CGB hardware has 32KB WRAM / 16KB VRAM (even in DMG-compat mode); DMG/SGB
     * have 8KB / 8KB. Match the size SameBoy's GB_get_direct_access reports. */
    int cgb_hw = (ctx->config.model == GB_MODEL_CGB);
    uint32_t wram_sz = cgb_hw ? 0x8000u : 0x2000u;
    uint32_t vram_sz = cgb_hw ? 0x4000u : 0x2000u;

    uint8_t f_packed = (uint8_t)((ctx->f_z ? 0x80 : 0) | (ctx->f_n ? 0x40 : 0) |
                                 (ctx->f_h ? 0x20 : 0) | (ctx->f_c ? 0x10 : 0));
    s.cpu = gb_neutral_cpu_hash(ctx->a, f_packed, ctx->b, ctx->c, ctx->d, ctx->e,
                                ctx->h, ctx->l, ctx->sp, ctx->pc, ctx->ime);
    s.wram = gb_neutral_fnv_bytes(GB_NEUTRAL_FNV_OFFSET, ctx->wram, wram_sz);
    s.vram = gb_neutral_fnv_bytes(GB_NEUTRAL_FNV_OFFSET, ctx->vram, vram_sz);
    s.oam  = gb_neutral_fnv_bytes(GB_NEUTRAL_FNV_OFFSET, ctx->oam, COSIM_OAM_SIZE);
    s.hram = gb_neutral_fnv_bytes(GB_NEUTRAL_FNV_OFFSET, ctx->hram, COSIM_HRAM_SIZE);
    s.cart_ram = (ctx->eram && ctx->eram_size)
                 ? gb_neutral_fnv_bytes(GB_NEUTRAL_FNV_OFFSET, ctx->eram, (uint32_t)ctx->eram_size)
                 : GB_NEUTRAL_FNV_OFFSET;

    if (sub) *sub = s;
    uint64_t top = GB_NEUTRAL_FNV_OFFSET;
    for (int i = 0; i < GB_NEUTRAL_SUBHASH_COUNT; i++)
        top = fnv_u64(top, gb_neutral_subhash_by_index(&s, i));
    return top;
}

uint64_t gb_neutral_subhash_by_index(const GBNeutralSubHashes* sub, int index) {
    switch (index) {
        case 0: return sub->cpu;
        case 1: return sub->wram;
        case 2: return sub->vram;
        case 3: return sub->oam;
        case 4: return sub->hram;
        case 5: return sub->cart_ram;
        default: return 0;
    }
}

const char* gb_neutral_subhash_name(int index) {
    switch (index) {
        case 0: return "cpu";
        case 1: return "wram";
        case 2: return "vram";
        case 3: return "oam";
        case 4: return "hram";
        case 5: return "cart_ram";
        default: return "?";
    }
}

const char* gb_cosim_subhash_name(int index) {
    switch (index) {
        case 0:  return "cpu";
        case 1:  return "timer";
        case 2:  return "dma";
        case 3:  return "serial";
        case 4:  return "mbc";
        case 5:  return "ppu";
        case 6:  return "apu";
        case 7:  return "wram";
        case 8:  return "vram";
        case 9:  return "oam";
        case 10: return "hram";
        case 11: return "io";
        case 12: return "eram";
        case 13: return "clock";
        default: return "?";
    }
}
