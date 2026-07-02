/* sameboy_oracle.h — clean C API over an embedded SameBoy Core, for the
 * independent cross-oracle co-simulation (recomp vs SameBoy). See COSIM_ORACLE.md.
 *
 * This header deliberately exposes NO SameBoy types — sameboy_oracle.c is the
 * only TU that includes SameBoy's Core headers (the Beetle-driver isolation
 * pattern), so the rest of the runtime never sees GB_gameboy_t et al.
 */
#ifndef GB_SAMEBOY_ORACLE_H
#define GB_SAMEBOY_ORACLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "cosim_neutral.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SBOracle SBOracle;

typedef enum {
    SB_MODEL_DMG = 0,
    SB_MODEL_CGB = 1,
} SBOracleModel;

/* Create an oracle: GB_init(model) + load the boot ROM (BIOS) + load the
 * cartridge + reset to power-on so it boots the real BIOS from cycle 0. Returns
 * NULL on failure. `boot_rom` must be the SAME BIOS the recomp uses for cycle-0
 * alignment. */
SBOracle* sb_oracle_create(SBOracleModel model,
                           const uint8_t* boot_rom, size_t boot_rom_size,
                           const uint8_t* rom, size_t rom_size);
void sb_oracle_destroy(SBOracle* o);

/* Advance until the oracle's guest T-cycle counter reaches >= target_tcycles;
 * returns the counter afterward (lands at the first GB_run boundary past it). */
uint64_t sb_oracle_run_to_tcycle(SBOracle* o, uint64_t target_tcycles);
uint64_t sb_oracle_tcycles(const SBOracle* o);

/* Reporting / alignment helpers. */
uint16_t sb_oracle_pc(SBOracle* o);
uint8_t  sb_oracle_read(SBOracle* o, uint16_t addr); /* GB_read_memory (masked) */
bool     sb_oracle_boot_done(const SBOracle* o);     /* BIOS handed off? */

/* Fill the implementation-neutral architectural sub-hashes (same layout + field
 * order as gb_cosim_neutral_hash on the recomp side). Returns the folded top hash. */
uint64_t sb_oracle_neutral_hash(SBOracle* o, GBNeutralSubHashes* sub);

/* ---- Instruction-stream stepping (for instruction-count-aligned lockstep) ----
 * Arm SameBoy's per-instruction execution callback so every executed instruction
 * pushes its fetch PC into an internal FIFO. Call once after create. */
void sb_oracle_enable_execution_trace(SBOracle* o);

/* Return the fetch PC of the NEXT instruction SameBoy executes, advancing it by
 * exactly one instruction (running GB_run under the hood to refill the FIFO).
 * `*div`/`*ly` (may be NULL) get DIV/LY sampled at that instruction boundary.
 * `*cyc` (may be NULL) gets SameBoy's absolute T-cycle count at that boundary
 * (absolute_debugger_ticks / 2, matching sb_oracle_tcycles' single-speed unit),
 * so lockstep can report the exact cycle offset at a divergence — DIV's high
 * byte alone does not pin the cycle.
 * Returns false only if it cannot make progress (safety cap). */
bool sb_oracle_next_instruction(SBOracle* o, uint16_t* pc, uint8_t* div, uint8_t* ly, uint32_t* cyc);

/* Instructions executed so far (execution-callback count). */
uint64_t sb_oracle_instruction_count(const SBOracle* o);

/* Interrupt state (IME / IF / IE; IF+IE masked to bits 0-4) sampled at the fetch
 * boundary of the instruction MOST RECENTLY returned by sb_oracle_next_instruction.
 * Captured inside the execution callback, so it reflects the boundary state — NOT
 * a later state (SameBoy runs ahead to refill its FIFO). Lets the co-sim ring
 * explain interrupt-dispatch divergences: was the interrupt pending+enabled on
 * one side but not the other at the same instruction boundary. Any out-ptr may be NULL. */
void sb_oracle_last_intr(SBOracle* o, uint8_t* ime, uint8_t* iflag, uint8_t* ie);

#ifdef __cplusplus
}
#endif

#endif /* GB_SAMEBOY_ORACLE_H */
