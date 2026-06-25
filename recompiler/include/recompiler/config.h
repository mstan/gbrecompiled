#ifndef RECOMPILER_CONFIG_H
#define RECOMPILER_CONFIG_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gbrecomp {

struct HramOverlayConfig {
    uint16_t ram_addr;
    uint8_t source_bank;
    uint16_t source_addr;
    uint16_t size;
};

struct DataRegionConfig {
    int bank;           // -1 = all banks
    uint16_t start;
    uint16_t end;       // exclusive
};

struct GameConfig {
    // ROM
    std::string rom_path;
    std::string output_dir;
    std::string runtime_dir;  // Path to runtime/ (relative to output_dir)
    std::string output_prefix;  // Override for generated symbol/file prefix

    // Options (all optional — unset means "use default / CLI value")
    std::optional<bool> verbose;
    std::optional<bool> trace_log;
    std::optional<bool> aggressive_scan;
    std::optional<bool> emit_comments;
    std::optional<bool> single_function;
    std::optional<size_t> limit_instructions;
    std::optional<int> specific_bank;
    std::string trace_file;

    // Entry points: bank number -> list of addresses
    std::map<uint8_t, std::vector<uint16_t>> entry_points;

    // HRAM overlays
    std::vector<HramOverlayConfig> hram_overlays;

    // Data regions (excluded from code analysis)
    std::vector<DataRegionConfig> data_regions;

    // Valid CRC32s (for multi-version ROM support, e.g. Red + Blue)
    std::vector<uint32_t> valid_crcs;

    // Tier-0 dispatch-miss manifest path actually ingested (if any), for logging.
    std::string dispatch_misses_file;
};

// Load config from TOML file. Returns nullopt on parse error.
std::optional<GameConfig> load_config(const std::string& path);

// ── Tier-0 dispatch-miss manifest ───────────────────────────────────────────
// A dispatch_misses.toml records ROM addresses that fell through to the
// interpreter at runtime — provably-real function entries the static finder
// missed (zero false positives, since they executed as opcodes). load_config()
// auto-ingests a sibling dispatch_misses.toml (or the [options] dispatch_misses
// path) as entry-point seeds. harvest_dispatch_misses() folds a runtime
// interp_fallbacks.log into the manifest, replacing the per-game
// harvest_seeds.sh with a built-in, agnostic step.

struct DispatchMiss {
    uint8_t  bank;   // decimal ROM bank (0 for 0x0000-0x3FFF)
    uint16_t addr;   // ROM address (< 0x8000)
    uint64_t hits;   // cumulative interpreter entries observed (informational)
};

// Parse a dispatch_misses.toml. Returns ROM-only entries; empty on a missing
// manifest (not an error) or parse error (warned).
std::vector<DispatchMiss> load_dispatch_misses(const std::string& manifest_path);

// Fold a runtime interp_fallbacks.log into manifest_path (ROM-only, dedup,
// hit-accumulating). Creates or updates the manifest in place. Returns the
// number of distinct ROM entries after merge, or -1 on error (e.g. log
// unreadable).
int harvest_dispatch_misses(const std::string& log_path,
                            const std::string& manifest_path);

} // namespace gbrecomp

#endif
