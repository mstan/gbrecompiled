#include "recompiler/config.h"

#include <toml.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace gbrecomp {

std::optional<GameConfig> load_config(const std::string& path) {
    GameConfig config;
    toml::table tbl;

    try {
        tbl = toml::parse_file(path);
    } catch (const toml::parse_error& err) {
        std::cerr << "TOML parse error in " << path << ":\n" << err << "\n";
        return std::nullopt;
    }

    fs::path config_dir = fs::path(path).parent_path();
    if (config_dir.empty()) config_dir = ".";

    // [rom]
    if (auto rom = tbl["rom"].as_table()) {
        if (auto p = rom->get("path")) {
            config.rom_path = p->value_or(std::string{});
            if (!config.rom_path.empty() && fs::path(config.rom_path).is_relative()) {
                config.rom_path = (config_dir / config.rom_path).string();
            }
        }
        if (auto o = rom->get("output_dir")) {
            config.output_dir = o->value_or(std::string{});
            if (!config.output_dir.empty() && fs::path(config.output_dir).is_relative()) {
                config.output_dir = (config_dir / config.output_dir).string();
            }
        }
        if (auto r = rom->get("runtime_dir")) {
            config.runtime_dir = r->value_or(std::string{});
        }
        if (auto p = rom->get("output_prefix")) {
            config.output_prefix = p->value_or(std::string{});
        }
        if (auto crcs = rom->get("valid_crcs")) {
            if (auto arr = crcs->as_array()) {
                for (auto& elem : *arr) {
                    if (auto v = elem.value<int64_t>()) {
                        config.valid_crcs.push_back(static_cast<uint32_t>(*v));
                    }
                }
            }
        }
    }

    // [options]
    if (auto opts = tbl["options"].as_table()) {
        if (auto v = opts->get("verbose"))         config.verbose = v->value_or(false);
        if (auto v = opts->get("trace_log"))       config.trace_log = v->value_or(false);
        if (auto v = opts->get("aggressive_scan")) config.aggressive_scan = v->value_or(true);
        if (auto v = opts->get("emit_comments"))   config.emit_comments = v->value_or(true);
        if (auto v = opts->get("single_function")) config.single_function = v->value_or(false);
        if (auto v = opts->get("limit_instructions"))
            config.limit_instructions = static_cast<size_t>(v->value_or(int64_t{0}));
        if (auto v = opts->get("bank"))
            config.specific_bank = static_cast<int>(v->value_or(int64_t{-1}));
        if (auto v = opts->get("trace_file")) {
            config.trace_file = v->value_or(std::string{});
            if (!config.trace_file.empty() && fs::path(config.trace_file).is_relative()) {
                config.trace_file = (config_dir / config.trace_file).string();
            }
        }
        if (auto v = opts->get("dispatch_misses")) {
            config.dispatch_misses_file = v->value_or(std::string{});
            if (!config.dispatch_misses_file.empty() &&
                fs::path(config.dispatch_misses_file).is_relative()) {
                config.dispatch_misses_file =
                    (config_dir / config.dispatch_misses_file).string();
            }
        }
    }

    // [entry_points]
    if (auto ep = tbl["entry_points"].as_table()) {
        for (auto& [key, val] : *ep) {
            // Keys are "bank_N"
            std::string key_str(key);
            if (key_str.substr(0, 5) != "bank_") {
                std::cerr << "Warning: ignoring unknown entry_points key '" << key_str << "'\n";
                continue;
            }
            uint8_t bank;
            try {
                bank = static_cast<uint8_t>(std::stoi(key_str.substr(5)));
            } catch (...) {
                std::cerr << "Warning: cannot parse bank number from '" << key_str << "'\n";
                continue;
            }

            if (auto arr = val.as_array()) {
                for (auto& elem : *arr) {
                    if (auto addr = elem.value<int64_t>()) {
                        config.entry_points[bank].push_back(static_cast<uint16_t>(*addr));
                    }
                }
            }
        }
    }

    // [[hram_overlay]]
    if (auto overlays = tbl["hram_overlay"].as_array()) {
        for (auto& elem : *overlays) {
            if (auto t = elem.as_table()) {
                HramOverlayConfig ov{};
                ov.ram_addr    = static_cast<uint16_t>((*t)["ram_addr"].value_or(int64_t{0}));
                ov.source_bank = static_cast<uint8_t>((*t)["source_bank"].value_or(int64_t{0}));
                ov.source_addr = static_cast<uint16_t>((*t)["source_addr"].value_or(int64_t{0}));
                ov.size        = static_cast<uint16_t>((*t)["size"].value_or(int64_t{0}));

                if (ov.ram_addr != 0 && ov.size != 0) {
                    config.hram_overlays.push_back(ov);
                } else {
                    std::cerr << "Warning: skipping incomplete hram_overlay entry\n";
                }
            }
        }
    }

    // [[imm_override]] — reviewed ALU-immediate sites routed through
    // gbrt_imm_override8() at runtime (opt-in enhancement chokepoints).
    if (auto overrides = tbl["imm_override"].as_array()) {
        for (auto& elem : *overrides) {
            if (auto t = elem.as_table()) {
                ImmOverrideConfig io{};
                io.bank = static_cast<uint8_t>((*t)["bank"].value_or(int64_t{0}));
                io.addr = static_cast<uint16_t>((*t)["addr"].value_or(int64_t{0}));
                io.note = (*t)["note"].value_or(std::string{});
                if (io.addr != 0) {
                    config.imm_overrides.push_back(io);
                } else {
                    std::cerr << "Warning: skipping incomplete imm_override entry\n";
                }
            }
        }
    }

    // [[data_region]]
    if (auto regions = tbl["data_region"].as_array()) {
        for (auto& elem : *regions) {
            if (auto t = elem.as_table()) {
                DataRegionConfig dr{};
                dr.bank  = static_cast<int>((*t)["bank"].value_or(int64_t{-1}));
                dr.start = static_cast<uint16_t>((*t)["start"].value_or(int64_t{0}));
                dr.end   = static_cast<uint16_t>((*t)["end"].value_or(int64_t{0}));

                if (dr.end > dr.start) {
                    config.data_regions.push_back(dr);
                } else {
                    std::cerr << "Warning: skipping invalid data_region (end <= start)\n";
                }
            }
        }
    }

    // Tier-0: ingest the dispatch-miss manifest as entry-point seeds. Explicit
    // [options] dispatch_misses wins; otherwise auto-discover a sibling
    // dispatch_misses.toml next to the config. A missing manifest is fine.
    {
        std::string manifest = config.dispatch_misses_file;
        if (manifest.empty()) {
            fs::path sibling = config_dir / "dispatch_misses.toml";
            if (fs::exists(sibling)) manifest = sibling.string();
        }
        if (!manifest.empty() && fs::exists(manifest)) {
            std::vector<DispatchMiss> misses = load_dispatch_misses(manifest);
            size_t added = 0;
            for (const auto& m : misses) {
                auto& vec = config.entry_points[m.bank];
                if (std::find(vec.begin(), vec.end(), m.addr) == vec.end()) {
                    vec.push_back(m.addr);
                    ++added;
                }
            }
            config.dispatch_misses_file = manifest;
            std::cerr << "[tier0] ingested " << misses.size()
                      << " dispatch-miss seed(s) from " << manifest << " ("
                      << added << " new entry point(s))\n";
        }
    }

    return config;
}

std::vector<DispatchMiss> load_dispatch_misses(const std::string& manifest_path) {
    std::vector<DispatchMiss> out;
    if (manifest_path.empty() || !fs::exists(manifest_path)) return out;

    toml::table tbl;
    try {
        tbl = toml::parse_file(manifest_path);
    } catch (const toml::parse_error& err) {
        std::cerr << "Warning: dispatch_misses parse error in " << manifest_path
                  << ":\n" << err << "\n";
        return out;
    }

    if (auto arr = tbl["miss"].as_array()) {
        for (auto& elem : *arr) {
            if (auto t = elem.as_table()) {
                int64_t bank = (*t)["bank"].value_or(int64_t{0});
                int64_t addr = (*t)["addr"].value_or(int64_t{-1});
                int64_t hits = (*t)["hits"].value_or(int64_t{0});
                if (addr < 0 || addr >= 0x8000) continue;  // ROM-only
                DispatchMiss m{};
                m.bank = static_cast<uint8_t>(bank);
                m.addr = static_cast<uint16_t>(addr);
                m.hits = static_cast<uint64_t>(hits < 0 ? 0 : hits);
                out.push_back(m);
            }
        }
    }
    return out;
}

int harvest_dispatch_misses(const std::string& log_path,
                            const std::string& manifest_path) {
    std::ifstream log(log_path);
    if (!log) {
        std::cerr << "error: cannot open interpreter fallback log: " << log_path
                  << "\n";
        return -1;
    }

    // Seed from any existing manifest so hits accumulate across runs.
    std::map<uint32_t, DispatchMiss> merged;
    for (const auto& m : load_dispatch_misses(manifest_path)) {
        merged[(static_cast<uint32_t>(m.bank) << 16) | m.addr] = m;
    }

    // Log lines: "<decimal_bank> 0x<ADDR> <global_count>"; '#' = comment.
    // Each line is one interpreter fallback event — count occurrences per
    // (bank, addr) as this run's hits. ROM-only (addr < 0x8000); RAM/HRAM
    // code is runtime-copied and handled by stubs, not static recompilation.
    std::string line;
    size_t rom_events = 0;
    while (std::getline(log, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        int bank = 0;
        std::string addr_str;
        long count = 0;
        if (!(ss >> bank >> addr_str >> count)) continue;
        unsigned long addr = std::strtoul(addr_str.c_str(), nullptr, 0);  // "0x.."
        if (addr >= 0x8000) continue;  // ROM-only
        ++rom_events;
        uint32_t key = (static_cast<uint32_t>(static_cast<uint8_t>(bank)) << 16) |
                       static_cast<uint16_t>(addr);
        auto it = merged.find(key);
        if (it == merged.end()) {
            merged[key] = DispatchMiss{static_cast<uint8_t>(bank),
                                       static_cast<uint16_t>(addr), 1};
        } else {
            it->second.hits += 1;
        }
    }

    std::vector<DispatchMiss> entries;
    entries.reserve(merged.size());
    for (auto& kv : merged) entries.push_back(kv.second);
    std::sort(entries.begin(), entries.end(),
              [](const DispatchMiss& a, const DispatchMiss& b) {
                  return a.bank != b.bank ? a.bank < b.bank : a.addr < b.addr;
              });

    std::ofstream out(manifest_path, std::ios::trunc);
    if (!out) {
        std::cerr << "error: cannot write manifest: " << manifest_path << "\n";
        return -1;
    }
    out << "# Tier-0 dispatch-miss manifest — runtime-confirmed interpreter\n"
        << "# fallbacks fed back to the recompiler as entry-point seeds. Each\n"
        << "# address provably executed as an opcode (zero false positives).\n"
        << "# ROM-only (addr < 0x8000); RAM/HRAM code is handled by runtime stubs.\n"
        << "# Generated by: gbrecomp --harvest <interp_fallbacks.log> [--manifest <file>]\n"
        << "# Auto-ingested when sitting next to the game config, or via\n"
        << "# [options] dispatch_misses = \"...\".\n\n";
    for (const auto& m : entries) {
        char addrbuf[16];
        std::snprintf(addrbuf, sizeof(addrbuf), "0x%04X", m.addr);
        out << "[[miss]]\n"
            << "bank = " << static_cast<int>(m.bank) << "\n"
            << "addr = " << addrbuf << "\n"
            << "hits = " << m.hits << "\n\n";
    }
    std::cerr << "[tier0] harvested " << rom_events << " ROM fallback event(s) → "
              << entries.size() << " distinct seed(s) in " << manifest_path << "\n";
    return static_cast<int>(entries.size());
}

} // namespace gbrecomp
