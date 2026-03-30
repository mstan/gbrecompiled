/**
 * @file main.cpp
 * @brief GameBoy Recompiler CLI entry point
 */

#include "recompiler/rom.h"
#include "recompiler/decoder.h"
#include "recompiler/analyzer.h"
#include "recompiler/config.h"
#include "recompiler/ir/ir.h"
#include "recompiler/ir/ir_builder.h"
#include "recompiler/codegen/c_emitter.h"

#include <iostream>
#include <string>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

void print_banner() {
    std::cout << R"(
   ____                      ____              ____                                _ _          _ 
  / ___| __ _ _ __ ___   ___| __ )  ___  _   _|  _ \ ___  ___ ___  _ __ ___  _ __ (_) | ___  __| |
 | |  _ / _` | '_ ` _ \ / _ \  _ \ / _ \| | | | |_) / _ \/ __/ _ \| '_ ` _ \| '_ \| | |/ _ \/ _` |
 | |_| | (_| | | | | | |  __/ |_) | (_) | |_| |  _ <  __/ (_| (_) | | | | | | |_) | | |  __/ (_| |
  \____|\__,_|_| |_| |_|\___|____/ \___/ \__, |_| \_\___|\___\___/|_| |_| |_| .__/|_|_|\___|\__,_|
                                         |___/                              |_|                   
)" << '\n';
    std::cout << "  GameBoy Static Recompiler v0.1.0\n";
    std::cout << "  https://github.com/arcanite24/gb-recompiled\n\n";
}

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [<rom.gb>] [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --config <file.toml>  Load per-game configuration from TOML file\n";
    std::cout << "  -o, --output <dir>    Output directory (default: <rom>_output)\n";
    std::cout << "  -d, --disasm          Disassemble only (don't generate code)\n";
    std::cout << "  -a, --analyze         Analyze control flow only\n";
    std::cout << "  -v, --verbose         Verbose output\n";
    std::cout << "  --trace               Trace execution analysis (very verbose)\n";
    std::cout << "  --limit <n>           Limit analysis to n instructions\n";
    std::cout << "  --single-function     Generate all code in a single function\n";
    std::cout << "  --no-comments         Don't include disassembly comments\n";
    std::cout << "  --bank <n>            Only process bank n\n";
    std::cout << "  --add-entry-point b:a Add manual entry point (e.g. 1:4000)\n";
    std::cout << "  --no-scan             Disable aggressive code scanning (enabled by default)\n";
    std::cout << "  --use-trace <file>    Use runtime trace to find entry points\n";
    std::cout << "  -h, --help            Show this help\n";
    std::cout << "\nCLI flags override TOML config values.\n";
}

std::string sanitize_prefix(const std::string& name) {
    std::string result = name;
    for (char& c : result) {
        if (!isalnum(c) && c != '_') {
            c = '_';
        }
    }
    if (!result.empty() && isdigit(result[0])) {
        result = "_" + result;
    }
    return result;
}

int main(int argc, char* argv[]) {
    // Parse command line
    if (argc < 2) {
        print_banner();
        print_usage(argv[0]);
        return 1;
    }

    std::string rom_path;
    std::string output_dir;
    std::string config_path;
    bool disasm_only = false;
    bool analyze_only = false;
    // Use optionals for flags that can come from TOML — CLI wins when set
    std::optional<bool> cli_verbose;
    std::optional<bool> cli_trace_log;
    std::optional<size_t> cli_limit_instructions;
    bool single_function = false;
    std::optional<bool> cli_emit_comments;
    std::optional<bool> cli_aggressive_scan;
    std::optional<int> cli_specific_bank;
    std::vector<uint32_t> manual_entry_points;
    std::string trace_file_path;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--config") {
            if (i + 1 < argc) {
                config_path = argv[++i];
            }
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 < argc) {
                output_dir = argv[++i];
            }
        } else if (arg == "-d" || arg == "--disasm") {
            disasm_only = true;
        } else if (arg == "-a" || arg == "--analyze") {
            analyze_only = true;
        } else if (arg == "-v" || arg == "--verbose") {
            cli_verbose = true;
        } else if (arg == "--trace") {
            cli_trace_log = true;
        } else if (arg == "--limit") {
            if (i + 1 < argc) {
                cli_limit_instructions = std::stoul(argv[++i]);
            }
        } else if (arg == "--single-function") {
            single_function = true;
        } else if (arg == "--no-comments") {
            cli_emit_comments = false;
        } else if (arg == "--bank") {
            if (i + 1 < argc) {
                cli_specific_bank = std::stoi(argv[++i]);
            }
        } else if (arg == "--add-entry-point") {
            if (i + 1 < argc) {
                std::string param = argv[++i];
                size_t colon = param.find(':');
                if (colon != std::string::npos) {
                    try {
                        int bank = std::stoi(param.substr(0, colon), nullptr, 0);
                        int addr = std::stoi(param.substr(colon + 1), nullptr, 16);
                        manual_entry_points.push_back(gbrecomp::AnalysisResult::make_addr(bank, addr));
                        std::cout << "Added manual entry point: Bank " << bank << " Address 0x" << std::hex << addr << std::dec << "\n";
                    } catch (...) {
                        std::cerr << "Invalid entry point format: " << param << "\n";
                    }
                } else {
                     std::cerr << "Invalid entry point format (expected bank:addr): " << param << "\n";
                }
            }
        } else if (arg == "--no-scan") {
            cli_aggressive_scan = false;
        } else if (arg == "--use-trace") {
            if (i + 1 < argc) {
                trace_file_path = argv[++i];
            }
        } else if (arg[0] != '-') {
            rom_path = arg;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return 1;
        }
    }

    // Load TOML config if specified
    gbrecomp::GameConfig game_config;
    std::vector<gbrecomp::HramOverlayConfig> config_hram_overlays;

    if (!config_path.empty()) {
        auto loaded = gbrecomp::load_config(config_path);
        if (!loaded) {
            std::cerr << "Error: Failed to parse config file: " << config_path << "\n";
            return 1;
        }
        game_config = *loaded;
        std::cout << "Loaded config: " << config_path << "\n";

        // Apply TOML values where CLI didn't override
        if (rom_path.empty()) rom_path = game_config.rom_path;
        if (output_dir.empty()) output_dir = game_config.output_dir;
        if (!trace_file_path.empty()) { /* CLI wins */ }
        else trace_file_path = game_config.trace_file;

        // Merge TOML entry points with CLI entry points
        for (const auto& [bank, addrs] : game_config.entry_points) {
            for (uint16_t addr : addrs) {
                manual_entry_points.push_back(gbrecomp::AnalysisResult::make_addr(bank, addr));
                std::cout << "Config entry point: Bank " << (int)bank << " Address 0x"
                          << std::hex << addr << std::dec << "\n";
            }
        }

        config_hram_overlays = game_config.hram_overlays;
    }

    // Resolve final option values: CLI > TOML > defaults
    bool verbose          = cli_verbose.value_or(game_config.verbose.value_or(false));
    bool trace_log        = cli_trace_log.value_or(game_config.trace_log.value_or(false));
    size_t limit_instructions = cli_limit_instructions.value_or(game_config.limit_instructions.value_or(0));
    bool emit_comments    = cli_emit_comments.value_or(game_config.emit_comments.value_or(true));
    bool aggressive_scan  = cli_aggressive_scan.value_or(game_config.aggressive_scan.value_or(true));
    int specific_bank     = cli_specific_bank.value_or(game_config.specific_bank.value_or(-1));

    if (rom_path.empty()) {
        std::cerr << "Error: No ROM file specified (use positional arg or [rom].path in config)\n";
        print_usage(argv[0]);
        return 1;
    }
    
    print_banner();
    
    // Load ROM
    std::cout << "Loading ROM: " << rom_path << "\n";
    
    auto rom_opt = gbrecomp::ROM::load(rom_path);
    if (!rom_opt) {
        std::cerr << "Error: Failed to load ROM\n";
        return 1;
    }
    
    auto& rom = *rom_opt;
    
    if (!rom.is_valid()) {
        std::cerr << "Error: " << rom.error() << "\n";
        return 1;
    }
    
    // Print ROM info
    gbrecomp::print_rom_info(rom);
    
    // Set default output directory
    if (output_dir.empty()) {
        output_dir = fs::path(rom_path).stem().string() + "_output";
    }
    
    // Disassemble only mode
    if (disasm_only) {
        std::cout << "\nDisassembly:\n";
        std::cout << "============\n";
        
        uint8_t bank = (specific_bank >= 0) ? specific_bank : 0;
        auto instructions = gbrecomp::decode_bank(rom, bank);
        
        for (const auto& instr : instructions) {
            std::cout << instr.disassemble() << "\n";
        }
        return 0;
    }
    
    // Analyze control flow
    std::cout << "\nAnalyzing control flow...\n";
    

    gbrecomp::AnalyzerOptions analyze_opts;
    analyze_opts.trace_log = trace_log;
    analyze_opts.verbose = verbose;
    analyze_opts.verbose = verbose;
    analyze_opts.max_instructions = limit_instructions;
    analyze_opts.entry_points = manual_entry_points;
    analyze_opts.aggressive_scan = aggressive_scan;
    analyze_opts.trace_file_path = trace_file_path;

    // Add data regions from config file
    for (const auto& dr : game_config.data_regions) {
        gbrecomp::AnalyzerOptions::DataRegion region;
        region.bank = dr.bank;
        region.start = dr.start;
        region.end = dr.end;
        analyze_opts.data_regions.push_back(region);
        std::cout << "Config data region: bank " << dr.bank << " 0x" << std::hex
                  << dr.start << "-0x" << dr.end << std::dec << "\n";
    }

    // Add HRAM overlays from config file
    for (const auto& ov : config_hram_overlays) {
        gbrecomp::AnalyzerOptions::RamOverlay overlay;
        overlay.ram_addr = ov.ram_addr;
        overlay.rom_addr = gbrecomp::AnalysisResult::make_addr(ov.source_bank, ov.source_addr);
        overlay.size = ov.size;
        analyze_opts.ram_overlays.push_back(overlay);
        std::cout << "Config HRAM overlay: 0x" << std::hex << ov.ram_addr
                  << " <- Bank " << (int)ov.source_bank << ":0x" << ov.source_addr
                  << " (" << std::dec << ov.size << " bytes)\n";
    }

    // Auto-detect standard HRAM DMA routine (fallback when no overlays in config)
    // Routine: LDH (46),A; LD A,28; DEC A; JR NZ,-3; RET
    if (config_hram_overlays.empty()) {
        const std::vector<uint8_t> pattern = {0xE0, 0x46, 0x3E, 0x28, 0x3D, 0x20, 0xFD, 0xC9};
        const std::vector<uint8_t>& data = rom.bytes();

        auto it = std::search(data.begin(), data.end(), pattern.begin(), pattern.end());
        if (it != data.end()) {
                size_t rom_idx = std::distance(data.begin(), it);
                uint8_t bank = rom_idx / 0x4000;
                uint16_t offset = rom_idx % 0x4000;
                if (bank > 0) offset += 0x4000;

                std::cout << "Detected OAM DMA routine at ROM 0x" << std::hex << rom_idx
                            << " (Bank " << (int)bank << ":0x" << offset << "). Mapping to HRAM 0xFF80.\n" << std::dec;

                gbrecomp::AnalyzerOptions::RamOverlay overlay;
                overlay.ram_addr = 0xFF80;
                overlay.rom_addr = gbrecomp::AnalysisResult::make_addr(bank, offset);
                overlay.size = 8;
                analyze_opts.ram_overlays.push_back(overlay);
        }
    }

    auto analysis = gbrecomp::analyze(rom, analyze_opts);
    
    std::cout << "  Found " << analysis.stats.total_functions << " functions\n";
    std::cout << "  Found " << analysis.stats.total_blocks << " basic blocks\n";
    std::cout << "  " << analysis.label_addresses.size() << " labels needed\n";
    
    if (verbose) {
        gbrecomp::print_analysis_summary(analysis);
    }
    
    if (analyze_only) {
        return 0;
    }
    
    // Build IR
    std::cout << "\nBuilding IR...\n";
    
    gbrecomp::ir::BuilderOptions ir_opts;
    ir_opts.emit_comments = emit_comments;
    
    gbrecomp::ir::IRBuilder builder(ir_opts);
    auto ir_program = builder.build(analysis, rom.name());
    
    std::cout << "  " << ir_program.blocks.size() << " IR blocks\n";
    std::cout << "  " << ir_program.functions.size() << " IR functions\n";
    
    // Generate code
    std::cout << "\nGenerating C code...\n";
    
    gbrecomp::codegen::GeneratorOptions gen_opts;
    gen_opts.output_prefix = sanitize_prefix(fs::path(rom_path).stem().string());
    gen_opts.output_dir = output_dir;
    gen_opts.emit_comments = emit_comments;
    gen_opts.single_function_mode = single_function;
    gen_opts.runtime_dir = game_config.runtime_dir;
    
    auto output = gbrecomp::codegen::generate_output(
        ir_program, rom.data(), rom.size(), gen_opts);
    
    // Create output directory
    fs::path out_path = output_dir;
    if (!fs::exists(out_path)) {
        std::cout << "Creating output directory: " << out_path << "\n";
        fs::create_directories(out_path);
    }
    
    // Write output files
    if (!gbrecomp::codegen::write_output(output, output_dir)) {
        std::cerr << "Error: Failed to write output files\n";
        return 1;
    }
    
    std::cout << "\nGenerated files:\n";
    std::cout << "  " << (out_path / output.header_file) << "\n";
    std::cout << "  " << (out_path / output.source_file) << "\n";
    std::cout << "  " << (out_path / output.main_file) << "\n";
    std::cout << "  " << (out_path / output.cmake_file) << "\n";
    if (!output.rom_data_file.empty()) {
        std::cout << "  " << (out_path / output.rom_data_file) << "\n";
    }
    
    std::cout << "\nBuild instructions:\n";
    std::cout << "  cd " << out_path << " && mkdir -p build && cd build && cmake -G Ninja .. && cmake --build . && ./" << gen_opts.output_prefix << "\n";
    
    return 0;
}
