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
    ss << "#define SDL_MAIN_HANDLED\n";
    ss << "extern \"C\" {\n";
    for (const MultiRomModule& module : modules) {
        ss << "#include \"" << module.output_prefix << ".h\"\n";
    }
    ss << "}\n\n";
    ss << R"LAUNCHER(#include <SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform_sdl.h"
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

typedef int (*GBLauncherMainFn)(int argc, char* argv[]);

typedef struct {
    const char* id;
    const char* title;
    const char* rom_path;
    GBLauncherMainFn main_fn;
} GBLauncherGame;

)LAUNCHER";
    for (const MultiRomModule& module : modules) {
        ss << "static int launch_" << module.output_prefix << "(int argc, char* argv[]) {\n";
        ss << "    return " << module.output_prefix << "_main(argc, argv);\n";
        ss << "}\n";
    }
    ss << "\n";
    ss << "static GBLauncherGame g_games[] = {\n";
    for (const MultiRomModule& module : modules) {
        ss << "    {\""
           << escape_c_string(module.output_prefix) << "\", \""
           << escape_c_string(module.display_name) << "\", \""
           << escape_c_string(module.source_rom) << "\", "
           << "launch_" << module.output_prefix << "},\n";
    }
    ss << "};\n\n";
    ss << "static const char* g_launcher_name = \"" << escape_c_string(launcher_name) << "\";\n";
    ss << R"LAUNCHER(static const size_t g_game_count = sizeof(g_games) / sizeof(g_games[0]);

static char* trim_ascii(char* text) {
    while (text && *text && isspace((unsigned char)*text)) {
        text++;
    }
    if (!text || !*text) {
        return text;
    }
    char* end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return text;
}

static void print_usage(const char* program) {
    fprintf(stderr, "Usage: %s [--list-games] [--game <id>] [game arguments...]\n", program);
    fprintf(stderr, "Run without --game to open the graphical launcher.\n");
}

static void print_games(void) {
    fprintf(stderr, "Available games in %s:\n", g_launcher_name);
    for (size_t i = 0; i < g_game_count; i++) {
        fprintf(stderr, "  %zu. %s [%s]\n", i + 1, g_games[i].title, g_games[i].id);
    }
}

static const GBLauncherGame* find_game_by_id(const char* id) {
    if (!id) {
        return NULL;
    }
    for (size_t i = 0; i < g_game_count; i++) {
        if (strcmp(g_games[i].id, id) == 0) {
            return &g_games[i];
        }
    }
    return NULL;
}

static const GBLauncherGame* prompt_for_game(void) {
    char line[256];
    print_games();
    fprintf(stderr, "Select a game by number or id: ");
    if (!fgets(line, sizeof(line), stdin)) {
        return NULL;
    }
    char* selection = trim_ascii(line);
    if (!selection || !*selection) {
        return NULL;
    }
    char* end = NULL;
    long index = strtol(selection, &end, 10);
    end = trim_ascii(end);
    if (selection != end && end && !*end) {
        if (index >= 1 && (size_t)index <= g_game_count) {
            return &g_games[index - 1];
        }
    }
    return find_game_by_id(selection);
}

static bool draw_game_row(const GBLauncherGame* game, int index, bool is_selected, bool* out_double_clicked) {
    const float row_height = ImGui::GetTextLineHeight() * 2.8f + 10.0f;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    ImGui::PushID(index);
    ImVec2 size = ImVec2(ImGui::GetContentRegionAvail().x, row_height);
    if (size.x < 1.0f) {
        size.x = 1.0f;
    }

    ImVec2 min = ImGui::GetCursorScreenPos();
    bool activated = ImGui::InvisibleButton("game_row", size);
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();
    bool double_clicked = hovered && ImGui::IsMouseDoubleClicked(0);
    ImVec2 max = ImGui::GetItemRectMax();
    ImVec2 title_pos = ImVec2(min.x + 12.0f, min.y + 8.0f);
    ImVec2 id_pos = ImVec2(title_pos.x, title_pos.y + ImGui::GetTextLineHeight() + 4.0f);

    ImU32 bg_color = ImGui::GetColorU32(is_selected ? ImGuiCol_Header : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
    if (held) {
        bg_color = ImGui::GetColorU32(ImGuiCol_HeaderActive);
    }
    draw_list->AddRectFilled(min, max, bg_color, 10.0f);
    if (is_selected) {
        draw_list->AddRectFilled(ImVec2(min.x + 2.0f, min.y + 6.0f),
                                 ImVec2(min.x + 6.0f, max.y - 6.0f),
                                 ImGui::GetColorU32(ImGuiCol_Button),
                                 3.0f);
    }

    ImGui::PushClipRect(ImVec2(min.x + 10.0f, min.y), ImVec2(max.x - 10.0f, max.y), true);
    draw_list->AddText(title_pos, ImGui::GetColorU32(ImGuiCol_Text), game->title);
    draw_list->AddText(id_pos, ImGui::GetColorU32(ImGuiCol_TextDisabled), game->id);
    ImGui::PopClipRect();
    ImGui::PopID();

    if (out_double_clicked) {
        *out_double_clicked = double_clicked;
    }
    return activated;
}

static void apply_launcher_style(void) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 18.0f;
    style.ChildRounding = 16.0f;
    style.FrameRounding = 12.0f;
    style.PopupRounding = 12.0f;
    style.GrabRounding = 12.0f;
    style.ScrollbarRounding = 12.0f;
    style.WindowPadding = ImVec2(20.0f, 20.0f);
    style.FramePadding = ImVec2(14.0f, 10.0f);
    style.ItemSpacing = ImVec2(12.0f, 12.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.08f, 0.11f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.12f, 0.16f, 0.95f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.17f, 0.21f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.22f, 0.27f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.27f, 0.33f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.11f, 0.15f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.09f, 0.13f, 0.18f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.14f, 0.45f, 0.40f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.56f, 0.50f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.12f, 0.37f, 0.33f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.18f, 0.24f, 0.32f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.23f, 0.31f, 0.41f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.16f, 0.22f, 0.31f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.24f, 0.31f, 0.40f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.24f, 0.31f, 0.40f, 0.55f);
}

static int init_graphical_launcher(SDL_Window** out_window, SDL_Renderer** out_renderer) {
    *out_window = NULL;
    *out_renderer = NULL;

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[LAUNCH] SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }

    SDL_Window* window = SDL_CreateWindow(
        "GameBoy Recompiled Launcher",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1040,
        640,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!window) {
        fprintf(stderr, "[LAUNCH] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 0;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        fprintf(stderr, "[LAUNCH] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;

    apply_launcher_style();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    *out_window = window;
    *out_renderer = renderer;
    return 1;
}

static void shutdown_graphical_launcher(SDL_Window* window, SDL_Renderer* renderer) {
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
}

static int run_graphical_launcher(const GBLauncherGame** out_selected) {
    *out_selected = NULL;
    if (g_game_count == 0) {
        return 1;
    }

    SDL_Window* window = NULL;
    SDL_Renderer* renderer = NULL;
    if (!init_graphical_launcher(&window, &renderer)) {
        return 0;
    }

    int selected_index = 0;
    bool running = true;
    bool accepted = false;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                running = false;
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings;

        if (ImGui::Begin("Launcher", NULL, window_flags)) {
            const GBLauncherGame* game = &g_games[selected_index];
            float list_width = ImGui::GetContentRegionAvail().x * 0.46f;

            ImGui::TextUnformatted("GameBoy Recompiled");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", g_launcher_name);
            ImGui::Separator();
            ImGui::TextWrapped("Choose one of the generated ROMs and press Launch Game. Any additional command line arguments will be forwarded to the selected runtime.");
            ImGui::Spacing();

            ImGui::BeginChild("game_list", ImVec2(list_width, -72.0f), true);
            ImGui::TextDisabled("Available Games");
            ImGui::Separator();
            for (size_t i = 0; i < g_game_count; i++) {
                bool is_selected = selected_index == (int)i;
                bool double_clicked = false;
                if (draw_game_row(&g_games[i], (int)i, is_selected, &double_clicked)) {
                    selected_index = (int)i;
                    if (double_clicked) {
                        accepted = true;
                        running = false;
                    }
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("game_details", ImVec2(0.0f, -72.0f), true);
            ImGui::TextDisabled("Selected Game");
            ImGui::Separator();
            ImGui::Text("%s", game->title);
            ImGui::Spacing();
            ImGui::TextDisabled("Game ID");
            ImGui::TextUnformatted(game->id);
            ImGui::Spacing();
            ImGui::TextDisabled("Source ROM");
            ImGui::TextWrapped("%s", game->rom_path);
            ImGui::Spacing();
            ImGui::TextDisabled("Controls");
            ImGui::BulletText("Double-click a game to launch immediately.");
            ImGui::BulletText("Press Enter to launch the current selection.");
            ImGui::BulletText("Press Escape or Quit to close the launcher.");
            ImGui::EndChild();

            if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                accepted = true;
                running = false;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                running = false;
            }

            if (ImGui::Button("Launch Game", ImVec2(180.0f, 0.0f))) {
                accepted = true;
                running = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Quit", ImVec2(120.0f, 0.0f))) {
                running = false;
            }
        }
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 11, 14, 19, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
        SDL_RenderPresent(renderer);
    }

    if (accepted && selected_index >= 0 && (size_t)selected_index < g_game_count) {
        *out_selected = &g_games[selected_index];
    }

    shutdown_graphical_launcher(window, renderer);
    return 1;
}

int main(int argc, char* argv[]) {
    const GBLauncherGame* selected = NULL;
    char** forwarded_argv = (char**)calloc((size_t)argc + 1, sizeof(char*));
    int forwarded_argc = 1;
    if (!forwarded_argv) {
        fprintf(stderr, "Failed to allocate launcher argument buffer\n");
        return 1;
    }

    forwarded_argv[0] = argv[0];
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list-games") == 0) {
            print_games();
            free(forwarded_argv);
            return 0;
        }
        if ((strcmp(argv[i], "-h") == 0) || (strcmp(argv[i], "--help") == 0)) {
            print_usage(argv[0]);
            print_games();
            free(forwarded_argv);
            return 0;
        }
        if (strcmp(argv[i], "--game") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --game\n");
                print_usage(argv[0]);
                free(forwarded_argv);
                return 1;
            }
            selected = find_game_by_id(argv[++i]);
            if (!selected) {
                fprintf(stderr, "Unknown game id '%s'\n", argv[i]);
                print_games();
                free(forwarded_argv);
                return 1;
            }
            continue;
        }
        forwarded_argv[forwarded_argc++] = argv[i];
    }
    forwarded_argv[forwarded_argc] = NULL;

    for (;;) {
        if (!selected) {
            if (run_graphical_launcher(&selected)) {
                if (!selected) {
                    free(forwarded_argv);
                    return 0;
                }
            } else {
                fprintf(stderr, "[LAUNCH] Graphical launcher unavailable, falling back to terminal selection.\n");
                selected = prompt_for_game();
            }
            if (!selected) {
                fprintf(stderr, "No game selected\n");
                free(forwarded_argv);
                return 1;
            }
        }

        fprintf(stderr, "[LAUNCH] Starting %s [%s]\n", selected->title, selected->id);
        gb_platform_set_launcher_return_enabled(true);
        int rc = selected->main_fn(forwarded_argc, forwarded_argv);
        if (rc == GB_PLATFORM_RETURN_TO_LAUNCHER_EXIT_CODE) {
            selected = NULL;
            continue;
        }
        free(forwarded_argv);
        return rc;
    }
}
)LAUNCHER";
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

    std::vector<std::string> executable_sources = {"launcher_main.cpp"};
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

        const fs::path launcher_source_path = out_path / "launcher_main.cpp";
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
