/**
 * @file heuristic_eval.cpp
 * @brief Heuristic function-finder evaluation harness implementation.
 *
 * See heuristic_eval.h for the methodology. In short:
 *
 *   ORACLE (ground truth)  : recursive-descent disassembly seeded from the real
 *                            disassembly's code symbols + the live CPU vectors,
 *                            following control flow. Control-flow reachability is
 *                            the code/data classifier — data is never reached, so
 *                            it is never decoded. Yields the authoritative set of
 *                            real instruction boundaries (g_code) and the real
 *                            function entries (g_func).
 *   HEURISTIC (under test) : analyze() run COLD (no symbols) with aggressive
 *                            scan. Yields h_code (every byte it decoded as an
 *                            instruction start) and h_entry (function entries).
 *
 *   recall          = |h_entry ∩ g_func| / |g_func|
 *   false positives = { a in h_code : a not in g_code }   (must be 0)
 *
 * A false positive is the heuristic decoding a byte that is not a real
 * instruction boundary — i.e. it read data, or a mid-instruction byte, as code.
 */

#include "recompiler/heuristic_eval.h"

#include "recompiler/analyzer.h"
#include "recompiler/decoder.h"
#include "recompiler/rom.h"
#include "recompiler/symbol_table.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <vector>

namespace gbrecomp {

namespace {

uint8_t addr_bank(uint32_t a) { return static_cast<uint8_t>(a >> 16); }
uint16_t addr_off(uint32_t a) { return static_cast<uint16_t>(a & 0xFFFF); }
uint32_t mk(uint8_t b, uint16_t o) { return (static_cast<uint32_t>(b) << 16) | o; }

// Exclusive end-of-bank offset for the canonical addressing scheme: bank 0 owns
// 0x0000-0x3FFF, switchable banks own 0x4000-0x7FFF.
uint16_t bank_end(uint8_t bank) { return (bank == 0) ? 0x4000 : 0x8000; }

// Canonical bank for a control-flow target: low region is always bank 0, the
// switchable window stays in the current bank (matches analyzer addressing).
uint8_t target_bank(uint8_t cur_bank, uint16_t target) {
    return (target < 0x4000) ? 0 : cur_bank;
}

std::string fmt_addr(uint32_t a) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(addr_bank(a))
       << ":" << std::setw(4) << addr_off(a);
    return ss.str();
}

// Recursive-descent oracle. Decodes only bytes reachable by control flow from a
// real code seed, so data is never decoded. Returns the set of real instruction
// boundaries (canonical bank:addr).
std::set<uint32_t> trace_oracle(const ROM& rom, const std::set<uint32_t>& seeds) {
    std::set<uint32_t> g_code;
    Decoder decoder(rom);
    std::queue<uint32_t> work;
    for (uint32_t s : seeds) work.push(s);

    auto enqueue = [&](uint8_t bank, uint16_t off) {
        if (off >= bank_end(bank)) return;
        uint32_t ca = mk(bank, off);
        if (!g_code.count(ca)) work.push(ca);
    };

    while (!work.empty()) {
        uint32_t ca = work.front();
        work.pop();
        uint8_t bank = addr_bank(ca);
        uint16_t off = addr_off(ca);
        if (off >= bank_end(bank)) continue;
        if (g_code.count(ca)) continue;

        Instruction instr = decoder.decode(off, bank);
        if (instr.type == InstructionType::UNDEFINED ||
            instr.type == InstructionType::INVALID || instr.length == 0) {
            continue;  // wandered off real code (should not happen on a real path)
        }
        if (static_cast<uint32_t>(off) + instr.length > bank_end(bank)) {
            continue;  // instruction would straddle bank end — not real code here
        }

        g_code.insert(ca);
        uint16_t fall = static_cast<uint16_t>(off + instr.length);

        if (instr.type == InstructionType::RST) {
            // pokered's RST vectors are all padding/traps (never return). Record
            // the RST but do not fall through past it.
            continue;
        }
        if (instr.is_return) {
            if (instr.is_conditional) enqueue(bank, fall);  // RET cc falls through
            continue;                                       // RET / RETI terminate
        }
        if (instr.type == InstructionType::JP_HL) {
            continue;  // computed; real targets are seeded labels
        }
        if (instr.is_call) {  // CALL nn / CALL cc nn
            enqueue(target_bank(bank, instr.imm16), instr.imm16);
            enqueue(bank, fall);  // returns
            continue;
        }
        if (instr.is_jump) {
            if (instr.type == InstructionType::JP_NN || instr.type == InstructionType::JP_CC_NN) {
                enqueue(target_bank(bank, instr.imm16), instr.imm16);
            } else if (instr.type == InstructionType::JR_N || instr.type == InstructionType::JR_CC_N) {
                uint16_t t = static_cast<uint16_t>(off + instr.length + instr.offset);
                enqueue(bank, t);
            }
            if (instr.is_conditional) enqueue(bank, fall);  // conditional jumps fall through
            continue;                                       // unconditional jumps terminate
        }
        // Ordinary instruction: fall through.
        enqueue(bank, fall);
    }
    return g_code;
}

[[maybe_unused]] const char* sym_type_name(SymbolType t) {
    switch (t) {
        case SymbolType::FUNCTION: return "function";
        case SymbolType::LABEL:    return "label";
        case SymbolType::DATA:     return "data";
        default:                   return "unknown";
    }
}

}  // namespace

int run_heuristic_eval(const HeuristicEvalOptions& options) {
    // ---- 1. Load ROM ----------------------------------------------------------
    auto rom_opt = ROM::load(options.rom_path);
    if (!rom_opt || !rom_opt->is_valid()) {
        std::cerr << "[eval] Failed to load ROM: " << options.rom_path << "\n";
        if (rom_opt) std::cerr << "[eval]   " << rom_opt->error() << "\n";
        return 1;
    }
    const ROM& rom = *rom_opt;

    // ---- 2. Load + classify ground-truth symbols ------------------------------
    SymbolTable gt;
    std::string err;
    if (!gt.load_sym_file(options.ground_truth_sym_path, &rom, &err)) {
        std::cerr << "[eval] Failed to load symbols: " << err << "\n";
        return 1;
    }

    std::set<uint32_t> g_func;        // real function entries (recall denominator)
    std::set<uint32_t> g_label;       // real jump-target labels (code, not entries)
    std::set<uint32_t> all_syms;      // every ROM-space symbol address
    std::map<uint32_t, std::string> sym_name;
    std::map<uint32_t, SymbolType> sym_type;
    std::map<uint8_t, std::vector<uint16_t>> per_bank_offsets;  // sorted, for enclosing lookup
    size_t n_func = 0, n_label = 0, n_data = 0, n_unknown = 0, n_ram = 0;

    for (const auto& [addr, sym] : gt.symbols()) {
        uint16_t off = addr_off(addr);
        if (off >= 0x8000) { n_ram++; continue; }  // RAM/HRAM symbol — not ROM code/data
        all_syms.insert(addr);
        sym_name[addr] = sym.source_name;
        sym_type[addr] = sym.type;
        per_bank_offsets[addr_bank(addr)].push_back(off);

        switch (sym.type) {
            case SymbolType::FUNCTION: g_func.insert(addr); n_func++; break;
            case SymbolType::LABEL:    g_label.insert(addr); n_label++; break;
            case SymbolType::DATA:     n_data++; break;
            case SymbolType::UNKNOWN:  n_unknown++; break;
            default: break;
        }
    }
    for (auto& [bank, offs] : per_bank_offsets) std::sort(offs.begin(), offs.end());

    // Nearest preceding symbol (the "enclosing" symbol) for a byte address.
    auto enclosing_sym = [&](uint32_t a) -> uint32_t {
        uint8_t bank = addr_bank(a);
        uint16_t off = addr_off(a);
        auto it = per_bank_offsets.find(bank);
        if (it == per_bank_offsets.end()) return UINT32_MAX;
        const auto& offs = it->second;
        auto up = std::upper_bound(offs.begin(), offs.end(), off);
        if (up == offs.begin()) return UINT32_MAX;  // before first symbol in bank
        return mk(bank, *(up - 1));
    };

    // Seeds: real code labels + the live CPU vectors (pokered does not label the
    // RST/interrupt vectors, and the RST vectors are 0xFF/00 padding traps).
    std::set<uint32_t> code_seeds;
    code_seeds.insert(g_func.begin(), g_func.end());
    code_seeds.insert(g_label.begin(), g_label.end());
    code_seeds.insert(mk(0, 0x0100));
    for (uint16_t vec : {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38,
                         0x40, 0x48, 0x50, 0x58, 0x60}) {
        uint8_t b = rom.read_banked(0, vec);
        if (b != 0x00 && b != 0xFF) code_seeds.insert(mk(0, vec));  // skip padding/traps
    }

    // ---- 3. Build the oracle --------------------------------------------------
    std::set<uint32_t> g_code = trace_oracle(rom, code_seeds);

    // ---- 4. Run the heuristic COLD (no symbols) -------------------------------
    AnalyzerOptions opts;
    opts.aggressive_scan = options.aggressive_scan;
    opts.enable_pointer_scan = options.enable_pointer_scan;
    opts.analyze_all_banks = true;
    opts.verbose = false;
    opts.add_builtin_rom_annotations = true;  // header/logo data, as in normal runs
    // Deliberately NO annotations: this measures unaided discovery.

    AnalysisResult res = analyze(rom, opts);

    std::set<uint32_t> h_code;
    for (const auto& [a, idx] : res.addr_to_index) { (void)idx; h_code.insert(a); }
    std::set<uint32_t> h_entry;            // final, usable function entries
    for (const auto& [a, f] : res.functions) { (void)f; h_entry.insert(a); }
    std::set<uint32_t> h_targets = res.call_targets;  // looser: every flagged entry

    // ---- 5. Metrics -----------------------------------------------------------
    size_t found_strict = 0, found_loose = 0;
    std::vector<uint32_t> missing;
    for (uint32_t g : g_func) {
        bool strict = h_entry.count(g) > 0;
        bool loose = strict || h_targets.count(g) > 0;
        if (strict) found_strict++;
        if (loose) found_loose++;
        if (!loose) missing.push_back(g);
    }

    // False positives: heuristic-decoded bytes that are not real boundaries.
    // Bucket A: coincides with a real symbol (likely real code our classifier
    //           filed as unknown — a classifier issue, low concern).
    // Bucket B: not any real symbol (heuristic invented a boundary in data or
    //           mid-instruction — a genuine FP candidate, high concern).
    std::vector<uint32_t> fp_entry_bucketB, fp_entry_bucketA, fp_bytes_bucketB;
    size_t fp_bytes_bucketA = 0;

    std::set<uint32_t> all_flagged_entries = h_entry;
    all_flagged_entries.insert(h_targets.begin(), h_targets.end());

    for (uint32_t a : all_flagged_entries) {
        if (g_code.count(a)) continue;
        if (all_syms.count(a)) fp_entry_bucketA.push_back(a);
        else fp_entry_bucketB.push_back(a);
    }
    for (uint32_t a : h_code) {
        if (g_code.count(a)) continue;
        if (all_syms.count(a)) fp_bytes_bucketA++;
        else fp_bytes_bucketB.push_back(a);
    }
    std::sort(fp_entry_bucketB.begin(), fp_entry_bucketB.end());
    std::sort(fp_entry_bucketA.begin(), fp_entry_bucketA.end());
    std::sort(fp_bytes_bucketB.begin(), fp_bytes_bucketB.end());
    std::sort(missing.begin(), missing.end());

    // ---- 6. Report ------------------------------------------------------------
    std::cout << std::dec;  // analyzer leaves cout in hex mode; reset.
    auto pct = [](size_t num, size_t den) -> double {
        return den == 0 ? 0.0 : (100.0 * static_cast<double>(num) / static_cast<double>(den));
    };

    std::cout << "\n================ HEURISTIC FINDER EVALUATION ================\n";
    std::cout << "ROM         : " << options.rom_path << "\n";
    std::cout << "Ground truth: " << options.ground_truth_sym_path << "\n";
    std::cout << "Aggressive scan: " << (options.aggressive_scan ? "ON" : "OFF")
              << "   Pointer scan: " << (options.enable_pointer_scan ? "ON" : "OFF") << "\n";
    std::cout << "\n-- Ground-truth symbol classification --\n";
    std::cout << "  functions : " << n_func << "\n";
    std::cout << "  labels    : " << n_label << "\n";
    std::cout << "  data      : " << n_data << "\n";
    std::cout << "  unknown   : " << n_unknown << " (not seeded; mostly data)\n";
    std::cout << "  ram/hram  : " << n_ram << " (ignored)\n";
    std::cout << "  oracle instruction boundaries (g_code): " << g_code.size() << "\n";

    std::cout << "\n-- Heuristic cold-run output --\n";
    std::cout << "  decoded instruction starts (h_code): " << h_code.size() << "\n";
    std::cout << "  call targets (h_targets)           : " << h_targets.size() << "\n";
    std::cout << "  final function entries (h_entry)   : " << h_entry.size() << "\n";

    std::cout << "\n-- COVERAGE (recall over " << g_func.size() << " real functions) --\n";
    std::cout << "  found as function entry (strict): " << found_strict << "  ("
              << std::fixed << std::setprecision(2) << pct(found_strict, g_func.size()) << "%)\n";
    std::cout << "  found as any call target (loose): " << found_loose << "  ("
              << pct(found_loose, g_func.size()) << "%)\n";
    std::cout << "  back-out remainder (must stay in manual TOML): " << missing.size() << "\n";

    std::cout << "\n-- FALSE POSITIVES (hard rule: must be 0) --\n";
    std::cout << "  Bucket B (genuine FP candidates — not any real symbol):\n";
    std::cout << "    flagged entries: " << fp_entry_bucketB.size() << "\n";
    std::cout << "    decoded bytes  : " << fp_bytes_bucketB.size() << "\n";
    std::cout << "  Bucket A (coincide with a real symbol — classifier noise):\n";
    std::cout << "    flagged entries: " << fp_entry_bucketA.size() << "\n";
    std::cout << "    decoded bytes  : " << fp_bytes_bucketA << "\n";

    // Diagnostic: where does Bucket B contamination live? Tally the contaminated
    // bytes by the type of their enclosing symbol, and the worst enclosing
    // symbols by name. This reveals genuine FPs (enclosed by data symbols) vs
    // oracle gaps (enclosed by code symbols whose blocks the oracle couldn't
    // reach, e.g. pointer-table-only code).
    {
        size_t under_func = 0, under_label = 0, under_unknown = 0, under_none = 0;
        std::map<uint32_t, size_t> by_enclosing;
        for (uint32_t a : fp_bytes_bucketB) {
            uint32_t e = enclosing_sym(a);
            by_enclosing[e]++;
            if (e == UINT32_MAX) { under_none++; continue; }
            switch (sym_type[e]) {
                case SymbolType::FUNCTION: under_func++; break;
                case SymbolType::LABEL:    under_label++; break;
                case SymbolType::UNKNOWN:  under_unknown++; break;
                default:                   under_unknown++; break;
            }
        }
        std::cout << "\n  Bucket B bytes by enclosing-symbol type:\n";
        std::cout << "    under function symbol: " << under_func << "\n";
        std::cout << "    under label symbol   : " << under_label << "\n";
        std::cout << "    under unknown symbol : " << under_unknown << "\n";
        std::cout << "    before any symbol    : " << under_none << "\n";

        std::vector<std::pair<uint32_t, size_t>> ranked(by_enclosing.begin(), by_enclosing.end());
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& x, const auto& y) { return x.second > y.second; });
        std::cout << "\n  Worst enclosing symbols (contaminated bytes — name [type]):\n";
        size_t shown = 0;
        for (const auto& [e, cnt] : ranked) {
            if (shown++ >= 30) break;
            std::cout << "    " << std::setw(7) << cnt << "  ";
            if (e == UINT32_MAX) { std::cout << "(before first symbol)\n"; continue; }
            std::cout << fmt_addr(e) << "  " << sym_name[e]
                      << " [" << sym_type_name(sym_type[e]) << "]\n";
        }
    }

    const size_t cap = options.list_cap;
    auto dump_list = [&](const char* title, const std::vector<uint32_t>& v, bool with_sym) {
        if (v.empty()) return;
        std::cout << "\n  " << title << " (" << v.size() << "):\n";
        size_t shown = 0;
        for (uint32_t a : v) {
            if (cap && !options.verbose && shown >= cap) {
                std::cout << "    ... (" << (v.size() - shown) << " more; pass --eval-verbose)\n";
                break;
            }
            std::cout << "    " << fmt_addr(a);
            if (with_sym) {
                auto it = sym_name.find(a);
                if (it != sym_name.end()) std::cout << "  " << it->second;
            }
            std::cout << "\n";
            shown++;
        }
    };

    dump_list("GENUINE FP entries (Bucket B)", fp_entry_bucketB, false);
    if (options.verbose) {
        dump_list("FP entries coinciding with symbol (Bucket A)", fp_entry_bucketA, true);
        dump_list("MISSING real functions (back-out remainder)", missing, true);
    }

    // ---- 7. Optional: write back-out remainder --------------------------------
    if (!options.emit_remainder_path.empty()) {
        std::ofstream out(options.emit_remainder_path);
        if (out) {
            out << "# Real functions the heuristic did NOT find unaided.\n";
            out << "# These are the minimal manual entry points still needed.\n";
            out << "# Format: function BB:OOOO name\n";
            for (uint32_t a : missing) {
                out << "function " << fmt_addr(a);
                auto it = sym_name.find(a);
                if (it != sym_name.end()) out << " " << it->second;
                out << "\n";
            }
            std::cout << "\n[eval] Wrote back-out remainder (" << missing.size()
                      << " funcs) to " << options.emit_remainder_path << "\n";
        } else {
            std::cerr << "[eval] Could not write remainder file: "
                      << options.emit_remainder_path << "\n";
        }
    }

    std::cout << "\n-- VERDICT --\n";
    if (fp_entry_bucketB.empty() && fp_bytes_bucketB.empty()) {
        std::cout << "  PASS (zero genuine false positives). Recall "
                  << std::fixed << std::setprecision(2) << pct(found_strict, g_func.size())
                  << "%.\n";
    } else {
        std::cout << "  FAIL: " << fp_entry_bucketB.size() << " genuine FP entries, "
                  << fp_bytes_bucketB.size() << " contaminated bytes. Triage Bucket B.\n";
    }
    std::cout << "=============================================================\n\n";

    return 0;
}

}  // namespace gbrecomp
