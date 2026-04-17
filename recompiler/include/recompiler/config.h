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
};

// Load config from TOML file. Returns nullopt on parse error.
std::optional<GameConfig> load_config(const std::string& path);

} // namespace gbrecomp

#endif
