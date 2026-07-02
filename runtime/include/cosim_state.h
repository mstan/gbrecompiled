/* cosim_state.h — full-architectural-state canonical hash for the GBC
 * first-divergence co-simulation oracle. See COSIM_ORACLE.md.
 *
 * The single correctness rule: this hash must cover EVERY piece of guest state
 * that can influence future execution, and NOTHING that is host-only (pointers,
 * padding, host audio sample/DC-blocker/mixer output, emulator bookkeeping
 * counters, framebuffers, PC — see the caveats in COSIM_ORACLE.md). A missed
 * execution-relevant field is a blind spot (false "no divergence"); an included
 * host-only field is a false positive. The validation gates (recomp-vs-recomp
 * == 0, injected-divergence halts at the right subsystem) exist to prove this
 * list is exactly right.
 *
 * Hashing is 64-bit FNV-1a over an explicit little-endian serialization; struct
 * fields are hashed individually (never memcpy'd whole) so host struct padding
 * cannot leak nondeterminism into the hash.
 */
#ifndef GB_COSIM_STATE_H
#define GB_COSIM_STATE_H

#include <stdint.h>
#include "gbrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-subsystem sub-hashes so a mismatch localizes to a subsystem before a
 * full field/byte diff. Order must match gb_cosim_subhash_by_index() /
 * gb_cosim_subhash_name(). */
typedef struct {
    uint64_t cpu;      /* GPRs + flags + IME/HALT/STOP/HALT-bug/double-speed latches (NOT pc) */
    uint64_t timer;    /* div_counter + tima_reload_pending */
    uint64_t dma;      /* OAM DMA + CGB HDMA transfer + scheduling micro-state */
    uint64_t serial;   /* serial_transfer struct + serial_cycles_remaining */
    uint64_t mbc;      /* mapper regs + banks + MBC3 RTC registers */
    uint64_t ppu;      /* all GBPPU regs/latches/mode/fetcher/palette RAM (NOT framebuffers) */
    uint64_t apu;      /* all 4 channels' guest internals + NR50/51/52 + frame sequencer */
    uint64_t wram;     /* 8 x 4 KB work RAM */
    uint64_t vram;     /* 2 x 8 KB video RAM */
    uint64_t oam;      /* object attribute memory */
    uint64_t hram;     /* high RAM */
    uint64_t io;       /* I/O register page */
    uint64_t eram;     /* cartridge (battery) RAM */
    uint64_t clock;    /* ctx->cycles — the shared alignment ruler */
} CosimSubHashes;

#define GB_COSIM_SUBHASH_COUNT 14

/* Compute the full canonical state hash of `ctx`. Fills `sub` (may be NULL) and
 * returns the folded top hash. Pure read — never mutates guest state. */
uint64_t gb_cosim_state_hash(const GBContext* ctx, CosimSubHashes* sub);

/* Index/name helpers for the reporter: iterate sub-hashes to name which
 * subsystem split first. index in [0, GB_COSIM_SUBHASH_COUNT). */
uint64_t gb_cosim_subhash_by_index(const CosimSubHashes* sub, int index);
const char* gb_cosim_subhash_name(int index);

/* APU sub-hash — implemented in audio.c because GBAudio is opaque outside it.
 * Hashes ONLY guest-architectural APU fields (channels, NR50/51/52, frame
 * sequencer); excludes host sample timers, the DC-blocker, and last output. */
uint64_t gb_audio_cosim_hash(const void* apu);

#ifdef __cplusplus
}
#endif

#endif /* GB_COSIM_STATE_H */
