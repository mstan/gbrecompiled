/**
 * @file platform_sdl.cpp
 * @brief SDL2 platform implementation for GameBoy runtime with ImGui
 */

#include "platform_sdl.h"
#include "gbrt.h"   /* For GBPlatformCallbacks */
#include "ppu.h"
#include "audio_stats.h"
#include "gbrt_debug.h"

#ifdef GB_HAS_SDL2
#include <SDL.h>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <string>
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

namespace fs = std::filesystem;

/* ============================================================================
 * SDL State
 * ========================================================================== */

static SDL_Window* g_window = NULL;
static SDL_Renderer* g_renderer = NULL;
static SDL_Texture* g_texture = NULL;
static int g_scale = 5;
static int g_windowed_width = GB_SCREEN_WIDTH * 5;
static int g_windowed_height = GB_SCREEN_HEIGHT * 5;
static SDL_Rect g_game_viewport = {0, 0, GB_SCREEN_WIDTH * 5, GB_SCREEN_HEIGHT * 5};
static uint32_t g_last_frame_time = 0;
static SDL_AudioDeviceID g_audio_device = 0;
static SDL_GameController* g_controller = NULL;
static bool g_vsync = false;  /* VSync OFF - we pace with wall clock for 59.7 FPS */
static bool g_audio_started = false;
static uint32_t g_audio_start_threshold = 0;

/* Performance timing diagnostics */
static double g_timing_render_total = 0.0;
static double g_timing_vsync_total = 0.0;
static uint32_t g_timing_frame_count = 0;
static GBPlatformTimingInfo g_last_timing = {};

/* Menu State */
static bool g_show_menu = false;
static bool g_show_overlay = false;
static int g_speed_percent = 100;
static int g_palette_idx = 0;
static bool g_smooth_lcd_transitions = true;
static bool g_launcher_return_enabled = false;
static bool g_benchmark_mode = false;
static bool g_fullscreen = false;
static bool g_app_suspended = false;
static bool g_renderer_reset_pending = false;
static GBPlatformExitAction g_exit_action = GB_PLATFORM_EXIT_QUIT;
static const char* g_palette_names[] = { "Original (Green)", "Black & White (Pocket)", "Amber (Plasma)" };
static const char* g_scale_names[] = { "1x (160x144)", "2x (320x288)", "3x (480x432)", "4x (640x576)", "5x (800x720)", "6x (960x864)", "7x (1120x1008)", "8x (1280x1152)" };
typedef enum GBRenderScalingMode {
    GB_RENDER_SCALING_PIXEL_PERFECT = 0,
    GB_RENDER_SCALING_ASPECT_FIT = 1,
    GB_RENDER_SCALING_ASPECT_FILL = 2,
    GB_RENDER_SCALING_STRETCH = 3,
} GBRenderScalingMode;
typedef enum GBRenderFilterMode {
    GB_RENDER_FILTER_NEAREST = 0,
    GB_RENDER_FILTER_LINEAR = 1,
} GBRenderFilterMode;
typedef enum GBControllerLabelProfile {
    GB_CONTROLLER_LABEL_GENERIC = 0,
    GB_CONTROLLER_LABEL_XBOX = 1,
    GB_CONTROLLER_LABEL_PLAYSTATION = 2,
    GB_CONTROLLER_LABEL_NINTENDO = 3,
} GBControllerLabelProfile;
static GBRenderScalingMode g_render_scaling_mode = GB_RENDER_SCALING_PIXEL_PERFECT;
static GBRenderFilterMode g_render_filter_mode = GB_RENDER_FILTER_NEAREST;
static SDL_GameControllerType g_controller_type = SDL_CONTROLLER_TYPE_UNKNOWN;
static GBControllerLabelProfile g_controller_label_profile = GB_CONTROLLER_LABEL_GENERIC;
static std::string g_controller_name;
static const char* g_render_scaling_mode_names[] = {
    "Pixel Perfect",
    "Aspect Fit",
    "Aspect Fill",
    "Stretch",
};
static const char* g_render_filter_names[] = {
    "Nearest",
    "Linear",
};
static const uint32_t g_palettes[][4] = {
    { 0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820 }, // Original
    { 0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000 }, // B&W
    { 0xFFFFB000, 0xFFCB4F0E, 0xFF800000, 0xFF330000 }  // Amber
};
static uint32_t g_lcd_off_framebuffer[GB_FRAMEBUFFER_SIZE];
static uint32_t g_last_guest_framebuffer[GB_FRAMEBUFFER_SIZE];
static bool g_lcd_off_framebuffer_initialized = false;
static bool g_last_guest_framebuffer_valid = false;
static uint64_t g_present_count = 0;
static GBContext* g_registered_ctx = NULL;

static bool has_interpreter_activity(const GBContext* ctx) {
    return ctx != NULL &&
           (ctx->total_dispatch_fallbacks > 0 ||
            ctx->total_interpreter_entries > 0 ||
            ctx->has_unimplemented_interpreter_opcode);
}

static void update_audio_stats_from_ring(void);
static uint32_t current_audio_underruns(void);
static uint32_t current_audio_ring_fill_samples(void);
static uint32_t current_audio_ring_capacity(void);
static uint32_t audio_ring_fill_samples(void);

/* Joypad state - exported for gbrt.c to access */
uint8_t g_joypad_buttons = 0xFF;  /* Active low: Start, Select, B, A */
uint8_t g_joypad_dpad = 0xFF;     /* Active low: Down, Up, Left, Right */
static uint8_t g_manual_joypad_buttons = 0xFF;
static uint8_t g_manual_joypad_dpad = 0xFF;
static uint8_t g_script_joypad_buttons = 0xFF;
static uint8_t g_script_joypad_dpad = 0xFF;

/* ============================================================================
 * Automation State
 * ========================================================================== */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define MAX_SCRIPT_ENTRIES 100
typedef enum {
    SCRIPT_ANCHOR_FRAME = 0,
    SCRIPT_ANCHOR_CYCLE = 1,
} ScriptAnchor;

typedef struct {
    ScriptAnchor anchor;
    uint64_t start;
    uint64_t duration;
    uint8_t dpad;    /* Active LOW mask to apply (0 = Pressed) */
    uint8_t buttons; /* Active LOW mask to apply (0 = Pressed) */
} ScriptEntry;

static ScriptEntry g_input_script[MAX_SCRIPT_ENTRIES];
static int g_script_count = 0;
static FILE* g_input_record_file = NULL;
static bool g_input_record_exit_handler_registered = false;
static bool g_input_record_wrote_entry = false;
static bool g_input_record_has_segment = false;
static uint64_t g_input_record_start_cycle = 0;
static uint64_t g_input_record_end_cycle = 0;
static uint8_t g_input_record_dpad = 0xFF;
static uint8_t g_input_record_buttons = 0xFF;

#define MAX_DUMP_FRAMES 100
static uint32_t g_dump_frames[MAX_DUMP_FRAMES];
static int g_dump_count = 0;
static uint32_t g_dump_present_frames[MAX_DUMP_FRAMES];
static int g_dump_present_count = 0;
static char g_screenshot_prefix[64] = "screenshot";

static bool env_flag_enabled(const char* name) {
    const char* value = SDL_getenv(name);
    if (!value || !value[0]) {
        return false;
    }
    return strcmp(value, "1") == 0 ||
           strcmp(value, "true") == 0 ||
           strcmp(value, "TRUE") == 0 ||
           strcmp(value, "yes") == 0 ||
           strcmp(value, "YES") == 0 ||
           strcmp(value, "on") == 0 ||
           strcmp(value, "ON") == 0;
}

static bool platform_default_fullscreen(void) {
#if defined(__ANDROID__)
    return true;
#else
    return false;
#endif
}

static bool platform_uses_app_storage_for_relative_paths(void) {
#if defined(__ANDROID__)
    return true;
#else
    return false;
#endif
}

static const char* overlay_menu_hint_text(void) {
#if defined(__ANDROID__)
    return "Press Back for Menu";
#else
    return "Press ESC for Menu";
#endif
}

static const char* overlay_visibility_hint_text(void) {
#if defined(__ANDROID__)
    return "Use the settings menu to toggle the overlay";
#else
    return "Press F1 for Overlay";
#endif
}

static bool string_contains_case_insensitive(const char* haystack, const char* needle) {
    if (!haystack || !needle || !haystack[0] || !needle[0]) {
        return false;
    }

    std::string text(haystack);
    std::string pattern(needle);
    for (char& ch : text) ch = (char)std::tolower((unsigned char)ch);
    for (char& ch : pattern) ch = (char)std::tolower((unsigned char)ch);
    return text.find(pattern) != std::string::npos;
}

static bool should_ignore_controller_name(const char* name) {
    return string_contains_case_insensitive(name, "qwerty") ||
           string_contains_case_insensitive(name, "keyboard") ||
           string_contains_case_insensitive(name, "keypad");
}

static const char* controller_type_name(SDL_GameControllerType type) {
    switch (type) {
        case SDL_CONTROLLER_TYPE_XBOX360: return "Xbox 360";
        case SDL_CONTROLLER_TYPE_XBOXONE: return "Xbox One";
        case SDL_CONTROLLER_TYPE_PS3: return "PlayStation 3";
        case SDL_CONTROLLER_TYPE_PS4: return "PlayStation 4";
        case SDL_CONTROLLER_TYPE_PS5: return "PlayStation 5";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return "Nintendo Switch Pro";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return "Joy-Con Left";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return "Joy-Con Right";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Joy-Con Pair";
        case SDL_CONTROLLER_TYPE_GOOGLE_STADIA: return "Stadia";
        case SDL_CONTROLLER_TYPE_AMAZON_LUNA: return "Luna";
        case SDL_CONTROLLER_TYPE_NVIDIA_SHIELD: return "NVIDIA Shield";
        case SDL_CONTROLLER_TYPE_VIRTUAL: return "Virtual";
        case SDL_CONTROLLER_TYPE_UNKNOWN:
        default:
            return "Unknown";
    }
}

static GBControllerLabelProfile detect_controller_label_profile(SDL_GameController* controller) {
    if (controller) {
        const SDL_GameControllerType type = SDL_GameControllerGetType(controller);
        switch (type) {
            case SDL_CONTROLLER_TYPE_XBOX360:
            case SDL_CONTROLLER_TYPE_XBOXONE:
            case SDL_CONTROLLER_TYPE_GOOGLE_STADIA:
            case SDL_CONTROLLER_TYPE_AMAZON_LUNA:
            case SDL_CONTROLLER_TYPE_NVIDIA_SHIELD:
                return GB_CONTROLLER_LABEL_XBOX;

            case SDL_CONTROLLER_TYPE_PS3:
            case SDL_CONTROLLER_TYPE_PS4:
            case SDL_CONTROLLER_TYPE_PS5:
                return GB_CONTROLLER_LABEL_PLAYSTATION;

            case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
            case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
            case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
            case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
                return GB_CONTROLLER_LABEL_NINTENDO;

            default:
                break;
        }

        const char* name = SDL_GameControllerName(controller);
        if (string_contains_case_insensitive(name, "xbox") ||
            string_contains_case_insensitive(name, "xinput") ||
            string_contains_case_insensitive(name, "stadia") ||
            string_contains_case_insensitive(name, "luna") ||
            string_contains_case_insensitive(name, "shield") ||
            string_contains_case_insensitive(name, "kishi") ||
            string_contains_case_insensitive(name, "odin") ||
            string_contains_case_insensitive(name, "retroid")) {
            return GB_CONTROLLER_LABEL_XBOX;
        }
        if (string_contains_case_insensitive(name, "playstation") ||
            string_contains_case_insensitive(name, "dualshock") ||
            string_contains_case_insensitive(name, "dualsense") ||
            string_contains_case_insensitive(name, "ps3") ||
            string_contains_case_insensitive(name, "ps4") ||
            string_contains_case_insensitive(name, "ps5")) {
            return GB_CONTROLLER_LABEL_PLAYSTATION;
        }
        if (string_contains_case_insensitive(name, "switch") ||
            string_contains_case_insensitive(name, "joy-con") ||
            string_contains_case_insensitive(name, "joycon") ||
            string_contains_case_insensitive(name, "nintendo")) {
            return GB_CONTROLLER_LABEL_NINTENDO;
        }
    }

#if defined(__ANDROID__)
    return GB_CONTROLLER_LABEL_XBOX;
#else
    return GB_CONTROLLER_LABEL_GENERIC;
#endif
}

static const char* controller_face_south_label(void) {
    switch (g_controller_label_profile) {
        case GB_CONTROLLER_LABEL_XBOX: return "A (Bottom)";
        case GB_CONTROLLER_LABEL_PLAYSTATION: return "Cross (Bottom)";
        case GB_CONTROLLER_LABEL_NINTENDO: return "B (Bottom)";
        case GB_CONTROLLER_LABEL_GENERIC:
        default:
            return "South / Bottom";
    }
}

static const char* controller_face_east_label(void) {
    switch (g_controller_label_profile) {
        case GB_CONTROLLER_LABEL_XBOX: return "B (Right)";
        case GB_CONTROLLER_LABEL_PLAYSTATION: return "Circle (Right)";
        case GB_CONTROLLER_LABEL_NINTENDO: return "A (Right)";
        case GB_CONTROLLER_LABEL_GENERIC:
        default:
            return "East / Right";
    }
}

static const char* controller_left_shoulder_label(void) {
    switch (g_controller_label_profile) {
        case GB_CONTROLLER_LABEL_XBOX: return "LB";
        case GB_CONTROLLER_LABEL_PLAYSTATION: return "L1";
        case GB_CONTROLLER_LABEL_NINTENDO: return "L";
        case GB_CONTROLLER_LABEL_GENERIC:
        default:
            return "Left Shoulder";
    }
}

static const char* controller_right_shoulder_label(void) {
    switch (g_controller_label_profile) {
        case GB_CONTROLLER_LABEL_XBOX: return "RB";
        case GB_CONTROLLER_LABEL_PLAYSTATION: return "R1";
        case GB_CONTROLLER_LABEL_NINTENDO: return "R";
        case GB_CONTROLLER_LABEL_GENERIC:
        default:
            return "Right Shoulder";
    }
}

static const char* controller_back_label(void) {
    switch (g_controller_label_profile) {
        case GB_CONTROLLER_LABEL_XBOX: return "View / Back";
        case GB_CONTROLLER_LABEL_PLAYSTATION: return "Create / Share";
        case GB_CONTROLLER_LABEL_NINTENDO: return "-";
        case GB_CONTROLLER_LABEL_GENERIC:
        default:
            return "Back / Select";
    }
}

static const char* controller_start_label(void) {
    switch (g_controller_label_profile) {
        case GB_CONTROLLER_LABEL_XBOX: return "Menu / Start";
        case GB_CONTROLLER_LABEL_PLAYSTATION: return "Options";
        case GB_CONTROLLER_LABEL_NINTENDO: return "+";
        case GB_CONTROLLER_LABEL_GENERIC:
        default:
            return "Start";
    }
}

static const char* controller_guide_label(void) {
    switch (g_controller_label_profile) {
        case GB_CONTROLLER_LABEL_XBOX: return "Xbox / Guide";
        case GB_CONTROLLER_LABEL_PLAYSTATION: return "PS";
        case GB_CONTROLLER_LABEL_NINTENDO: return "Home";
        case GB_CONTROLLER_LABEL_GENERIC:
        default:
            return "Guide / Home";
    }
}

static void clear_controller_state(void) {
    if (g_controller) {
        SDL_GameControllerClose(g_controller);
        g_controller = NULL;
    }
    g_controller_type = SDL_CONTROLLER_TYPE_UNKNOWN;
    g_controller_label_profile = detect_controller_label_profile(NULL);
    g_controller_name.clear();
}

static void refresh_controller_profile(void) {
    if (!g_controller) {
        g_controller_type = SDL_CONTROLLER_TYPE_UNKNOWN;
        g_controller_label_profile = detect_controller_label_profile(NULL);
        g_controller_name.clear();
        return;
    }

    g_controller_type = SDL_GameControllerGetType(g_controller);
    g_controller_label_profile = detect_controller_label_profile(g_controller);
    const char* name = SDL_GameControllerName(g_controller);
    g_controller_name = (name && name[0]) ? name : "Controller";
    fprintf(stderr,
            "[SDL] Controller: %s [%s]\n",
            g_controller_name.c_str(),
            controller_type_name(g_controller_type));
}

static bool open_controller_index(int joystick_index) {
    if (joystick_index < 0 || joystick_index >= SDL_NumJoysticks() || !SDL_IsGameController(joystick_index)) {
        return false;
    }

    SDL_GameController* controller = SDL_GameControllerOpen(joystick_index);
    if (!controller) {
        return false;
    }

    if (should_ignore_controller_name(SDL_GameControllerName(controller))) {
        SDL_GameControllerClose(controller);
        return false;
    }

    clear_controller_state();
    g_controller = controller;
    refresh_controller_profile();
    return true;
}

static bool open_first_available_controller(void) {
    if (g_controller) {
        return true;
    }

    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (open_controller_index(i)) {
            return true;
        }
    }
    return false;
}

static SDL_JoystickID active_controller_instance_id(void) {
    if (!g_controller) {
        return -1;
    }
    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(g_controller);
    return joystick ? SDL_JoystickInstanceID(joystick) : -1;
}

static bool ensure_parent_directory(const fs::path& path) {
    if (path.parent_path().empty()) {
        return true;
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    return !ec;
}

static std::string extract_path_leaf(const char* path) {
    if (!path || !path[0]) {
        return "game";
    }
    try {
        fs::path input(path);
        std::string leaf = input.filename().string();
        return leaf.empty() ? std::string(path) : leaf;
    } catch (...) {
        return std::string(path);
    }
}

static std::string make_pref_storage_dir(const char* app_component) {
    const char* safe_component = (app_component && app_component[0]) ? app_component : "runtime";
    char* pref_path = SDL_GetPrefPath("gbrecompiled", safe_component);
    if (!pref_path) {
        return std::string();
    }
    std::string result(pref_path);
    SDL_free(pref_path);
    return result;
}

static std::string resolve_writable_path(const char* requested_path, const char* app_component) {
    if (!requested_path || !requested_path[0]) {
        return std::string();
    }

    fs::path requested(requested_path);
    if (!platform_uses_app_storage_for_relative_paths() || requested.is_absolute()) {
        return requested.lexically_normal().string();
    }

    const std::string pref_dir = make_pref_storage_dir(app_component);
    if (pref_dir.empty()) {
        return requested.lexically_normal().string();
    }

    fs::path resolved = fs::path(pref_dir) / requested;
    ensure_parent_directory(resolved);
    return resolved.lexically_normal().string();
}

static bool recreate_streaming_texture(void);
static void set_app_suspended(bool suspended);

static bool frame_is_selected_for_dump(const uint32_t* frames, int count, uint32_t frame) {
    for (int i = 0; i < count; i++) {
        if (frames[i] == frame) {
            return true;
        }
    }
    return false;
}

/* Helper to parse button string "U,D,L,R,A,B,S,T" */
static void parse_buttons(const char* btn_str, uint8_t* dpad, uint8_t* buttons) {
    *dpad = 0xFF;
    *buttons = 0xFF;
    // Simple parser: check for existence of characters
    if (strchr(btn_str, 'U')) *dpad &= ~0x04;
    if (strchr(btn_str, 'D')) *dpad &= ~0x08;
    if (strchr(btn_str, 'L')) *dpad &= ~0x02;
    if (strchr(btn_str, 'R')) *dpad &= ~0x01;
    if (strchr(btn_str, 'A')) *buttons &= ~0x01;
    if (strchr(btn_str, 'B')) *buttons &= ~0x02;
    if (strchr(btn_str, 'S')) *buttons &= ~0x08; /* Start */
    if (strchr(btn_str, 'T')) *buttons &= ~0x04; /* Select (T for selecT) */
}

static void update_effective_joypad_state(void) {
    g_joypad_dpad = g_manual_joypad_dpad & g_script_joypad_dpad;
    g_joypad_buttons = g_manual_joypad_buttons & g_script_joypad_buttons;
}

static void request_joypad_interrupt(GBContext* ctx) {
    if (!ctx) return;
    ctx->io[0x0F] |= 0x10;
    if (ctx->halted) ctx->halted = 0;
}

static bool input_state_has_press(uint8_t dpad, uint8_t buttons) {
    return dpad != 0xFF || buttons != 0xFF;
}

static void write_buttons(FILE* file, uint8_t dpad, uint8_t buttons) {
    if (!(dpad & 0x04)) fputc('U', file);
    if (!(dpad & 0x08)) fputc('D', file);
    if (!(dpad & 0x02)) fputc('L', file);
    if (!(dpad & 0x01)) fputc('R', file);
    if (!(buttons & 0x01)) fputc('A', file);
    if (!(buttons & 0x02)) fputc('B', file);
    if (!(buttons & 0x08)) fputc('S', file);
    if (!(buttons & 0x04)) fputc('T', file);
}

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

static bool parse_script_token(char* token, ScriptEntry* entry, char* button_buf, size_t button_buf_size) {
    char* start_text = trim_ascii(token);
    if (!start_text || !*start_text) {
        return false;
    }

    char* first_colon = strchr(start_text, ':');
    if (!first_colon) {
        return false;
    }
    *first_colon = '\0';

    char* buttons_text = trim_ascii(first_colon + 1);
    char* second_colon = strchr(buttons_text, ':');
    if (!second_colon) {
        return false;
    }
    *second_colon = '\0';

    char* duration_text = trim_ascii(second_colon + 1);
    start_text = trim_ascii(start_text);
    buttons_text = trim_ascii(buttons_text);
    duration_text = trim_ascii(duration_text);
    if (!*start_text || !*duration_text) {
        return false;
    }

    entry->anchor = SCRIPT_ANCHOR_FRAME;
    if (*start_text == 'c' || *start_text == 'C') {
        entry->anchor = SCRIPT_ANCHOR_CYCLE;
        start_text++;
    } else if (*start_text == 'f' || *start_text == 'F') {
        start_text++;
    }
    start_text = trim_ascii(start_text);
    if (!*start_text) {
        return false;
    }

    char* start_end = NULL;
    char* duration_end = NULL;
    entry->start = strtoull(start_text, &start_end, 10);
    entry->duration = strtoull(duration_text, &duration_end, 10);
    if ((start_end && *trim_ascii(start_end)) || (duration_end && *trim_ascii(duration_end))) {
        return false;
    }
    if (entry->duration == 0) {
        return false;
    }

    snprintf(button_buf, button_buf_size, "%s", buttons_text);
    return true;
}

static void flush_input_record_segment(void) {
    if (!g_input_record_file || !g_input_record_has_segment) return;

    uint64_t duration_cycles = 0;
    if (g_input_record_end_cycle > g_input_record_start_cycle) {
        duration_cycles = g_input_record_end_cycle - g_input_record_start_cycle;
    }

    if (duration_cycles > 0 && input_state_has_press(g_input_record_dpad, g_input_record_buttons)) {
        if (g_input_record_wrote_entry) {
            fputc(',', g_input_record_file);
        }
        fprintf(g_input_record_file, "c%llu:", (unsigned long long)g_input_record_start_cycle);
        write_buttons(g_input_record_file, g_input_record_dpad, g_input_record_buttons);
        fprintf(g_input_record_file, ":%llu", (unsigned long long)duration_cycles);
        fflush(g_input_record_file);
        g_input_record_wrote_entry = true;
    }

    g_input_record_has_segment = false;
    g_input_record_start_cycle = 0;
    g_input_record_end_cycle = 0;
    g_input_record_dpad = 0xFF;
    g_input_record_buttons = 0xFF;
}

static void close_input_record_file(void) {
    if (!g_input_record_file) return;
    flush_input_record_segment();
    fputc('\n', g_input_record_file);
    fclose(g_input_record_file);
    g_input_record_file = NULL;
    g_input_record_wrote_entry = false;
}

static void record_manual_input_state(uint64_t cycle_count) {
    if (!g_input_record_file) return;

    if (!g_input_record_has_segment) {
        g_input_record_has_segment = true;
        g_input_record_start_cycle = cycle_count;
        g_input_record_end_cycle = cycle_count;
        g_input_record_dpad = g_manual_joypad_dpad;
        g_input_record_buttons = g_manual_joypad_buttons;
        return;
    }

    if (g_input_record_dpad == g_manual_joypad_dpad && g_input_record_buttons == g_manual_joypad_buttons) {
        g_input_record_end_cycle = cycle_count;
        return;
    }

    g_input_record_end_cycle = cycle_count;
    flush_input_record_segment();
    g_input_record_has_segment = true;
    g_input_record_start_cycle = cycle_count;
    g_input_record_end_cycle = cycle_count;
    g_input_record_dpad = g_manual_joypad_dpad;
    g_input_record_buttons = g_manual_joypad_buttons;
}

void gb_platform_set_input_script(const char* script) {
    // Formats: frame:buttons:duration,... or ccycle:buttons:duration,...
    g_script_count = 0;
    g_script_joypad_dpad = 0xFF;
    g_script_joypad_buttons = 0xFF;
    update_effective_joypad_state();

    if (!script) return;
    
    char* copy = strdup(script);
    char* token = strtok(copy, ",");
    
    while (token && g_script_count < MAX_SCRIPT_ENTRIES) {
        char btn_buf[16] = {0};
        ScriptEntry parsed = {};

        if (parse_script_token(token, &parsed, btn_buf, sizeof(btn_buf))) {
            ScriptEntry* e = &g_input_script[g_script_count++];
            *e = parsed;
            parse_buttons(btn_buf, &e->dpad, &e->buttons);
            printf("[AUTO] Added input: %s %llu, Btns '%s', Dur %llu\n",
                   e->anchor == SCRIPT_ANCHOR_CYCLE ? "Cycle" : "Frame",
                   (unsigned long long)e->start,
                   btn_buf,
                   (unsigned long long)e->duration);
        } else {
            fprintf(stderr, "[AUTO] Ignoring invalid input token '%s'\n", token);
        }
        token = strtok(NULL, ",");
    }
    free(copy);
}

void gb_platform_set_input_record_file(const char* path) {
    if (!g_input_record_exit_handler_registered) {
        atexit(close_input_record_file);
        g_input_record_exit_handler_registered = true;
    }

    close_input_record_file();
    g_input_record_has_segment = false;
    g_input_record_start_cycle = 0;
    g_input_record_end_cycle = 0;
    g_input_record_dpad = 0xFF;
    g_input_record_buttons = 0xFF;

    if (!path || !path[0]) return;

    const std::string resolved_path = resolve_writable_path(path, "artifacts");
    g_input_record_file = fopen(resolved_path.c_str(), "w");
    if (!g_input_record_file) {
        fprintf(stderr, "[AUTO] Failed to open input record file '%s'\n", resolved_path.c_str());
        return;
    }

    fprintf(stderr, "[AUTO] Recording live input to %s (cycle anchored)\n", resolved_path.c_str());
}

void gb_platform_set_dump_frames(const char* frames) {
    if (!frames) return;
    char* copy = strdup(frames);
    char* token = strtok(copy, ",");
    g_dump_count = 0;
    while (token && g_dump_count < MAX_DUMP_FRAMES) {
        g_dump_frames[g_dump_count++] = (uint32_t)strtoul(token, NULL, 10);
        token = strtok(NULL, ",");
    }
    free(copy);
}

void gb_platform_set_dump_present_frames(const char* frames) {
    if (!frames) return;
    char* copy = strdup(frames);
    char* token = strtok(copy, ",");
    g_dump_present_count = 0;
    while (token && g_dump_present_count < MAX_DUMP_FRAMES) {
        g_dump_present_frames[g_dump_present_count++] = (uint32_t)strtoul(token, NULL, 10);
        token = strtok(NULL, ",");
    }
    free(copy);
}

void gb_platform_set_screenshot_prefix(const char* prefix) {
    if (prefix) snprintf(g_screenshot_prefix, sizeof(g_screenshot_prefix), "%s", prefix);
}

static void save_ppm(const char* filename, const uint32_t* fb, int width, int height, int frame_count) {
    const std::string resolved_filename = resolve_writable_path(filename, "artifacts");
    // Calculate simple hash
    uint32_t hash = 0;
    for (int k = 0; k < width * height; k++) {
        hash = (hash * 33) ^ fb[k];
    }
    printf("[AUTO] Frame %d hash: %08X\n", frame_count, hash);

    FILE* f = fopen(resolved_filename.c_str(), "wb");
    if (!f) return;
    
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    
    uint8_t* row = (uint8_t*)malloc(width * 3);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t p = fb[y * width + x];
            row[x*3+0] = (p >> 16) & 0xFF; // R
            row[x*3+1] = (p >> 8) & 0xFF;  // G
            row[x*3+2] = (p >> 0) & 0xFF;  // B
        }
        fwrite(row, 1, width * 3, f);
    }
    
    free(row);
    fclose(f);
    printf("[AUTO] Saved screenshot: %s\n", resolved_filename.c_str());
}


static int g_frame_count = 0;

static double sdl_now_ms(void) {
    uint64_t ticks = SDL_GetPerformanceCounter();
    uint64_t freq = SDL_GetPerformanceFrequency();
    return freq ? ((double)ticks * 1000.0) / (double)freq : 0.0;
}

static int round_to_int(double value) {
    return (int)(value + 0.5);
}

static void update_render_filter(void) {
    if (!g_renderer) {
        return;
    }

#if SDL_VERSION_ATLEAST(2, 0, 12)
    if (g_texture) {
        SDL_SetTextureScaleMode(g_texture,
                                g_render_filter_mode == GB_RENDER_FILTER_LINEAR
                                    ? SDL_ScaleModeLinear
                                    : SDL_ScaleModeNearest);
    }
#else
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
                g_render_filter_mode == GB_RENDER_FILTER_LINEAR ? "linear" : "nearest");
#endif
}

static void update_game_viewport(void) {
    if (!g_window) {
        g_game_viewport.x = 0;
        g_game_viewport.y = 0;
        g_game_viewport.w = GB_SCREEN_WIDTH * g_scale;
        g_game_viewport.h = GB_SCREEN_HEIGHT * g_scale;
        return;
    }

    int window_w = 0;
    int window_h = 0;
    SDL_GetWindowSize(g_window, &window_w, &window_h);
    if (window_w <= 0) window_w = GB_SCREEN_WIDTH;
    if (window_h <= 0) window_h = GB_SCREEN_HEIGHT;

    int viewport_w = window_w;
    int viewport_h = window_h;

    switch (g_render_scaling_mode) {
        case GB_RENDER_SCALING_PIXEL_PERFECT: {
            int scale_x = window_w / GB_SCREEN_WIDTH;
            int scale_y = window_h / GB_SCREEN_HEIGHT;
            int integer_scale = (scale_x < scale_y) ? scale_x : scale_y;
            if (integer_scale < 1) {
                integer_scale = 1;
            }
            viewport_w = GB_SCREEN_WIDTH * integer_scale;
            viewport_h = GB_SCREEN_HEIGHT * integer_scale;
            break;
        }
        case GB_RENDER_SCALING_ASPECT_FIT: {
            double scale_x = (double)window_w / (double)GB_SCREEN_WIDTH;
            double scale_y = (double)window_h / (double)GB_SCREEN_HEIGHT;
            double scale = (scale_x < scale_y) ? scale_x : scale_y;
            if (scale <= 0.0) {
                scale = 1.0;
            }
            viewport_w = round_to_int((double)GB_SCREEN_WIDTH * scale);
            viewport_h = round_to_int((double)GB_SCREEN_HEIGHT * scale);
            break;
        }
        case GB_RENDER_SCALING_ASPECT_FILL: {
            double scale_x = (double)window_w / (double)GB_SCREEN_WIDTH;
            double scale_y = (double)window_h / (double)GB_SCREEN_HEIGHT;
            double scale = (scale_x > scale_y) ? scale_x : scale_y;
            if (scale <= 0.0) {
                scale = 1.0;
            }
            viewport_w = round_to_int((double)GB_SCREEN_WIDTH * scale);
            viewport_h = round_to_int((double)GB_SCREEN_HEIGHT * scale);
            break;
        }
        case GB_RENDER_SCALING_STRETCH:
        default:
            viewport_w = window_w;
            viewport_h = window_h;
            break;
    }

    if (viewport_w < 1) viewport_w = 1;
    if (viewport_h < 1) viewport_h = 1;

    g_game_viewport.w = viewport_w;
    g_game_viewport.h = viewport_h;
    g_game_viewport.x = (window_w - viewport_w) / 2;
    g_game_viewport.y = (window_h - viewport_h) / 2;
}

static void apply_window_scale_preset(void) {
    g_windowed_width = GB_SCREEN_WIDTH * g_scale;
    g_windowed_height = GB_SCREEN_HEIGHT * g_scale;

    if (g_window && !g_fullscreen) {
        SDL_SetWindowSize(g_window, g_windowed_width, g_windowed_height);
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    update_game_viewport();
}

static void set_fullscreen_enabled(bool enabled) {
    if (!g_window || g_fullscreen == enabled) {
        return;
    }

    if (enabled) {
        SDL_GetWindowSize(g_window, &g_windowed_width, &g_windowed_height);
        if (SDL_SetWindowFullscreen(g_window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0) {
            g_fullscreen = true;
        }
    } else {
        if (SDL_SetWindowFullscreen(g_window, 0) == 0) {
            g_fullscreen = false;
            SDL_SetWindowSize(g_window, g_windowed_width, g_windowed_height);
            SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
    }

    update_game_viewport();
}

static bool recreate_streaming_texture(void) {
    if (!g_renderer || g_benchmark_mode) {
        return true;
    }

    if (g_texture) {
        SDL_DestroyTexture(g_texture);
        g_texture = NULL;
    }

    g_texture = SDL_CreateTexture(
        g_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        GB_SCREEN_WIDTH,
        GB_SCREEN_HEIGHT
    );
    if (!g_texture) {
        fprintf(stderr, "[SDL] Failed to recreate texture: %s\n", SDL_GetError());
        return false;
    }

    update_render_filter();
    g_renderer_reset_pending = false;
    return true;
}

static void set_app_suspended(bool suspended) {
    if (g_app_suspended == suspended) {
        return;
    }

    g_app_suspended = suspended;
    if (g_audio_device) {
        SDL_PauseAudioDevice(g_audio_device, suspended ? 1 : (g_audio_started ? 0 : 1));
    }

    if (!suspended) {
        g_last_frame_time = SDL_GetTicks();
        if (g_window) {
            update_game_viewport();
        }
        g_renderer_reset_pending = true;
    }
}

static void reset_runtime_display_defaults(void) {
    g_scale = 5;
    g_speed_percent = 100;
    g_palette_idx = 0;
    g_smooth_lcd_transitions = true;
    g_vsync = false;
    g_show_overlay = false;
    g_render_scaling_mode = GB_RENDER_SCALING_PIXEL_PERFECT;
    g_render_filter_mode = GB_RENDER_FILTER_NEAREST;
    update_render_filter();

    if (g_renderer) {
        SDL_RenderSetVSync(g_renderer, 0);
    }

    const bool want_fullscreen = platform_default_fullscreen();
    if (g_window) {
        if (g_fullscreen != want_fullscreen) {
            set_fullscreen_enabled(want_fullscreen);
        }
        if (!g_fullscreen) {
            apply_window_scale_preset();
        } else {
            update_game_viewport();
        }
    } else {
        g_fullscreen = want_fullscreen;
    }
}

static void ensure_lcd_off_framebuffer(void) {
    if (g_lcd_off_framebuffer_initialized) {
        return;
    }

    for (int i = 0; i < GB_FRAMEBUFFER_SIZE; i++) {
        g_lcd_off_framebuffer[i] = 0xFFE0F8D0;
    }

    g_lcd_off_framebuffer_initialized = true;
}

static void render_frame_internal(const uint32_t* framebuffer, bool count_guest_frame) {
    if (!framebuffer) {
        DBG_FRAME("Platform render_frame: SKIPPED (null: texture=%d, renderer=%d, fb=%d)",
                  g_texture == NULL, g_renderer == NULL, framebuffer == NULL);
        return;
    }
    g_present_count++;
    if (count_guest_frame) {
        g_frame_count++;
        memcpy(g_last_guest_framebuffer, framebuffer, sizeof(g_last_guest_framebuffer));
        g_last_guest_framebuffer_valid = true;
    }

    if (count_guest_frame) {
        /* Handle Screenshot Dumping */
        if (frame_is_selected_for_dump(g_dump_frames, g_dump_count, (uint32_t)g_frame_count)) {
            char filename[128];
            snprintf(filename, sizeof(filename), "%s_%05d.ppm", g_screenshot_prefix, g_frame_count);
            save_ppm(filename, framebuffer, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT, g_frame_count);
        }
    }

    if (frame_is_selected_for_dump(g_dump_present_frames, g_dump_present_count, (uint32_t)g_frame_count)) {
        char filename[160];
        snprintf(filename,
                 sizeof(filename),
                 "%s_guest_%05d_present_%06llu.ppm",
                 g_screenshot_prefix,
                 g_frame_count,
                 (unsigned long long)g_present_count);
        save_ppm(filename, framebuffer, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT, g_frame_count);
    }

    if (g_benchmark_mode || g_app_suspended || !g_renderer) {
        DBG_FRAME("Platform render_frame: host render skipped (benchmark=%d texture=%d renderer=%d)",
                  g_benchmark_mode ? 1 : 0,
                  g_texture == NULL,
                  g_renderer == NULL);
        g_last_timing.upload_ms = 0.0;
        g_last_timing.compose_ms = 0.0;
        g_last_timing.present_ms = 0.0;
        g_last_timing.total_render_ms = 0.0;
        return;
    }

    if ((!g_texture || g_renderer_reset_pending) && !recreate_streaming_texture()) {
        g_last_timing.upload_ms = 0.0;
        g_last_timing.compose_ms = 0.0;
        g_last_timing.present_ms = 0.0;
        g_last_timing.total_render_ms = 0.0;
        return;
    }

    double total_render_start_ms = sdl_now_ms();

    /* Debug: check framebuffer content on first few guest frames */
    if (count_guest_frame && g_frame_count <= 3) {
        bool has_content = false;
        uint32_t white = 0xFFE0F8D0;
        for (int i = 0; i < GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT; i++) {
            if (framebuffer[i] != white) {
                has_content = true;
                break;
            }
        }
        DBG_FRAME("Platform frame %d - has_content=%d, first_pixel=0x%08X",
                  g_frame_count, has_content, framebuffer[0]);
    }

    if (count_guest_frame && (g_frame_count % 60) == 0) {
        char title[64];
        snprintf(title, sizeof(title), "GameBoy Recompiled - Frame %d", g_frame_count);
        SDL_SetWindowTitle(g_window, title);
    } else if (!count_guest_frame && (g_present_count % 30) == 0) {
        char title[96];
        if (g_frame_count == 0) {
            snprintf(title, sizeof(title), "GameBoy Recompiled - Starting... (%llu)",
                     (unsigned long long)(g_present_count / 30));
        } else {
            snprintf(title, sizeof(title), "GameBoy Recompiled - Frame %d (Working...)",
                     g_frame_count);
        }
        SDL_SetWindowTitle(g_window, title);
    }

    /* Update texture */
    double upload_start_ms = sdl_now_ms();
    void* pixels;
    int pitch;
    SDL_LockTexture(g_texture, NULL, &pixels, &pitch);

    const uint32_t* src = framebuffer;
    uint32_t* dst = (uint32_t*)pixels;

    if (g_palette_idx == 0) {
        memcpy(dst, src, GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT * sizeof(uint32_t));
    } else {
        uint32_t original_palette[4] = { 0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820 };

        for (int i = 0; i < GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT; i++) {
            uint32_t c = src[i];
            int color_idx = -1;
            if (c == original_palette[0]) color_idx = 0;
            else if (c == original_palette[1]) color_idx = 1;
            else if (c == original_palette[2]) color_idx = 2;
            else if (c == original_palette[3]) color_idx = 3;

            if (color_idx >= 0) {
                dst[i] = g_palettes[g_palette_idx][color_idx];
            } else {
                dst[i] = c;
            }
        }
    }

    SDL_UnlockTexture(g_texture);
    g_last_timing.upload_ms = sdl_now_ms() - upload_start_ms;

    /* Clear and render */
    double compose_start_ms = sdl_now_ms();
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, &g_game_viewport);

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    if (g_show_menu) {
        ImGui::Begin("GameBoy Recompiled", &g_show_menu);
        ImGui::Text("Performance: %.1f FPS", ImGui::GetIO().Framerate);

        int window_w = 0;
        int window_h = 0;
        SDL_GetWindowSize(g_window, &window_w, &window_h);

        ImGui::Separator();
        ImGui::TextDisabled("Graphics");
        ImGui::Text("Window: %d x %d", window_w, window_h);
        ImGui::Text("Viewport: %d x %d (%.2fx)",
                    g_game_viewport.w,
                    g_game_viewport.h,
                    (double)g_game_viewport.w / (double)GB_SCREEN_WIDTH);

        bool fullscreen = g_fullscreen;
        if (ImGui::Checkbox("Fullscreen", &fullscreen)) {
            set_fullscreen_enabled(fullscreen);
        }

        int scaling_mode = (int)g_render_scaling_mode;
        if (ImGui::Combo("Scaling Mode",
                         &scaling_mode,
                         g_render_scaling_mode_names,
                         IM_ARRAYSIZE(g_render_scaling_mode_names))) {
            g_render_scaling_mode = (GBRenderScalingMode)scaling_mode;
            update_game_viewport();
        }

        int filter_mode = (int)g_render_filter_mode;
        if (ImGui::Combo("Scale Filter",
                         &filter_mode,
                         g_render_filter_names,
                         IM_ARRAYSIZE(g_render_filter_names))) {
            g_render_filter_mode = (GBRenderFilterMode)filter_mode;
            update_render_filter();
        }

        if (!g_fullscreen) {
            int scale_idx = g_scale - 1;
            if (ImGui::Combo("Window Size",
                             &scale_idx,
                             g_scale_names,
                             IM_ARRAYSIZE(g_scale_names))) {
                g_scale = scale_idx + 1;
                apply_window_scale_preset();
            }
        } else {
            ImGui::TextDisabled("Window Size is disabled while fullscreen is active.");
        }

        ImGui::Separator();
        ImGui::TextDisabled("Runtime");
        if (ImGui::Checkbox("V-Sync", &g_vsync)) {
            SDL_RenderSetVSync(g_renderer, g_vsync ? 1 : 0);
        }

        ImGui::Checkbox("Show Overlay (F1)", &g_show_overlay);
        ImGui::Checkbox("Smooth Slow Frames", &g_smooth_lcd_transitions);
        ImGui::SliderInt("Speed %", &g_speed_percent, 10, 500);
        if (ImGui::Button("Reset Speed")) g_speed_percent = 100;
        ImGui::Combo("Palette", &g_palette_idx, g_palette_names, IM_ARRAYSIZE(g_palette_names));

        ImGui::Separator();
        ImGui::TextDisabled("Controls");
        if (!g_controller_name.empty()) {
            ImGui::Text("Detected Controller: %s", g_controller_name.c_str());
            ImGui::TextDisabled("Profile: %s", controller_type_name(g_controller_type));
        } else {
#if defined(__ANDROID__)
            ImGui::TextDisabled("No controller detected. Showing the default handheld layout.");
#else
            ImGui::TextDisabled("No controller detected.");
            ImGui::BulletText("Arrows / WASD: Move");
            ImGui::BulletText("Z/J: Game Boy A");
            ImGui::BulletText("X/K: Game Boy B");
            ImGui::BulletText("Backspace: Select");
            ImGui::BulletText("Enter: Start");
#endif
        }
        ImGui::BulletText("D-Pad / Left Stick: Move");
        ImGui::BulletText("%s: Game Boy B", controller_face_south_label());
        ImGui::BulletText("%s: Game Boy A", controller_face_east_label());
        ImGui::BulletText("%s: Game Boy B (alt)", controller_left_shoulder_label());
        ImGui::BulletText("%s: Game Boy A (alt)", controller_right_shoulder_label());
        ImGui::BulletText("%s: Select", controller_back_label());
        ImGui::BulletText("%s: Start", controller_start_label());
        ImGui::BulletText("%s: Settings Menu", controller_guide_label());
#if defined(__ANDROID__)
        ImGui::BulletText("Android Back / Escape: Settings Menu");
#else
        ImGui::BulletText("Escape: Settings Menu");
#endif

        if (has_interpreter_activity(g_registered_ctx)) {
            const GBInterpreterHotspot* hotspot = &g_registered_ctx->interpreter_hotspots[0];
            ImGui::Separator();
            ImGui::TextDisabled("Interpreter Fallback");
            ImGui::Text("Frame Fallbacks: %u", g_registered_ctx->frame_dispatch_fallbacks);
            ImGui::Text("Interp Entries: %llu",
                        (unsigned long long)g_registered_ctx->total_interpreter_entries);
            ImGui::Text("Total Fallbacks: %llu",
                        (unsigned long long)g_registered_ctx->total_dispatch_fallbacks);
            ImGui::Text("Interp Instr: frame %llu total %llu",
                        (unsigned long long)g_registered_ctx->frame_interpreter_instructions,
                        (unsigned long long)g_registered_ctx->total_interpreter_instructions);
            ImGui::Text("Interp Cycles: frame %llu total %llu",
                        (unsigned long long)g_registered_ctx->frame_interpreter_cycles,
                        (unsigned long long)g_registered_ctx->total_interpreter_cycles);
            if (hotspot->valid && hotspot->entries > 0) {
                ImGui::Text("Top Hotspot: %03X:%04X", hotspot->bank, hotspot->addr);
                ImGui::Text("Hits %llu Instr %llu Cycles %llu",
                            (unsigned long long)hotspot->entries,
                            (unsigned long long)hotspot->instructions,
                            (unsigned long long)hotspot->cycles);
            }
            if (g_registered_ctx->has_unimplemented_interpreter_opcode) {
                ImGui::Text("Coverage Gap: %02X at %03X:%04X",
                            g_registered_ctx->last_unimplemented_opcode,
                            g_registered_ctx->last_unimplemented_bank,
                            g_registered_ctx->last_unimplemented_addr);
            }
        }

        if (ImGui::Button("Reset to Defaults")) {
            reset_runtime_display_defaults();
        }

        if (g_launcher_return_enabled) {
            if (ImGui::Button("Return to Launcher")) {
                g_exit_action = GB_PLATFORM_EXIT_RETURN_TO_LAUNCHER;
                SDL_Event quit_event;
                quit_event.type = SDL_QUIT;
                SDL_PushEvent(&quit_event);
            }
            ImGui::SameLine();
        }

        if (ImGui::Button("Quit")) {
            g_exit_action = GB_PLATFORM_EXIT_QUIT;
            SDL_Event quit_event;
            quit_event.type = SDL_QUIT;
            SDL_PushEvent(&quit_event);
        }
        ImGui::End();
    } else if (g_show_overlay) {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.35f);
        if (ImGui::Begin("Overlay", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
            update_audio_stats_from_ring();
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            ImGui::Text("%s", overlay_menu_hint_text());
            ImGui::Text("%s", overlay_visibility_hint_text());
            ImGui::Text("Viewport: %d x %d (%s)",
                        g_game_viewport.w,
                        g_game_viewport.h,
                        g_render_scaling_mode_names[(int)g_render_scaling_mode]);
            if (g_timing_frame_count > 0) {
                float avg_render = (float)(g_timing_render_total / g_timing_frame_count);
                float avg_vsync = (float)(g_timing_vsync_total / g_timing_frame_count);
                ImGui::Text("Render: %.1fms, VSync: %.1fms", avg_render, avg_vsync);
                ImGui::Text("Upload: %.2f Compose: %.2f Present: %.2f",
                            (float)g_last_timing.upload_ms,
                            (float)g_last_timing.compose_ms,
                            (float)g_last_timing.present_ms);
                ImGui::Text("AudioBuf: %u/%u, Underruns:%u",
                            current_audio_ring_fill_samples(),
                            current_audio_ring_capacity(),
                            current_audio_underruns());
                ImGui::Text("Smooth Slow Frames: %s", g_smooth_lcd_transitions ? "On" : "Off");
                ImGui::TextUnformatted(audio_stats_get_summary());
                if (has_interpreter_activity(g_registered_ctx)) {
                    const GBInterpreterHotspot* hotspot = &g_registered_ctx->interpreter_hotspots[0];
                    ImGui::Separator();
                    ImGui::Text("Interp: frame %u total %llu entries %llu",
                                g_registered_ctx->frame_dispatch_fallbacks,
                                (unsigned long long)g_registered_ctx->total_dispatch_fallbacks,
                                (unsigned long long)g_registered_ctx->total_interpreter_entries);
                    if (hotspot->valid && hotspot->entries > 0) {
                        ImGui::Text("Hotspot: %03X:%04X (%llu hits)",
                                    hotspot->bank,
                                    hotspot->addr,
                                    (unsigned long long)hotspot->entries);
                    }
                }
            }
            ImGui::End();
        }
    }

    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
    g_last_timing.compose_ms = sdl_now_ms() - compose_start_ms;

    double present_start_ms = sdl_now_ms();
    SDL_RenderPresent(g_renderer);
    g_last_timing.present_ms = sdl_now_ms() - present_start_ms;
    g_last_timing.total_render_ms = sdl_now_ms() - total_render_start_ms;
    g_timing_render_total += g_last_timing.total_render_ms;
}

/* ============================================================================
 * Platform Functions
 * ========================================================================== */

void gb_platform_shutdown(void) {
    close_input_record_file();

    if (g_audio_device) {
        SDL_CloseAudioDevice(g_audio_device);
        g_audio_device = 0;
    }

    clear_controller_state();
    g_audio_started = false;
    g_audio_start_threshold = 0;
    
    if (ImGui::GetCurrentContext() != NULL) {
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
    }

    if (g_texture) {
        SDL_DestroyTexture(g_texture);
        g_texture = NULL;
    }
    if (g_renderer) {
        SDL_DestroyRenderer(g_renderer);
        g_renderer = NULL;
    }
    if (g_window) {
        SDL_DestroyWindow(g_window);
        g_window = NULL;
    }
    g_registered_ctx = NULL;
    g_app_suspended = false;
    g_renderer_reset_pending = false;
    SDL_Quit();
}

void gb_platform_set_benchmark_mode(bool enabled) {
    g_benchmark_mode = enabled;
}

void gb_platform_set_launcher_return_enabled(bool enabled) {
    g_launcher_return_enabled = enabled;
}

GBPlatformExitAction gb_platform_get_exit_action(void) {
    return g_exit_action;
}

/* ============================================================================
 * Audio - Simple Push Mode with SDL_QueueAudio
 * ========================================================================== */

#define AUDIO_SAMPLE_RATE 44100

/* 
 * Audio: Simple circular buffer with SDL callback
 * The callback pulls samples at exactly 44100 Hz.
 * The emulator pushes samples as they're generated.
 * A large buffer provides tolerance for timing variations.
 */
#define AUDIO_RING_SIZE 16384  /* ~370ms buffer - plenty of headroom */
static int16_t g_audio_ring[AUDIO_RING_SIZE * 2];  /* Stereo */
static std::atomic<uint32_t> g_audio_write_pos{0};
static std::atomic<uint32_t> g_audio_read_pos{0};
static uint32_t g_audio_device_sample_rate = AUDIO_SAMPLE_RATE;

/* Debug counters */
static uint32_t g_audio_samples_written = 0;
static uint32_t g_audio_underruns = 0;

static uint32_t current_audio_underruns(void) {
    return g_audio_underruns;
}

static uint32_t current_audio_ring_fill_samples(void) {
    return audio_ring_fill_samples();
}

static uint32_t current_audio_ring_capacity(void) {
    return AUDIO_RING_SIZE;
}

static uint32_t audio_ring_fill_samples(void) {
    uint32_t write_pos = g_audio_write_pos.load(std::memory_order_acquire);
    uint32_t read_pos = g_audio_read_pos.load(std::memory_order_acquire);
    return (write_pos >= read_pos) ? (write_pos - read_pos) : (AUDIO_RING_SIZE - read_pos + write_pos);
}

static void update_audio_stats_from_ring(void) {
    audio_stats_update_buffer(audio_ring_fill_samples(), AUDIO_RING_SIZE, g_audio_device_sample_rate);
}

/* SDL callback - pulls samples from ring buffer */
static void sdl_audio_callback(void* userdata, Uint8* stream, int len) {
    (void)userdata;
    int16_t* out = (int16_t*)stream;
    int samples_needed = len / 4;  /* Stereo 16-bit = 4 bytes per sample */
    
    uint32_t write_pos = g_audio_write_pos.load(std::memory_order_acquire);
    uint32_t read_pos = g_audio_read_pos.load(std::memory_order_relaxed);
    
    for (int i = 0; i < samples_needed; i++) {
        if (read_pos != write_pos) {
            /* Have data - copy it */
            out[i * 2] = g_audio_ring[read_pos * 2];
            out[i * 2 + 1] = g_audio_ring[read_pos * 2 + 1];
            read_pos = (read_pos + 1) % AUDIO_RING_SIZE;
        } else {
            /* Underrun - output silence */
            out[i * 2] = 0;
            out[i * 2 + 1] = 0;
            g_audio_underruns++;
            audio_stats_underrun();
        }
    }
    
    g_audio_read_pos.store(read_pos, std::memory_order_release);
}

static void on_audio_sample(GBContext* ctx, int16_t left, int16_t right) {
    (void)ctx;
    if (g_audio_device == 0) return;
    
    uint32_t write_pos = g_audio_write_pos.load(std::memory_order_relaxed);
    uint32_t next_write = (write_pos + 1) % AUDIO_RING_SIZE;
    
    /* If buffer is full, drop this sample (prevents blocking) */
    if (next_write == g_audio_read_pos.load(std::memory_order_acquire)) {
        audio_stats_samples_dropped(1);
        return;  /* Drop sample */
    }
    
    g_audio_ring[write_pos * 2] = left;
    g_audio_ring[write_pos * 2 + 1] = right;
    g_audio_write_pos.store(next_write, std::memory_order_release);
    g_audio_samples_written++;
    audio_stats_samples_queued(1);

    if (!g_audio_started && audio_ring_fill_samples() >= g_audio_start_threshold) {
        SDL_PauseAudioDevice(g_audio_device, 0);
        g_audio_started = true;
    }
}

bool gb_platform_init(int scale) {
    g_benchmark_mode = g_benchmark_mode || env_flag_enabled("GBRECOMP_BENCHMARK");
    g_scale = scale;
    if (g_scale < 1) g_scale = 1;
    if (g_scale > 8) g_scale = 8;
    g_windowed_width = GB_SCREEN_WIDTH * g_scale;
    g_windowed_height = GB_SCREEN_HEIGHT * g_scale;
    g_game_viewport = {0, 0, g_windowed_width, g_windowed_height};
    g_exit_action = GB_PLATFORM_EXIT_QUIT;
    g_frame_count = 0;
    g_manual_joypad_buttons = 0xFF;
    g_manual_joypad_dpad = 0xFF;
    g_script_joypad_buttons = 0xFF;
    g_script_joypad_dpad = 0xFF;
    g_fullscreen = platform_default_fullscreen();
    g_app_suspended = false;
    g_renderer_reset_pending = false;
    g_show_overlay = false;
    update_effective_joypad_state();
    g_last_guest_framebuffer_valid = false;
    g_present_count = 0;
    g_last_timing = {};

    if (g_benchmark_mode) {
        if (!SDL_getenv("SDL_VIDEODRIVER")) {
            SDL_setenv("SDL_VIDEODRIVER", "dummy", 0);
        }
        if (!SDL_getenv("SDL_AUDIODRIVER")) {
            SDL_setenv("SDL_AUDIODRIVER", "dummy", 0);
        }
        if (SDL_Init(SDL_INIT_TIMER) < 0) {
            fprintf(stderr, "[SDL] SDL_Init failed in benchmark mode: %s\n", SDL_GetError());
            return false;
        }
        g_last_frame_time = SDL_GetTicks();
        return true;
    }
    
    fprintf(stderr, "[SDL] Initializing SDL...\n");
#if defined(__ANDROID__)
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "[SDL] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    fprintf(stderr, "[SDL] SDL initialized.\n");

#if defined(__ANDROID__)
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
#endif

    clear_controller_state();
    open_first_available_controller();
    
    /* Initialize Audio - Callback Mode with large buffer */
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 2048;  /* Larger buffer for smooth playback */
    want.callback = sdl_audio_callback;
    want.userdata = NULL;
    
    g_audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio_device == 0) {
        fprintf(stderr, "[SDL] Failed to open audio: %s\n", SDL_GetError());
    } else {
        g_audio_write_pos.store(0, std::memory_order_relaxed);
        g_audio_read_pos.store(0, std::memory_order_relaxed);
        g_audio_started = false;
        g_audio_device_sample_rate = (uint32_t)have.freq;
        g_audio_start_threshold = (uint32_t)have.samples;
        if (g_audio_start_threshold == 0) g_audio_start_threshold = 1;
        if (g_audio_start_threshold > AUDIO_RING_SIZE / 2) {
            g_audio_start_threshold = AUDIO_RING_SIZE / 2;
        }
        fprintf(stderr, "[SDL] Audio initialized: %d Hz, %d channels, buffer %d samples (Callback Mode)\n", 
                have.freq, have.channels, have.samples);
        update_audio_stats_from_ring();
    }
    
    fprintf(stderr, "[SDL] Creating window...\n");
    g_window = SDL_CreateWindow(
        "GameBoy Recompiled",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        g_windowed_width,
        g_windowed_height,
        SDL_WINDOW_SHOWN |
        (g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_RESIZABLE)
    );
    
    if (!g_window) {
        fprintf(stderr, "[SDL] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }
    fprintf(stderr, "[SDL] Window created.\n");
    SDL_SetWindowMinimumSize(g_window, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT);
    
    fprintf(stderr, "[SDL] Creating renderer...\n");
    /* 
     * NO VSync - we use wall-clock timing to run at exactly 59.7 FPS.
     * This is essential for non-60Hz monitors (like 100Hz).
     */
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
        
    if (!g_renderer) {
        fprintf(stderr, "[SDL] Hardware renderer failed, trying software fallback...\n");
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }
        
    if (!g_renderer) {
        fprintf(stderr, "[SDL] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return false;
    }
    
    update_render_filter();

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplSDL2_InitForSDLRenderer(g_window, g_renderer);
    ImGui_ImplSDLRenderer2_Init(g_renderer);

    if (!recreate_streaming_texture()) {
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return false;
    }
    update_game_viewport();
    
    g_last_frame_time = SDL_GetTicks();
    
    return true;
}

static bool handle_runtime_event(const SDL_Event* event, GBContext* ctx) {
    if (!event) {
        return true;
    }

    const uint8_t joyp = ctx ? ctx->io[0x00] : 0xFF;
    const bool dpad_selected = !(joyp & 0x10);
    const bool buttons_selected = !(joyp & 0x20);

    if (event->type == SDL_QUIT) {
        return false;
    }
    if (event->type == SDL_WINDOWEVENT &&
        event->window.event == SDL_WINDOWEVENT_CLOSE &&
        (!g_window || event->window.windowID == SDL_GetWindowID(g_window))) {
        return false;
    }

    switch (event->type) {
        case SDL_APP_WILLENTERBACKGROUND:
            set_app_suspended(true);
            break;

        case SDL_APP_DIDENTERFOREGROUND:
            set_app_suspended(false);
            break;

        case SDL_RENDER_TARGETS_RESET:
        case SDL_RENDER_DEVICE_RESET:
            g_renderer_reset_pending = true;
            recreate_streaming_texture();
            break;

        case SDL_CONTROLLERDEVICEADDED:
            if (!g_controller) {
                open_controller_index(event->cdevice.which);
            }
            break;

        case SDL_CONTROLLERDEVICEREMOVED:
            if (g_controller && active_controller_instance_id() == event->cdevice.which) {
                clear_controller_state();
                open_first_available_controller();
            }
            break;

        case SDL_CONTROLLERDEVICEREMAPPED:
            if (g_controller && active_controller_instance_id() == event->cdevice.which) {
                refresh_controller_profile();
            }
            break;

        case SDL_CONTROLLERAXISMOTION: {
            const int deadzone = 8000;

            if (event->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                if (event->caxis.value > deadzone) {
                    g_manual_joypad_dpad &= ~0x01;
                    g_manual_joypad_dpad |= 0x02;
                } else if (event->caxis.value < -deadzone) {
                    g_manual_joypad_dpad &= ~0x02;
                    g_manual_joypad_dpad |= 0x01;
                } else {
                    g_manual_joypad_dpad |= 0x01;
                    g_manual_joypad_dpad |= 0x02;
                }
            }

            if (event->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                if (event->caxis.value > deadzone) {
                    g_manual_joypad_dpad &= ~0x08;
                    g_manual_joypad_dpad |= 0x04;
                } else if (event->caxis.value < -deadzone) {
                    g_manual_joypad_dpad &= ~0x04;
                    g_manual_joypad_dpad |= 0x08;
                } else {
                    g_manual_joypad_dpad |= 0x04;
                    g_manual_joypad_dpad |= 0x08;
                }
            }
            break;
        }

        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            const bool pressed = (event->type == SDL_CONTROLLERBUTTONDOWN);
            bool trigger = false;

            switch (event->cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP:
                    if (pressed) { g_manual_joypad_dpad &= ~0x04; if (dpad_selected) trigger = true; }
                    else g_manual_joypad_dpad |= 0x04;
                    break;

                case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                    if (pressed) { g_manual_joypad_dpad &= ~0x08; if (dpad_selected) trigger = true; }
                    else g_manual_joypad_dpad |= 0x08;
                    break;

                case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                    if (pressed) { g_manual_joypad_dpad &= ~0x02; if (dpad_selected) trigger = true; }
                    else g_manual_joypad_dpad |= 0x02;
                    break;

                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                    if (pressed) { g_manual_joypad_dpad &= ~0x01; if (dpad_selected) trigger = true; }
                    else g_manual_joypad_dpad |= 0x01;
                    break;

                case SDL_CONTROLLER_BUTTON_A:
                    if (pressed) { g_manual_joypad_buttons &= ~0x02; if (buttons_selected) trigger = true; }
                    else g_manual_joypad_buttons |= 0x02;
                    break;

                case SDL_CONTROLLER_BUTTON_B:
                    if (pressed) { g_manual_joypad_buttons &= ~0x01; if (buttons_selected) trigger = true; }
                    else g_manual_joypad_buttons |= 0x01;
                    break;

                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                    if (pressed) { g_manual_joypad_buttons &= ~0x02; if (buttons_selected) trigger = true; }
                    else g_manual_joypad_buttons |= 0x02;
                    break;

                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                    if (pressed) { g_manual_joypad_buttons &= ~0x01; if (buttons_selected) trigger = true; }
                    else g_manual_joypad_buttons |= 0x01;
                    break;

                case SDL_CONTROLLER_BUTTON_START:
                    if (pressed) { g_manual_joypad_buttons &= ~0x08; if (buttons_selected) trigger = true; }
                    else g_manual_joypad_buttons |= 0x08;
                    break;

                case SDL_CONTROLLER_BUTTON_BACK:
                    if (pressed) { g_manual_joypad_buttons &= ~0x04; if (buttons_selected) trigger = true; }
                    else g_manual_joypad_buttons |= 0x04;
                    break;

                case SDL_CONTROLLER_BUTTON_GUIDE:
                    if (pressed) {
                        g_show_menu = !g_show_menu;
                    }
                    return true;

                default:
                    break;
            }

            if (trigger && ctx && pressed) {
                request_joypad_interrupt(ctx);
            }
            break;
        }

        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            const bool pressed = (event->type == SDL_KEYDOWN);
            bool trigger = false;

            switch (event->key.keysym.scancode) {
                case SDL_SCANCODE_UP:
                case SDL_SCANCODE_W:
                    if (pressed) { g_manual_joypad_dpad &= ~0x04; if (dpad_selected) trigger = true; }
                    else g_manual_joypad_dpad |= 0x04;
                    break;
                case SDL_SCANCODE_DOWN:
                case SDL_SCANCODE_S:
                    if (pressed) { g_manual_joypad_dpad &= ~0x08; if (dpad_selected) trigger = true; }
                    else g_manual_joypad_dpad |= 0x08;
                    break;
                case SDL_SCANCODE_LEFT:
                case SDL_SCANCODE_A:
                    if (pressed) { g_manual_joypad_dpad &= ~0x02; if (dpad_selected) trigger = true; }
                    else g_manual_joypad_dpad |= 0x02;
                    break;
                case SDL_SCANCODE_RIGHT:
                case SDL_SCANCODE_D:
                    if (pressed) { g_manual_joypad_dpad &= ~0x01; if (dpad_selected) trigger = true; }
                    else g_manual_joypad_dpad |= 0x01;
                    break;

                case SDL_SCANCODE_Z:
                case SDL_SCANCODE_J:
                    if (pressed) { g_manual_joypad_buttons &= ~0x01; if (buttons_selected) trigger = true; }
                    else g_manual_joypad_buttons |= 0x01;
                    break;
                case SDL_SCANCODE_X:
                case SDL_SCANCODE_K:
                    if (pressed) { g_manual_joypad_buttons &= ~0x02; if (buttons_selected) trigger = true; }
                    else g_manual_joypad_buttons |= 0x02;
                    break;
                case SDL_SCANCODE_RSHIFT:
                case SDL_SCANCODE_BACKSPACE:
                    if (pressed) { g_manual_joypad_buttons &= ~0x04; if (buttons_selected) trigger = true; }
                    else g_manual_joypad_buttons |= 0x04;
                    break;
                case SDL_SCANCODE_RETURN:
                    if (pressed) { g_manual_joypad_buttons &= ~0x08; if (buttons_selected) trigger = true; }
                    else g_manual_joypad_buttons |= 0x08;
                    break;

                case SDL_SCANCODE_ESCAPE:
                case SDL_SCANCODE_AC_BACK:
                    if (pressed && event->key.repeat == 0) {
                        g_show_menu = !g_show_menu;
                    }
                    return true;

                case SDL_SCANCODE_F1:
                    if (pressed && event->key.repeat == 0) {
                        g_show_overlay = !g_show_overlay;
                    }
                    return true;

                default:
                    break;
            }

            if (trigger && ctx && pressed && event->key.repeat == 0) {
                request_joypad_interrupt(ctx);
            }
            break;
        }

        case SDL_WINDOWEVENT:
            if (event->window.event == SDL_WINDOWEVENT_RESIZED ||
                event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                if (!g_fullscreen) {
                    g_windowed_width = event->window.data1;
                    g_windowed_height = event->window.data2;
                }
                update_game_viewport();
            }
            break;

        default:
            break;
    }

    return true;
}

bool gb_platform_poll_events(GBContext* ctx) {
    SDL_Event event;

    if (!g_benchmark_mode) {
        while (SDL_PollEvent(&event)) {
            if (ImGui::GetCurrentContext() != NULL) {
                ImGui_ImplSDL2_ProcessEvent(&event);
            }
            if (!handle_runtime_event(&event, ctx)) {
                return false;
            }
        }

        while (g_app_suspended) {
            if (!SDL_WaitEvent(&event)) {
                continue;
            }
            if (ImGui::GetCurrentContext() != NULL) {
                ImGui_ImplSDL2_ProcessEvent(&event);
            }
            if (!handle_runtime_event(&event, ctx)) {
                return false;
            }
        }
    }
    
    /* Handle Automation Inputs */
    uint8_t previous_script_dpad = g_script_joypad_dpad;
    uint8_t previous_script_buttons = g_script_joypad_buttons;
    g_script_joypad_dpad = 0xFF;
    g_script_joypad_buttons = 0xFF;

    uint64_t current_cycles = ctx ? ctx->cycles : 0;
    for (int i = 0; i < g_script_count; i++) {
        ScriptEntry* e = &g_input_script[i];
        bool active = false;
        if (e->anchor == SCRIPT_ANCHOR_CYCLE) {
            active = current_cycles >= e->start &&
                     current_cycles < (e->start + e->duration);
        } else {
            uint64_t current_frame = (uint64_t)g_frame_count;
            active = current_frame >= e->start &&
                     current_frame < (e->start + e->duration);
        }
        if (active) {
            g_script_joypad_dpad &= e->dpad;
            g_script_joypad_buttons &= e->buttons;
        }
    }

    uint8_t new_script_dpad = (uint8_t)(previous_script_dpad & (uint8_t)(~g_script_joypad_dpad) & 0x0F);
    uint8_t new_script_buttons = (uint8_t)(previous_script_buttons & (uint8_t)(~g_script_joypad_buttons) & 0x0F);
    uint8_t joyp = ctx ? ctx->io[0x00] : 0xFF;
    bool dpad_selected = !(joyp & 0x10);
    bool buttons_selected = !(joyp & 0x20);
    if (ctx && ((new_script_dpad && dpad_selected) || (new_script_buttons && buttons_selected))) {
        request_joypad_interrupt(ctx);
    }

    update_effective_joypad_state();
    record_manual_input_state(current_cycles);

    return true;
}



void gb_platform_render_frame(const uint32_t* framebuffer) {
    render_frame_internal(framebuffer, true);
}

void gb_platform_present_framebuffer(const uint32_t* framebuffer) {
    const uint32_t* stable_framebuffer = g_last_guest_framebuffer_valid ? g_last_guest_framebuffer : framebuffer;
    render_frame_internal(stable_framebuffer, false);
}

void gb_platform_render_lcd_off_frame(void) {
    ensure_lcd_off_framebuffer();
    const uint32_t* stable_framebuffer = g_last_guest_framebuffer_valid ? g_last_guest_framebuffer : g_lcd_off_framebuffer;
    render_frame_internal(stable_framebuffer, false);
}

void gb_platform_get_timing_info(GBPlatformTimingInfo* out) {
    if (!out) return;
    *out = g_last_timing;
}

uint8_t gb_platform_get_joypad(void) {
    /* Return combined state based on P1 register selection */
    /* Caller should AND with the appropriate selection bits */
    return g_joypad_buttons & g_joypad_dpad;
}

void gb_platform_vsync(uint32_t frame_cycles) {
    if (g_benchmark_mode || g_app_suspended) {
        g_last_timing.pacing_cycles = (frame_cycles > 0) ? frame_cycles : 70224u;
        g_last_timing.pacing_ms = 0.0;
        return;
    }
    /* 
     * Frame pacing: Run at the DMG frame cadence derived from 70224 cycles
     * at 4194304 Hz, and ease off sleeping when audio fill is too low.
     *
     * Each call accounts for the frame that just completed. Keep an
     * accumulated wall-clock target and advance it before waiting so the
     * current frame's cycle count is what determines the current sleep.
     */
    static uint64_t next_frame_time = 0;
    static uint64_t frame_remainder = 0;
    const uint64_t gb_frame_cycles = (frame_cycles > 0) ? (uint64_t)frame_cycles : 70224ull;
    const uint64_t gb_cpu_hz = 4194304;
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t now = SDL_GetPerformanceCounter();
    uint32_t speed_percent = (g_speed_percent > 0) ? (uint32_t)g_speed_percent : 100;
    uint64_t frame_ticks_num = (freq * gb_frame_cycles * 100ull) + frame_remainder;
    uint64_t frame_ticks_den = gb_cpu_hz * (uint64_t)speed_percent;
    uint64_t frame_ticks = frame_ticks_num / frame_ticks_den;
    double pacing_start_ms = sdl_now_ms();

    if (frame_ticks == 0) {
        frame_ticks = 1;
    }

    if (next_frame_time == 0) {
        next_frame_time = now;
    }

    frame_remainder = frame_ticks_num % frame_ticks_den;
    next_frame_time += frame_ticks;
    uint64_t target_frame_time = next_frame_time;

    uint32_t audio_fill = audio_ring_fill_samples();
    bool audio_starved = g_audio_started && audio_fill < (g_audio_start_threshold / 2);

    if (!audio_starved && now < target_frame_time) {
        for (;;) {
            uint64_t wait_ticks = target_frame_time - now;
            uint32_t wait_us = (uint32_t)((wait_ticks * 1000000) / freq);

            /*
             * SDL_Delay() can oversleep by several milliseconds on desktop OSes.
             * Sleep in small chunks, then busy-wait the tail for stable pacing.
             */
            if (wait_us > 4000) {
                SDL_Delay((wait_us / 1000) - 2);
            } else if (wait_us > 1500) {
                SDL_Delay(1);
            } else {
                break;
            }

            now = SDL_GetPerformanceCounter();
            if (now >= target_frame_time) {
                break;
            }
        }

        while (SDL_GetPerformanceCounter() < target_frame_time) {
            /* spin */
        }
        now = SDL_GetPerformanceCounter();
    }

    /* If we fell behind by more than 3 frames, reset (don't try to catch up) */
    uint64_t max_frame_lag = frame_ticks * 3;
    if (now > target_frame_time + max_frame_lag) {
        next_frame_time = now;
        frame_remainder = 0;
    }
    
    update_audio_stats_from_ring();
    audio_stats_tick(SDL_GetTicks64());
    g_last_timing.pacing_cycles = (uint32_t)gb_frame_cycles;
    g_last_timing.pacing_ms = sdl_now_ms() - pacing_start_ms;
    g_timing_vsync_total += g_last_timing.pacing_ms;
    g_timing_frame_count++;
    g_last_frame_time = SDL_GetTicks();
}

bool gb_platform_get_smooth_lcd_transitions(void) {
    return g_smooth_lcd_transitions;
}

void gb_platform_set_smooth_lcd_transitions(bool enabled) {
    g_smooth_lcd_transitions = enabled;
}

void gb_platform_set_title(const char* title) {
    if (g_window && !g_benchmark_mode) {
        SDL_SetWindowTitle(g_window, title);
    }
}

/* ============================================================================
 * Save Data
 * ========================================================================== */

static void sdl_get_save_path(char* buffer, size_t size, const char* rom_name) {
    const std::string base_name = extract_path_leaf(rom_name);
    const std::string filename = base_name + ".sav";

#if defined(__ANDROID__)
    const std::string resolved = resolve_writable_path(filename.c_str(), base_name.c_str());
    snprintf(buffer, size, "%s", resolved.c_str());
#else
    char* base_path = SDL_GetBasePath();
    if (base_path) {
        fs::path resolved = fs::path(base_path) / filename;
        SDL_free(base_path);
        snprintf(buffer, size, "%s", resolved.lexically_normal().string().c_str());
    } else {
        const std::string resolved = resolve_writable_path(filename.c_str(), base_name.c_str());
        snprintf(buffer, size, "%s", resolved.c_str());
    }
#endif
}

static bool sdl_load_battery_ram(GBContext* ctx, const char* rom_name, void* data, size_t size) {
    (void)ctx;
    char filename[512];
    sdl_get_save_path(filename, sizeof(filename), rom_name);
    
    FILE* f = fopen(filename, "rb");
    if (!f) return false;
    
    size_t read = fread(data, 1, size, f);
    fclose(f);
    
    return read == size;
}

static bool sdl_save_battery_ram(GBContext* ctx, const char* rom_name, const void* data, size_t size) {
    (void)ctx;
    char filename[512];
    sdl_get_save_path(filename, sizeof(filename), rom_name);
    
    FILE* f = fopen(filename, "wb");
    if (!f) return false;
    
    size_t written = fwrite(data, 1, size, f);
    fclose(f);
    
    return written == size;
}

void gb_platform_register_context(GBContext* ctx) {
    g_registered_ctx = ctx;
    GBPlatformCallbacks callbacks = {
        .on_audio_sample = on_audio_sample,
        .load_battery_ram = sdl_load_battery_ram,
        .save_battery_ram = sdl_save_battery_ram
    };
    gb_set_platform_callbacks(ctx, &callbacks);
}

#else  /* !GB_HAS_SDL2 */

/* Stub implementations when SDL2 is not available */

bool gb_platform_init(int scale) {
    (void)scale;
    return false;
}

void gb_platform_shutdown(void) {}

void gb_platform_set_launcher_return_enabled(bool enabled) {
    (void)enabled;
}

GBPlatformExitAction gb_platform_get_exit_action(void) {
    return GB_PLATFORM_EXIT_QUIT;
}

bool gb_platform_poll_events(GBContext* ctx) {
    (void)ctx;
    return true;
}

void gb_platform_set_input_script(const char* script) { (void)script; }

void gb_platform_set_input_record_file(const char* path) { (void)path; }
void gb_platform_set_benchmark_mode(bool enabled) { (void)enabled; }

void gb_platform_render_frame(const uint32_t* framebuffer) {
    (void)framebuffer;
}

void gb_platform_present_framebuffer(const uint32_t* framebuffer) {
    (void)framebuffer;
}

void gb_platform_render_lcd_off_frame(void) {}

void gb_platform_get_timing_info(GBPlatformTimingInfo* out) {
    if (!out) return;
    *out = GBPlatformTimingInfo{};
}

uint8_t gb_platform_get_joypad(void) {
    return 0xFF;
}

void gb_platform_vsync(uint32_t frame_cycles) { (void)frame_cycles; }

bool gb_platform_get_smooth_lcd_transitions(void) { return false; }

void gb_platform_set_smooth_lcd_transitions(bool enabled) { (void)enabled; }

void gb_platform_set_title(const char* title) {
    (void)title;
}

void gb_platform_set_dump_frames(const char* frames) { (void)frames; }

void gb_platform_set_screenshot_prefix(const char* prefix) { (void)prefix; }

void gb_platform_register_context(GBContext* ctx) { (void)ctx; }

#endif /* GB_HAS_SDL2 */
