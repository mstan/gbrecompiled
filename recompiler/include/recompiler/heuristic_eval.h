/**
 * @file heuristic_eval.h
 * @brief Heuristic function-finder evaluation harness.
 *
 * Measures the analyzer's UNAIDED (cold, no-symbol) function discovery against a
 * known-good disassembly's symbol file (e.g. pokered.sym). Reports recall and,
 * critically, the false-positive list under the hard rule: the heuristic must
 * never decode data (or a mid-instruction byte) as code. A single false positive
 * is a failure of that heuristic.
 *
 * The ground-truth oracle is built independently of the analyzer-under-test, by
 * data-range-aware recursive descent from the real symbol set, so it cannot be
 * contaminated by the very heuristics it is meant to judge.
 */
#ifndef RECOMPILER_HEURISTIC_EVAL_H
#define RECOMPILER_HEURISTIC_EVAL_H

#include <string>

namespace gbrecomp {

struct HeuristicEvalOptions {
    std::string rom_path;
    std::string ground_truth_sym_path;
    bool aggressive_scan = true;   // run the heuristic with aggressive scan on
    bool enable_pointer_scan = false;  // speculative pointer scan (opt-in; default off)
    bool verbose = false;          // dump full FP / missing lists
    std::string emit_remainder_path;  // optional: write back-out remainder (missed funcs) here
    size_t list_cap = 60;          // cap on how many entries to print per list (0 = all)
};

// Returns 0 on success (regardless of FP count — the caller inspects the report).
// Returns non-zero only on hard errors (ROM/sym load failure).
int run_heuristic_eval(const HeuristicEvalOptions& options);

} // namespace gbrecomp

#endif // RECOMPILER_HEURISTIC_EVAL_H
