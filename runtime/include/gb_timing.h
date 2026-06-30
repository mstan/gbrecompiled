/* gb_timing.h — per-opcode data-bus access timing (SM83 / LR35902).
 *
 * Shared reference data for the cycle-accurate tick model (Axis 2/3). Used by
 * BOTH the runtime interpreter (runtime/src/interpreter.c) and the recompiler
 * emitter (recompiler/src/codegen/c_emitter.cpp) so compiled and interpreted
 * code place hardware ticks identically.
 *
 * Source: Gekkio, "Game Boy: Complete Technical Reference" (SM83 instruction
 * timing) + Pan Docs. Cross-checked against blargg mem_timing and the mooneye
 * acceptance suite.
 *
 * MODEL: an instruction's data-bus access(es) to memory/IO land on specific
 * M-cycles (4 T-cycles each). The tick model advances hardware up to an access's
 * M-cycle, performs the access (so it samples PPU/timer/IO at the right cycle),
 * then advances the remainder. Most loads/stores access on the LAST M-cycle —
 * the existing "tick whole instruction before the access" already samples those
 * correctly. The timing-critical exception is read-modify-write: its READ lands
 * one M-cycle (4 T) BEFORE the write, so a single whole-instruction tick samples
 * the read late (this is blargg mem_timing's "Failed 3").
 *
 * NOTE: stack accesses (PUSH/POP/CALL/RET/PUSH-style) target RAM/HRAM, never
 * PPU/IO, so their intra-instruction sampling point is immaterial here and they
 * are left GB_ACC_NONE (kept on the existing tail/whole-instruction tick). This
 * table covers only the memory/IO data bus that the timing ROMs probe.
 *
 * NOTHING READS THIS YET — it is the reviewable data foundation for the tick
 * model change. See runtime/src/gbrt.log / interpreter.log when it is wired in.
 */
#ifndef GB_TIMING_H
#define GB_TIMING_H

#include <stdint.h>

typedef enum {
    GB_ACC_NONE  = 0, /* no timing-relevant memory/IO data access */
    GB_ACC_READ  = 1, /* single READ, lands on the last M-cycle */
    GB_ACC_WRITE = 2, /* single WRITE, lands on the last M-cycle */
    GB_ACC_RMW   = 3  /* READ then WRITE; the read is `split_t` T-cycles before
                       * the (last-M-cycle) write */
} GBAccessKind;

typedef struct {
    uint8_t kind;     /* GBAccessKind */
    uint8_t split_t;  /* RMW only: T-cycles from the read to the write (always 4
                       * on SM83 — read on M(n-1), write on M(n)); 0 otherwise */
} GBOpTiming;

/* ----------------------------------------------------------------------------
 * Main (unprefixed) opcode table. Entries default to {GB_ACC_NONE, 0}.
 * Single-access loads/stores are tagged READ/WRITE (last-M-cycle, already
 * correct under tick-before) for completeness; only GB_ACC_RMW changes behavior.
 * -------------------------------------------------------------------------- */
static const GBOpTiming GB_OP_TIMING[256] = {
    /* --- reads: LD A,(rr) / LD A,(HL±) --- */
    [0x0A] = {GB_ACC_READ, 0},  /* LD A,(BC)  */
    [0x1A] = {GB_ACC_READ, 0},  /* LD A,(DE)  */
    [0x2A] = {GB_ACC_READ, 0},  /* LD A,(HL+) */
    [0x3A] = {GB_ACC_READ, 0},  /* LD A,(HL-) */

    /* --- writes: LD (rr),A / LD (HL±),A --- */
    [0x02] = {GB_ACC_WRITE, 0}, /* LD (BC),A  */
    [0x12] = {GB_ACC_WRITE, 0}, /* LD (DE),A  */
    [0x22] = {GB_ACC_WRITE, 0}, /* LD (HL+),A */
    [0x32] = {GB_ACC_WRITE, 0}, /* LD (HL-),A */

    /* --- read-modify-write on (HL): READ on M2, WRITE on M3 (split 4) --- */
    [0x34] = {GB_ACC_RMW, 4},   /* INC (HL) */
    [0x35] = {GB_ACC_RMW, 4},   /* DEC (HL) */

    /* --- LD (HL),n : immediate then store, write on last M-cycle --- */
    [0x36] = {GB_ACC_WRITE, 0}, /* LD (HL),n */

    /* --- LD r,(HL) : read on last M-cycle (0x46+8k; 0x76 is HALT, excluded) --- */
    [0x46] = {GB_ACC_READ, 0},  /* LD B,(HL) */
    [0x4E] = {GB_ACC_READ, 0},  /* LD C,(HL) */
    [0x56] = {GB_ACC_READ, 0},  /* LD D,(HL) */
    [0x5E] = {GB_ACC_READ, 0},  /* LD E,(HL) */
    [0x66] = {GB_ACC_READ, 0},  /* LD H,(HL) */
    [0x6E] = {GB_ACC_READ, 0},  /* LD L,(HL) */
    [0x7E] = {GB_ACC_READ, 0},  /* LD A,(HL) */

    /* --- LD (HL),r : write on last M-cycle (0x70..0x77, 0x76 is HALT) --- */
    [0x70] = {GB_ACC_WRITE, 0}, /* LD (HL),B */
    [0x71] = {GB_ACC_WRITE, 0}, /* LD (HL),C */
    [0x72] = {GB_ACC_WRITE, 0}, /* LD (HL),D */
    [0x73] = {GB_ACC_WRITE, 0}, /* LD (HL),E */
    [0x74] = {GB_ACC_WRITE, 0}, /* LD (HL),H */
    [0x75] = {GB_ACC_WRITE, 0}, /* LD (HL),L */
    [0x77] = {GB_ACC_WRITE, 0}, /* LD (HL),A */

    /* --- ALU A,(HL) : read on last M-cycle --- */
    [0x86] = {GB_ACC_READ, 0},  /* ADD A,(HL) */
    [0x8E] = {GB_ACC_READ, 0},  /* ADC A,(HL) */
    [0x96] = {GB_ACC_READ, 0},  /* SUB (HL)   */
    [0x9E] = {GB_ACC_READ, 0},  /* SBC A,(HL) */
    [0xA6] = {GB_ACC_READ, 0},  /* AND (HL)   */
    [0xAE] = {GB_ACC_READ, 0},  /* XOR (HL)   */
    [0xB6] = {GB_ACC_READ, 0},  /* OR (HL)    */
    [0xBE] = {GB_ACC_READ, 0},  /* CP (HL)    */

    /* --- high-IO / absolute loads & stores : access on last M-cycle --- */
    [0xE0] = {GB_ACC_WRITE, 0}, /* LDH (n),A */
    [0xE2] = {GB_ACC_WRITE, 0}, /* LD (C),A  */
    [0xEA] = {GB_ACC_WRITE, 0}, /* LD (nn),A */
    [0xF0] = {GB_ACC_READ, 0},  /* LDH A,(n) */
    [0xF2] = {GB_ACC_READ, 0},  /* LD A,(C)  */
    [0xFA] = {GB_ACC_READ, 0},  /* LD A,(nn) */
};

/* ----------------------------------------------------------------------------
 * CB-prefixed opcode table. The (HL) operand is encoding index 6: opcodes with
 * (op & 0x07) == 6. Of those, BIT n,(HL) (0x46..0x7E) is READ-only; every other
 * (HL) form (rotates/shifts/swap, RES, SET) is read-modify-write.
 * -------------------------------------------------------------------------- */
static const GBOpTiming GB_CB_TIMING[256] = {
    /* rotates / shifts / swap on (HL) — RMW */
    [0x06] = {GB_ACC_RMW, 4},   /* RLC  (HL) */
    [0x0E] = {GB_ACC_RMW, 4},   /* RRC  (HL) */
    [0x16] = {GB_ACC_RMW, 4},   /* RL   (HL) */
    [0x1E] = {GB_ACC_RMW, 4},   /* RR   (HL) */
    [0x26] = {GB_ACC_RMW, 4},   /* SLA  (HL) */
    [0x2E] = {GB_ACC_RMW, 4},   /* SRA  (HL) */
    [0x36] = {GB_ACC_RMW, 4},   /* SWAP (HL) */
    [0x3E] = {GB_ACC_RMW, 4},   /* SRL  (HL) */

    /* BIT n,(HL) — READ only (no write-back), last M-cycle */
    [0x46] = {GB_ACC_READ, 0}, [0x4E] = {GB_ACC_READ, 0},
    [0x56] = {GB_ACC_READ, 0}, [0x5E] = {GB_ACC_READ, 0},
    [0x66] = {GB_ACC_READ, 0}, [0x6E] = {GB_ACC_READ, 0},
    [0x76] = {GB_ACC_READ, 0}, [0x7E] = {GB_ACC_READ, 0},

    /* RES n,(HL) — RMW */
    [0x86] = {GB_ACC_RMW, 4}, [0x8E] = {GB_ACC_RMW, 4},
    [0x96] = {GB_ACC_RMW, 4}, [0x9E] = {GB_ACC_RMW, 4},
    [0xA6] = {GB_ACC_RMW, 4}, [0xAE] = {GB_ACC_RMW, 4},
    [0xB6] = {GB_ACC_RMW, 4}, [0xBE] = {GB_ACC_RMW, 4},

    /* SET n,(HL) — RMW */
    [0xC6] = {GB_ACC_RMW, 4}, [0xCE] = {GB_ACC_RMW, 4},
    [0xD6] = {GB_ACC_RMW, 4}, [0xDE] = {GB_ACC_RMW, 4},
    [0xE6] = {GB_ACC_RMW, 4}, [0xEE] = {GB_ACC_RMW, 4},
    [0xF6] = {GB_ACC_RMW, 4}, [0xFE] = {GB_ACC_RMW, 4},
};

#endif /* GB_TIMING_H */
