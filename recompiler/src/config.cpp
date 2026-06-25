#include "recompiler/config.h"

#include <toml.hpp>

#include <filesystem>
#include <iostream>

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

    return config;
}

} // namespace gbrecomp
