#pragma once
#include "c_emitter.h"
#include "../decoder.h"

namespace gbrecomp::codegen {
// Standalone native instruction, with constant ROM operands and a relocatable PC.
// Live operands are reserved for instruction fetches crossing a ROM window.
std::string emit_exhaustive_instruction(const Instruction& instruction,
                                       const GeneratorOptions& options,
                                       bool halt_bug, bool live_operands);
void append_exhaustive_rom(GeneratedOutput& output, const uint8_t* rom, size_t size,
                           const GeneratorOptions& options);
}
