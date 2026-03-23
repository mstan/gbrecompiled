/**
 * @file main.cpp
 * @brief GameBoy Recompiler CLI entry point
 */

#include "recompiler/rom.h"
#include "recompiler/decoder.h"
#include "recompiler/analyzer.h"
#include "recompiler/symbol_table.h"
#include "recompiler/ir/ir.h"
#include "recompiler/ir/ir_builder.h"
#include "recompiler/codegen/c_emitter.h"

#include <iostream>
#include <string>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
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
    std::cout << "  GameBoy Static Recompiler v0.0.2\n";
    std::cout << "  https://github.com/arcanite24/gb-recompiled\n\n";
}

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " <rom.gb|rom_directory> [options]\n\n";
    std::cout << "Directory mode recompiles every .gb/.gbc/.sgb file under the folder,\n";
    std::cout << "emits one module per ROM, and generates a launcher to choose the game.\n\n";
    std::cout << "Options:\n";
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
    std::cout << "  --symbols <file>      Load a .sym symbol file and use names for generated functions and labels\n";
    std::cout << "  --use-trace <file>    Use runtime trace to find entry points\n";
    std::cout << "  -h, --help            Show this help\n";
}

static bool detect_oam_dma_overlay(const gbrecomp::ROM& rom,
                                   gbrecomp::AnalyzerOptions::RamOverlay& overlay) {
    const std::vector<uint8_t>& data = rom.bytes();
    const std::vector<uint8_t> wait_pattern = {0xE0, 0x46, 0x3E, 0x28, 0x3D, 0x20, 0xFD, 0xC9};

    auto it = std::search(data.begin(), data.end(), wait_pattern.begin(), wait_pattern.end());
    if (it == data.end()) {
        return false;
    }

    size_t rom_idx = static_cast<size_t>(std::distance(data.begin(), it));
    size_t overlay_start = rom_idx;
    uint16_t overlay_size = static_cast<uint16_t>(wait_pattern.size());

    // Many games copy the full helper starting with LD A,imm8 before the
    // standard wait loop body. If that prefix is present, compile the actual
    // copied entry point instead of the inner loop.
    if (rom_idx >= 2 && data[rom_idx - 2] == 0x3E) {
        overlay_start = rom_idx - 2;
        overlay_size += 2;
    }

    uint8_t bank = static_cast<uint8_t>(overlay_start / 0x4000);
    uint16_t offset = static_cast<uint16_t>(overlay_start % 0x4000);
    if (bank > 0) {
        offset = static_cast<uint16_t>(offset + 0x4000);
    }

    overlay.ram_addr = 0xFF80;
    overlay.rom_addr = gbrecomp::AnalysisResult::make_addr(bank, offset);
    overlay.size = overlay_size;
    return true;
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

struct GenerationOptions {
    bool verbose = false;
    bool trace_log = false;
    size_t limit_instructions = 0;
    bool single_function = false;
    bool emit_comments = true;
    bool aggressive_scan = true;
    std::vector<uint32_t> manual_entry_points;
    std::string trace_file_path;
    std::string symbol_file_path;
};

struct MultiRomModule {
    std::string output_prefix;
    std::string display_name;
    std::string source_rom;
    gbrecomp::codegen::GeneratedOutput output;
};

static std::string escape_c_string(const std::string& input) {
    std::ostringstream ss;
    for (unsigned char ch : input) {
        switch (ch) {
            case '\\':
                ss << "\\\\";
                break;
            case '"':
                ss << "\\\"";
                break;
            case '\n':
                ss << "\\n";
                break;
            case '\r':
                ss << "\\r";
                break;
            case '\t':
                ss << "\\t";
                break;
            default:
                if (ch < 0x20) {
                    ss << '?';
                } else {
                    ss << static_cast<char>(ch);
                }
                break;
        }
    }
    return ss.str();
}

static std::string escape_json_string(const std::string& input) {
    std::ostringstream ss;
    for (unsigned char ch : input) {
        switch (ch) {
            case '\\':
                ss << "\\\\";
                break;
            case '"':
                ss << "\\\"";
                break;
            case '\n':
                ss << "\\n";
                break;
            case '\r':
                ss << "\\r";
                break;
            case '\t':
                ss << "\\t";
                break;
            default:
                if (ch < 0x20) {
                    ss << "\\u"
                       << std::hex << std::setfill('0') << std::setw(4)
                       << static_cast<unsigned>(ch)
                       << std::dec;
                } else {
                    ss << static_cast<char>(ch);
                }
                break;
        }
    }
    return ss.str();
}

static std::string to_lower_ascii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

static bool is_rom_file(const fs::path& path) {
    if (!fs::is_regular_file(path)) {
        return false;
    }

    const std::string ext = to_lower_ascii(path.extension().string());
    return ext == ".gb" || ext == ".gbc" || ext == ".sgb";
}

static std::vector<fs::path> collect_rom_paths(const fs::path& root) {
    std::vector<fs::path> rom_paths;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (is_rom_file(entry.path())) {
            rom_paths.push_back(entry.path());
        }
    }
    std::sort(rom_paths.begin(), rom_paths.end());
    return rom_paths;
}

static std::string make_unique_prefix(const std::string& base, std::set<std::string>& used_prefixes) {
    std::string candidate = sanitize_prefix(base);
    if (candidate.empty()) {
        candidate = "rom";
    }

    std::string unique = candidate;
    unsigned suffix = 2;
    while (!used_prefixes.insert(unique).second) {
        unique = candidate + "_" + std::to_string(suffix++);
    }
    return unique;
}

static std::string build_runtime_relative_path(const fs::path& output_dir) {
    fs::path relative_out;
    try {
        relative_out = fs::relative(output_dir);
    } catch (...) {
        relative_out = output_dir;
    }

    int depth = 0;
    for (const auto& part : relative_out) {
        const std::string text = part.string();
        if (text == "." || text == "/" || text.empty()) {
            continue;
        }
        depth++;
    }
    if (depth == 0) {
        depth = 1;
    }

    std::ostringstream ss;
    for (int i = 0; i < depth; i++) {
        ss << "../";
    }
    ss << "runtime";
    return ss.str();
}

static bool write_text_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << content;
    return static_cast<bool>(out);
}

static std::string make_launcher_source(const std::string& launcher_name,
                                        const std::vector<MultiRomModule>& modules) {
    std::ostringstream ss;
    ss << "/* Multi-ROM launcher generated by gbrecomp */\n";
    for (const MultiRomModule& module : modules) {
        ss << "#include \"" << module.output_prefix << ".h\"\n";
    }
    ss << "#include <ctype.h>\n";
    ss << "#include <stdio.h>\n";
    ss << "#include <stdlib.h>\n";
    ss << "#include <string.h>\n\n";
    ss << "typedef int (*GBLauncherMainFn)(int argc, char* argv[]);\n\n";
    ss << "typedef struct {\n";
    ss << "    const char* id;\n";
    ss << "    const char* title;\n";
    ss << "    const char* rom_path;\n";
    ss << "    GBLauncherMainFn main_fn;\n";
    ss << "} GBLauncherGame;\n\n";
    ss << "static GBLauncherGame g_games[] = {\n";
    for (const MultiRomModule& module : modules) {
        ss << "    {\""
           << escape_c_string(module.output_prefix) << "\", \""
           << escape_c_string(module.display_name) << "\", \""
           << escape_c_string(module.source_rom) << "\", "
           << module.output_prefix << "_main},\n";
    }
    ss << "};\n\n";
    ss << "static const size_t g_game_count = sizeof(g_games) / sizeof(g_games[0]);\n\n";
    ss << "static char* trim_ascii(char* text) {\n";
    ss << "    while (text && *text && isspace((unsigned char)*text)) {\n";
    ss << "        text++;\n";
    ss << "    }\n";
    ss << "    if (!text || !*text) {\n";
    ss << "        return text;\n";
    ss << "    }\n";
    ss << "    char* end = text + strlen(text);\n";
    ss << "    while (end > text && isspace((unsigned char)end[-1])) {\n";
    ss << "        end--;\n";
    ss << "    }\n";
    ss << "    *end = '\\0';\n";
    ss << "    return text;\n";
    ss << "}\n\n";
    ss << "static void print_usage(const char* program) {\n";
    ss << "    fprintf(stderr, \"Usage: %s [--list-games] [--game <id>] [game arguments...]\\n\", program);\n";
    ss << "}\n\n";
    ss << "static void print_games(void) {\n";
    ss << "    fprintf(stderr, \"Available games in " << escape_c_string(launcher_name) << ":\\n\");\n";
    ss << "    for (size_t i = 0; i < g_game_count; i++) {\n";
    ss << "        fprintf(stderr, \"  %zu. %s [%s]\\n\", i + 1, g_games[i].title, g_games[i].id);\n";
    ss << "    }\n";
    ss << "}\n\n";
    ss << "static const GBLauncherGame* find_game_by_id(const char* id) {\n";
    ss << "    if (!id) {\n";
    ss << "        return NULL;\n";
    ss << "    }\n";
    ss << "    for (size_t i = 0; i < g_game_count; i++) {\n";
    ss << "        if (strcmp(g_games[i].id, id) == 0) {\n";
    ss << "            return &g_games[i];\n";
    ss << "        }\n";
    ss << "    }\n";
    ss << "    return NULL;\n";
    ss << "}\n\n";
    ss << "static const GBLauncherGame* prompt_for_game(void) {\n";
    ss << "    char line[256];\n";
    ss << "    print_games();\n";
    ss << "    fprintf(stderr, \"Select a game by number or id: \");\n";
    ss << "    if (!fgets(line, sizeof(line), stdin)) {\n";
    ss << "        return NULL;\n";
    ss << "    }\n";
    ss << "    char* selection = trim_ascii(line);\n";
    ss << "    if (!selection || !*selection) {\n";
    ss << "        return NULL;\n";
    ss << "    }\n";
    ss << "    char* end = NULL;\n";
    ss << "    long index = strtol(selection, &end, 10);\n";
    ss << "    end = trim_ascii(end);\n";
    ss << "    if (selection != end && end && !*end) {\n";
    ss << "        if (index >= 1 && (size_t)index <= g_game_count) {\n";
    ss << "            return &g_games[index - 1];\n";
    ss << "        }\n";
    ss << "    }\n";
    ss << "    return find_game_by_id(selection);\n";
    ss << "}\n\n";
    ss << "int main(int argc, char* argv[]) {\n";
    ss << "    const GBLauncherGame* selected = NULL;\n";
    ss << "    char** forwarded_argv = (char**)calloc((size_t)argc + 1, sizeof(char*));\n";
    ss << "    int forwarded_argc = 1;\n";
    ss << "    if (!forwarded_argv) {\n";
    ss << "        fprintf(stderr, \"Failed to allocate launcher argument buffer\\n\");\n";
    ss << "        return 1;\n";
    ss << "    }\n";
    ss << "    forwarded_argv[0] = argv[0];\n";
    ss << "    for (int i = 1; i < argc; i++) {\n";
    ss << "        if (strcmp(argv[i], \"--list-games\") == 0) {\n";
    ss << "            print_games();\n";
    ss << "            free(forwarded_argv);\n";
    ss << "            return 0;\n";
    ss << "        }\n";
    ss << "        if ((strcmp(argv[i], \"-h\") == 0) || (strcmp(argv[i], \"--help\") == 0)) {\n";
    ss << "            print_usage(argv[0]);\n";
    ss << "            print_games();\n";
    ss << "            free(forwarded_argv);\n";
    ss << "            return 0;\n";
    ss << "        }\n";
    ss << "        if (strcmp(argv[i], \"--game\") == 0) {\n";
    ss << "            if (i + 1 >= argc) {\n";
    ss << "                fprintf(stderr, \"Missing value for --game\\n\");\n";
    ss << "                print_usage(argv[0]);\n";
    ss << "                free(forwarded_argv);\n";
    ss << "                return 1;\n";
    ss << "            }\n";
    ss << "            selected = find_game_by_id(argv[++i]);\n";
    ss << "            if (!selected) {\n";
    ss << "                fprintf(stderr, \"Unknown game id '%s'\\n\", argv[i]);\n";
    ss << "                print_games();\n";
    ss << "                free(forwarded_argv);\n";
    ss << "                return 1;\n";
    ss << "            }\n";
    ss << "            continue;\n";
    ss << "        }\n";
    ss << "        forwarded_argv[forwarded_argc++] = argv[i];\n";
    ss << "    }\n";
    ss << "    forwarded_argv[forwarded_argc] = NULL;\n";
    ss << "    if (!selected) {\n";
    ss << "        if (g_game_count == 1) {\n";
    ss << "            selected = &g_games[0];\n";
    ss << "        } else {\n";
    ss << "            selected = prompt_for_game();\n";
    ss << "            if (!selected) {\n";
    ss << "                fprintf(stderr, \"No game selected\\n\");\n";
    ss << "                free(forwarded_argv);\n";
    ss << "                return 1;\n";
    ss << "            }\n";
    ss << "        }\n";
    ss << "    }\n";
    ss << "    fprintf(stderr, \"[LAUNCH] Starting %s [%s]\\n\", selected->title, selected->id);\n";
    ss << "    int rc = selected->main_fn(forwarded_argc, forwarded_argv);\n";
    ss << "    free(forwarded_argv);\n";
    ss << "    return rc;\n";
    ss << "}\n";
    return ss.str();
}

static std::string make_launcher_manifest(const std::string& launcher_name,
                                          const std::vector<MultiRomModule>& modules) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"launcher\": \"" << escape_json_string(launcher_name) << "\",\n";
    ss << "  \"games\": [\n";
    for (size_t i = 0; i < modules.size(); i++) {
        const MultiRomModule& module = modules[i];
        ss << "    {\n";
        ss << "      \"id\": \"" << escape_json_string(module.output_prefix) << "\",\n";
        ss << "      \"title\": \"" << escape_json_string(module.display_name) << "\",\n";
        ss << "      \"rom_path\": \"" << escape_json_string(module.source_rom) << "\",\n";
        ss << "      \"metadata_file\": \""
           << escape_json_string(module.output_prefix + "_metadata.json") << "\"\n";
        ss << "    }" << (i + 1 < modules.size() ? "," : "") << "\n";
    }
    ss << "  ]\n";
    ss << "}\n";
    return ss.str();
}

static std::string make_multi_rom_cmake(const std::string& project_name,
                                        const fs::path& output_dir,
                                        const std::vector<MultiRomModule>& modules) {
    const std::string runtime_path = build_runtime_relative_path(output_dir);

    std::vector<std::string> executable_sources = {"launcher_main.c"};
    std::vector<std::string> generated_sources;

    for (const MultiRomModule& module : modules) {
        executable_sources.push_back(module.output.main_file);
        executable_sources.push_back(module.output.source_file);
        executable_sources.push_back(module.output.rom_data_file);
        generated_sources.push_back(module.output.main_file);
        generated_sources.push_back(module.output.source_file);
        for (const auto& extra_file : module.output.extra_files) {
            if (extra_file.is_source) {
                executable_sources.push_back(extra_file.filename);
                generated_sources.push_back(extra_file.filename);
            }
        }
    }

    std::ostringstream ss;
    ss << "cmake_minimum_required(VERSION 3.16)\n";
    ss << "project(" << project_name << " C CXX)\n\n";
    ss << "set(CMAKE_C_STANDARD 11)\n";
    ss << "set(CMAKE_C_STANDARD_REQUIRED ON)\n\n";
    ss << "if(NOT CMAKE_BUILD_TYPE)\n";
    ss << "    set(CMAKE_BUILD_TYPE Release)\n";
    ss << "endif()\n";
    ss << "if(CMAKE_C_COMPILER_ID MATCHES \"GNU|Clang\")\n";
    ss << "    add_compile_options(-O3)\n";
    ss << "endif()\n\n";
    ss << "set(GBRT_DIR \"${CMAKE_CURRENT_SOURCE_DIR}/" << runtime_path << "\")\n\n";
    ss << "find_package(SDL2 REQUIRED)\n\n";
    ss << "add_library(gbrt STATIC\n";
    ss << "    ${GBRT_DIR}/src/gbrt.c\n";
    ss << "    ${GBRT_DIR}/src/differential.c\n";
    ss << "    ${GBRT_DIR}/src/ppu.c\n";
    ss << "    ${GBRT_DIR}/src/audio.c\n";
    ss << "    ${GBRT_DIR}/src/audio_stats.c\n";
    ss << "    ${GBRT_DIR}/src/interpreter.c\n";
    ss << "    ${GBRT_DIR}/src/platform_sdl.cpp\n";
    ss << ")\n\n";
    ss << "target_sources(gbrt PRIVATE\n";
    ss << "    ${GBRT_DIR}/third_party/imgui/imgui.cpp\n";
    ss << "    ${GBRT_DIR}/third_party/imgui/imgui_draw.cpp\n";
    ss << "    ${GBRT_DIR}/third_party/imgui/imgui_tables.cpp\n";
    ss << "    ${GBRT_DIR}/third_party/imgui/imgui_widgets.cpp\n";
    ss << "    ${GBRT_DIR}/third_party/imgui/imgui_demo.cpp\n";
    ss << "    ${GBRT_DIR}/third_party/imgui/backends/imgui_impl_sdl2.cpp\n";
    ss << "    ${GBRT_DIR}/third_party/imgui/backends/imgui_impl_sdlrenderer2.cpp\n";
    ss << ")\n";
    ss << "target_include_directories(gbrt PUBLIC\n";
    ss << "    ${GBRT_DIR}/include\n";
    ss << "    ${GBRT_DIR}/third_party/imgui\n";
    ss << ")\n";
    ss << "target_link_libraries(gbrt PUBLIC SDL2::SDL2)\n";
    ss << "target_compile_definitions(gbrt PUBLIC GB_HAS_SDL2)\n\n";
    ss << "add_executable(" << project_name << "\n";
    for (const std::string& filename : executable_sources) {
        ss << "    " << filename << "\n";
    }
    ss << ")\n\n";
    ss << "set(GBRECOMP_GENERATED_OPT_LEVEL \"1\" CACHE STRING \"Optimization level for generated ROM source files\")\n";
    ss << "set(GBRECOMP_GENERATED_SOURCES\n";
    for (const std::string& filename : generated_sources) {
        ss << "    " << filename << "\n";
    }
    ss << ")\n";
    ss << "set_source_files_properties(${GBRECOMP_GENERATED_SOURCES} PROPERTIES COMPILE_OPTIONS \"-O${GBRECOMP_GENERATED_OPT_LEVEL}\")\n\n";
    ss << "target_link_libraries(" << project_name << " gbrt)\n";
    return ss.str();
}

static bool generate_multi_rom_module(const fs::path& rom_path,
                                      const GenerationOptions& options,
                                      const fs::path& output_dir,
                                      const std::string& output_prefix,
                                      MultiRomModule& module) {
    std::cout << "\n[" << output_prefix << "] Loading ROM: " << rom_path << "\n";

    auto rom_opt = gbrecomp::ROM::load(rom_path);
    if (!rom_opt) {
        std::cerr << "Error: Failed to load ROM " << rom_path << "\n";
        return false;
    }

    auto& rom = *rom_opt;
    if (!rom.is_valid()) {
        std::cerr << "Error: " << rom.error() << "\n";
        return false;
    }

    const std::string display_name = !rom.header().title.empty() ? rom.header().title : rom.name();
    std::cout << "[" << output_prefix << "] Title: " << display_name << "\n";
    if (options.verbose) {
        gbrecomp::print_rom_info(rom);
    }

    gbrecomp::SymbolTable symbol_table;
    if (!options.symbol_file_path.empty()) {
        std::string error;
        if (!symbol_table.load_sym_file(options.symbol_file_path, &error)) {
            std::cerr << "Error: " << error << "\n";
            return false;
        }
    }

    gbrecomp::AnalyzerOptions analyze_opts;
    analyze_opts.trace_log = options.trace_log;
    analyze_opts.verbose = options.verbose;
    analyze_opts.max_instructions = options.limit_instructions;
    analyze_opts.entry_points = options.manual_entry_points;
    analyze_opts.aggressive_scan = options.aggressive_scan;
    analyze_opts.trace_file_path = options.trace_file_path;

    gbrecomp::AnalyzerOptions::RamOverlay overlay;
    if (detect_oam_dma_overlay(rom, overlay)) {
        uint8_t bank = static_cast<uint8_t>(overlay.rom_addr >> 16);
        uint16_t offset = static_cast<uint16_t>(overlay.rom_addr & 0xFFFF);
        size_t rom_idx = (offset < 0x4000)
            ? offset
            : static_cast<size_t>(bank) * 0x4000 + (offset - 0x4000);

        std::cout << "[" << output_prefix << "] Detected OAM DMA routine at ROM 0x"
                  << std::hex << rom_idx
                  << " (Bank " << (int)bank << ":0x" << offset << ", size "
                  << std::dec << overlay.size << "). Mapping to HRAM 0xFF80.\n";
        analyze_opts.ram_overlays.push_back(overlay);
    }

    auto analysis = gbrecomp::analyze(rom, analyze_opts);
    if (symbol_table.size() > 0) {
        gbrecomp::apply_symbols_to_analysis(symbol_table, analysis);
    }

    std::cout << "[" << output_prefix << "] Found "
              << analysis.stats.total_functions << " functions, "
              << analysis.stats.total_blocks << " basic blocks\n";

    gbrecomp::ir::BuilderOptions ir_opts;
    ir_opts.emit_comments = options.emit_comments;

    gbrecomp::ir::IRBuilder builder(ir_opts);
    auto ir_program = builder.build(analysis, rom.name());

    gbrecomp::codegen::GeneratorOptions gen_opts;
    gen_opts.output_prefix = output_prefix;
    gen_opts.output_dir = output_dir.string();
    gen_opts.emit_comments = options.emit_comments;
    gen_opts.single_function_mode = options.single_function;
    gen_opts.use_prefixed_symbols = true;
    gen_opts.emit_main_entry_point = false;
    gen_opts.emit_cmake = false;

    auto output = gbrecomp::codegen::generate_output(
        ir_program, rom.data(), rom.size(), gen_opts);

    if (!gbrecomp::codegen::write_output(output, output_dir.string())) {
        std::cerr << "Error: Failed to write output files for " << rom_path << "\n";
        return false;
    }

    module.output_prefix = output_prefix;
    module.display_name = display_name;
    module.source_rom = rom_path.lexically_normal().string();
    module.output = std::move(output);
    return true;
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
    bool disasm_only = false;
    bool analyze_only = false;
    bool verbose = false;
    bool trace_log = false;
    size_t limit_instructions = 0;
    bool single_function = false;
    bool emit_comments = true;
    bool aggressive_scan = true;
    int specific_bank = -1;
    std::vector<uint32_t> manual_entry_points;
    std::string trace_file_path;
    std::string symbol_file_path;
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 < argc) {
                output_dir = argv[++i];
            }
        } else if (arg == "-d" || arg == "--disasm") {
            disasm_only = true;
        } else if (arg == "-a" || arg == "--analyze") {
            analyze_only = true;
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "--trace") {
            trace_log = true;
        } else if (arg == "--limit") {
            if (i + 1 < argc) {
                limit_instructions = std::stoul(argv[++i]);
            }
        } else if (arg == "--single-function") {
            single_function = true;
        } else if (arg == "--no-comments") {
            emit_comments = false;
        } else if (arg == "--bank") {
            if (i + 1 < argc) {
                specific_bank = std::stoi(argv[++i]);
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
            aggressive_scan = false;
        } else if (arg == "--use-trace") {
            if (i + 1 < argc) {
                trace_file_path = argv[++i];
            }
        } else if (arg == "--symbols") {
            if (i + 1 < argc) {
                symbol_file_path = argv[++i];
            }
        } else if (arg[0] != '-') {
            rom_path = arg;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return 1;
        }
    }
    
    if (rom_path.empty()) {
        std::cerr << "Error: No ROM file specified\n";
        print_usage(argv[0]);
        return 1;
    }

    GenerationOptions generation_opts;
    generation_opts.verbose = verbose;
    generation_opts.trace_log = trace_log;
    generation_opts.limit_instructions = limit_instructions;
    generation_opts.single_function = single_function;
    generation_opts.emit_comments = emit_comments;
    generation_opts.aggressive_scan = aggressive_scan;
    generation_opts.manual_entry_points = manual_entry_points;
    generation_opts.trace_file_path = trace_file_path;
    generation_opts.symbol_file_path = symbol_file_path;
    
    print_banner();

    fs::path input_path = rom_path;
    if (fs::exists(input_path) && fs::is_directory(input_path)) {
        if (disasm_only || analyze_only) {
            std::cerr << "Error: Directory mode does not support --disasm or --analyze\n";
            return 1;
        }
        if (specific_bank >= 0 || !manual_entry_points.empty()) {
            std::cerr << "Error: Directory mode does not support --bank or --add-entry-point\n";
            return 1;
        }
        if (!trace_file_path.empty() || !symbol_file_path.empty()) {
            std::cerr << "Error: Directory mode does not support --use-trace or --symbols\n";
            return 1;
        }

        if (output_dir.empty()) {
            output_dir = input_path.filename().string() + "_output";
        }

        fs::path out_path = output_dir;
        if (!fs::exists(out_path)) {
            std::cout << "Creating output directory: " << out_path << "\n";
            fs::create_directories(out_path);
        }

        std::vector<fs::path> rom_paths = collect_rom_paths(input_path);
        if (rom_paths.empty()) {
            std::cerr << "Error: No .gb/.gbc/.sgb files found under " << input_path << "\n";
            return 1;
        }

        std::cout << "Discovered " << rom_paths.size() << " ROMs under " << input_path << "\n";

        std::set<std::string> used_prefixes;
        std::vector<MultiRomModule> modules;
        modules.reserve(rom_paths.size());

        for (const fs::path& rom_file : rom_paths) {
            const std::string prefix = make_unique_prefix(rom_file.stem().string(), used_prefixes);
            MultiRomModule module;
            if (!generate_multi_rom_module(rom_file, generation_opts, out_path, prefix, module)) {
                return 1;
            }
            modules.push_back(std::move(module));
        }

        std::string launcher_name = sanitize_prefix(out_path.filename().string());
        if (launcher_name.empty()) {
            launcher_name = sanitize_prefix(input_path.filename().string());
        }
        if (launcher_name.empty()) {
            launcher_name = "multi_rom_launcher";
        }

        const fs::path launcher_source_path = out_path / "launcher_main.c";
        const fs::path launcher_manifest_path = out_path / "launcher_manifest.json";
        const fs::path launcher_cmake_path = out_path / "CMakeLists.txt";

        if (!write_text_file(launcher_source_path, make_launcher_source(launcher_name, modules))) {
            std::cerr << "Error: Failed to write " << launcher_source_path << "\n";
            return 1;
        }
        if (!write_text_file(launcher_manifest_path, make_launcher_manifest(launcher_name, modules))) {
            std::cerr << "Error: Failed to write " << launcher_manifest_path << "\n";
            return 1;
        }
        if (!write_text_file(launcher_cmake_path, make_multi_rom_cmake(launcher_name, out_path, modules))) {
            std::cerr << "Error: Failed to write " << launcher_cmake_path << "\n";
            return 1;
        }

        std::cout << "\nGenerated multi-ROM launcher files:\n";
        std::cout << "  " << launcher_source_path << "\n";
        std::cout << "  " << launcher_manifest_path << "\n";
        std::cout << "  " << launcher_cmake_path << "\n";
        std::cout << "\nGames:\n";
        for (const MultiRomModule& module : modules) {
            std::cout << "  " << module.output_prefix << " -> " << module.display_name << "\n";
        }
        std::cout << "\nBuild instructions:\n";
        std::cout << "  cd " << out_path << " && mkdir -p build && cd build && cmake -G Ninja .. && ninja && ./" << launcher_name << "\n";
        return 0;
    }
    
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

    gbrecomp::SymbolTable symbol_table;
    if (!symbol_file_path.empty()) {
        std::string error;
        if (!symbol_table.load_sym_file(symbol_file_path, &error)) {
            std::cerr << "Error: " << error << "\n";
            return 1;
        }
        std::cout << "Loaded " << symbol_table.size() << " symbols from " << symbol_file_path << "\n";
    }
    
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

    gbrecomp::AnalyzerOptions::RamOverlay overlay;
    if (detect_oam_dma_overlay(rom, overlay)) {
            uint8_t bank = static_cast<uint8_t>(overlay.rom_addr >> 16);
            uint16_t offset = static_cast<uint16_t>(overlay.rom_addr & 0xFFFF);
            size_t rom_idx = (offset < 0x4000)
                ? offset
                : static_cast<size_t>(bank) * 0x4000 + (offset - 0x4000);

            std::cout << "Detected OAM DMA routine at ROM 0x" << std::hex << rom_idx
                        << " (Bank " << (int)bank << ":0x" << offset << ", size "
                        << std::dec << overlay.size << "). Mapping to HRAM 0xFF80.\n";

            analyze_opts.ram_overlays.push_back(overlay);
    }

    auto analysis = gbrecomp::analyze(rom, analyze_opts);
    if (symbol_table.size() > 0) {
        gbrecomp::apply_symbols_to_analysis(symbol_table, analysis);
    }
    
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
