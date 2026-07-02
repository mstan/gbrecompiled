/* cosim_neutral.h — implementation-NEUTRAL architectural-state hash for the
 * cross-oracle co-simulation (recomp vs SameBoy). See COSIM_ORACLE.md.
 *
 * Unlike cosim_state.h (which hashes our runtime's specific representation and
 * is only valid within pairing 1), this hashes only state that is architecturally
 * defined and therefore comparable across two independently-authored emulators:
 *   - CPU registers (A,F,B,C,D,E,H,L,SP,PC) + IME
 *   - the guest RAM regions (WRAM, VRAM, OAM, HRAM, cart RAM), read raw
 * Micro-architectural representation (PPU fetcher, APU internal counters, the
 * exact mode-cycle convention) is EXCLUDED — it legitimately differs between
 * faithful implementations. I/O register values are excluded from the hash for
 * v1 (readback masking differs); DIV/LY are reported at a divergence instead.
 *
 * Both sides (gb_cosim_neutral_hash for our GBContext; sb_oracle_neutral_hash in
 * sameboy_oracle.c for GB_gameboy_t) MUST serialize the same logical fields in
 * the same fixed order for the hashes to be comparable.
 */
#ifndef GB_COSIM_NEUTRAL_H
#define GB_COSIM_NEUTRAL_H

/* Self-contained (only <stdint.h>) so the SameBoy driver TU can include this
 * alongside SameBoy's Core/gb.h without pulling in our gbrt.h. The recomp-side
 * extractor prototype (which needs GBContext) lives in cosim_state.h. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t cpu;       /* A,F(packed),B,C,D,E,H,L,SP,PC,IME — fixed order */
    uint64_t wram;      /* work RAM (0x2000 DMG / 0x8000 CGB) */
    uint64_t vram;      /* video RAM (0x2000 DMG / 0x4000 CGB) */
    uint64_t oam;       /* 0xA0 */
    uint64_t hram;      /* 0x7F */
    uint64_t cart_ram;  /* cartridge (battery) RAM, 0 if none */
} GBNeutralSubHashes;

#define GB_NEUTRAL_SUBHASH_COUNT 6

/* Shared FNV-1a helpers so both extractors serialize identically. */
#define GB_NEUTRAL_FNV_OFFSET 1469598103934665603ULL
#define GB_NEUTRAL_FNV_PRIME  1099511628211ULL
static inline uint64_t gb_neutral_fnv_u8(uint64_t h, uint8_t v) {
    return (h ^ (uint64_t)v) * GB_NEUTRAL_FNV_PRIME;
}
static inline uint64_t gb_neutral_fnv_u16(uint64_t h, uint16_t v) {
    h = gb_neutral_fnv_u8(h, (uint8_t)(v & 0xFF));
    return gb_neutral_fnv_u8(h, (uint8_t)((v >> 8) & 0xFF));
}
static inline uint64_t gb_neutral_fnv_bytes(uint64_t h, const uint8_t* p, uint32_t n) {
    if (!p) return h;
    for (uint32_t i = 0; i < n; i++) h = gb_neutral_fnv_u8(h, p[i]);
    return h;
}
/* Canonical CPU serialization used by BOTH sides. */
static inline uint64_t gb_neutral_cpu_hash(uint8_t a, uint8_t f_packed,
                                           uint8_t b, uint8_t c, uint8_t d, uint8_t e,
                                           uint8_t hh, uint8_t l, uint16_t sp, uint16_t pc,
                                           uint8_t ime) {
    uint64_t h = GB_NEUTRAL_FNV_OFFSET;
    h = gb_neutral_fnv_u8(h, a);
    h = gb_neutral_fnv_u8(h, (uint8_t)(f_packed & 0xF0)); /* only the 4 real flag bits */
    h = gb_neutral_fnv_u8(h, b);
    h = gb_neutral_fnv_u8(h, c);
    h = gb_neutral_fnv_u8(h, d);
    h = gb_neutral_fnv_u8(h, e);
    h = gb_neutral_fnv_u8(h, hh);
    h = gb_neutral_fnv_u8(h, l);
    h = gb_neutral_fnv_u16(h, sp);
    h = gb_neutral_fnv_u16(h, pc);
    h = gb_neutral_fnv_u8(h, ime ? 1u : 0u);
    return h;
}

/* Recomp-side extractor gb_cosim_neutral_hash(const GBContext*, ...) is declared
 * in cosim_state.h (it needs GBContext). */

uint64_t gb_neutral_subhash_by_index(const GBNeutralSubHashes* sub, int index);
const char* gb_neutral_subhash_name(int index);

#ifdef __cplusplus
}
#endif

#endif /* GB_COSIM_NEUTRAL_H */
