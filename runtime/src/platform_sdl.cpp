/**
 * @file platform_sdl.cpp
 * @brief SDL2 platform implementation for GameBoy runtime with ImGui
 */

#include "platform_sdl.h"
#include "gbrt.h"   /* For GBPlatformCallbacks */
#include "ppu.h"
#include "audio_stats.h"
#include "gbrt_debug.h"
#include "serial_link.h"
#include "network_discovery.h"
#include "sgb.h"
#include "relay_client.h"
#include "cheats.h"
#ifdef GBRT_HAVE_GBCAM
#include "gbcam.h"
#endif
#include "gb_printer.h"
#include "debug_server.h"
#include "color_lut.h"  /* present-time screen-color LUT (opt-in, default raw) */
extern "C" {
#include "keybinds.h"
}

/* Forward declaration for debug server context setter */
extern "C" void gb_debug_server_set_context(GBContext *ctx);

#ifdef GB_HAS_SDL2
#include <SDL.h>
#include <SDL_opengles2.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <filesystem>
#include <string>
#include <vector>
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#define IMGUI_IMPL_OPENGL_ES2
#include "backends/imgui_impl_opengl3.h"

#include "shader_pipeline.h"
#include "game_extras.h"  /* game_draw_overlay() hook (no-op default in gbrt) */
#include "gb_widescreen.h" /* opt-in extended view: render width + arming */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
/* Implementation lives in gb_printer.c (linked into gbrt). Including
 * the header here (without STB_IMAGE_WRITE_IMPLEMENTATION) gives us
 * just the function prototypes, no duplicate-symbol clash. */
#include "stb_image_write.h"

namespace fs = std::filesystem;

/* SGB border geometry — the 256x224 SGB frame surrounds a 160x144 GB screen
 * centered with 48px horizontal and 40px vertical insets. */
static constexpr int GB_BORDER_FULL_W = 256;
static constexpr int GB_BORDER_FULL_H = 224;
static constexpr int GB_BORDER_INSET_X = 48;
static constexpr int GB_BORDER_INSET_Y = 40;
static constexpr const char* GB_BORDER_DIR_NAME = "borders";

/* ============================================================================
 * SDL State
 * ========================================================================== */

static SDL_Window* g_window = NULL;
static SDL_GLContext g_gl_context = NULL;
static GLuint g_game_tex = 0;
static GBShaderPipeline* g_shader_pipeline = NULL;
static std::string g_active_shader_pref = "sharp";

/* Present-time screen-color LUT (opt-in via GBCRECOMP_SCREEN; default raw =>
 * passthrough, frame byte-identical). Built lazily on first present; NULL when
 * disabled/raw/unrecognized so the upload path is untouched. Applied to a
 * scratch ARGB copy before the RGBA GL upload — never the PPU framebuffer or
 * the verify path. */
static ColorLut* g_color_lut = NULL;
static bool g_color_lut_resolved = false;
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
/* Color correction: 0=Off, 1=GBC (modern LCDs viewing GBC-tinted output),
 * 2=GBA (compensate for AGB LCD's darker / washed-out look). Applied
 * per-pixel during the framebuffer upload. Defaults to Off but gets
 * auto-bumped to a matching mode when the user picks CGB / GBA
 * hardware. Only relevant in CGB/GBA hardware modes — hidden in DMG/SGB. */
static int g_color_correction = 0;
static bool g_smooth_lcd_transitions = true;
static bool g_launcher_return_enabled = false;
static bool g_benchmark_mode = false;
static bool g_fullscreen = false;
static bool g_app_suspended = false;
static bool g_renderer_reset_pending = false;

/* Custom SGB border state */
static bool g_border_enabled = false;
static std::vector<std::string> g_border_files;
static int g_border_idx = 0;
static std::string g_border_dir;
static std::string g_border_loaded_filename;
static GLuint g_border_texture = 0;

static GBPrinter* g_printer = NULL;

static GBHardwareModePref hardware_mode_pref_from_string(const std::string& s);
static const char* hardware_mode_pref_to_string(GBHardwareModePref mode);
static void set_active_game_pref(const char* key, const std::string& value);
static void set_active_game_pref_bool(const char* key, bool value);
static void set_active_game_pref_int(const char* key, int value);

/* Per-game preferences keyed under "game.<id>." in runtime_prefs.ini.
 * gb_platform_set_game_id reads from this when the recompiled main
 * tells the platform which cart is loading, and the menu UI writes
 * back here when the user changes a per-game option. */
static std::map<std::string, std::map<std::string, std::string>> g_per_game_prefs;
static std::string g_active_game_id;
static GBHardwareModePref g_active_hardware_mode_pref = GB_HARDWARE_MODE_AUTO;

/* SGB display preferences. Both default true. The engine itself runs
 * any time the cart supports SGB; these flags only control what the
 * platform renders from the engine's captured state. They're
 * independent — you can have palette tints without the cart border, or
 * the cart border without palette tints. */
static bool g_sgb_colors_pref = true;

static bool g_show_fps = false;
/* ImGui theme: 0 = Dark (default), 1 = Light, 2 = Classic. */
static int  g_imgui_theme = 0;
static void apply_imgui_theme(int idx) {
    switch (idx) {
        case 1:  ImGui::StyleColorsLight();   break;
        case 2:  ImGui::StyleColorsClassic(); break;
        default: ImGui::StyleColorsDark();    break;
    }
    /* Override the nav highlight to a softer blue across all themes
     * so keyboard/gamepad focus reads consistently. */
    ImGui::GetStyle().Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.45f, 0.65f, 0.95f, 0.65f);
}

/* SGB cart-supplied border (CHR_TRN+PCT_TRN). Rebuilt from sgb engine
 * data whenever the revision counter advances. Default enabled — most
 * SGB carts ship with a border specifically authored for them. */
static bool g_sgb_cart_border_enabled = true;
static GLuint g_sgb_cart_border_texture = 0;

/* LAN auto-discovery + direct-link state. The discovery engine itself
 * lives in network_discovery.{c,h}; these are just the menu-side
 * mirrors and the user's persisted preferences. */
static bool g_lan_enabled_pref = false;       /* persisted user choice */
static bool g_link_listening = false;         /* TCP listener up */
static int  g_link_port_pref = 1989;
static std::string g_lan_uuid_pref;           /* loaded from prefs (mirror of gb_lan_self_uuid) */
static std::string g_lan_nickname_pref;       /* same for nickname */
static char g_link_manual_host[128] = "";
static char g_link_manual_port[16] = "1989";
static char g_link_nickname_edit[GB_LAN_NICKNAME_LEN] = "";
static std::string g_link_status_text;

/* Rendezvous (relay) state — the nickname and host port come from the
 * LAN side (g_link_nickname_edit / g_link_port_pref) so the user picks
 * them once. Only the URL and room code are relay-specific. */
static char g_relay_url[256]   = "";
static char g_relay_code[GB_RELAY_ROOM_CODE_LEN] = "";
static bool g_relay_hosting    = false;  /* set by Host via Relay; cleared
                                            by Disconnect / Forget room */
static bool g_relay_joined     = false;  /* set after Join via Relay
                                            successfully connected */
static std::string g_relay_status;
/* Open-room list pulled from the server on demand. The selected index
 * is what the dropdown is showing; -1 means "nothing picked yet". */
static std::vector<GBRelayRoomInfo> g_relay_rooms;
static int  g_relay_room_pick = -1;
static std::string g_relay_list_status;

static void generate_random_room_code(char* out, size_t cap) {
    static const char alphabet[] = "abcdefghijkmnopqrstuvwxyz23456789";
    if (cap == 0) return;
    size_t letters = (cap > 7) ? 6 : (cap - 1);
    for (size_t i = 0; i < letters; i++) {
        int idx = rand() % (int)(sizeof(alphabet) - 1);
        out[i] = alphabet[idx];
    }
    out[letters] = '\0';
}
static uint32_t g_sgb_cart_border_revision = 0;
static std::vector<uint32_t> g_sgb_cart_border_pixels;  /* scratch RGBA buffer */

/* Cross-session cache for the cart's authored SGB border. The cart
 * only emits CHR_TRN/PCT_TRN packets while running with SGB hardware
 * mode, but the resulting image is static — capture it once and
 * replay forever. Lets the user switch a game to CGB hardware mode
 * (which silences the SGB engine) and still get the cart border on
 * top of full-color CGB output. Cache lives at
 * <cwd>/sgb_borders/<game_id>.png. */
static bool g_sgb_cart_border_from_cache = false;
static bool g_sgb_cart_border_cache_saved = false;
static GBPlatformExitAction g_exit_action = GB_PLATFORM_EXIT_QUIT;
/* Set by the Esc menu's "Restart Game" button; consumed by the launcher. */
static bool g_restart_game_requested = false;
static const char* g_palette_names[] = {
    "DMG",
    "Game Boy Pocket",
    "Game Boy Light",
    "Black & White",
    "Amber (Phosphor)",
};
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
typedef enum GBInputAction {
    GB_INPUT_ACTION_RIGHT = 0,
    GB_INPUT_ACTION_LEFT = 1,
    GB_INPUT_ACTION_UP = 2,
    GB_INPUT_ACTION_DOWN = 3,
    GB_INPUT_ACTION_A = 4,
    GB_INPUT_ACTION_B = 5,
    GB_INPUT_ACTION_SELECT = 6,
    GB_INPUT_ACTION_START = 7,
    GB_INPUT_ACTION_FAST_FORWARD = 8,
    GB_INPUT_ACTION_TOGGLE_MAX_SPEED = 9,
    GB_INPUT_ACTION_SAVE_STATE = 10,
    GB_INPUT_ACTION_LOAD_STATE = 11,
    GB_INPUT_ACTION_PREVIOUS_STATE_SLOT = 12,
    GB_INPUT_ACTION_NEXT_STATE_SLOT = 13,
    GB_INPUT_ACTION_TOGGLE_OVERLAY = 14,
    GB_INPUT_ACTION_TOGGLE_MUTE = 15,
    GB_INPUT_ACTION_TOGGLE_MENU = 16,
    GB_INPUT_ACTION_COUNT = 17,
} GBInputAction;
typedef enum GBInputBindingKind {
    GB_INPUT_BINDING_NONE = 0,
    GB_INPUT_BINDING_KEY = 1,
    GB_INPUT_BINDING_CONTROLLER_BUTTON = 2,
    GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE = 3,
    GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE = 4,
} GBInputBindingKind;
typedef enum GBBindingCaptureDevice {
    GB_CAPTURE_DEVICE_NONE = 0,
    GB_CAPTURE_DEVICE_KEYBOARD = 1,
    GB_CAPTURE_DEVICE_CONTROLLER = 2,
} GBBindingCaptureDevice;
typedef struct GBInputBinding {
    GBInputBindingKind kind;
    int16_t code;
} GBInputBinding;
static GBRenderScalingMode g_render_scaling_mode = GB_RENDER_SCALING_PIXEL_PERFECT;
static GBRenderFilterMode g_render_filter_mode = GB_RENDER_FILTER_NEAREST;
static SDL_GameControllerType g_controller_type = SDL_CONTROLLER_TYPE_UNKNOWN;
static GBControllerLabelProfile g_controller_label_profile = GB_CONTROLLER_LABEL_GENERIC;
static std::string g_controller_name;
static bool g_audio_output_enabled = true;
static bool g_audio_muted = false;
static uint32_t g_audio_latency_ms = 80;
static uint32_t g_audio_volume_percent = 100;
static uint32_t g_audio_low_watermark = 0;
static uint32_t g_audio_device_sample_rate = 44100;
static uint32_t g_audio_device_buffer_samples = 0;
static std::vector<std::string> g_audio_output_devices;
static std::string g_audio_target_device_name;
static std::string g_audio_active_device_name;
static constexpr int GB_SAVESTATE_SLOT_COUNT = 10;
static constexpr int GB_JOYPAD_ACTION_COUNT = 8;
static constexpr int GB_FAST_FORWARD_SPEED_PERCENT = 250;
static constexpr int GB_MAX_SHORTCUT_SPEED_PERCENT = 500;
static int g_savestate_slot = 0;
static std::string g_savestate_status;
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
    { 0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820 }, // DMG
    /* Game Boy Pocket — cool, slightly blue-tinted greyscale, mimicking
     * the Pocket's reflective LCD against its silver shell. */
    { 0xFFC9D2D5, 0xFF8B95A0, 0xFF4C5562, 0xFF181C20 }, // GB Pocket
    /* Game Boy Light — warm yellow-green, tuned to evoke the Light's
     * indiglo-style backlit panel (essentially a Pocket with a glowing
     * EL backlight). */
    { 0xFFCFE07A, 0xFF8AA34A, 0xFF45612A, 0xFF131D11 }, // GB Light
    { 0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000 }, // B&W
    { 0xFFFFB000, 0xFFCB4F0E, 0xFF800000, 0xFF330000 }, // Amber (Phosphor)
};
static uint32_t g_lcd_off_framebuffer[GB_MAX_FRAMEBUFFER_SIZE];
static uint32_t g_last_guest_framebuffer[GB_MAX_FRAMEBUFFER_SIZE];
static bool g_lcd_off_framebuffer_initialized = false;
static bool g_last_guest_framebuffer_valid = false;
static uint64_t g_present_count = 0;
static GBContext* g_registered_ctx = NULL;
static GBInputBinding g_keyboard_bindings[GB_INPUT_ACTION_COUNT][2] = {};
static GBInputBinding g_controller_bindings[GB_INPUT_ACTION_COUNT][2] = {};
static bool g_keyboard_binding_pressed[GB_INPUT_ACTION_COUNT][2] = {};
static bool g_controller_button_binding_pressed[GB_INPUT_ACTION_COUNT][2] = {};
static bool g_controller_axis_binding_pressed[GB_INPUT_ACTION_COUNT][2] = {};
static bool g_runtime_action_pressed[GB_INPUT_ACTION_COUNT] = {};
static Sint16 g_controller_axis_values[SDL_CONTROLLER_AXIS_MAX] = {};
static bool g_binding_capture_active = false;
static GBBindingCaptureDevice g_binding_capture_device = GB_CAPTURE_DEVICE_NONE;
static GBInputAction g_binding_capture_action = GB_INPUT_ACTION_RIGHT;
static int g_binding_capture_slot = 0;
static bool g_fast_forward_active = false;
static bool g_max_speed_mode = false;
static const char* g_input_action_names[GB_INPUT_ACTION_COUNT] = {
    "Right",
    "Left",
    "Up",
    "Down",
    "Game Boy A",
    "Game Boy B",
    "Select",
    "Start",
    "Fast Forward (Hold)",
    "Toggle Max Speed",
    "Save State",
    "Load State",
    "Previous State Slot",
    "Next State Slot",
    "Toggle Overlay",
    "Toggle Mute",
    "Toggle Menu",
};

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
static void sdl_audio_callback(void* userdata, Uint8* stream, int len);
static void refresh_audio_output_devices(void);
static const char* current_audio_output_device_label(void);
static bool current_audio_output_device_available(void);
static void close_audio_output_device(void);
static bool reopen_audio_output_device(bool preserve_stats);
static void update_controller_axis_binding_state(void);
static void recompute_audio_targets(void);
static void refresh_audio_device_pause_state(void);
static void reset_audio_output_buffer(bool preserve_stats);
static char* trim_ascii(char* text);
static void update_effective_joypad_state(void);
static void save_runtime_preferences(void);
static bool save_savestate_slot(GBContext* ctx, int slot);
static bool load_savestate_slot(GBContext* ctx, int slot);
static bool delete_savestate_slot(GBContext* ctx, int slot);
static bool savestate_slot_exists(const GBContext* ctx, int slot, std::string* out_path);
static float settings_ui_scale_for_size(const ImVec2& display_size);
static const char* controller_menu_hint_text(void);
static bool input_action_is_runtime(GBInputAction action);
static int effective_speed_percent(void);
static void update_runtime_action_state(GBContext* ctx);

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

/* Forward declarations needed by recording/playback */
static void parse_buttons(const char* btn_str, uint8_t* dpad, uint8_t* buttons);
static int g_frame_count = 0;
static bool g_turbo = false;
static GBContext* g_ctx = NULL;  /* Stored from register_context for SRAM flush */

/* ---- Input Recording ---- */
static FILE* g_record_file = NULL;
static uint8_t g_prev_record_dpad = 0xFF;
static uint8_t g_prev_record_buttons = 0xFF;

static const char* buttons_to_str(uint8_t dpad, uint8_t buttons, char* buf, size_t sz) {
    buf[0] = '\0';
    int n = 0;
    /* dpad is active-low: bit clear = pressed */
    if (!(dpad & 0x01)) { if (n) buf[n++] = ','; buf[n++] = 'R'; }
    if (!(dpad & 0x02)) { if (n) buf[n++] = ','; buf[n++] = 'L'; }
    if (!(dpad & 0x04)) { if (n) buf[n++] = ','; buf[n++] = 'U'; }
    if (!(dpad & 0x08)) { if (n) buf[n++] = ','; buf[n++] = 'D'; }
    if (!(buttons & 0x01)) { if (n) buf[n++] = ','; buf[n++] = 'A'; }
    if (!(buttons & 0x02)) { if (n) buf[n++] = ','; buf[n++] = 'B'; }
    if (!(buttons & 0x04)) { if (n) buf[n++] = ','; buf[n++] = 'T'; } /* Select */
    if (!(buttons & 0x08)) { if (n) buf[n++] = ','; buf[n++] = 'S'; } /* Start */
    if (n == 0) { buf[0] = '-'; n = 1; }
    buf[n] = '\0';
    (void)sz;
    return buf;
}

void gb_platform_start_recording(const char* path) {
    if (g_record_file) fclose(g_record_file);
    g_record_file = fopen(path, "w");
    if (g_record_file) {
        fprintf(g_record_file, "# gb-recompiled input recording\n");
        fprintf(g_record_file, "# format: frame buttons\n");
        fprintf(g_record_file, "# buttons: U,D,L,R,A,B,S(tart),T(selecT) or - for none\n");
        printf("[RECORD] Recording inputs to %s\n", path);
    } else {
        fprintf(stderr, "[RECORD] Failed to open %s for writing\n", path);
    }
    g_prev_record_dpad = 0xFF;
    g_prev_record_buttons = 0xFF;
}

void gb_platform_stop_recording(void) {
    if (g_record_file) {
        fclose(g_record_file);
        g_record_file = NULL;
        printf("[RECORD] Recording stopped.\n");
    }
}

static void record_frame_input(void) {
    if (!g_record_file) return;
    /* Only write a line when input state changes */
    if (g_joypad_dpad != g_prev_record_dpad || g_joypad_buttons != g_prev_record_buttons) {
        char buf[32];
        buttons_to_str(g_joypad_dpad, g_joypad_buttons, buf, sizeof(buf));
        fprintf(g_record_file, "%d %s\n", g_frame_count, buf);
        fflush(g_record_file);
        g_prev_record_dpad = g_joypad_dpad;
        g_prev_record_buttons = g_joypad_buttons;
    }
}

void gb_platform_load_script_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[SCRIPT] Cannot open %s\n", path); return; }
    g_script_count = 0;
    char line[256];
    uint32_t prev_frame = 0;
    while (fgets(line, sizeof(line), f) && g_script_count < MAX_SCRIPT_ENTRIES - 1) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        uint32_t frame = 0;
        char btn_buf[64] = {0};
        if (sscanf(line, "%u %63s", &frame, btn_buf) < 2) continue;

        /* If there's a gap from the previous entry, fill it with the previous state
         * by setting duration on the prior entry */
        if (g_script_count > 0) {
            ScriptEntry* prev = &g_input_script[g_script_count - 1];
            prev->duration = frame - prev->start;
        }

        ScriptEntry* e = &g_input_script[g_script_count++];
        e->anchor = SCRIPT_ANCHOR_FRAME;
        e->start = frame;
        e->duration = 0xFFFFFFFF; /* until next entry or forever */
        parse_buttons(btn_buf, &e->dpad, &e->buttons);
        prev_frame = frame;
    }
    fclose(f);
    printf("[SCRIPT] Loaded %d input events from %s\n", g_script_count, path);
}

#define MAX_DUMP_FRAMES 100
static uint32_t g_dump_frames[MAX_DUMP_FRAMES];
static int g_dump_count = 0;
static uint32_t g_dump_present_frames[MAX_DUMP_FRAMES];
static int g_dump_present_count = 0;
static char g_screenshot_prefix[64] = "screenshot";

/* Guest-CYCLE-based frame dump. Unlike g_dump_frames (keyed to rendered/presented
 * VBlank frames), this triggers on elapsed guest cycles, so it captures a frame
 * even when the ROM keeps the LCD off, enables it late, or halts after rendering
 * (e.g. Mealybug PPU test ROMs). A target "frame N" = N * 70224 T-cycles, matching
 * SameBoy's gb_fb_oracle GB_run_frame cadence. Checked every gb_run_cycles slice. */
#define GB_T_CYCLES_PER_FRAME 70224ull
static uint64_t g_dump_cycle_targets[MAX_DUMP_FRAMES];
static uint32_t g_dump_cycle_framenums[MAX_DUMP_FRAMES];
static bool     g_dump_cycle_done[MAX_DUMP_FRAMES];
static int      g_dump_cycle_count = 0;
static uint64_t g_dump_cycle_max = 0;

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
    return "Press Back, L3, or R3 for Menu";
#else
    return "Press ESC for Menu";
#endif
}

static float settings_ui_scale_for_size(const ImVec2& display_size) {
    float min_dimension = display_size.x < display_size.y ? display_size.x : display_size.y;
    if (min_dimension <= 0.0f) {
        return 1.0f;
    }

#if defined(__ANDROID__)
    float scale = min_dimension / 520.0f;
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 1.65f) scale = 1.65f;
#else
    float scale = min_dimension / 720.0f;
    if (scale < 0.95f) scale = 0.95f;
    if (scale > 1.25f) scale = 1.25f;
#endif

    return scale;
}

static const char* controller_menu_hint_text(void) {
#if defined(__ANDROID__)
    return "Guide, L3, or R3: Settings Menu";
#else
    return "Guide / Home: Settings Menu";
#endif
}

static bool input_action_is_runtime(GBInputAction action) {
    return action >= GB_JOYPAD_ACTION_COUNT && action < GB_INPUT_ACTION_COUNT;
}

static int effective_speed_percent(void) {
    int speed_percent = (g_speed_percent > 0) ? g_speed_percent : 100;
    if (g_max_speed_mode) {
        return GB_MAX_SHORTCUT_SPEED_PERCENT;
    }
    if (g_fast_forward_active && speed_percent < GB_FAST_FORWARD_SPEED_PERCENT) {
        return GB_FAST_FORWARD_SPEED_PERCENT;
    }
    return speed_percent;
}

static const char* overlay_visibility_hint_text(void) {
#if defined(__ANDROID__)
    return "Use the settings menu or a shortcut binding to toggle the overlay";
#else
    return "Use F1 or a shortcut binding for Overlay";
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

static const char* controller_button_label(SDL_GameControllerButton button) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A: return controller_face_south_label();
        case SDL_CONTROLLER_BUTTON_B: return controller_face_east_label();
        case SDL_CONTROLLER_BUTTON_X: return "West Face";
        case SDL_CONTROLLER_BUTTON_Y: return "North Face";
        case SDL_CONTROLLER_BUTTON_BACK: return controller_back_label();
        case SDL_CONTROLLER_BUTTON_GUIDE: return controller_guide_label();
        case SDL_CONTROLLER_BUTTON_START: return controller_start_label();
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return controller_left_shoulder_label();
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return controller_right_shoulder_label();
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-Pad Up";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-Pad Down";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-Pad Left";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-Pad Right";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "Left Stick Click";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "Right Stick Click";
        default:
            return "Controller Button";
    }
}

static const char* controller_axis_label(SDL_GameControllerAxis axis, bool positive_direction) {
    switch (axis) {
        case SDL_CONTROLLER_AXIS_LEFTX: return positive_direction ? "Left Stick Right" : "Left Stick Left";
        case SDL_CONTROLLER_AXIS_LEFTY: return positive_direction ? "Left Stick Down" : "Left Stick Up";
        case SDL_CONTROLLER_AXIS_RIGHTX: return positive_direction ? "Right Stick Right" : "Right Stick Left";
        case SDL_CONTROLLER_AXIS_RIGHTY: return positive_direction ? "Right Stick Down" : "Right Stick Up";
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return "Left Trigger";
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "Right Trigger";
        default:
            return positive_direction ? "Axis +" : "Axis -";
    }
}

static std::string binding_display_label(const GBInputBinding& binding) {
    switch (binding.kind) {
        case GB_INPUT_BINDING_KEY: {
            const char* name = SDL_GetScancodeName((SDL_Scancode)binding.code);
            return (name && name[0]) ? std::string(name) : ("Scancode " + std::to_string((int)binding.code));
        }

        case GB_INPUT_BINDING_CONTROLLER_BUTTON:
            return controller_button_label((SDL_GameControllerButton)binding.code);

        case GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE:
            return controller_axis_label((SDL_GameControllerAxis)binding.code, true);

        case GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE:
            return controller_axis_label((SDL_GameControllerAxis)binding.code, false);

        case GB_INPUT_BINDING_NONE:
        default:
            return "Unbound";
    }
}

static void start_binding_capture(GBBindingCaptureDevice device, GBInputAction action, int slot) {
    g_binding_capture_active = true;
    g_binding_capture_device = device;
    g_binding_capture_action = action;
    g_binding_capture_slot = slot;
}

static void cancel_binding_capture(void) {
    g_binding_capture_active = false;
    g_binding_capture_device = GB_CAPTURE_DEVICE_NONE;
    g_binding_capture_slot = 0;
}

static void assign_binding_slot(GBBindingCaptureDevice device,
                                GBInputAction action,
                                int slot,
                                const GBInputBinding& binding) {
    if (slot < 0 || slot >= 2) {
        return;
    }

    if (device == GB_CAPTURE_DEVICE_KEYBOARD) {
        g_keyboard_bindings[action][slot] = binding;
        g_keyboard_binding_pressed[action][slot] = false;
    } else if (device == GB_CAPTURE_DEVICE_CONTROLLER) {
        g_controller_bindings[action][slot] = binding;
        g_controller_button_binding_pressed[action][slot] = false;
        g_controller_axis_binding_pressed[action][slot] = false;
        update_controller_axis_binding_state();
    }

    update_effective_joypad_state();
    save_runtime_preferences();
}

static void commit_binding_capture(const GBInputBinding& binding) {
    if (!g_binding_capture_active || g_binding_capture_slot < 0 || g_binding_capture_slot >= 2) {
        cancel_binding_capture();
        return;
    }

    assign_binding_slot(g_binding_capture_device, g_binding_capture_action, g_binding_capture_slot, binding);
    cancel_binding_capture();
}

static void clear_controller_state(void) {
    if (g_controller) {
        SDL_GameControllerClose(g_controller);
        g_controller = NULL;
    }
    memset(g_controller_button_binding_pressed, 0, sizeof(g_controller_button_binding_pressed));
    memset(g_controller_axis_binding_pressed, 0, sizeof(g_controller_axis_binding_pressed));
    memset(g_controller_axis_values, 0, sizeof(g_controller_axis_values));
    g_controller_type = SDL_CONTROLLER_TYPE_UNKNOWN;
    g_controller_label_profile = detect_controller_label_profile(NULL);
    g_controller_name.clear();
    update_effective_joypad_state();
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

static GBInputBinding make_binding(GBInputBindingKind kind, int code) {
    GBInputBinding binding = {};
    binding.kind = kind;
    binding.code = (int16_t)code;
    return binding;
}

static const char* input_action_config_name(GBInputAction action) {
    switch (action) {
        case GB_INPUT_ACTION_RIGHT: return "right";
        case GB_INPUT_ACTION_LEFT: return "left";
        case GB_INPUT_ACTION_UP: return "up";
        case GB_INPUT_ACTION_DOWN: return "down";
        case GB_INPUT_ACTION_A: return "a";
        case GB_INPUT_ACTION_B: return "b";
        case GB_INPUT_ACTION_SELECT: return "select";
        case GB_INPUT_ACTION_START: return "start";
        case GB_INPUT_ACTION_FAST_FORWARD: return "fast_forward";
        case GB_INPUT_ACTION_TOGGLE_MAX_SPEED: return "toggle_max_speed";
        case GB_INPUT_ACTION_SAVE_STATE: return "save_state";
        case GB_INPUT_ACTION_LOAD_STATE: return "load_state";
        case GB_INPUT_ACTION_PREVIOUS_STATE_SLOT: return "previous_state_slot";
        case GB_INPUT_ACTION_NEXT_STATE_SLOT: return "next_state_slot";
        case GB_INPUT_ACTION_TOGGLE_OVERLAY: return "toggle_overlay";
        case GB_INPUT_ACTION_TOGGLE_MUTE: return "toggle_mute";
        case GB_INPUT_ACTION_TOGGLE_MENU: return "toggle_menu";
        case GB_INPUT_ACTION_COUNT:
        default:
            return "unknown";
    }
}

static bool parse_input_action_name(const char* text, GBInputAction* out_action) {
    if (!text || !out_action) {
        return false;
    }

    for (int action = 0; action < GB_INPUT_ACTION_COUNT; action++) {
        if (strcmp(text, input_action_config_name((GBInputAction)action)) == 0) {
            *out_action = (GBInputAction)action;
            return true;
        }
    }

    return false;
}

static void clear_all_binding_pressed_state(void) {
    memset(g_keyboard_binding_pressed, 0, sizeof(g_keyboard_binding_pressed));
    memset(g_controller_button_binding_pressed, 0, sizeof(g_controller_button_binding_pressed));
    memset(g_controller_axis_binding_pressed, 0, sizeof(g_controller_axis_binding_pressed));
    memset(g_runtime_action_pressed, 0, sizeof(g_runtime_action_pressed));
    memset(g_controller_axis_values, 0, sizeof(g_controller_axis_values));
    g_fast_forward_active = false;
}

static bool binding_is_valid(const GBInputBinding& binding) {
    switch (binding.kind) {
        case GB_INPUT_BINDING_KEY:
            return binding.code > SDL_SCANCODE_UNKNOWN && binding.code < SDL_NUM_SCANCODES;

        case GB_INPUT_BINDING_CONTROLLER_BUTTON:
            return binding.code >= 0 && binding.code < SDL_CONTROLLER_BUTTON_MAX;

        case GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE:
        case GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE:
            return binding.code >= 0 && binding.code < SDL_CONTROLLER_AXIS_MAX;

        case GB_INPUT_BINDING_NONE:
        default:
            return false;
    }
}

static bool binding_matches_scancode(const GBInputBinding& binding, SDL_Scancode scancode) {
    return binding.kind == GB_INPUT_BINDING_KEY && binding.code == (int)scancode;
}

static bool binding_matches_controller_button(const GBInputBinding& binding, uint8_t button) {
    return binding.kind == GB_INPUT_BINDING_CONTROLLER_BUTTON && binding.code == (int)button;
}

static bool binding_active_for_axis_value(const GBInputBinding& binding, Sint16 value) {
    const Sint16 threshold = 16000;
    if (binding.kind == GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE) {
        return value >= threshold;
    }
    if (binding.kind == GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE) {
        return value <= -threshold;
    }
    return false;
}

static void set_default_audio_preferences(void) {
    g_audio_output_enabled = true;
    g_audio_muted = false;
    g_audio_latency_ms = 80;
    g_audio_volume_percent = 100;
    g_audio_target_device_name.clear();
}

static void set_default_input_bindings(void) {
    memset(g_keyboard_bindings, 0, sizeof(g_keyboard_bindings));
    memset(g_controller_bindings, 0, sizeof(g_controller_bindings));

    g_keyboard_bindings[GB_INPUT_ACTION_UP][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_UP);
    g_keyboard_bindings[GB_INPUT_ACTION_UP][1] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_W);
    g_keyboard_bindings[GB_INPUT_ACTION_DOWN][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_DOWN);
    g_keyboard_bindings[GB_INPUT_ACTION_DOWN][1] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_S);
    g_keyboard_bindings[GB_INPUT_ACTION_LEFT][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_LEFT);
    g_keyboard_bindings[GB_INPUT_ACTION_LEFT][1] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_A);
    g_keyboard_bindings[GB_INPUT_ACTION_RIGHT][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_RIGHT);
    g_keyboard_bindings[GB_INPUT_ACTION_RIGHT][1] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_D);
    g_keyboard_bindings[GB_INPUT_ACTION_A][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_Z);
    g_keyboard_bindings[GB_INPUT_ACTION_A][1] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_J);
    g_keyboard_bindings[GB_INPUT_ACTION_B][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_X);
    g_keyboard_bindings[GB_INPUT_ACTION_B][1] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_K);
    g_keyboard_bindings[GB_INPUT_ACTION_SELECT][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_BACKSPACE);
    g_keyboard_bindings[GB_INPUT_ACTION_SELECT][1] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_RSHIFT);
    g_keyboard_bindings[GB_INPUT_ACTION_START][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_RETURN);
    g_keyboard_bindings[GB_INPUT_ACTION_FAST_FORWARD][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_TAB);
    g_keyboard_bindings[GB_INPUT_ACTION_TOGGLE_MAX_SPEED][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_GRAVE);
    g_keyboard_bindings[GB_INPUT_ACTION_SAVE_STATE][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F5);
    g_keyboard_bindings[GB_INPUT_ACTION_LOAD_STATE][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F8);
    g_keyboard_bindings[GB_INPUT_ACTION_PREVIOUS_STATE_SLOT][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F6);
    g_keyboard_bindings[GB_INPUT_ACTION_NEXT_STATE_SLOT][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F7);
    g_keyboard_bindings[GB_INPUT_ACTION_TOGGLE_OVERLAY][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F1);
    g_keyboard_bindings[GB_INPUT_ACTION_TOGGLE_MUTE][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_M);
    g_keyboard_bindings[GB_INPUT_ACTION_TOGGLE_MENU][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F10);

    g_controller_bindings[GB_INPUT_ACTION_UP][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_UP);
    g_controller_bindings[GB_INPUT_ACTION_UP][1] = make_binding(GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE, SDL_CONTROLLER_AXIS_LEFTY);
    g_controller_bindings[GB_INPUT_ACTION_DOWN][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    g_controller_bindings[GB_INPUT_ACTION_DOWN][1] = make_binding(GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE, SDL_CONTROLLER_AXIS_LEFTY);
    g_controller_bindings[GB_INPUT_ACTION_LEFT][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    g_controller_bindings[GB_INPUT_ACTION_LEFT][1] = make_binding(GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE, SDL_CONTROLLER_AXIS_LEFTX);
    g_controller_bindings[GB_INPUT_ACTION_RIGHT][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    g_controller_bindings[GB_INPUT_ACTION_RIGHT][1] = make_binding(GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE, SDL_CONTROLLER_AXIS_LEFTX);
    g_controller_bindings[GB_INPUT_ACTION_A][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_B);
    g_controller_bindings[GB_INPUT_ACTION_A][1] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    g_controller_bindings[GB_INPUT_ACTION_B][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_A);
    g_controller_bindings[GB_INPUT_ACTION_B][1] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    g_controller_bindings[GB_INPUT_ACTION_SELECT][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_BACK);
    g_controller_bindings[GB_INPUT_ACTION_START][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_START);
    g_controller_bindings[GB_INPUT_ACTION_FAST_FORWARD][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    g_controller_bindings[GB_INPUT_ACTION_TOGGLE_MAX_SPEED][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    g_controller_bindings[GB_INPUT_ACTION_SAVE_STATE][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_X);
    g_controller_bindings[GB_INPUT_ACTION_LOAD_STATE][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_Y);
    g_controller_bindings[GB_INPUT_ACTION_TOGGLE_MENU][0] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_LEFTSTICK);
    g_controller_bindings[GB_INPUT_ACTION_TOGGLE_MENU][1] = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, SDL_CONTROLLER_BUTTON_RIGHTSTICK);

    clear_all_binding_pressed_state();
}

static std::string runtime_preferences_path(void) {
    /* Co-locate runtime prefs with the binary (same place .sav / .rtc /
     * .stateN files live) so the whole game folder is portable. */
    char* base_path = SDL_GetBasePath();
    if (base_path) {
        fs::path resolved = fs::path(base_path) / "runtime_prefs.ini";
        SDL_free(base_path);
        return resolved.lexically_normal().string();
    }
    return fs::path("runtime_prefs.ini").lexically_normal().string();
}

static void load_runtime_preferences(void) {
    set_default_audio_preferences();
    set_default_input_bindings();
    g_savestate_slot = 0;
    g_savestate_status.clear();
    g_max_speed_mode = false;
    g_per_game_prefs.clear();

    const std::string path = runtime_preferences_path();
    if (!path.empty()) {
        FILE* file = fopen(path.c_str(), "r");
        if (file) {
            char line[256];
            while (fgets(line, sizeof(line), file)) {
                char* text = trim_ascii(line);
                if (!text || !text[0] || text[0] == '#') {
                    continue;
                }

                char* equals = strchr(text, '=');
                if (!equals) {
                    continue;
                }

                *equals = '\0';
                char* key = trim_ascii(text);
                char* value = trim_ascii(equals + 1);
                if (!key || !value || !key[0]) {
                    continue;
                }

                if (strcmp(key, "audio.enabled") == 0) {
                    g_audio_output_enabled = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "audio.muted") == 0) {
                    g_audio_muted = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "audio.latency_ms") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed > 0) {
                        g_audio_latency_ms = (uint32_t)parsed;
                    }
                    continue;
                }
                if (strcmp(key, "audio.volume_percent") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 0) {
                        if (parsed > 200) parsed = 200;
                        g_audio_volume_percent = (uint32_t)parsed;
                    }
                    continue;
                }
                if (strcmp(key, "audio.device_name") == 0) {
                    g_audio_target_device_name = value;
                    continue;
                }
                if (strcmp(key, "ui.theme") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 0 && parsed <= 2) {
                        g_imgui_theme = (int)parsed;
                    }
                    continue;
                }
                if (strcmp(key, "savestate.slot") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 0 && parsed < GB_SAVESTATE_SLOT_COUNT) {
                        g_savestate_slot = (int)parsed;
                    }
                    continue;
                }
                if (strcmp(key, "border.enabled") == 0) {
                    g_border_enabled = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "sgb.colors") == 0 ||
                    strcmp(key, "sgb.enabled") == 0 /* legacy name */) {
                    g_sgb_colors_pref = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "sgb.cart_border") == 0) {
                    g_sgb_cart_border_enabled = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "diag.show_fps") == 0) {
                    g_show_fps = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "shader.active") == 0) {
                    g_active_shader_pref = value;
                    continue;
                }
                if (strcmp(key, "lan.enabled") == 0) {
                    g_lan_enabled_pref = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "lan.uuid") == 0) {
                    g_lan_uuid_pref = value;
                    continue;
                }
                if (strcmp(key, "lan.nickname") == 0) {
                    g_lan_nickname_pref = value;
                    continue;
                }
                if (strcmp(key, "link.port") == 0) {
                    int parsed = atoi(value);
                    if (parsed > 0 && parsed < 65536) g_link_port_pref = parsed;
                    continue;
                }
                if (strcmp(key, "relay.url") == 0) {
                    snprintf(g_relay_url, sizeof(g_relay_url), "%s", value);
                    continue;
                }
                if (strcmp(key, "relay.code") == 0) {
                    snprintf(g_relay_code, sizeof(g_relay_code), "%s", value);
                    continue;
                }
                if (strcmp(key, "border.file") == 0) {
                    g_border_loaded_filename = value; /* index resolved later */
                    continue;
                }
                /* Per-game prefs: "game.<id>.<key>=<value>". The
                 * platform looks these up when gb_platform_set_game_id
                 * is called by the recompiled main. */
                if (strncmp(key, "game.", 5) == 0) {
                    const char* after = key + 5;
                    const char* dot = strchr(after, '.');
                    if (dot && dot > after && dot[1]) {
                        std::string game_id(after, (size_t)(dot - after));
                        std::string game_key(dot + 1);
                        g_per_game_prefs[game_id][game_key] = value;
                    }
                    continue;
                }

                bool is_keyboard = strncmp(key, "keyboard.", 9) == 0;
                bool is_controller = strncmp(key, "controller.", 11) == 0;
                if (!is_keyboard && !is_controller) {
                    continue;
                }

                char* section = key + (is_keyboard ? 9 : 11);
                char* dot = strrchr(section, '.');
                if (!dot) {
                    continue;
                }
                *dot = '\0';
                char* slot_text = dot + 1;
                long slot = strtol(slot_text, NULL, 10);
                if (slot < 0 || slot >= 2) {
                    continue;
                }

                GBInputAction action = GB_INPUT_ACTION_RIGHT;
                if (!parse_input_action_name(section, &action)) {
                    continue;
                }

                GBInputBinding binding = {};
                if (strcmp(value, "none") == 0) {
                    binding = make_binding(GB_INPUT_BINDING_NONE, 0);
                } else if (is_keyboard) {
                    if (strncmp(value, "key:", 4) != 0) {
                        continue;
                    }
                    long code = strtol(value + 4, NULL, 10);
                    binding = make_binding(GB_INPUT_BINDING_KEY, (int)code);
                } else if (strncmp(value, "button:", 7) == 0) {
                    long code = strtol(value + 7, NULL, 10);
                    binding = make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, (int)code);
                } else if (strncmp(value, "axis:", 5) == 0) {
                    char* value_copy = strdup(value + 5);
                    if (!value_copy) {
                        continue;
                    }
                    char* axis_text = strtok(value_copy, ":");
                    char* direction_text = strtok(NULL, ":");
                    if (axis_text && direction_text) {
                        long axis = strtol(axis_text, NULL, 10);
                        binding = make_binding(
                            (strcmp(direction_text, "+") == 0) ? GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE
                                                                 : GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE,
                            (int)axis);
                    }
                    free(value_copy);
                } else {
                    continue;
                }

                if (!binding_is_valid(binding) && binding.kind != GB_INPUT_BINDING_NONE) {
                    continue;
                }

                if (is_keyboard) {
                    g_keyboard_bindings[action][slot] = binding;
                } else {
                    g_controller_bindings[action][slot] = binding;
                }
            }
            fclose(file);
        }
    }
    update_effective_joypad_state();
}

static void binding_to_config_value(const GBInputBinding& binding, char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }

    switch (binding.kind) {
        case GB_INPUT_BINDING_KEY:
            snprintf(out, out_size, "key:%d", (int)binding.code);
            break;

        case GB_INPUT_BINDING_CONTROLLER_BUTTON:
            snprintf(out, out_size, "button:%d", (int)binding.code);
            break;

        case GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE:
            snprintf(out, out_size, "axis:%d:+", (int)binding.code);
            break;

        case GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE:
            snprintf(out, out_size, "axis:%d:-", (int)binding.code);
            break;

        case GB_INPUT_BINDING_NONE:
        default:
            snprintf(out, out_size, "none");
            break;
    }
}

static void save_runtime_preferences(void) {
    const std::string path = runtime_preferences_path();
    if (path.empty()) {
        return;
    }

    FILE* file = fopen(path.c_str(), "w");
    if (!file) {
        fprintf(stderr, "[SDL] Failed to save runtime prefs to %s\n", path.c_str());
        return;
    }

    fprintf(file, "audio.enabled=%d\n", g_audio_output_enabled ? 1 : 0);
    fprintf(file, "audio.muted=%d\n", g_audio_muted ? 1 : 0);
    fprintf(file, "audio.latency_ms=%u\n", g_audio_latency_ms);
    fprintf(file, "audio.volume_percent=%u\n", g_audio_volume_percent);
    fprintf(file, "audio.device_name=%s\n", g_audio_target_device_name.c_str());
    fprintf(file, "ui.theme=%d\n", g_imgui_theme);
    fprintf(file, "savestate.slot=%d\n", g_savestate_slot);
    fprintf(file, "border.enabled=%d\n", g_border_enabled ? 1 : 0);
    fprintf(file, "sgb.colors=%d\n", g_sgb_colors_pref ? 1 : 0);
    fprintf(file, "sgb.cart_border=%d\n", g_sgb_cart_border_enabled ? 1 : 0);
    fprintf(file, "diag.show_fps=%d\n", g_show_fps ? 1 : 0);
    if (!g_active_shader_pref.empty()) {
        fprintf(file, "shader.active=%s\n", g_active_shader_pref.c_str());
    }
    fprintf(file, "lan.enabled=%d\n", g_lan_enabled_pref ? 1 : 0);
    if (!g_lan_uuid_pref.empty())     fprintf(file, "lan.uuid=%s\n", g_lan_uuid_pref.c_str());
    if (!g_lan_nickname_pref.empty()) fprintf(file, "lan.nickname=%s\n", g_lan_nickname_pref.c_str());
    fprintf(file, "link.port=%d\n", g_link_port_pref);
    fprintf(file, "relay.url=%s\n", g_relay_url);
    fprintf(file, "relay.code=%s\n", g_relay_code);
    if (g_border_enabled && !g_border_files.empty() &&
        g_border_idx >= 0 && g_border_idx < (int)g_border_files.size()) {
        fprintf(file, "border.file=%s\n", g_border_files[g_border_idx].c_str());
    }

    /* Per-game prefs: "game.<id>.<key>=<value>". Stable order so the
     * file diffs cleanly across saves. */
    for (const auto& [game_id, prefs] : g_per_game_prefs) {
        for (const auto& [key, value] : prefs) {
            fprintf(file, "game.%s.%s=%s\n", game_id.c_str(), key.c_str(), value.c_str());
        }
    }

    for (int action = 0; action < GB_INPUT_ACTION_COUNT; action++) {
        for (int slot = 0; slot < 2; slot++) {
            char value[64];
            binding_to_config_value(g_keyboard_bindings[action][slot], value, sizeof(value));
            fprintf(file, "keyboard.%s.%d=%s\n", input_action_config_name((GBInputAction)action), slot, value);
        }
    }

    for (int action = 0; action < GB_INPUT_ACTION_COUNT; action++) {
        for (int slot = 0; slot < 2; slot++) {
            char value[64];
            binding_to_config_value(g_controller_bindings[action][slot], value, sizeof(value));
            fprintf(file, "controller.%s.%d=%s\n", input_action_config_name((GBInputAction)action), slot, value);
        }
    }

    fclose(file);
}

static void set_savestate_status(const char* action, int slot, bool success, const char* detail) {
    char message[512];
    snprintf(message,
             sizeof(message),
             "%s slot %d %s%s%s",
             action ? action : "Savestate",
             slot + 1,
             success ? "succeeded" : "failed",
             (detail && detail[0]) ? ": " : "",
             (detail && detail[0]) ? detail : "");
    g_savestate_status = message;
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

static bool input_action_is_pressed(GBInputAction action) {
    for (int slot = 0; slot < 2; slot++) {
        if (g_keyboard_binding_pressed[action][slot] ||
            g_controller_button_binding_pressed[action][slot] ||
            g_controller_axis_binding_pressed[action][slot]) {
            return true;
        }
    }
    return false;
}

static void update_runtime_action_state(GBContext* ctx) {
    const int previous_effective_speed = effective_speed_percent();

    for (int action = 0; action < GB_INPUT_ACTION_COUNT; action++) {
        if (!input_action_is_runtime((GBInputAction)action)) {
            continue;
        }

        const bool pressed = input_action_is_pressed((GBInputAction)action);
        const bool was_pressed = g_runtime_action_pressed[action];
        if (pressed && !was_pressed) {
            switch ((GBInputAction)action) {
                case GB_INPUT_ACTION_TOGGLE_MAX_SPEED:
                    g_max_speed_mode = !g_max_speed_mode;
                    break;

                case GB_INPUT_ACTION_SAVE_STATE:
                    save_savestate_slot(ctx, g_savestate_slot);
                    break;

                case GB_INPUT_ACTION_LOAD_STATE:
                    load_savestate_slot(ctx, g_savestate_slot);
                    break;

                case GB_INPUT_ACTION_PREVIOUS_STATE_SLOT:
                    g_savestate_slot = (g_savestate_slot + GB_SAVESTATE_SLOT_COUNT - 1) % GB_SAVESTATE_SLOT_COUNT;
                    g_savestate_status = "Selected slot " + std::to_string(g_savestate_slot + 1);
                    save_runtime_preferences();
                    break;

                case GB_INPUT_ACTION_NEXT_STATE_SLOT:
                    g_savestate_slot = (g_savestate_slot + 1) % GB_SAVESTATE_SLOT_COUNT;
                    g_savestate_status = "Selected slot " + std::to_string(g_savestate_slot + 1);
                    save_runtime_preferences();
                    break;

                case GB_INPUT_ACTION_TOGGLE_OVERLAY:
                    g_show_overlay = !g_show_overlay;
                    break;

                case GB_INPUT_ACTION_TOGGLE_MUTE:
                    g_audio_muted = !g_audio_muted;
                    save_runtime_preferences();
                    break;

                case GB_INPUT_ACTION_TOGGLE_MENU:
                    g_show_menu = !g_show_menu;
                    break;

                case GB_INPUT_ACTION_FAST_FORWARD:
                case GB_INPUT_ACTION_RIGHT:
                case GB_INPUT_ACTION_LEFT:
                case GB_INPUT_ACTION_UP:
                case GB_INPUT_ACTION_DOWN:
                case GB_INPUT_ACTION_A:
                case GB_INPUT_ACTION_B:
                case GB_INPUT_ACTION_SELECT:
                case GB_INPUT_ACTION_START:
                case GB_INPUT_ACTION_COUNT:
                default:
                    break;
            }
        }

        g_runtime_action_pressed[action] = pressed;
    }

    g_fast_forward_active = g_runtime_action_pressed[GB_INPUT_ACTION_FAST_FORWARD];

    if (effective_speed_percent() != previous_effective_speed) {
        reset_audio_output_buffer(true);
    }
}

static void rebuild_manual_joypad_state_from_bindings(void) {
    g_manual_joypad_dpad = 0xFF;
    g_manual_joypad_buttons = 0xFF;

    if (input_action_is_pressed(GB_INPUT_ACTION_RIGHT)) g_manual_joypad_dpad &= (uint8_t)~0x01;
    if (input_action_is_pressed(GB_INPUT_ACTION_LEFT)) g_manual_joypad_dpad &= (uint8_t)~0x02;
    if (input_action_is_pressed(GB_INPUT_ACTION_UP)) g_manual_joypad_dpad &= (uint8_t)~0x04;
    if (input_action_is_pressed(GB_INPUT_ACTION_DOWN)) g_manual_joypad_dpad &= (uint8_t)~0x08;
    if (input_action_is_pressed(GB_INPUT_ACTION_A)) g_manual_joypad_buttons &= (uint8_t)~0x01;
    if (input_action_is_pressed(GB_INPUT_ACTION_B)) g_manual_joypad_buttons &= (uint8_t)~0x02;
    if (input_action_is_pressed(GB_INPUT_ACTION_SELECT)) g_manual_joypad_buttons &= (uint8_t)~0x04;
    if (input_action_is_pressed(GB_INPUT_ACTION_START)) g_manual_joypad_buttons &= (uint8_t)~0x08;
}

static void update_effective_joypad_state(void) {
    rebuild_manual_joypad_state_from_bindings();
    /* While the in-game menu is up, hide all joypad activity from the
     * cart. The game keeps running (so NPCs walk, music plays, RTC
     * advances), but arrow keys / A / Start used to navigate the
     * menu don't bleed through and move the player or open the cart's
     * own menu. Active-low: 0xFF = nothing pressed. */
    if (g_show_menu) {
        g_joypad_dpad = 0xFF;
        g_joypad_buttons = 0xFF;
        return;
    }
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

void gb_platform_set_dump_cycle_frames(const char* frames) {
    g_dump_cycle_count = 0;
    g_dump_cycle_max = 0;
    if (!frames) return;
    char* copy = strdup(frames);
    char* token = strtok(copy, ",");
    while (token && g_dump_cycle_count < MAX_DUMP_FRAMES) {
        uint32_t fn = (uint32_t)strtoul(token, NULL, 10);
        uint64_t target = (uint64_t)fn * GB_T_CYCLES_PER_FRAME;
        g_dump_cycle_framenums[g_dump_cycle_count] = fn;
        g_dump_cycle_targets[g_dump_cycle_count] = target;
        g_dump_cycle_done[g_dump_cycle_count] = false;
        if (target > g_dump_cycle_max) g_dump_cycle_max = target;
        g_dump_cycle_count++;
        token = strtok(NULL, ",");
    }
    free(copy);
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

/* Capture the current framebuffer when guest time crosses each requested target.
 * Called every gb_run_cycles slice from the main loop, so it fires regardless of
 * LCD/render/halt state. In benchmark (headless) mode, exits once all targets are
 * captured so the harness terminates without relying on rendered-frame counts. */
void gb_platform_check_cycle_dump(GBContext* ctx) {
    if (g_dump_cycle_count == 0 || !ctx) return;
    uint64_t cyc = (uint64_t)ctx->cycles;
    for (int i = 0; i < g_dump_cycle_count; i++) {
        if (!g_dump_cycle_done[i] && cyc >= g_dump_cycle_targets[i]) {
            const uint32_t* fb = gb_get_framebuffer(ctx);
            if (fb) {
                char filename[160];
                snprintf(filename, sizeof(filename), "%s_%05u.ppm",
                         g_screenshot_prefix, g_dump_cycle_framenums[i]);
                save_ppm(filename, fb, gb_ws_render_width(), GB_SCREEN_HEIGHT,
                         (int)g_dump_cycle_framenums[i]);
            }
            g_dump_cycle_done[i] = true;
        }
    }
    if (g_dump_cycle_max > 0 && cyc >= g_dump_cycle_max && g_benchmark_mode) {
        bool all = true;
        for (int i = 0; i < g_dump_cycle_count; i++) if (!g_dump_cycle_done[i]) all = false;
        if (all) { fflush(stdout); exit(0); }
    }
}


/* Guest-FPS sampler. We snapshot (g_frame_count, wall_clock_ms) once a
 * second and compute FPS as the delta against the previous snapshot.
 * That gives the rate the recompiled GB CPU is actually producing
 * frames at — which under fast-forward / max-speed will exceed 60 even
 * though the present rate stays pinned to the display. */
static double g_guest_fps = 0.0;
static int    g_guest_fps_last_count = 0;
static uint32_t g_guest_fps_last_ms = 0;

static void update_guest_fps(void) {
    uint32_t now = SDL_GetTicks();
    if (g_guest_fps_last_ms == 0) {
        g_guest_fps_last_ms = now;
        g_guest_fps_last_count = g_frame_count;
        return;
    }
    uint32_t elapsed = now - g_guest_fps_last_ms;
    if (elapsed >= 500) {  /* update twice a second */
        int frames = g_frame_count - g_guest_fps_last_count;
        g_guest_fps = (double)frames * 1000.0 / (double)elapsed;
        g_guest_fps_last_ms = now;
        g_guest_fps_last_count = g_frame_count;
    }
}

static double sdl_now_ms(void) {
    uint64_t ticks = SDL_GetPerformanceCounter();
    uint64_t freq = SDL_GetPerformanceFrequency();
    return freq ? ((double)ticks * 1000.0) / (double)freq : 0.0;
}

static int round_to_int(double value) {
    return (int)(value + 0.5);
}

/* ============================================================================
 * Custom SGB borders (optional, opt-in via menu)
 * ========================================================================== */

static bool sgb_cart_border_active(void) {
    if (!g_sgb_cart_border_enabled) return false;
    if (!g_sgb_cart_border_texture) return false;
    /* Border render is gated on its own display flag — independent of
     * the SGB Colors toggle. The engine keeps capturing CHR_TRN/
     * PCT_TRN updates regardless, so toggling back on shows the latest
     * border without a restart. */
    if (g_registered_ctx) {
        GBSgbState* sgb = (GBSgbState*)g_registered_ctx->sgb;
        if (!gb_sgb_is_display_border(sgb)) return false;
    }
    return true;
}

static int border_content_width(void) {
    if ((g_border_enabled && g_border_texture) || sgb_cart_border_active()) {
        return GB_BORDER_FULL_W;
    }
    return gb_ws_render_width();   /* 160 unless the extended view is armed */
}

static int border_content_height(void) {
    if ((g_border_enabled && g_border_texture) || sgb_cart_border_active()) {
        return GB_BORDER_FULL_H;
    }
    return GB_SCREEN_HEIGHT;
}

static void unload_border_texture(void) {
    if (g_border_texture) {
        glDeleteTextures(1, &g_border_texture);
        g_border_texture = 0;
    }
    g_border_loaded_filename.clear();
}

/* Forward declarations: apply_window_scale_preset rescales the window
 * when the border layout flips between 160x144 and 256x224.
 * unload_sgb_cart_border_texture lives just below refresh_*.
 * save_sgb_cart_border_cache_if_new lives below too — needs to be
 * callable from refresh_*. */
static void apply_window_scale_preset(void);
static void unload_sgb_cart_border_texture(void);
static void save_sgb_cart_border_cache_if_new(void);

/* Rebuild g_sgb_cart_border_texture from the current SGB engine state if
 * the cart has just shipped fresh CHR_TRN/PCT_TRN data. Cheap when the
 * revision counter hasn't changed, so it's safe to call every frame. */
static void refresh_sgb_cart_border_texture(void) {
    if (!g_gl_context) return;
    if (!g_registered_ctx) {
        unload_sgb_cart_border_texture();
        return;
    }
    GBSgbState* sgb = (GBSgbState*)g_registered_ctx->sgb;
    if (!gb_sgb_border_ready(sgb)) {
        /* Engine hasn't received CHR_TRN/PCT_TRN this session. If we
         * loaded a cached border earlier, leave it in place — it's
         * the only thing the user has in non-SGB hardware modes. */
        if (g_sgb_cart_border_texture && !g_sgb_cart_border_from_cache) {
            unload_sgb_cart_border_texture();
            apply_window_scale_preset();
        }
        return;
    }
    uint32_t rev = gb_sgb_border_revision(sgb);
    if (rev == g_sgb_cart_border_revision && g_sgb_cart_border_texture) return;

    if (g_sgb_cart_border_pixels.size() != (size_t)(GB_SGB_BORDER_W * GB_SGB_BORDER_H)) {
        g_sgb_cart_border_pixels.assign((size_t)GB_SGB_BORDER_W * GB_SGB_BORDER_H, 0);
    } else {
        std::fill(g_sgb_cart_border_pixels.begin(), g_sgb_cart_border_pixels.end(), 0u);
    }
    if (!gb_sgb_render_border(sgb, g_sgb_cart_border_pixels.data())) return;

    bool first_creation = false;
    if (!g_sgb_cart_border_texture) {
        glGenTextures(1, &g_sgb_cart_border_texture);
        glBindTexture(GL_TEXTURE_2D, g_sgb_cart_border_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     GB_SGB_BORDER_W, GB_SGB_BORDER_H, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        first_creation = true;
    } else {
        glBindTexture(GL_TEXTURE_2D, g_sgb_cart_border_texture);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    GB_SGB_BORDER_W, GB_SGB_BORDER_H,
                    GL_RGBA, GL_UNSIGNED_BYTE,
                    g_sgb_cart_border_pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    g_sgb_cart_border_revision = rev;
    g_sgb_cart_border_from_cache = false;
    /* First successful render this session — persist it so a later
     * launch in CGB / DMG hardware mode (which silences the SGB
     * engine) still has a border to show. */
    save_sgb_cart_border_cache_if_new();

    if (first_creation && g_sgb_cart_border_enabled) {
        apply_window_scale_preset();
    }
}

/* Path to the cross-session cached SGB border for the active game.
 * Empty string when no game is loaded yet. */
static std::string sgb_cart_border_cache_path(void) {
    if (g_active_game_id.empty()) return "";
    return (fs::current_path() / "sgb_borders" /
            (g_active_game_id + ".png")).string();
}

/* Try to populate g_sgb_cart_border_texture from disk. Used when the
 * SGB engine isn't running (CGB / DMG hardware mode) so the user can
 * still see the cart border captured during a previous SGB-mode run.
 * No-op if the file doesn't exist or doesn't decode to 256x224. */
static void try_load_sgb_cart_border_cache(void) {
    g_sgb_cart_border_from_cache = false;
    g_sgb_cart_border_cache_saved = false;
    std::string path = sgb_cart_border_cache_path();
    if (path.empty() || !fs::exists(path)) return;

    int w = 0, h = 0, ch = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!pixels) return;
    if (w != GB_SGB_BORDER_W || h != GB_SGB_BORDER_H) {
        stbi_image_free(pixels);
        return;
    }

    if (g_gl_context) {
        if (!g_sgb_cart_border_texture) {
            glGenTextures(1, &g_sgb_cart_border_texture);
            glBindTexture(GL_TEXTURE_2D, g_sgb_cart_border_texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                         GB_SGB_BORDER_W, GB_SGB_BORDER_H, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            glBindTexture(GL_TEXTURE_2D, g_sgb_cart_border_texture);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        GB_SGB_BORDER_W, GB_SGB_BORDER_H,
                        GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glBindTexture(GL_TEXTURE_2D, 0);
        g_sgb_cart_border_from_cache = true;
        g_sgb_cart_border_cache_saved = true;
        fprintf(stderr, "[SGB] Loaded cart border cache: %s\n", path.c_str());
    }
    stbi_image_free(pixels);
}

/* Persist the freshly-decoded RGBA buffer to disk. Called once per
 * session as soon as the engine produces a complete border, so a
 * later switch to CGB / DMG hardware mode can still display it. */
static void save_sgb_cart_border_cache_if_new(void) {
    /* Carts ship more than one border state — LADX, for example, sets up
     * an intro/logo border before transitioning to the in-game border.
     * Saving only the first revision permanently caches that intermediate
     * state, so the user sees a partially-decoded border forever after.
     * Overwrite on every new revision instead — the latest one wins. */
    if (g_active_game_id.empty()) return;
    if (g_sgb_cart_border_pixels.size() !=
        (size_t)(GB_SGB_BORDER_W * GB_SGB_BORDER_H)) return;

    std::string path = sgb_cart_border_cache_path();
    if (path.empty()) return;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (stbi_write_png(path.c_str(),
                       GB_SGB_BORDER_W, GB_SGB_BORDER_H, 4,
                       g_sgb_cart_border_pixels.data(),
                       GB_SGB_BORDER_W * 4)) {
        g_sgb_cart_border_cache_saved = true;
        fprintf(stderr, "[SGB] Saved cart border cache (rev %u): %s\n",
                (unsigned)g_sgb_cart_border_revision, path.c_str());
    }
}

static void unload_sgb_cart_border_texture(void) {
    if (g_sgb_cart_border_texture) {
        glDeleteTextures(1, &g_sgb_cart_border_texture);
        g_sgb_cart_border_texture = 0;
    }
    g_sgb_cart_border_revision = 0;
    g_sgb_cart_border_from_cache = false;
    g_sgb_cart_border_pixels.clear();
}

/* Load the PNG `<g_border_dir>/<filename>` into g_border_texture. Returns true
 * on success. Frees the previous texture either way. The PNG must be
 * 256x224 RGB(A) — anything else is rejected. */
static bool load_border_texture(const std::string& filename) {
    unload_border_texture();
    if (!g_gl_context || filename.empty() || g_border_dir.empty()) {
        return false;
    }
    fs::path full = fs::path(g_border_dir) / filename;
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load(full.string().c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        fprintf(stderr, "[BORDER] stbi_load failed for %s: %s\n",
                full.string().c_str(), stbi_failure_reason());
        return false;
    }
    if (w != GB_BORDER_FULL_W || h != GB_BORDER_FULL_H) {
        fprintf(stderr, "[BORDER] %s is %dx%d, expected %dx%d - skipping\n",
                full.string().c_str(), w, h, GB_BORDER_FULL_W, GB_BORDER_FULL_H);
        stbi_image_free(pixels);
        return false;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);
    g_border_texture = tex;
    g_border_loaded_filename = filename;
    return true;
}

/* Find a borders/ directory next to the cwd or next to the executable, scan
 * it for .png files, and populate g_border_files. Idempotent — call again to
 * pick up newly-added PNGs. */
static void scan_border_directory(void) {
    g_border_files.clear();
    g_border_dir.clear();

    std::vector<fs::path> candidates;
    candidates.push_back(fs::current_path() / GB_BORDER_DIR_NAME);
    char* base_path = SDL_GetBasePath();
    if (base_path) {
        candidates.push_back(fs::path(base_path) / GB_BORDER_DIR_NAME);
        SDL_free(base_path);
    }

    for (const fs::path& candidate : candidates) {
        std::error_code ec;
        if (!fs::is_directory(candidate, ec)) continue;
        for (const auto& entry : fs::directory_iterator(candidate, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext != ".png") continue;
            g_border_files.push_back(entry.path().filename().string());
        }
        if (!g_border_files.empty()) {
            std::sort(g_border_files.begin(), g_border_files.end());
            g_border_dir = candidate.string();
            fprintf(stderr, "[BORDER] Found %zu border(s) in %s\n",
                    g_border_files.size(), g_border_dir.c_str());
            return;
        }
    }

    fprintf(stderr, "[BORDER] No borders/ directory found (looked in cwd and exe dir)\n");
}

static void apply_border_change(void); /* forward */

static void update_render_filter(void) {
    /* Filtering is now decided per shader at draw time
     * (gb_shader_pipeline_draw chooses NEAREST or LINEAR based on the
     * active shader). The legacy SDL_Renderer scale-mode toggle is
     * gone, so this is a no-op kept around for the existing call sites. */
}

static void update_game_viewport(void) {
    const int content_w = border_content_width();
    const int content_h = border_content_height();

    if (!g_window) {
        g_game_viewport.x = 0;
        g_game_viewport.y = 0;
        g_game_viewport.w = content_w * g_scale;
        g_game_viewport.h = content_h * g_scale;
        return;
    }

    int window_w = 0;
    int window_h = 0;
    SDL_GetWindowSize(g_window, &window_w, &window_h);
    if (window_w <= 0) window_w = content_w;
    if (window_h <= 0) window_h = content_h;

    int viewport_w = window_w;
    int viewport_h = window_h;

    switch (g_render_scaling_mode) {
        case GB_RENDER_SCALING_PIXEL_PERFECT: {
            int scale_x = window_w / content_w;
            int scale_y = window_h / content_h;
            int integer_scale = (scale_x < scale_y) ? scale_x : scale_y;
            if (integer_scale < 1) {
                integer_scale = 1;
            }
            viewport_w = content_w * integer_scale;
            viewport_h = content_h * integer_scale;
            break;
        }
        case GB_RENDER_SCALING_ASPECT_FIT: {
            double scale_x = (double)window_w / (double)content_w;
            double scale_y = (double)window_h / (double)content_h;
            double scale = (scale_x < scale_y) ? scale_x : scale_y;
            if (scale <= 0.0) {
                scale = 1.0;
            }
            viewport_w = round_to_int((double)content_w * scale);
            viewport_h = round_to_int((double)content_h * scale);
            break;
        }
        case GB_RENDER_SCALING_ASPECT_FILL: {
            double scale_x = (double)window_w / (double)content_w;
            double scale_y = (double)window_h / (double)content_h;
            double scale = (scale_x > scale_y) ? scale_x : scale_y;
            if (scale <= 0.0) {
                scale = 1.0;
            }
            viewport_w = round_to_int((double)content_w * scale);
            viewport_h = round_to_int((double)content_h * scale);
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
    g_windowed_width = border_content_width() * g_scale;
    g_windowed_height = border_content_height() * g_scale;

    if (g_window && !g_fullscreen) {
        SDL_SetWindowSize(g_window, g_windowed_width, g_windowed_height);
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    update_game_viewport();
}

/* Reload texture for the currently-selected border and resize the window so
 * the border (or lack of one) fits at the chosen integer scale. Called when
 * the user toggles the master switch or cycles to a different file. */
static void apply_border_change(void) {
    if (g_border_enabled && !g_border_files.empty()) {
        /* If a filename was pulled from saved prefs and we haven't picked an
         * index yet, look it up in the scanned list. */
        if (!g_border_loaded_filename.empty() && g_border_texture == NULL) {
            for (size_t i = 0; i < g_border_files.size(); i++) {
                if (g_border_files[i] == g_border_loaded_filename) {
                    g_border_idx = (int)i;
                    break;
                }
            }
            g_border_loaded_filename.clear();
        }
        if (g_border_idx < 0 || g_border_idx >= (int)g_border_files.size()) {
            g_border_idx = 0;
        }
        const std::string& filename = g_border_files[g_border_idx];
        if (filename != g_border_loaded_filename) {
            load_border_texture(filename);
        }
    } else {
        unload_border_texture();
    }
    apply_window_scale_preset();
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
    if (!g_gl_context || g_benchmark_mode) {
        return true;
    }

    if (g_game_tex) {
        glDeleteTextures(1, &g_game_tex);
        g_game_tex = 0;
    }

    glGenTextures(1, &g_game_tex);
    glBindTexture(GL_TEXTURE_2D, g_game_tex);
    /* RGBA8 storage — the GB framebuffer is uint32 ARGB; we re-pack to
     * RGBA on upload. Filter and wrap are reset every shader draw, but
     * set sane defaults so even pre-shader code paths see a usable
     * texture. */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 gb_ws_render_width(), GB_SCREEN_HEIGHT, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    g_renderer_reset_pending = false;
    return true;
}

static void set_app_suspended(bool suspended) {
    if (g_app_suspended == suspended) {
        return;
    }

    g_app_suspended = suspended;
    refresh_audio_device_pause_state();

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
    g_max_speed_mode = false;
    g_render_scaling_mode = GB_RENDER_SCALING_PIXEL_PERFECT;
    g_render_filter_mode = GB_RENDER_FILTER_NEAREST;
    update_render_filter();

    if (g_gl_context) {
        SDL_GL_SetSwapInterval(0);
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

    reset_audio_output_buffer(true);
}

static void reset_runtime_audio_defaults(void) {
    set_default_audio_preferences();
    refresh_audio_output_devices();
    reopen_audio_output_device(true);
    save_runtime_preferences();
}

static void reset_runtime_control_defaults(void) {
    set_default_input_bindings();
    update_effective_joypad_state();
    save_runtime_preferences();
}

static void update_controller_axis_binding_state(void) {
    for (int action = 0; action < GB_INPUT_ACTION_COUNT; action++) {
        for (int slot = 0; slot < 2; slot++) {
            const GBInputBinding& binding = g_controller_bindings[action][slot];
            if (binding.kind == GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE ||
                binding.kind == GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE) {
                Sint16 value = 0;
                if (binding.code >= 0 && binding.code < SDL_CONTROLLER_AXIS_MAX) {
                    value = g_controller_axis_values[binding.code];
                }
                g_controller_axis_binding_pressed[action][slot] = binding_active_for_axis_value(binding, value);
            } else {
                g_controller_axis_binding_pressed[action][slot] = false;
            }
        }
    }
}

static bool input_transition_creates_press(uint8_t previous_dpad,
                                           uint8_t previous_buttons,
                                           uint8_t next_dpad,
                                           uint8_t next_buttons,
                                           bool dpad_selected,
                                           bool buttons_selected) {
    if (dpad_selected && ((previous_dpad & ~next_dpad) != 0)) {
        return true;
    }
    if (buttons_selected && ((previous_buttons & ~next_buttons) != 0)) {
        return true;
    }
    return false;
}

static void render_binding_editor(const char* section_label,
                                  GBBindingCaptureDevice device,
                                  GBInputBinding bindings[GB_INPUT_ACTION_COUNT][2],
                                  GBInputAction action_begin,
                                  GBInputAction action_end) {
    ImGui::TextDisabled("%s", section_label);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float content_width = ImGui::GetContentRegionAvail().x;
    const bool compact_layout = content_width < 720.0f;

    for (int action = (int)action_begin; action < (int)action_end; action++) {
        ImGui::PushID(section_label);
        ImGui::PushID(action);
        ImGui::Text("%s", g_input_action_names[action]);

        const float clear_width = ImGui::CalcTextSize("Clear").x + style.FramePadding.x * 2.0f + 12.0f;
        if (!compact_layout) {
            const float action_column_width = content_width * 0.26f;
            float binding_width =
                (content_width - action_column_width - clear_width * 2.0f - style.ItemSpacing.x * 4.0f) / 2.0f;
            if (binding_width < 120.0f) {
                binding_width = 120.0f;
            }
            ImGui::SameLine(action_column_width);

            for (int slot = 0; slot < 2; slot++) {
                std::string label;
                if (g_binding_capture_active &&
                    g_binding_capture_device == device &&
                    g_binding_capture_action == (GBInputAction)action &&
                    g_binding_capture_slot == slot) {
                    label = "Press Input...";
                } else {
                    label = binding_display_label(bindings[action][slot]);
                }

                ImGui::PushID(slot);
                if (ImGui::Button(label.c_str(), ImVec2(binding_width, 0.0f))) {
                    start_binding_capture(device, (GBInputAction)action, slot);
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear", ImVec2(clear_width, 0.0f))) {
                    assign_binding_slot(device, (GBInputAction)action, slot, make_binding(GB_INPUT_BINDING_NONE, 0));
                }
                ImGui::PopID();
                if (slot == 0) {
                    ImGui::SameLine();
                }
            }
        } else {
            float compact_button_width = content_width - clear_width - style.ItemSpacing.x;
            if (compact_button_width < 120.0f) {
                compact_button_width = 120.0f;
            }
            for (int slot = 0; slot < 2; slot++) {
                std::string label;
                if (g_binding_capture_active &&
                    g_binding_capture_device == device &&
                    g_binding_capture_action == (GBInputAction)action &&
                    g_binding_capture_slot == slot) {
                    label = "Press Input...";
                } else {
                    label = binding_display_label(bindings[action][slot]);
                }

                ImGui::PushID(slot);
                if (ImGui::Button(label.c_str(), ImVec2(compact_button_width, 0.0f))) {
                    start_binding_capture(device, (GBInputAction)action, slot);
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear", ImVec2(clear_width, 0.0f))) {
                    assign_binding_slot(device, (GBInputAction)action, slot, make_binding(GB_INPUT_BINDING_NONE, 0));
                }
                ImGui::PopID();
            }
        }
        ImGui::PopID();
        ImGui::PopID();
    }
}

static void ensure_lcd_off_framebuffer(void) {
    if (g_lcd_off_framebuffer_initialized) {
        return;
    }

    for (int i = 0; i < GB_MAX_FRAMEBUFFER_SIZE; i++) {
        g_lcd_off_framebuffer[i] = 0xFFE0F8D0;
    }

    g_lcd_off_framebuffer_initialized = true;
}

static void render_frame_internal(const uint32_t* framebuffer, bool count_guest_frame) {
    if (!framebuffer) {
        DBG_FRAME("Platform render_frame: SKIPPED (null: tex=%d, gl=%d, fb=%d)",
                  g_game_tex == 0, g_gl_context == NULL, framebuffer == NULL);
        return;
    }
    g_present_count++;
    if (count_guest_frame) {
        g_frame_count++;
        update_guest_fps();
        memcpy(g_last_guest_framebuffer, framebuffer, sizeof(g_last_guest_framebuffer));
        g_last_guest_framebuffer_valid = true;
    }

    if (count_guest_frame) {
        /* Handle Screenshot Dumping */
        if (frame_is_selected_for_dump(g_dump_frames, g_dump_count, (uint32_t)g_frame_count)) {
            char filename[128];
            snprintf(filename, sizeof(filename), "%s_%05d.ppm", g_screenshot_prefix, g_frame_count);
            save_ppm(filename, framebuffer, gb_ws_render_width(), GB_SCREEN_HEIGHT, g_frame_count);
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
        save_ppm(filename, framebuffer, gb_ws_render_width(), GB_SCREEN_HEIGHT, g_frame_count);
    }

    if (g_benchmark_mode || g_app_suspended || !g_gl_context) {
        DBG_FRAME("Platform render_frame: host render skipped (benchmark=%d gl=%d)",
                  g_benchmark_mode ? 1 : 0,
                  g_gl_context == NULL);
        g_last_timing.upload_ms = 0.0;
        g_last_timing.compose_ms = 0.0;
        g_last_timing.present_ms = 0.0;
        g_last_timing.total_render_ms = 0.0;
        return;
    }

    if ((!g_game_tex || g_renderer_reset_pending) && !recreate_streaming_texture()) {
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
        for (int i = 0; i < gb_ws_render_width() * GB_SCREEN_HEIGHT; i++) {
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

    /* Upload framebuffer to the GL game texture. The PPU stores pixels
     * as 0xAARRGGBB (CPU-side ARGB), which on a little-endian host is
     * BGRA in memory. GL ES 2.0 doesn't reliably expose GL_BGRA, so
     * shuffle into RGBA byte order on the way in. The optional palette
     * override (Original / Pocket / Plasma) is applied during the same
     * pass since we're touching every pixel anyway. */
    double upload_start_ms = sdl_now_ms();
    static uint32_t s_upload_buf[GB_MAX_FRAMEBUFFER_SIZE];
    {
        const uint32_t* src = framebuffer;
        /* Present-time screen-color LUT (opt-in via GBCRECOMP_SCREEN; default
         * raw => NULL => passthrough, byte-identical). Map the ARGB framebuffer
         * into a scratch copy that feeds the RGBA repack below; never mutates
         * the PPU framebuffer or the verify path. */
        if (!g_color_lut_resolved) {
            g_color_lut = color_lut_create_from_env();
            g_color_lut_resolved = true;
        }
        if (g_color_lut && !color_lut_is_passthrough(g_color_lut)) {
            static uint32_t s_lut_buf[GB_MAX_FRAMEBUFFER_SIZE];
            color_lut_map_argb8888(g_color_lut, framebuffer, s_lut_buf,
                                   gb_ws_render_width(), GB_SCREEN_HEIGHT);
            src = s_lut_buf;
        }
        /* Match the actual post-conversion DMG-green values produced by
         * ppu.c's rgb555_to_rgba (which uses *255/31 with integer
         * truncation), not the dmg_palette_rgba constants — those are
         * only used directly on the very first reset frame, every other
         * frame goes through the rgb555 round-trip and lands a few LSBs
         * away. Comparing against the wrong constants made the palette
         * dropdown a silent no-op. */
        const uint32_t orig[4] = { 0xFFE6F6CDu, 0xFF73BD62u, 0xFF319C52u, 0xFF081010u };
        const uint32_t* pal = (g_palette_idx == 0) ? NULL : g_palettes[g_palette_idx];

        /* Color correction matrices applied per-pixel.
         *
         *   GBC mode: viewing original-CGB-cartridge output on a modern
         *   sRGB display. Real CGB LCDs had crushed saturation; the cart
         *   art was tuned for that. Modern displays show those values
         *   too vibrantly, so we desaturate slightly.
         *
         *   GBA mode: the AGB LCD was darker and bluer than the CGB
         *   LCD. Games shipped for GBC look oversaturated on a real
         *   GBA. This matrix gives the on-GBA look back. From mGBA's
         *   GBA color correction; gamma is approximated as a linear
         *   scale to keep this in integer math.
         *
         * Both matrices are normalized so the [0..255] output range
         * stays in bounds. Coefficients * 1024 to fit uint16 multiply. */
        static const int16_t cc_gbc[9] = {
            /*  R from R,G,B   G from R,G,B   B from R,G,B */
              893, 123,   8,    102,  840,  82,    102,  133,  789,
        };
        static const int16_t cc_gba[9] = {
              860, 169,  -5,     92,  676, 256,     92,  261,  671,
        };
        const int16_t* cc = (g_color_correction == 1) ? cc_gbc :
                            (g_color_correction == 2) ? cc_gba : NULL;

        for (int i = 0; i < gb_ws_render_width() * GB_SCREEN_HEIGHT; i++) {
            uint32_t c = src[i];
            if (pal) {
                if      (c == orig[0]) c = pal[0];
                else if (c == orig[1]) c = pal[1];
                else if (c == orig[2]) c = pal[2];
                else if (c == orig[3]) c = pal[3];
            }
            uint32_t a = c & 0xFF000000u;
            uint32_t r = (c >> 16) & 0xFFu;
            uint32_t g = (c >>  8) & 0xFFu;
            uint32_t b =  c        & 0xFFu;
            if (cc) {
                int rn = ((int)r * cc[0] + (int)g * cc[1] + (int)b * cc[2]) >> 10;
                int gn = ((int)r * cc[3] + (int)g * cc[4] + (int)b * cc[5]) >> 10;
                int bn = ((int)r * cc[6] + (int)g * cc[7] + (int)b * cc[8]) >> 10;
                if (rn < 0) rn = 0; else if (rn > 255) rn = 255;
                if (gn < 0) gn = 0; else if (gn > 255) gn = 255;
                if (bn < 0) bn = 0; else if (bn > 255) bn = 255;
                r = (uint32_t)rn; g = (uint32_t)gn; b = (uint32_t)bn;
            }
            /* ARGB → RGBA: swap R<->B, keep G and A. */
            s_upload_buf[i] = a | (b << 16) | (g << 8) | r;
        }
    }
    glBindTexture(GL_TEXTURE_2D, g_game_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    gb_ws_render_width(), GB_SCREEN_HEIGHT,
                    GL_RGBA, GL_UNSIGNED_BYTE, s_upload_buf);
    glBindTexture(GL_TEXTURE_2D, 0);
    g_last_timing.upload_ms = sdl_now_ms() - upload_start_ms;

    /* Clear + compose. */
    double compose_start_ms = sdl_now_ms();
    int draw_w = 0, draw_h = 0;
    SDL_GL_GetDrawableSize(g_window, &draw_w, &draw_h);
    glViewport(0, 0, draw_w, draw_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    refresh_sgb_cart_border_texture();
    GLuint active_border = 0;
    if (sgb_cart_border_active()) {
        active_border = g_sgb_cart_border_texture;
    } else if (g_border_enabled && g_border_texture) {
        active_border = g_border_texture;
    }

    /* Convert the SDL_Rect-style game viewport (window-pixel coords)
     * into drawable-pixel coords for HiDPI consistency. */
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(g_window, &win_w, &win_h);
    const double dpr_x = (double)draw_w / (double)(win_w > 0 ? win_w : 1);
    const double dpr_y = (double)draw_h / (double)(win_h > 0 ? win_h : 1);
    int vp_x = (int)(g_game_viewport.x * dpr_x);
    int vp_y = (int)(g_game_viewport.y * dpr_y);
    int vp_w = (int)(g_game_viewport.w * dpr_x);
    int vp_h = (int)(g_game_viewport.h * dpr_y);

    if (g_shader_pipeline) {
        const int active_shader = gb_shader_pipeline_active(g_shader_pipeline);
        if (active_border) {
            /* Border is always drawn with the passthrough "sharp" shader
             * — the user-selected effect applies to the game pixels
             * only, which is the visually-right thing for an LCD-style
             * filter. */
            gb_shader_pipeline_set_active_by_name(g_shader_pipeline, "sharp");
            gb_shader_pipeline_draw(g_shader_pipeline,
                                    active_border,
                                    GB_BORDER_FULL_W, GB_BORDER_FULL_H,
                                    vp_x, vp_y, vp_w, vp_h,
                                    draw_w, draw_h);
            const double sx = (double)g_game_viewport.w / (double)GB_BORDER_FULL_W;
            const double sy = (double)g_game_viewport.h / (double)GB_BORDER_FULL_H;
            int gb_x = (int)((g_game_viewport.x + GB_BORDER_INSET_X * sx) * dpr_x);
            int gb_y = (int)((g_game_viewport.y + GB_BORDER_INSET_Y * sy) * dpr_y);
            int gb_w = (int)((GB_SCREEN_WIDTH  * sx) * dpr_x);
            int gb_h = (int)((GB_SCREEN_HEIGHT * sy) * dpr_y);
            if (active_shader >= 0) {
                gb_shader_pipeline_set_active(g_shader_pipeline, active_shader);
            }
            gb_shader_pipeline_draw(g_shader_pipeline,
                                    g_game_tex,
                                    GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT,
                                    gb_x, gb_y, gb_w, gb_h,
                                    draw_w, draw_h);
        } else {
            gb_shader_pipeline_draw(g_shader_pipeline,
                                    g_game_tex,
                                    gb_ws_render_width(), GB_SCREEN_HEIGHT,
                                    vp_x, vp_y, vp_w, vp_h,
                                    draw_w, draw_h);
        }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    ImGuiIO& imgui_io = ImGui::GetIO();
    imgui_io.FontGlobalScale = settings_ui_scale_for_size(imgui_io.DisplaySize);

    if (g_show_menu) {
        const float ui_scale = imgui_io.FontGlobalScale;
        const float footer_height = ImGui::GetFrameHeightWithSpacing() *
                                    (g_launcher_return_enabled ? 3.8f : 3.0f);

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(imgui_io.DisplaySize, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.96f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * ui_scale, 16.0f * ui_scale));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f * ui_scale, 10.0f * ui_scale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * ui_scale, 10.0f * ui_scale));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 18.0f * ui_scale);
        ImGui::Begin("GameBoy Recompiled",
                     &g_show_menu,
                     ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoCollapse);
        ImGui::Text("GameBoy Recompiled");
        ImGui::TextDisabled("Settings");
        ImGui::Separator();
        ImGui::BeginChild("SettingsScroll", ImVec2(0.0f, -footer_height), false, ImGuiWindowFlags_NavFlattened);

        /* ============================================================
         * Front-of-house: things you reach for during normal play.
         * Each subgroup is tagged with a small dim heading so the menu
         * doesn't read as one undifferentiated block.
         * ========================================================== */

        ImGui::TextDisabled("Playback");
        if (ImGui::SliderInt("Speed %", &g_speed_percent, 10, 500, "%d", ImGuiSliderFlags_NoInput)) {
            reset_audio_output_buffer(true);
        }
        if (ImGui::Button("Reset Speed")) {
            g_speed_percent = 100;
            g_max_speed_mode = false;
            reset_audio_output_buffer(true);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Effective: %d%%", effective_speed_percent());
        if (g_fast_forward_active) {
            ImGui::TextDisabled("Fast forward shortcut is active.");
        } else if (g_max_speed_mode) {
            ImGui::TextDisabled("Max speed shortcut is active.");
        }
        if (ImGui::Checkbox("Show FPS", &g_show_fps)) {
            save_runtime_preferences();
        }
        static const char* const THEME_NAMES[] = { "Classic", "Light", "Dark" };
        if (ImGui::BeginCombo("Menu Theme", THEME_NAMES[g_imgui_theme])) {
            for (int i = 0; i < 3; i++) {
                bool selected = (i == g_imgui_theme);
                if (ImGui::Selectable(THEME_NAMES[i], selected)) {
                    g_imgui_theme = i;
                    apply_imgui_theme(g_imgui_theme);
                    save_runtime_preferences();
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Audio");
        if (ImGui::Checkbox("Mute", &g_audio_muted)) {
            save_runtime_preferences();
        }
        int audio_volume_percent = (int)g_audio_volume_percent;
        if (ImGui::SliderInt("Master Volume (%)", &audio_volume_percent, 0, 200, "%d", ImGuiSliderFlags_NoInput)) {
            g_audio_volume_percent = (uint32_t)audio_volume_percent;
            save_runtime_preferences();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Look");

        /* Hardware Mode: per-game pref controlling how the cart's
         * boot-time mode resolution shakes out. The cart's actual mode
         * is locked at gb_context_load_rom time, so changes here only
         * apply on the next launch. The dropdown shows the cart's
         * resolved default until the user picks something else. */
        if (!g_active_game_id.empty()) {
            static const char* hw_mode_labels[] = { "DMG", "SGB", "CGB", "GBA" };
            static const GBHardwareModePref hw_mode_values[] = {
                GB_HARDWARE_MODE_DMG, GB_HARDWARE_MODE_SGB,
                GB_HARDWARE_MODE_CGB, GB_HARDWARE_MODE_GBA,
            };
            int hw_mode_idx = 0;
            for (int i = 0; i < (int)IM_ARRAYSIZE(hw_mode_values); i++) {
                if (g_active_hardware_mode_pref == hw_mode_values[i]) { hw_mode_idx = i; break; }
            }
            if (ImGui::Combo("Hardware Mode", &hw_mode_idx,
                             hw_mode_labels, IM_ARRAYSIZE(hw_mode_labels))) {
                g_active_hardware_mode_pref = hw_mode_values[hw_mode_idx];
                g_per_game_prefs[g_active_game_id]["hardware_mode"] =
                    hardware_mode_pref_to_string(g_active_hardware_mode_pref);
                save_runtime_preferences();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(restart needed)");
        }

        /* Custom Palette: 4-color framebuffer post-remap that looks for
         * the DMG-green values and rewrites them. Only meaningful when
         * the PPU is actually emitting DMG-green pixels — in CGB or
         * GBA hardware mode the PPU uses CGB palette RAM so the
         * values never match and the remap is a silent no-op. */
        if (g_active_hardware_mode_pref != GB_HARDWARE_MODE_CGB &&
            g_active_hardware_mode_pref != GB_HARDWARE_MODE_GBA) {
            if (ImGui::Combo("Palette", &g_palette_idx, g_palette_names, IM_ARRAYSIZE(g_palette_names))) {
                set_active_game_pref_int("palette", g_palette_idx);
                save_runtime_preferences();
            }
        }

        /* CGB BIOS Palette: only relevant when a DMG cart is running on
         * CGB hardware. Default "Auto" runs the cart's title-hash
         * through the BIOS LUT (Nintendo-curated palette for ~80 known
         * games, "Brown" default for the rest); the rest force one of
         * the 12 button-combo presets a real CGB picks when you hold
         * a button at boot. */
        if ((g_active_hardware_mode_pref == GB_HARDWARE_MODE_CGB ||
             g_active_hardware_mode_pref == GB_HARDWARE_MODE_GBA) &&
            g_registered_ctx &&
            !g_registered_ctx->config.cartridge_supports_cgb) {
            /* Color names follow the canonical CGB BIOS labels (Brown,
             * Pastel Mix, Inverted, etc.) — the button combo each
             * corresponds to on real hardware is shown alongside so
             * the mapping is unambiguous. */
            static const char* cgb_pal_names[] = {
                "Auto",
                "Green (Right)",        "Blue (Left)",
                "Brown (Up)",           "Pastel Mix (Down)",
                "Dark Green (Right+A)", "Dark Blue (Left+A)",
                "Red (Up+A)",           "Orange (Down+A)",
                "Inverted (Right+B)",   "Grayscale (Left+B)",
                "Dark Brown (Up+B)",    "Yellow (Down+B)",
            };
            int cgb_pal_idx = g_registered_ctx->cgb_compat_palette_override + 1;
            if (cgb_pal_idx < 0 || cgb_pal_idx >= (int)IM_ARRAYSIZE(cgb_pal_names)) {
                cgb_pal_idx = 0;
            }
            if (ImGui::Combo("CGB BIOS Palette", &cgb_pal_idx,
                             cgb_pal_names, IM_ARRAYSIZE(cgb_pal_names))) {
                g_registered_ctx->cgb_compat_palette_override = cgb_pal_idx - 1;
                if (g_registered_ctx->ppu) {
                    ppu_reload_cgb_compat_palette((GBPPU*)g_registered_ctx->ppu,
                                                  g_registered_ctx);
                }
                set_active_game_pref_int("cgb_palette",
                                         g_registered_ctx->cgb_compat_palette_override);
                save_runtime_preferences();
            }
        }

        /* Color Correction: only visible in CGB / GBA hardware modes
         * (DMG and SGB output goes through different paths). Off is
         * the raw PPU output; GBC matches what an original CGB LCD
         * would show; GBA compensates for the AGB LCD's washed-out
         * look. Defaults auto-pick a sensible mode based on hardware
         * mode at game-load time. */
        if (g_active_hardware_mode_pref == GB_HARDWARE_MODE_CGB ||
            g_active_hardware_mode_pref == GB_HARDWARE_MODE_GBA) {
            static const char* cc_names[] = { "Off", "GBC", "GBA" };
            if (ImGui::Combo("Color Correction", &g_color_correction,
                             cc_names, IM_ARRAYSIZE(cc_names))) {
                set_active_game_pref_int("color_correction", g_color_correction);
                save_runtime_preferences();
            }
        }

        /* Post-process shader (sharp / smooth / lcd / dmg-green plus any
         * shaders/<name>.frag.glsl on disk). Active selection persists
         * in runtime_prefs.ini. Reload re-compiles every entry — handy
         * when iterating on a custom shader without rebuilding. */
        if (g_shader_pipeline) {
            const int count = gb_shader_pipeline_count(g_shader_pipeline);
            int active = gb_shader_pipeline_active(g_shader_pipeline);
            if (active < 0) active = 0;
            const char* preview = gb_shader_pipeline_name(g_shader_pipeline, active);
            if (ImGui::BeginCombo("Shader", preview ? preview : "(none)")) {
                for (int i = 0; i < count; i++) {
                    const char* name = gb_shader_pipeline_name(g_shader_pipeline, i);
                    bool selected = (i == active);
                    if (ImGui::Selectable(name, selected)) {
                        if (gb_shader_pipeline_set_active(g_shader_pipeline, i)) {
                            g_active_shader_pref = name ? name : "";
                            set_active_game_pref("shader", g_active_shader_pref);
                            save_runtime_preferences();
                        }
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        /* Three optional toggles laid out inline. Each only appears
         * when it's actually applicable: SGB Colors only when the
         * engine is on AND the cart has actually issued a PAL/ATTR
         * command this session (covers e.g. mono LA which ships
         * SGB-flagless and never sends color commands — toggling
         * "SGB Colors" there would be a no-op); SGB Cart Border only
         * when the engine has decoded one (or a cache loaded); Custom
         * PNG Border only when there's at least one PNG in borders/.
         * Hiding the irrelevant ones keeps the menu honest. */
        const bool show_sgb_colors_toggle =
            (g_active_hardware_mode_pref == GB_HARDWARE_MODE_SGB) &&
            g_registered_ctx && g_registered_ctx->sgb &&
            gb_sgb_palettes_active((GBSgbState*)g_registered_ctx->sgb);
        const bool show_sgb_border_toggle = g_sgb_cart_border_texture != 0;
        const bool show_custom_border_toggle = !g_border_files.empty();
        bool placed_any = false;

        if (show_sgb_colors_toggle) {
            placed_any = true;
            if (ImGui::Checkbox("SGB Colors", &g_sgb_colors_pref)) {
                if (g_registered_ctx && g_registered_ctx->sgb) {
                    gb_sgb_set_display_palettes((GBSgbState*)g_registered_ctx->sgb,
                                                g_sgb_colors_pref);
                }
                set_active_game_pref_bool("sgb_colors", g_sgb_colors_pref);
                save_runtime_preferences();
            }
        }
        if (show_sgb_border_toggle) {
            if (placed_any) ImGui::SameLine();
            placed_any = true;
            if (ImGui::Checkbox("SGB Border", &g_sgb_cart_border_enabled)) {
                if (g_registered_ctx && g_registered_ctx->sgb) {
                    gb_sgb_set_display_border((GBSgbState*)g_registered_ctx->sgb,
                                              g_sgb_cart_border_enabled);
                }
                apply_window_scale_preset();
                set_active_game_pref_bool("sgb_border", g_sgb_cart_border_enabled);
                save_runtime_preferences();
            }
        }
        if (show_custom_border_toggle) {
            if (placed_any) ImGui::SameLine();
            placed_any = true;
            if (ImGui::Checkbox("Custom Border", &g_border_enabled)) {
                apply_border_change();
                set_active_game_pref_bool("custom_border", g_border_enabled);
                save_runtime_preferences();
            }
        }

        /* Custom-border picker. Only shown when at least one PNG is
         * present in the borders/ folder. */
        if (show_custom_border_toggle) {
            const bool combo_enabled = g_border_enabled;
            if (!combo_enabled) ImGui::BeginDisabled();
            const int count = (int)g_border_files.size();
            if (g_border_idx < 0 || g_border_idx >= count) g_border_idx = 0;
            const char* preview = (count > 0)
                ? g_border_files[g_border_idx].c_str() : "(none)";
            if (ImGui::BeginCombo("Border", preview)) {
                for (int i = 0; i < count; i++) {
                    bool selected = (i == g_border_idx);
                    if (ImGui::Selectable(g_border_files[i].c_str(), selected)) {
                        if (i != g_border_idx) {
                            g_border_idx = i;
                            apply_border_change();
                            set_active_game_pref("custom_border_file",
                                                 g_border_files[i]);
                            save_runtime_preferences();
                        }
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (!combo_enabled) ImGui::EndDisabled();
        } else {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("Drop 256x224 PNGs into a borders/ folder next to the executable or working directory, then restart for a custom screen border.");
            ImGui::PopTextWrapPos();
        }

        /* Savestates */
        ImGui::Spacing();
        ImGui::TextDisabled("Savestates");
        int savestate_slot = g_savestate_slot + 1;
        if (ImGui::SliderInt("Active Slot", &savestate_slot, 1, GB_SAVESTATE_SLOT_COUNT, "%d", ImGuiSliderFlags_NoInput)) {
            g_savestate_slot = savestate_slot - 1;
            save_runtime_preferences();
        }
        if (ImGui::Button("Save State (F5)")) {
            save_savestate_slot(g_registered_ctx, g_savestate_slot);
        }
        ImGui::SameLine();
        if (ImGui::Button("Load State (F8)")) {
            load_savestate_slot(g_registered_ctx, g_savestate_slot);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete Slot")) {
            delete_savestate_slot(g_registered_ctx, g_savestate_slot);
        }
        {
            std::string savestate_path;
            const bool slot_exists = savestate_slot_exists(g_registered_ctx, g_savestate_slot, &savestate_path);
            ImGui::TextDisabled("Slot %d: %s", g_savestate_slot + 1, slot_exists ? "Present" : "Empty");
        }
        if (!g_savestate_status.empty()) {
            ImGui::TextWrapped("%s", g_savestate_status.c_str());
        }

#ifdef GBRT_HAVE_GBCAM
        /* Game Boy Camera device picker — only compiled in when the gbrt
         * was built with GBRT_ENABLE_GBCAM, and only shown for carts that
         * actually have the MBC $FC webcam hardware. */
        if (g_registered_ctx && g_registered_ctx->mbc_type == 0xFC) {
            ImGui::Spacing();
            ImGui::TextDisabled("Camera");
            static GBCamDevice cam_devs[16];
            static int cam_count = -1;       /* -1 = not enumerated yet */
            static int cam_selected_idx = -1;
            if (cam_count < 0) {
                cam_count = gbcam_enumerate_devices(cam_devs, 16);
                /* Sync the dropdown selection with whatever the backend
                 * currently has open (env-var, default, or per-game pref). */
                const char* current = gbcam_current_device();
                for (int i = 0; i < cam_count; i++) {
                    if (strcmp(cam_devs[i].path, current) == 0) {
                        cam_selected_idx = i;
                        break;
                    }
                }
            }
            if (cam_count == 0) {
                ImGui::TextDisabled("No video devices detected.");
            } else {
                /* Format each device's label as "<label> [<path>]". */
                static const char* cam_combo_items[16];
                static char cam_combo_buf[16][160];
                for (int i = 0; i < cam_count; i++) {
                    snprintf(cam_combo_buf[i], sizeof(cam_combo_buf[i]),
                             "%s  [%s]", cam_devs[i].label, cam_devs[i].path);
                    cam_combo_items[i] = cam_combo_buf[i];
                }
                int prev = cam_selected_idx;
                if (ImGui::Combo("Device", &cam_selected_idx, cam_combo_items, cam_count)) {
                    if (cam_selected_idx != prev && cam_selected_idx >= 0 &&
                        cam_selected_idx < cam_count) {
                        gbcam_close();
                        gbcam_open(cam_devs[cam_selected_idx].path);
                        set_active_game_pref("camera_device",
                                             cam_devs[cam_selected_idx].path);
                        save_runtime_preferences();
                    }
                }
            }
        }
#endif

        ImGui::Spacing();
        ImGui::Separator();

        /* ============================================================
         * Cheats: libretro .cht files dropped into cheats/<game_id>/
         * are loaded on cart launch. GameShark codes apply per-frame
         * via gb_cheats_tick(); Game Genie codes patch ROM at
         * toggle-on and restore at toggle-off. Cart-agnostic -- any
         * Game Boy game in the libretro database works.
         * ========================================================== */
        /* Always render the Cheats header so the surrounding
         * separators have something to bracket and the menu's
         * vertical rhythm stays consistent across all carts. When
         * expanded, the header either shows the full cheat list
         * (a .cht file was loaded) or a brief hint pointing the
         * user at the cheats/ folder. */
        if (ImGui::CollapsingHeader("Cheats")) {
            if (gb_cheats_count() == 0) {
                ImGui::TextDisabled(
                    "No .cht files found in cheats/%s/.\n"
                    "Drop a libretro-database .cht file there and relaunch.",
                    g_active_game_id.c_str());
            } else {
            static char cheat_filter[64] = "";
            static int  cheat_page = 0;
            const int   cheats_per_page = 10;

            int total  = gb_cheats_count();
            int active = 0;
            for (int i = 0; i < total; i++) {
                if (gb_cheats_get(i)->enabled) active++;
            }
            ImGui::TextDisabled("Loaded %d cheats from cheats/%s/  (active: %d)",
                                total, g_active_game_id.c_str(), active);
            if (ImGui::InputText("Search##cheats",
                                 cheat_filter, sizeof(cheat_filter))) {
                /* Reset to page 1 on filter change so the user isn't
                 * stranded on a page that no longer has matches. */
                cheat_page = 0;
            }
            ImGui::SameLine();
            if (ImGui::Button("Disable All##cheats")) {
                gb_cheats_disable_all(g_registered_ctx);
            }

            /* Lower-case the filter once per frame for case-insensitive
             * substring matching against descriptions. */
            char filter_lower[64];
            int fi = 0;
            for (; cheat_filter[fi] && fi < (int)sizeof(filter_lower) - 1; fi++) {
                char c = cheat_filter[fi];
                filter_lower[fi] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
            }
            filter_lower[fi] = '\0';

            /* Two-pass render. First pass: collect filtered indices
             * into a stack buffer (one int per cheat -- cheap even
             * at the 2048 cap). Second pass: render the page slice.
             *
             * Pagination beats a tall scrollable child here because
             * focus flows naturally through 12 widgets and exits
             * onto the next menu row -- no trap. */
            int filtered_indices[GB_CHEAT_MAX_ENTRIES];
            int filtered_count = 0;
            for (int i = 0; i < total; i++) {
                const GBCheat* c = gb_cheats_get(i);
                if (!c || c->op_count == 0 || c->description[0] == '\0') continue;
                if (filter_lower[0]) {
                    char desc_lower[GB_CHEAT_DESC_MAX];
                    int di = 0;
                    for (; c->description[di] &&
                           di < (int)sizeof(desc_lower) - 1; di++) {
                        char ch = c->description[di];
                        desc_lower[di] =
                            (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
                    }
                    desc_lower[di] = '\0';
                    if (!strstr(desc_lower, filter_lower)) continue;
                }
                filtered_indices[filtered_count++] = i;
            }

            int page_count =
                (filtered_count + cheats_per_page - 1) / cheats_per_page;
            if (page_count < 1) page_count = 1;
            if (cheat_page >= page_count) cheat_page = page_count - 1;
            if (cheat_page < 0) cheat_page = 0;

            int start = cheat_page * cheats_per_page;
            int end   = start + cheats_per_page;
            if (end > filtered_count) end = filtered_count;
            for (int j = start; j < end; j++) {
                int i = filtered_indices[j];
                const GBCheat* c = gb_cheats_get(i);
                bool on = c->enabled;
                ImGui::PushID(i);
                if (ImGui::Checkbox(c->description, &on)) {
                    gb_cheats_set_enabled(g_registered_ctx, i, on);
                }
                /* Edit button opens a popup with per-op value
                 * editors. Useful for "modifier" cheats whose .cht
                 * value byte is a placeholder (e.g. "Rival Name
                 * Mod Slot N" ships value $50 -- the name-terminator
                 * -- so the user can plug in the actual char byte). */
                ImGui::SameLine();
                if (ImGui::Button("...##cheat_edit")) {
                    ImGui::OpenPopup("cheat_editor");
                }
                if (ImGui::BeginPopup("cheat_editor")) {
                    ImGui::TextDisabled("%s", c->description);
                    ImGui::Separator();
                    for (int k = 0; k < c->op_count; k++) {
                        char val_buf[8];
                        snprintf(val_buf, sizeof(val_buf), "%02X",
                                 c->ops[k].value);
                        ImGui::PushID(k);
                        ImGui::SetNextItemWidth(60.0f);
                        if (ImGui::InputText("Value##op",
                                             val_buf, sizeof(val_buf),
                                             ImGuiInputTextFlags_CharsHexadecimal |
                                             ImGuiInputTextFlags_CharsUppercase)) {
                            long v = strtol(val_buf, NULL, 16);
                            if (v >= 0 && v <= 255) {
                                gb_cheats_set_op_value(g_registered_ctx,
                                                       i, k, (uint8_t)v);
                            }
                        }
                        ImGui::SameLine();
                        const char* kind = (c->ops[k].type ==
                                            GB_CHEAT_OP_GAMESHARK)
                                           ? "RAM" : "ROM";
                        ImGui::TextDisabled("at $%04X (%s)",
                                            c->ops[k].address, kind);
                        ImGui::PopID();
                    }
                    ImGui::Separator();
                    if (ImGui::Button("Close##cheat_editor")) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            if (filtered_count == 0) {
                ImGui::TextDisabled("(no matches)");
            }

            ImGui::Spacing();
            if (ImGui::Button("< Prev##cheat_page") && cheat_page > 0) {
                cheat_page--;
            }
            ImGui::SameLine();
            ImGui::Text("Page %d / %d   (%d match%s)",
                        cheat_page + 1, page_count,
                        filtered_count, filtered_count == 1 ? "" : "es");
            ImGui::SameLine();
            if (ImGui::Button("Next >##cheat_page") &&
                cheat_page < page_count - 1) {
                cheat_page++;
            }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();

        /* ============================================================
         * Network: identity, LAN auto-discovery, direct host:port join,
         * and rendezvous-server (relay) match-making. The actual link
         * traffic always rides serial_link.{c,h}; this section is just
         * the different ways of finding a peer.
         * ========================================================== */
        if (ImGui::CollapsingHeader("Network")) {
            const float lan_ui_scale = imgui_io.FontGlobalScale;

            /* Keep relay status flags in sync with the underlying link
             * state so a dropped connection or stopped listener flips
             * the labels back to "Not hosting" / "Not joined". */
            if (!g_link_listening) g_relay_hosting = false;
            if (!gb_serial_link_is_ready()) g_relay_joined = false;

            /* Status line at the top so you always know where you stand. */
            if (gb_serial_link_is_ready()) {
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "Link active (BGB v1.4)");
                if (ImGui::Button("Disconnect")) {
                    gb_serial_link_shutdown();
                    g_link_listening = false;
                    g_relay_hosting = false;
                    g_relay_joined = false;
                    g_link_status_text.clear();
                }
            } else if (g_link_listening) {
                ImGui::TextDisabled("Listening on TCP :%d - waiting for peer", g_link_port_pref);
            } else if (!g_link_status_text.empty()) {
                ImGui::TextDisabled("%s", g_link_status_text.c_str());
            }

            ImGui::Spacing();
            ImGui::TextDisabled("You");
            if (ImGui::InputText("Nickname", g_link_nickname_edit, sizeof(g_link_nickname_edit),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                gb_lan_set_self_nickname(g_link_nickname_edit);
            }
            ImGui::SameLine();
            if (ImGui::Button("Apply")) {
                gb_lan_set_self_nickname(g_link_nickname_edit);
            }
            ImGui::TextDisabled("UUID: %s", gb_lan_self_uuid());

            ImGui::Spacing();
            ImGui::TextDisabled("Local network");
            bool lan_on = g_lan_enabled_pref;
            if (ImGui::Checkbox("Auto-discover peers", &lan_on)) {
                gb_lan_set_enabled(lan_on);
                /* If turning on failed (no usable network interface, UDP
                 * port already taken), gb_lan_set_enabled returns
                 * silently and the engine stays off. Reflect the actual
                 * state in our pref + show why so the user isn't left
                 * staring at a forever-empty peer list. */
                bool actual = gb_lan_is_enabled();
                g_lan_enabled_pref = actual;
                if (lan_on && !actual) {
                    g_link_status_text =
                        "Couldn't open the LAN discovery socket. Are you "
                        "online and is UDP :8766 free?";
                } else {
                    g_link_status_text.clear();
                }
                save_runtime_preferences();
            }
            ImGui::TextDisabled("Broadcasts your nickname on UDP :8766. Off by default.");

            int net_port = g_link_port_pref;
            if (ImGui::InputInt("My TCP port", &net_port, 1, 100, ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (net_port > 0 && net_port < 65536) {
                    g_link_port_pref = net_port;
                    gb_lan_set_advertised_port((uint16_t)net_port);
                    save_runtime_preferences();
                }
            }
            if (!g_link_listening) {
                if (ImGui::Button("Host (start listening)")) {
                    if (gb_serial_link_start_listen((uint16_t)g_link_port_pref)) {
                        g_link_listening = true;
                        g_link_status_text = "Listening, waiting for peer...";
                    } else {
                        g_link_status_text = "Failed to start listener (port in use?)";
                    }
                }
            } else {
                if (ImGui::Button("Stop hosting")) {
                    gb_serial_link_shutdown();
                    g_link_listening = false;
                    g_link_status_text.clear();
                }
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Discovered peers");
            if (!gb_lan_is_enabled()) {
                ImGui::TextDisabled("(turn on auto-discover to see peers)");
            } else {
                GBLanPeer net_peers[16];
                size_t np = gb_lan_get_peers(net_peers, 16);
                if (np == 0) {
                    ImGui::TextDisabled("(searching... peers appear within a few seconds)");
                } else if (ImGui::BeginTable("peers", 4,
                                             ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn("Nickname", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Address",  ImGuiTableColumnFlags_WidthFixed, 170.0f * lan_ui_scale);
                    ImGui::TableSetupColumn("Status",   ImGuiTableColumnFlags_WidthFixed, 110.0f * lan_ui_scale);
                    ImGui::TableSetupColumn("Action",   ImGuiTableColumnFlags_WidthFixed, 110.0f * lan_ui_scale);
                    ImGui::TableHeadersRow();
                    const bool any_link = gb_serial_link_is_ready();
                    const char* current_peer_ip = gb_serial_link_peer_ip();
                    for (size_t i = 0; i < np; i++) {
                        const bool is_connected_peer = any_link && current_peer_ip && current_peer_ip[0] &&
                                                       strcmp(current_peer_ip, net_peers[i].ip) == 0;
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(net_peers[i].nickname);
                        ImGui::TableNextColumn(); ImGui::Text("%s:%u", net_peers[i].ip, (unsigned)net_peers[i].bgb_port);
                        ImGui::TableNextColumn();
                        if (is_connected_peer) {
                            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "Connected");
                        } else {
                            ImGui::TextDisabled("Not connected");
                        }
                        ImGui::TableNextColumn();
                        ImGui::PushID((int)i);
                        if (is_connected_peer) {
                            if (ImGui::Button("Disconnect")) {
                                gb_serial_link_shutdown();
                                g_link_listening = false;
                                g_link_status_text.clear();
                            }
                        } else if (any_link) {
                            ImGui::BeginDisabled();
                            ImGui::Button("Connect");
                            ImGui::EndDisabled();
                        } else if (ImGui::Button("Connect")) {
                            if (gb_serial_link_start_connect(net_peers[i].ip, net_peers[i].bgb_port)) {
                                g_link_status_text = std::string("Connecting to ") + net_peers[i].nickname + "...";
                            } else {
                                g_link_status_text = std::string("Connect failed: ") + net_peers[i].ip;
                            }
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Direct connect");
            ImGui::PushItemWidth(180.0f * lan_ui_scale);
            ImGui::InputText("Host", g_link_manual_host, sizeof(g_link_manual_host));
            ImGui::SameLine();
            ImGui::InputText("Port", g_link_manual_port, sizeof(g_link_manual_port));
            ImGui::PopItemWidth();
            ImGui::SameLine();
            if (ImGui::Button("Join##direct")) {
                int mp = atoi(g_link_manual_port);
                if (mp > 0 && mp < 65536 && g_link_manual_host[0]) {
                    if (gb_serial_link_start_connect(g_link_manual_host, (uint16_t)mp)) {
                        g_link_status_text = std::string("Connecting to ") + g_link_manual_host + "...";
                    } else {
                        g_link_status_text = "Connect failed";
                    }
                }
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Rendezvous server (internet play)");
            ImGui::InputText("Server URL", g_relay_url, sizeof(g_relay_url));

            const bool have_url = (g_relay_url[0] != '\0');

            ImGui::Spacing();
            ImGui::TextDisabled("Host Server");
            ImGui::SameLine();
            if (g_relay_hosting) {
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "Hosting");
            } else {
                ImGui::TextUnformatted("Not hosting");
            }
            if (!have_url) ImGui::BeginDisabled();
            ImGui::PushItemWidth(220.0f * lan_ui_scale);
            ImGui::InputText("Room name", g_relay_code, sizeof(g_relay_code));
            ImGui::PopItemWidth();
            ImGui::SameLine();
            if (ImGui::Button("Random")) {
                generate_random_room_code(g_relay_code, sizeof(g_relay_code));
            }
            ImGui::SameLine();
            const bool can_host = have_url && (g_relay_code[0] != '\0');
            if (ImGui::Button("Host via Relay")) {
                if (!can_host) {
                    g_relay_status = have_url
                        ? "Type a room name (or click Random) before hosting."
                        : "Set Server URL above first.";
                } else {
                    save_runtime_preferences();
                    gb_serial_link_shutdown();
                    g_relay_joined = false;
                    if (!gb_serial_link_start_listen((uint16_t)g_link_port_pref)) {
                        g_relay_status = "Failed to open local listener; pick another port above.";
                        g_relay_hosting = false;
                    } else {
                        g_link_listening = true;
                        GBRelayResult r = gb_relay_register(g_relay_url, g_relay_code,
                                                           "host", g_link_port_pref,
                                                           gb_lan_self_nickname(),
                                                           gb_lan_self_uuid());
                        if (!r.ok) {
                            g_relay_status = std::string("Register failed: ") + r.error;
                            gb_serial_link_shutdown();
                            g_link_listening = false;
                            g_relay_hosting = false;
                        } else {
                            g_relay_hosting = true;
                            if (r.peer.has_peer) {
                                g_relay_status = std::string("Listening on port ") +
                                                 std::to_string(g_link_port_pref) +
                                                 "; join already registered from " + r.peer.ip;
                            } else {
                                g_relay_status = std::string("Listening on port ") +
                                                 std::to_string(g_link_port_pref) +
                                                 "; share code '" + g_relay_code +
                                                 "' and wait for the other side.";
                            }
                        }
                    }
                }
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Join Server");
            ImGui::SameLine();
            if (g_relay_joined) {
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "Joined");
            } else {
                ImGui::TextUnformatted("Not joined");
            }
            if (ImGui::Button("Refresh list")) {
                GBRelayRoomInfo rooms[64];
                GBRelayListResult lr = gb_relay_list_rooms(g_relay_url, rooms, 64);
                if (!lr.ok) {
                    g_relay_list_status = std::string("List failed: ") + lr.error;
                    g_relay_rooms.clear();
                    g_relay_room_pick = -1;
                } else {
                    g_relay_rooms.assign(rooms, rooms + lr.count);
                    g_relay_room_pick = g_relay_rooms.empty() ? -1 : 0;
                    g_relay_list_status = std::string("Found ") +
                                          std::to_string(lr.count) +
                                          (lr.count == 1 ? " open room." : " open rooms.");
                }
            }
            ImGui::SameLine();
            ImGui::PushItemWidth(220.0f * lan_ui_scale);
            std::string preview;
            if (g_relay_room_pick >= 0 && g_relay_room_pick < (int)g_relay_rooms.size()) {
                const GBRelayRoomInfo& r = g_relay_rooms[g_relay_room_pick];
                preview = std::string(r.code) + (r.nickname[0] ? std::string(" - ") + r.nickname : std::string());
            } else if (g_relay_rooms.empty()) {
                preview = "(no open rooms)";
            } else {
                preview = "(pick one)";
            }
            if (ImGui::BeginCombo("##relay_rooms", preview.c_str())) {
                for (int i = 0; i < (int)g_relay_rooms.size(); i++) {
                    const GBRelayRoomInfo& r = g_relay_rooms[i];
                    char label[160];
                    if (r.nickname[0]) {
                        snprintf(label, sizeof(label), "%s - %s (%ds ago)",
                                 r.code, r.nickname, r.age_seconds);
                    } else {
                        snprintf(label, sizeof(label), "%s (%ds ago)",
                                 r.code, r.age_seconds);
                    }
                    bool selected = (i == g_relay_room_pick);
                    if (ImGui::Selectable(label, selected)) {
                        g_relay_room_pick = i;
                        snprintf(g_relay_code, sizeof(g_relay_code), "%s", r.code);
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PopItemWidth();
            ImGui::SameLine();
            const bool can_join = have_url && g_relay_room_pick >= 0 &&
                                  g_relay_room_pick < (int)g_relay_rooms.size();
            if (ImGui::Button("Join##relay")) {
                if (!can_join) {
                    g_relay_status = have_url
                        ? "Pick a room from the dropdown first (or hit Refresh list)."
                        : "Set Server URL above first.";
                } else {
                    save_runtime_preferences();
                    snprintf(g_relay_code, sizeof(g_relay_code), "%s",
                             g_relay_rooms[g_relay_room_pick].code);
                    g_relay_hosting = false;
                    g_relay_joined = false;
                    GBRelayResult r = gb_relay_register(g_relay_url, g_relay_code,
                                                       "join", 1, gb_lan_self_nickname(),
                                                       gb_lan_self_uuid());
                    if (!r.ok) {
                        g_relay_status = std::string("Register failed: ") + r.error;
                    } else if (!r.peer.has_peer) {
                        GBRelayResult lk = gb_relay_lookup(g_relay_url, g_relay_code, "join");
                        if (lk.ok && lk.peer.has_peer) r = lk;
                    }
                    if (r.ok && r.peer.has_peer) {
                        gb_serial_link_shutdown();
                        if (gb_serial_link_start_connect(r.peer.ip, (uint16_t)r.peer.port)) {
                            g_relay_status = std::string("Connecting to ") + r.peer.ip +
                                             ":" + std::to_string(r.peer.port);
                            g_relay_joined = true;
                        } else {
                            g_relay_status = std::string("Found host at ") + r.peer.ip +
                                             ":" + std::to_string(r.peer.port) +
                                             " but TCP connect failed (port not reachable).";
                        }
                    } else if (r.ok) {
                        g_relay_status = "Host disappeared between list and join - refresh and try again.";
                    }
                }
            }
            if (!g_relay_list_status.empty()) {
                ImGui::TextDisabled("%s", g_relay_list_status.c_str());
            }
            if (have_url) {
                if (ImGui::SmallButton("Forget my room")) {
                    if (g_relay_code[0]) {
                        gb_relay_unregister(g_relay_url, g_relay_code, "host");
                        gb_relay_unregister(g_relay_url, g_relay_code, "join");
                    }
                    g_relay_hosting = false;
                    g_relay_joined = false;
                    g_relay_status = "Removed from rendezvous server.";
                }
            }
            if (!have_url) ImGui::EndDisabled();

            if (!g_relay_status.empty()) {
                ImGui::TextWrapped("%s", g_relay_status.c_str());
            }
            if (!have_url) {
                ImGui::TextDisabled("Fill in server URL to enable.");
            }
        }

        ImGui::Spacing();
        ImGui::Separator();

        /* ============================================================
         * Advanced: tucked away. Display, audio plumbing, key bindings,
         * diagnostics. Open it once to remap keys or pick an output
         * device, then forget it exists.
         * ========================================================== */
        if (ImGui::CollapsingHeader("Advanced")) {
            int window_w = 0;
            int window_h = 0;
            SDL_GetWindowSize(g_window, &window_w, &window_h);

            ImGui::TextDisabled("Display");
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
                ImGui::TextDisabled("Window Size disabled while fullscreen.");
            }
            if (ImGui::Checkbox("V-Sync", &g_vsync)) {
                SDL_GL_SetSwapInterval(g_vsync ? 1 : 0);
            }
            ImGui::Checkbox("Show Overlay", &g_show_overlay);
            ImGui::Checkbox("Smooth Slow Frames", &g_smooth_lcd_transitions);

            ImGui::Spacing();
            ImGui::TextDisabled("Audio");
            if (ImGui::Checkbox("Enable Audio Output", &g_audio_output_enabled)) {
                reset_audio_output_buffer(true);
                save_runtime_preferences();
            }
            if (ImGui::BeginCombo("Output Device", current_audio_output_device_label())) {
                bool selected_default = g_audio_target_device_name.empty();
                if (ImGui::Selectable("System Default", selected_default)) {
                    g_audio_target_device_name.clear();
                    save_runtime_preferences();
                    reopen_audio_output_device(true);
                }
                if (selected_default) {
                    ImGui::SetItemDefaultFocus();
                }
                for (size_t i = 0; i < g_audio_output_devices.size(); i++) {
                    const bool selected = g_audio_output_devices[i] == g_audio_target_device_name;
                    if (ImGui::Selectable(g_audio_output_devices[i].c_str(), selected)) {
                        g_audio_target_device_name = g_audio_output_devices[i];
                        save_runtime_preferences();
                        reopen_audio_output_device(true);
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            int audio_latency_ms = (int)g_audio_latency_ms;
            if (ImGui::SliderInt("Target Audio Latency (ms)", &audio_latency_ms, 20, 250, "%d", ImGuiSliderFlags_NoInput)) {
                g_audio_latency_ms = (uint32_t)audio_latency_ms;
                recompute_audio_targets();
                reset_audio_output_buffer(true);
                save_runtime_preferences();
            }
            if (ImGui::Button("Refresh Devices")) {
                refresh_audio_output_devices();
            }
            ImGui::SameLine();
            if (ImGui::Button("Reconnect Audio")) {
                reopen_audio_output_device(true);
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear Audio Buffer")) {
                reset_audio_output_buffer(true);
            }
            ImGui::TextDisabled("Active: %s", g_audio_active_device_name.empty() ? "Unavailable" : g_audio_active_device_name.c_str());
            if (!current_audio_output_device_available()) {
                ImGui::TextDisabled("Requested device unavailable; using default.");
            }
            ImGui::TextDisabled("Ring %u / %u  ·  %u Hz buf %u",
                                current_audio_ring_fill_samples(), current_audio_ring_capacity(),
                                g_audio_device_sample_rate, g_audio_device_buffer_samples);
            if (effective_speed_percent() != 100) {
                ImGui::TextDisabled("Audio paused while Speed %% != 100.");
            }
            if (ImGui::Button("Reset Audio")) {
                reset_runtime_audio_defaults();
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Controls");
            if (!g_controller_name.empty()) {
                ImGui::Text("Controller: %s", g_controller_name.c_str());
                ImGui::TextDisabled("Profile: %s", controller_type_name(g_controller_type));
            } else {
#if defined(__ANDROID__)
                ImGui::TextDisabled("No controller detected. Controller mappings apply when one is connected.");
#else
                ImGui::TextDisabled("No controller detected.");
#endif
            }
            render_binding_editor("Keyboard Gameplay",
                                  GB_CAPTURE_DEVICE_KEYBOARD,
                                  g_keyboard_bindings,
                                  GB_INPUT_ACTION_RIGHT,
                                  GB_INPUT_ACTION_FAST_FORWARD);
            ImGui::Spacing();
            render_binding_editor("Controller Gameplay",
                                  GB_CAPTURE_DEVICE_CONTROLLER,
                                  g_controller_bindings,
                                  GB_INPUT_ACTION_RIGHT,
                                  GB_INPUT_ACTION_FAST_FORWARD);
            ImGui::Spacing();
            render_binding_editor("Keyboard Shortcuts",
                                  GB_CAPTURE_DEVICE_KEYBOARD,
                                  g_keyboard_bindings,
                                  GB_INPUT_ACTION_FAST_FORWARD,
                                  GB_INPUT_ACTION_COUNT);
            ImGui::Spacing();
            render_binding_editor("Controller Shortcuts",
                                  GB_CAPTURE_DEVICE_CONTROLLER,
                                  g_controller_bindings,
                                  GB_INPUT_ACTION_FAST_FORWARD,
                                  GB_INPUT_ACTION_COUNT);
            if (g_binding_capture_active) {
                ImGui::Separator();
                ImGui::TextDisabled("Waiting for %s input for %s slot %d",
                                    g_binding_capture_device == GB_CAPTURE_DEVICE_KEYBOARD ? "keyboard" : "controller",
                                    g_input_action_names[g_binding_capture_action],
                                    g_binding_capture_slot + 1);
                if (ImGui::Button("Cancel Capture")) {
                    cancel_binding_capture();
                }
            }
            if (ImGui::Button("Reset Controls")) {
                reset_runtime_control_defaults();
            }
            ImGui::TextDisabled("Shortcuts: fast forward, max speed, savestates, overlay, mute, menu.");

            ImGui::Spacing();
            ImGui::TextDisabled("Diagnostics");
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            ImGui::Text("Window: %d x %d  ·  Viewport: %d x %d (%.2fx)",
                        window_w, window_h,
                        g_game_viewport.w, g_game_viewport.h,
                        (double)g_game_viewport.w / (double)GB_SCREEN_WIDTH);
            if (has_interpreter_activity(g_registered_ctx)) {
                const GBInterpreterHotspot* hotspot = &g_registered_ctx->interpreter_hotspots[0];
                ImGui::Text("Frame Fallbacks: %u", g_registered_ctx->frame_dispatch_fallbacks);
                ImGui::Text("Total Fallbacks: %llu",
                            (unsigned long long)g_registered_ctx->total_dispatch_fallbacks);
                ImGui::Text("Interp Instr: frame %llu total %llu",
                            (unsigned long long)g_registered_ctx->frame_interpreter_instructions,
                            (unsigned long long)g_registered_ctx->total_interpreter_instructions);
                if (hotspot->valid && hotspot->entries > 0) {
                    ImGui::Text("Top Hotspot: %03X:%04X (%llu hits)",
                                hotspot->bank, hotspot->addr,
                                (unsigned long long)hotspot->entries);
                }
                if (g_registered_ctx->has_unimplemented_interpreter_opcode) {
                    ImGui::Text("Coverage Gap: %02X at %03X:%04X",
                                g_registered_ctx->last_unimplemented_opcode,
                                g_registered_ctx->last_unimplemented_bank,
                                g_registered_ctx->last_unimplemented_addr);
                }
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Toggle menu: %s", controller_menu_hint_text());
#if defined(__ANDROID__)
            ImGui::TextDisabled("Android Back / Escape also opens or closes the menu.");
#else
            ImGui::TextDisabled("Escape also opens or closes the menu.");
#endif
        }

        /* Game-specific overlay controls. The agnostic core renders nothing
         * here by default (no-op in game_extras_default.c); a game's extras
         * (e.g. the Pokémon repo) implements game_draw_overlay() to add its
         * own ImGui sections. This is where the former built-in "Pokemon
         * Options" CollapsingHeader now lives, moved out of the core. */
        game_draw_overlay(g_registered_ctx);

        ImGui::EndChild();
        ImGui::Separator();

        const int footer_button_count = g_launcher_return_enabled ? 3 : 2;
        const float footer_button_width =
            (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * (footer_button_count - 1)) /
            (float)footer_button_count;
        if (ImGui::Button("Resume", ImVec2(footer_button_width, 0.0f))) {
            g_show_menu = false;
        }

        if (g_launcher_return_enabled) {
            ImGui::SameLine();
            if (ImGui::Button("Return to Launcher", ImVec2(footer_button_width, 0.0f))) {
                g_exit_action = GB_PLATFORM_EXIT_RETURN_TO_LAUNCHER;
                SDL_Event quit_event;
                quit_event.type = SDL_QUIT;
                SDL_PushEvent(&quit_event);
            }
        } else {
            /* Single-game launcher mode: nowhere to return to, so offer
             * "Restart Game" in the same slot. The launcher loops back to
             * the same cart and pokegame_init runs a full reset.
             *
             * We don't change g_exit_action here (the recompiled main only
             * has translation logic for RETURN_TO_LAUNCHER → exit code 64);
             * instead we set a separate flag the launcher reads after the
             * cart's main_fn returns. */
            ImGui::SameLine();
            if (ImGui::Button("Restart Game", ImVec2(footer_button_width, 0.0f))) {
                g_restart_game_requested = true;
                g_exit_action = GB_PLATFORM_EXIT_RESTART_GAME;
                SDL_Event quit_event;
                quit_event.type = SDL_QUIT;
                SDL_PushEvent(&quit_event);
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(footer_button_width, 0.0f))) {
            g_exit_action = GB_PLATFORM_EXIT_QUIT;
            SDL_Event quit_event;
            quit_event.type = SDL_QUIT;
            SDL_PushEvent(&quit_event);
        }
        ImGui::End();
        ImGui::PopStyleVar(6);
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

    /* FPS overlay last so it draws on top of the menu (and over any
     * border/letterbox the game viewport leaves around the edges). */
    if (g_show_fps) {
        ImGuiIO& fps_io = ImGui::GetIO();
        const float pad = 8.0f * fps_io.FontGlobalScale;
        ImGui::SetNextWindowPos(ImVec2(fps_io.DisplaySize.x - pad, pad),
                                ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        if (ImGui::Begin("##fps_overlay", NULL,
                         ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoInputs)) {
            ImGui::Text("%.1f FPS", g_guest_fps);
        }
        ImGui::End();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    g_last_timing.compose_ms = sdl_now_ms() - compose_start_ms;

    double present_start_ms = sdl_now_ms();
    SDL_GL_SwapWindow(g_window);
    g_last_timing.present_ms = sdl_now_ms() - present_start_ms;
    g_last_timing.total_render_ms = sdl_now_ms() - total_render_start_ms;
    g_timing_render_total += g_last_timing.total_render_ms;
}

/* ============================================================================
 * Platform Functions
 * ========================================================================== */

void gb_platform_shutdown(void) {
    gb_serial_link_shutdown();
    gb_lan_shutdown();
    close_input_record_file();
    close_audio_output_device();
    g_audio_output_devices.clear();
    gb_debug_server_shutdown();

    clear_controller_state();
    g_binding_capture_active = false;
    g_binding_capture_device = GB_CAPTURE_DEVICE_NONE;
    
    if (ImGui::GetCurrentContext() != NULL) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
    }

    /* Tear down GL-owned objects before the context goes away. */
    if (g_shader_pipeline) {
        gb_shader_pipeline_destroy(g_shader_pipeline);
        g_shader_pipeline = NULL;
    }
    if (g_game_tex) {
        glDeleteTextures(1, &g_game_tex);
        g_game_tex = 0;
    }
    unload_border_texture();
    unload_sgb_cart_border_texture();
    if (g_gl_context) {
        SDL_GL_DeleteContext(g_gl_context);
        g_gl_context = NULL;
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

bool gb_platform_consume_restart_requested(void) {
    bool requested = g_restart_game_requested;
    g_restart_game_requested = false;
    return requested;
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

/* Debug counters */
static uint32_t g_audio_samples_written = 0;
static uint32_t g_audio_underruns = 0;

/* Last valid sample for smooth underrun handling */
static int16_t g_last_sample_l = 0;
static int16_t g_last_sample_r = 0;

static bool audio_subsystem_available(void) {
    return (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) != 0;
}

static void refresh_audio_output_devices(void) {
    g_audio_output_devices.clear();
    if (!audio_subsystem_available()) {
        return;
    }

    const int count = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < count; i++) {
        const char* name = SDL_GetAudioDeviceName(i, 0);
        if (name && name[0]) {
            g_audio_output_devices.emplace_back(name);
        }
    }
}

static int current_audio_output_device_index(void) {
    if (g_audio_target_device_name.empty()) {
        return 0;
    }

    for (size_t i = 0; i < g_audio_output_devices.size(); i++) {
        if (g_audio_output_devices[i] == g_audio_target_device_name) {
            return (int)i + 1;
        }
    }

    return 0;
}

static const char* current_audio_output_device_label(void) {
    if (!g_audio_target_device_name.empty()) {
        return g_audio_target_device_name.c_str();
    }
    return "System Default";
}

static bool current_audio_output_device_available(void) {
    return g_audio_target_device_name.empty() || current_audio_output_device_index() != 0;
}

static void close_audio_output_device(void) {
    if (g_audio_device) {
        SDL_CloseAudioDevice(g_audio_device);
        g_audio_device = 0;
    }
    g_audio_started = false;
    g_audio_start_threshold = 0;
    g_audio_low_watermark = 0;
    g_audio_device_sample_rate = AUDIO_SAMPLE_RATE;
    g_audio_device_buffer_samples = 0;
    g_audio_active_device_name.clear();
}

static bool open_audio_output_device(const char* device_name, bool preserve_stats) {
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 2048;
    want.callback = sdl_audio_callback;
    want.userdata = NULL;

    g_audio_device = SDL_OpenAudioDevice(device_name, 0, &want, &have, 0);
    if (g_audio_device == 0) {
        return false;
    }

    g_audio_device_sample_rate = (uint32_t)have.freq;
    g_audio_device_buffer_samples = (uint32_t)have.samples;
    g_audio_active_device_name = (device_name && device_name[0]) ? device_name : "System Default";
    recompute_audio_targets();
    reset_audio_output_buffer(preserve_stats);
    fprintf(stderr,
            "[SDL] Audio initialized: %d Hz, %d channels, device '%s', buffer %d samples, target latency %u ms\n",
            have.freq,
            have.channels,
            g_audio_active_device_name.c_str(),
            have.samples,
            g_audio_latency_ms);
    return true;
}

static bool reopen_audio_output_device(bool preserve_stats) {
    if (!audio_subsystem_available()) {
        return false;
    }

    const std::string requested_device = g_audio_target_device_name;
    close_audio_output_device();

    if (!requested_device.empty() && open_audio_output_device(requested_device.c_str(), preserve_stats)) {
        return true;
    }

    if (!requested_device.empty()) {
        fprintf(stderr,
                "[SDL] Failed to open requested audio device '%s': %s. Falling back to default output.\n",
                requested_device.c_str(),
                SDL_GetError());
    }

    if (open_audio_output_device(NULL, preserve_stats)) {
        return true;
    }

    fprintf(stderr, "[SDL] Failed to open audio: %s\n", SDL_GetError());
    return false;
}

static uint32_t current_audio_underruns(void) {
    return g_audio_underruns;
}

static uint32_t current_audio_ring_fill_samples(void) {
    return audio_ring_fill_samples();
}

static uint32_t current_audio_ring_capacity(void) {
    return AUDIO_RING_SIZE;
}

static bool audio_output_should_run(void) {
    return g_audio_device != 0 &&
           g_audio_output_enabled &&
           !g_benchmark_mode &&
           !g_app_suspended &&
           effective_speed_percent() == 100;
}

static uint32_t audio_ring_fill_samples(void) {
    uint32_t write_pos = g_audio_write_pos.load(std::memory_order_acquire);
    uint32_t read_pos = g_audio_read_pos.load(std::memory_order_acquire);
    return (write_pos >= read_pos) ? (write_pos - read_pos) : (AUDIO_RING_SIZE - read_pos + write_pos);
}

static void update_audio_stats_from_ring(void) {
    audio_stats_update_buffer(audio_ring_fill_samples(), AUDIO_RING_SIZE, g_audio_device_sample_rate);
}

static void recompute_audio_targets(void) {
    const uint32_t sample_rate = g_audio_device_sample_rate ? g_audio_device_sample_rate : AUDIO_SAMPLE_RATE;
    const uint32_t device_buffer = g_audio_device_buffer_samples ? g_audio_device_buffer_samples : 512u;
    uint32_t target = (uint32_t)(((uint64_t)sample_rate * (uint64_t)g_audio_latency_ms + 999ull) / 1000ull);

    if (target < device_buffer) {
        target = device_buffer;
    }
    if (target == 0) {
        target = 1;
    }
    if (target > (AUDIO_RING_SIZE / 2)) {
        target = AUDIO_RING_SIZE / 2;
    }

    g_audio_start_threshold = target;
    g_audio_low_watermark = target / 2;
    if (g_audio_low_watermark == 0) {
        g_audio_low_watermark = 1;
    }
}

static void clear_audio_ring_buffer_locked(void) {
    g_audio_write_pos.store(0, std::memory_order_relaxed);
    g_audio_read_pos.store(0, std::memory_order_relaxed);
    memset(g_audio_ring, 0, sizeof(g_audio_ring));
    g_audio_started = false;
    update_audio_stats_from_ring();
}

static void refresh_audio_device_pause_state(void) {
    if (!g_audio_device) {
        return;
    }

    const bool paused = !audio_output_should_run() || !g_audio_started;
    SDL_PauseAudioDevice(g_audio_device, paused ? 1 : 0);
}

static void reset_audio_output_buffer(bool preserve_stats) {
    if (g_audio_device) {
        SDL_LockAudioDevice(g_audio_device);
        clear_audio_ring_buffer_locked();
        refresh_audio_device_pause_state();
        SDL_UnlockAudioDevice(g_audio_device);
    } else {
        clear_audio_ring_buffer_locked();
    }

    if (!preserve_stats) {
        g_audio_underruns = 0;
        g_audio_samples_written = 0;
    }
}

/* SDL callback - pulls samples from ring buffer.
 * On underrun, holds the last valid sample with a gentle fade-out
 * instead of inserting silence, which eliminates audible pops. */
static void sdl_audio_callback(void* userdata, Uint8* stream, int len) {
    (void)userdata;
    int16_t* out = (int16_t*)stream;
    int samples_needed = len / 4;  /* Stereo 16-bit = 4 bytes per sample */

    uint32_t write_pos = g_audio_write_pos.load(std::memory_order_acquire);
    uint32_t read_pos = g_audio_read_pos.load(std::memory_order_relaxed);

    /* Count available samples */
    uint32_t available = (write_pos >= read_pos)
        ? (write_pos - read_pos)
        : (AUDIO_RING_SIZE - read_pos + write_pos);

    auto apply_volume = [](int16_t sample) -> int16_t {
        if (g_audio_muted || g_audio_volume_percent == 0) return 0;
        if (g_audio_volume_percent == 100) return sample;
        int32_t v = ((int32_t)sample * (int32_t)g_audio_volume_percent) / 100;
        if (v < -32768) v = -32768;
        if (v > 32767) v = 32767;
        return (int16_t)v;
    };

    if (available >= (uint32_t)samples_needed) {
        /* Normal path — enough samples */
        for (int i = 0; i < samples_needed; i++) {
            out[i * 2]     = apply_volume(g_audio_ring[read_pos * 2]);
            out[i * 2 + 1] = apply_volume(g_audio_ring[read_pos * 2 + 1]);
            read_pos = (read_pos + 1) % AUDIO_RING_SIZE;
        }
        g_last_sample_l = out[(samples_needed - 1) * 2];
        g_last_sample_r = out[(samples_needed - 1) * 2 + 1];
    } else if (available > 0) {
        /* Partial underrun — consume what's available, then hold last sample
         * with gentle fade to mask the gap until the emulator catches up. */
        int i = 0;
        for (; i < (int)available; i++) {
            out[i * 2]     = g_audio_ring[read_pos * 2];
            out[i * 2 + 1] = g_audio_ring[read_pos * 2 + 1];
            read_pos = (read_pos + 1) % AUDIO_RING_SIZE;
        }
        g_last_sample_l = out[(i - 1) * 2];
        g_last_sample_r = out[(i - 1) * 2 + 1];

        /* Fade out from last sample over remaining slots */
        int fade_len = samples_needed - i;
        for (int j = 0; j < fade_len; j++) {
            /* Linear fade: full → zero over the remaining samples */
            int32_t scale = (fade_len - j) * 256 / fade_len;
            out[(i + j) * 2]     = (int16_t)((g_last_sample_l * scale) >> 8);
            out[(i + j) * 2 + 1] = (int16_t)((g_last_sample_r * scale) >> 8);
        }
        g_audio_underruns++;
        audio_stats_underrun();
    } else {
        /* Full underrun — fade from last known sample */
        for (int i = 0; i < samples_needed; i++) {
            int32_t scale = (samples_needed - i) * 256 / samples_needed;
            out[i * 2]     = (int16_t)((g_last_sample_l * scale) >> 8);
            out[i * 2 + 1] = (int16_t)((g_last_sample_r * scale) >> 8);
        }
        g_last_sample_l = 0;
        g_last_sample_r = 0;
        g_audio_underruns++;
        audio_stats_underrun();
    }

    g_audio_read_pos.store(read_pos, std::memory_order_release);
}

static void on_audio_sample(GBContext* ctx, int16_t left, int16_t right) {
    (void)ctx;
    if (!audio_output_should_run()) return;
    
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
        g_audio_started = true;
        refresh_audio_device_pause_state();
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
    g_max_speed_mode = false;
    g_binding_capture_active = false;
    g_binding_capture_device = GB_CAPTURE_DEVICE_NONE;
    update_effective_joypad_state();
    g_last_guest_framebuffer_valid = false;
    g_present_count = 0;
    g_last_timing = {};

    /* Load configurable keybinds */
    keybinds_init(NULL);

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
        load_runtime_preferences();
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
    load_runtime_preferences();
    fprintf(stderr, "[SDL] SDL initialized.\n");

#if defined(__ANDROID__)
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
#endif

    clear_controller_state();
    open_first_available_controller();
    refresh_audio_output_devices();
    
    reopen_audio_output_device(false);
    
    fprintf(stderr, "[SDL] Creating window...\n");
#if defined(_WIN32)
    /* On Windows we link against ANGLE's libGLESv2/libEGL. SDL must create
     * the GL context through that same ES library (via EGL) — otherwise it
     * defaults to the desktop WGL driver's ES2-compat profile, leaving the
     * directly-linked ANGLE entry points with no current context (glGetString
     * == NULL, GL_SHADER_COMPILER == GL_FALSE, every shader compile fails with
     * an empty info log). "1" forces SDL to load the ES driver by default name
     * (libGLESv2.dll/libEGL.dll), matching our link-time GL functions. */
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
#endif
    /* Request a GLES 2.0 context — same code path on desktop Mesa
     * (which advertises a compatible profile) and embedded Mali / Adreno
     * GPUs. SDL_WINDOW_OPENGL must be set BEFORE SDL_CreateWindow. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    g_window = SDL_CreateWindow(
        "GB Recompiled",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        g_windowed_width,
        g_windowed_height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL |
        (g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_RESIZABLE)
    );

    if (!g_window) {
        fprintf(stderr, "[SDL] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }
    fprintf(stderr, "[SDL] Window created.\n");
    SDL_SetWindowMinimumSize(g_window, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT);

    fprintf(stderr, "[SDL] Creating GL context...\n");
    g_gl_context = SDL_GL_CreateContext(g_window);
    if (!g_gl_context) {
        fprintf(stderr, "[SDL] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return false;
    }
    SDL_GL_MakeCurrent(g_window, g_gl_context);

    /* One-time GL identity log. Confirms which driver SDL actually gave us
     * (real GLES2/ANGLE vs a desktop ES2-compat context) and whether an
     * online shader compiler is present — an absent compiler is the usual
     * cause of "compile failed" with an empty info log. */
    {
        const char* gl_vendor   = (const char*)glGetString(GL_VENDOR);
        const char* gl_renderer = (const char*)glGetString(GL_RENDERER);
        const char* gl_version  = (const char*)glGetString(GL_VERSION);
        const char* gl_glsl     = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
        GLboolean has_compiler = GL_FALSE;
        glGetBooleanv(GL_SHADER_COMPILER, &has_compiler);
        fprintf(stderr,
                "[GL] vendor=%s | renderer=%s | version=%s | glsl=%s | shader_compiler=%s\n",
                gl_vendor   ? gl_vendor   : "(null)",
                gl_renderer ? gl_renderer : "(null)",
                gl_version  ? gl_version  : "(null)",
                gl_glsl     ? gl_glsl     : "(null)",
                has_compiler ? "yes" : "NO");
    }

    /* No vsync — we drive timing in wall-clock to hit 59.7 FPS exactly,
     * including on non-60Hz monitors. */
    SDL_GL_SetSwapInterval(0);

    /* Custom SGB borders: scan optional borders/ dir and honor any saved
     * border preference (resizes the window to 256x224*scale if enabled). */
    scan_border_directory();
    apply_border_change();

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    apply_imgui_theme(g_imgui_theme);

    /* GLES 2 backend — `#version 100` GLSL, no VAOs, runs anywhere. */
    ImGui_ImplSDL2_InitForOpenGL(g_window, g_gl_context);
    ImGui_ImplOpenGL3_Init("#version 100");

    /* Build the post-process shader pipeline now that GL is current. */
    g_shader_pipeline = gb_shader_pipeline_create();
    if (g_shader_pipeline) {
        if (!gb_shader_pipeline_set_active_by_name(g_shader_pipeline,
                                                    g_active_shader_pref.c_str())) {
            gb_shader_pipeline_set_active_by_name(g_shader_pipeline, "sharp");
        }
    }

    if (!recreate_streaming_texture()) {
        SDL_GL_DeleteContext(g_gl_context);
        g_gl_context = NULL;
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return false;
    }
    update_game_viewport();

    /* LAN auto-discovery: load persisted UUID/nickname through hooks
     * so the network_discovery module owns generation while we own the
     * file-format. Auto-discover starts off — user toggles in the menu. */
    static GBLanPrefsHooks lan_hooks = {
        /* load_string */ [](const char* key, char* out, size_t out_size) -> bool {
            if (strcmp(key, "lan.uuid") == 0 && !g_lan_uuid_pref.empty()) {
                snprintf(out, out_size, "%s", g_lan_uuid_pref.c_str());
                return true;
            }
            if (strcmp(key, "lan.nickname") == 0 && !g_lan_nickname_pref.empty()) {
                snprintf(out, out_size, "%s", g_lan_nickname_pref.c_str());
                return true;
            }
            return false;
        },
        /* save_string */ [](const char* key, const char* value) {
            if (strcmp(key, "lan.uuid") == 0) g_lan_uuid_pref = value;
            else if (strcmp(key, "lan.nickname") == 0) g_lan_nickname_pref = value;
            save_runtime_preferences();
        },
    };
    gb_lan_set_prefs_hooks(&lan_hooks);
    gb_lan_init();
    gb_lan_set_advertised_port((uint16_t)g_link_port_pref);
    if (g_lan_enabled_pref) gb_lan_set_enabled(true);
    snprintf(g_link_nickname_edit, sizeof(g_link_nickname_edit), "%s", gb_lan_self_nickname());

    g_last_frame_time = SDL_GetTicks();

    return true;
}

static bool handle_binding_capture_event(const SDL_Event* event) {
    if (!g_binding_capture_active || !event) {
        return false;
    }

    switch (event->type) {
        case SDL_KEYDOWN:
            if (g_binding_capture_device != GB_CAPTURE_DEVICE_KEYBOARD || event->key.repeat != 0) {
                return true;
            }
            if (event->key.keysym.scancode == SDL_SCANCODE_ESCAPE ||
                event->key.keysym.scancode == SDL_SCANCODE_AC_BACK) {
                cancel_binding_capture();
                return true;
            }
            commit_binding_capture(make_binding(GB_INPUT_BINDING_KEY, event->key.keysym.scancode));
            return true;

        case SDL_CONTROLLERBUTTONDOWN:
            if (g_binding_capture_device != GB_CAPTURE_DEVICE_CONTROLLER) {
                return true;
            }
            if (event->cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
                cancel_binding_capture();
                return true;
            }
            commit_binding_capture(make_binding(GB_INPUT_BINDING_CONTROLLER_BUTTON, event->cbutton.button));
            return true;

        case SDL_CONTROLLERAXISMOTION:
            if (g_binding_capture_device != GB_CAPTURE_DEVICE_CONTROLLER) {
                return true;
            }
            if (event->caxis.value >= 16000) {
                commit_binding_capture(make_binding(GB_INPUT_BINDING_CONTROLLER_AXIS_POSITIVE, event->caxis.axis));
            } else if (event->caxis.value <= -16000) {
                commit_binding_capture(make_binding(GB_INPUT_BINDING_CONTROLLER_AXIS_NEGATIVE, event->caxis.axis));
            }
            return true;

        case SDL_KEYUP:
        case SDL_CONTROLLERBUTTONUP:
            return true;

        default:
            return false;
    }
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
    if (handle_binding_capture_event(event)) {
        return true;
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

        case SDL_AUDIODEVICEADDED:
            if (!event->adevice.iscapture) {
                refresh_audio_output_devices();
            }
            break;

        case SDL_AUDIODEVICEREMOVED:
            if (!event->adevice.iscapture) {
                refresh_audio_output_devices();
                if (g_audio_device != 0 && event->adevice.which == g_audio_device) {
                    reopen_audio_output_device(true);
                }
            }
            break;

        case SDL_CONTROLLERAXISMOTION: {
            uint8_t previous_dpad = g_joypad_dpad;
            uint8_t previous_buttons = g_joypad_buttons;

            if (event->caxis.axis >= 0 && event->caxis.axis < SDL_CONTROLLER_AXIS_MAX) {
                g_controller_axis_values[event->caxis.axis] = event->caxis.value;
            }
            update_controller_axis_binding_state();
            update_effective_joypad_state();
            update_runtime_action_state(ctx);
            if (ctx &&
                input_transition_creates_press(previous_dpad,
                                              previous_buttons,
                                              g_joypad_dpad,
                                              g_joypad_buttons,
                                              dpad_selected,
                                              buttons_selected)) {
                request_joypad_interrupt(ctx);
            }
            break;
        }

        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            const bool pressed = (event->type == SDL_CONTROLLERBUTTONDOWN);
            uint8_t previous_dpad = g_joypad_dpad;
            uint8_t previous_buttons = g_joypad_buttons;

            if (event->cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
                if (pressed) {
                    g_show_menu = !g_show_menu;
                }
                return true;
            }

            for (int action = 0; action < GB_INPUT_ACTION_COUNT; action++) {
                for (int slot = 0; slot < 2; slot++) {
                    if (binding_matches_controller_button(g_controller_bindings[action][slot], event->cbutton.button)) {
                        g_controller_button_binding_pressed[action][slot] = pressed;
                    }
                }
            }

            update_effective_joypad_state();
            update_runtime_action_state(ctx);
            if (ctx &&
                pressed &&
                input_transition_creates_press(previous_dpad,
                                              previous_buttons,
                                              g_joypad_dpad,
                                              g_joypad_buttons,
                                              dpad_selected,
                                              buttons_selected)) {
                request_joypad_interrupt(ctx);
            }
            break;
        }

        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            const bool pressed = (event->type == SDL_KEYDOWN);
            uint8_t previous_dpad = g_joypad_dpad;
            uint8_t previous_buttons = g_joypad_buttons;

            switch (event->key.keysym.scancode) {
                case SDL_SCANCODE_ESCAPE:
                case SDL_SCANCODE_AC_BACK:
                    if (pressed && event->key.repeat == 0) {
                        g_show_menu = !g_show_menu;
                    }
                    return true;

                default:
                    break;
            }

            for (int action = 0; action < GB_INPUT_ACTION_COUNT; action++) {
                for (int slot = 0; slot < 2; slot++) {
                    if (binding_matches_scancode(g_keyboard_bindings[action][slot], event->key.keysym.scancode)) {
                        g_keyboard_binding_pressed[action][slot] = pressed;
                    }
                }
            }

            update_effective_joypad_state();
            update_runtime_action_state(ctx);
            if (ctx &&
                pressed &&
                event->key.repeat == 0 &&
                input_transition_creates_press(previous_dpad,
                                              previous_buttons,
                                              g_joypad_dpad,
                                              g_joypad_buttons,
                                              dpad_selected,
                                              buttons_selected)) {
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
    /* Drain inbound BGB packets and apply them to the live serial state. */
    gb_serial_link_tick(ctx);

    /* Cheats (libretro .cht): iterate enabled GameShark codes and
     * write each value into RAM. No-op when nothing is enabled. */
    gb_cheats_tick(ctx);

    /* Debug server: poll TCP commands and block if paused */
    gb_debug_server_poll();
    gb_debug_server_wait_if_paused();

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
    /* Debug server: record frame state and check watchpoints */
    gb_debug_server_record_frame();
    gb_debug_server_check_watchpoints();

    /* Flush battery RAM to disk after no ERAM writes for ~5 seconds.
     * eram_dirty_frame tracks the LAST write, so we debounce properly
     * even if the game writes ERAM frequently (e.g. RTC updates). */
    if (g_registered_ctx && g_registered_ctx->eram_dirty) {
        g_registered_ctx->eram_dirty_frame = (uint32_t)g_frame_count;
        g_registered_ctx->eram_dirty = 0;
    }
    if (g_registered_ctx && g_registered_ctx->eram_dirty_frame != 0 &&
        (uint32_t)(g_frame_count - g_registered_ctx->eram_dirty_frame) > 300) {
        gb_context_save_ram(g_registered_ctx);
        g_registered_ctx->eram_dirty_frame = 0;
    }

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
    /* Check for debug server input override */
    int override = gb_debug_server_get_input_override();
    if (override >= 0) {
        /* Override bits: 0=Right,1=Left,2=Up,3=Down,4=A,5=B,6=Select,7=Start (active high)
         * GB joypad is active low, so invert */
        return (uint8_t)(~override & 0xFF);
    }
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
    if (g_turbo) {
        /* Skip frame pacing entirely — run as fast as possible */
        update_audio_stats_from_ring();
        audio_stats_tick(SDL_GetTicks64());
        g_last_frame_time = SDL_GetTicks();
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
    uint32_t speed_percent = (uint32_t)effective_speed_percent();
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
    bool audio_starved = audio_output_should_run() && g_audio_started && audio_fill < g_audio_low_watermark;

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

static void sdl_get_persistent_path(char* buffer, size_t size, const char* rom_name, const char* extension) {
    const std::string base_name = extract_path_leaf(rom_name);
    const std::string suffix = (extension && extension[0]) ? extension : ".sav";
    const std::string filename = base_name + suffix;

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

static void sdl_get_save_path(char* buffer, size_t size, const char* rom_name) {
    sdl_get_persistent_path(buffer, size, rom_name, ".sav");
}

static void sdl_get_rtc_path(char* buffer, size_t size, const char* rom_name) {
    sdl_get_persistent_path(buffer, size, rom_name, ".rtc");
}

static void context_storage_name(const GBContext* ctx, char* buffer, size_t size) {
    if (!buffer || size == 0) {
        return;
    }

    buffer[0] = '\0';
    if (ctx && ctx->save_id[0]) {
        snprintf(buffer, size, "%s", ctx->save_id);
        return;
    }

    if (ctx && ctx->rom && ctx->rom_size > 0x143) {
        char title[17];
        memset(title, 0, sizeof(title));
        memcpy(title, &ctx->rom[0x134], 16);
        for (int i = 0; i < 16; i++) {
            if (title[i] == 0 || title[i] < 32 || title[i] > 126) {
                title[i] = 0;
            }
        }
        if (title[0]) {
            snprintf(buffer, size, "%s", title);
            return;
        }
    }

    snprintf(buffer, size, "game");
}

static void sdl_get_savestate_path(char* buffer, size_t size, const GBContext* ctx, int slot) {
    char storage_name[64];
    char extension[32];
    context_storage_name(ctx, storage_name, sizeof(storage_name));
    if (slot < 0) {
        slot = 0;
    }
    if (slot >= GB_SAVESTATE_SLOT_COUNT) {
        slot = GB_SAVESTATE_SLOT_COUNT - 1;
    }
    snprintf(extension, sizeof(extension), ".state%d", slot + 1);
    sdl_get_persistent_path(buffer, size, storage_name, extension);
}

static bool savestate_slot_exists(const GBContext* ctx, int slot, std::string* out_path) {
    char filename[512];
    sdl_get_savestate_path(filename, sizeof(filename), ctx, slot);
    if (out_path) {
        *out_path = filename;
    }

    std::error_code ec;
    return fs::exists(fs::path(filename), ec) && !ec;
}

static bool save_savestate_slot(GBContext* ctx, int slot) {
    if (!ctx) {
        set_savestate_status("Save", slot, false, "No active game context");
        return false;
    }

    char filename[512];
    sdl_get_savestate_path(filename, sizeof(filename), ctx, slot);
    const bool success = gb_context_save_state_file(ctx, filename);
    set_savestate_status("Save", slot, success, filename);
    return success;
}

static bool load_savestate_slot(GBContext* ctx, int slot) {
    if (!ctx) {
        set_savestate_status("Load", slot, false, "No active game context");
        return false;
    }

    char filename[512];
    sdl_get_savestate_path(filename, sizeof(filename), ctx, slot);
    const bool success = gb_context_load_state_file(ctx, filename);
    if (success) {
        reset_audio_output_buffer(true);
        g_last_guest_framebuffer_valid = false;
        g_present_count = ctx->completed_frames;
        g_last_frame_time = SDL_GetTicks();
    }
    set_savestate_status("Load", slot, success, filename);
    return success;
}

static bool delete_savestate_slot(GBContext* ctx, int slot) {
    if (!ctx) {
        set_savestate_status("Delete", slot, false, "No active game context");
        return false;
    }

    char filename[512];
    sdl_get_savestate_path(filename, sizeof(filename), ctx, slot);
    std::error_code ec;
    const bool removed = fs::remove(fs::path(filename), ec);
    if (!removed && !ec) {
        set_savestate_status("Delete", slot, false, "Slot file does not exist");
        return false;
    }
    set_savestate_status("Delete", slot, removed && !ec, filename);
    return removed && !ec;
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

static bool sdl_load_rtc_data(GBContext* ctx, const char* rom_name, void* data, size_t size) {
    (void)ctx;
    char filename[512];
    sdl_get_rtc_path(filename, sizeof(filename), rom_name);

    FILE* f = fopen(filename, "rb");
    if (!f) return false;

    size_t read = fread(data, 1, size, f);
    fclose(f);

    return read == size;
}

static bool sdl_save_rtc_data(GBContext* ctx, const char* rom_name, const void* data, size_t size) {
    (void)ctx;
    char filename[512];
    sdl_get_rtc_path(filename, sizeof(filename), rom_name);

    FILE* f = fopen(filename, "wb");
    if (!f) return false;

    size_t written = fwrite(data, 1, size, f);
    fclose(f);

    return written == size;
}

/* Serial-byte fan-out: BGB peer link gets first refusal (it claims the
 * transfer when a peer is connected), and otherwise the virtual Game
 * Boy Printer hooks any traffic that matches its packet protocol.
 * Stray bytes outside both protocols fall through to the runtime's
 * "cable unplugged" 0xFF default. */
static bool g_printer_prefix_resolved = false;
static void cart_title_to_prefix(const GBContext* ctx, char* out, size_t cap);
static void resolve_printer_prefix_if_needed(GBContext* ctx) {
    if (g_printer_prefix_resolved || !g_printer) return;
    if (!ctx || !ctx->rom) return;
    std::string prints_dir = (fs::current_path() / "prints").string();
    char prefix[64];
    cart_title_to_prefix(ctx, prefix, sizeof(prefix));
    gb_printer_set_output(g_printer, prints_dir.c_str(), prefix);
    fprintf(stderr, "[PRINTER] Output: %s/%s_NNNN.png\n",
            prints_dir.c_str(), prefix);
    g_printer_prefix_resolved = true;
}
static void platform_on_serial_byte(GBContext* ctx, uint8_t outgoing) {
    gb_serial_link_on_serial_byte(ctx, outgoing);
    if (ctx->serial_transfer.deferred) return;
    resolve_printer_prefix_if_needed(ctx);
    if (g_printer && gb_printer_on_serial_byte(g_printer, ctx, outgoing)) return;
}

/* Sanitize cart-title bytes (0x134-0x143) into a safe filename prefix
 * — lowercase, [a-z0-9_] only, max 32 chars. */
static void cart_title_to_prefix(const GBContext* ctx, char* out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    if (!ctx || !ctx->rom || ctx->rom_size < 0x144) {
        snprintf(out, cap, "print");
        return;
    }
    size_t n = 0;
    for (int i = 0; i < 16 && n + 1 < cap; i++) {
        uint8_t b = ctx->rom[0x134 + i];
        if (b == 0) break;
        char c = (char)b;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        else if (c >= 'a' && c <= 'z') {}
        else if (c >= '0' && c <= '9') {}
        else c = '_';
        out[n++] = c;
    }
    while (n > 0 && out[n - 1] == '_') n--;
    out[n] = '\0';
    if (n == 0) snprintf(out, cap, "print");
}

static GBHardwareModePref hardware_mode_pref_from_string(const std::string& s) {
    if (s == "dmg") return GB_HARDWARE_MODE_DMG;
    if (s == "sgb") return GB_HARDWARE_MODE_SGB;
    if (s == "cgb") return GB_HARDWARE_MODE_CGB;
    if (s == "gba") return GB_HARDWARE_MODE_GBA;
    return GB_HARDWARE_MODE_AUTO;
}
static const char* hardware_mode_pref_to_string(GBHardwareModePref mode) {
    switch (mode) {
        case GB_HARDWARE_MODE_DMG: return "dmg";
        case GB_HARDWARE_MODE_SGB: return "sgb";
        case GB_HARDWARE_MODE_CGB: return "cgb";
        case GB_HARDWARE_MODE_GBA: return "gba";
        case GB_HARDWARE_MODE_AUTO:
        default:                   return "auto";
    }
}

/* Pick a concrete hardware mode for a cart from its header bits.
 * Mirrors the AUTO branch of gb_context_load_rom — kept in sync
 * because the platform UI never shows AUTO; it shows the resolved
 * choice as the default and lets the user override.
 *
 * Order of preference for dual-mode (SGB + CGB) carts: SGB. The cart
 * border + region-tinted palettes Pokemon Yellow / Gold / Silver
 * authored for SGB are visible features that CGB-mode loses in
 * exchange for marginally richer per-pixel color. Users who prefer
 * the CGB look can flip the dropdown. */
static GBHardwareModePref hardware_mode_default_for_cart(const GBContext* ctx) {
    if (!ctx) return GB_HARDWARE_MODE_DMG;
    if (ctx->config.cartridge_requires_cgb) return GB_HARDWARE_MODE_CGB;
    if (ctx->config.cartridge_supports_sgb) return GB_HARDWARE_MODE_SGB;
    if (ctx->config.cartridge_supports_cgb) return GB_HARDWARE_MODE_CGB;
    return GB_HARDWARE_MODE_DMG;
}

void gb_platform_set_game_id(GBContext* ctx, const char* game_id) {
    if (!game_id || !*game_id) return;
    g_active_game_id = game_id;
    g_active_hardware_mode_pref = hardware_mode_default_for_cart(ctx);
    auto it = g_per_game_prefs.find(g_active_game_id);
    if (it != g_per_game_prefs.end()) {
        auto& prefs = it->second;

        auto hwm = prefs.find("hardware_mode");
        if (hwm != prefs.end()) {
            GBHardwareModePref saved = hardware_mode_pref_from_string(hwm->second);
            if (saved != GB_HARDWARE_MODE_AUTO) {
                g_active_hardware_mode_pref = saved;
            }
        }

        /* Per-game Look settings: each one inherits the global default
         * if no per-game value is saved, so a fresh cart picks up
         * whatever was last set globally. Subsequent menu changes are
         * scoped to this cart only. */
        auto pal = prefs.find("palette");
        if (pal != prefs.end()) {
            int idx = atoi(pal->second.c_str());
            if (idx >= 0 && idx < (int)IM_ARRAYSIZE(g_palette_names)) {
                g_palette_idx = idx;
            }
        }
        auto cgbp = prefs.find("cgb_palette");
        if (cgbp != prefs.end() && ctx) {
            int idx = atoi(cgbp->second.c_str());
            /* -1 (AUTO) through 11 (Inverted) — anything else gets
             * clamped back to AUTO. */
            if (idx >= -1 && idx <= 11) {
                ctx->cgb_compat_palette_override = idx;
            }
        }
        auto cc = prefs.find("color_correction");
        if (cc != prefs.end()) {
            int idx = atoi(cc->second.c_str());
            if (idx >= 0 && idx <= 2) {
                g_color_correction = idx;
            }
        } else {
            /* No saved choice — pick a sensible default from hardware
             * mode. GBA picks GBA correction; CGB picks GBC; everything
             * else stays Off (irrelevant in DMG/SGB anyway). */
            if (g_active_hardware_mode_pref == GB_HARDWARE_MODE_GBA) {
                g_color_correction = 2;
            } else if (g_active_hardware_mode_pref == GB_HARDWARE_MODE_CGB) {
                g_color_correction = 1;
            } else {
                g_color_correction = 0;
            }
        }
        auto sha = prefs.find("shader");
        if (sha != prefs.end()) {
            g_active_shader_pref = sha->second;
            if (g_shader_pipeline) {
                gb_shader_pipeline_set_active_by_name(g_shader_pipeline,
                                                     sha->second.c_str());
            }
        }
        auto sc = prefs.find("sgb_colors");
        if (sc != prefs.end()) {
            g_sgb_colors_pref = (sc->second != "0");
            if (ctx && ctx->sgb) {
                gb_sgb_set_display_palettes((GBSgbState*)ctx->sgb,
                                            g_sgb_colors_pref);
            }
        }
        auto sb = prefs.find("sgb_border");
        if (sb != prefs.end()) {
            g_sgb_cart_border_enabled = (sb->second != "0");
            if (ctx && ctx->sgb) {
                gb_sgb_set_display_border((GBSgbState*)ctx->sgb,
                                          g_sgb_cart_border_enabled);
            }
        }
#ifdef GBRT_HAVE_GBCAM
        /* Per-game camera device preference. Gets bridged into the gbcam
         * backend by setting GBCAM_DEVICE so the cart's lazy first-open
         * picks it up automatically. */
        auto camdev = prefs.find("camera_device");
        if (camdev != prefs.end() && !camdev->second.empty()) {
            setenv("GBCAM_DEVICE", camdev->second.c_str(), 1);
        }
#endif
        auto cbe = prefs.find("custom_border");
        if (cbe != prefs.end()) {
            g_border_enabled = (cbe->second != "0");
        }
        auto cbf = prefs.find("custom_border_file");
        if (cbf != prefs.end()) {
            g_border_loaded_filename = cbf->second;
        }
    }

    if (ctx) {
        ctx->hardware_mode_pref = g_active_hardware_mode_pref;
    }

    /* Look for a cached SGB cart border for this cart. Populates the
     * texture even when the SGB engine isn't running this session
     * (CGB/DMG modes), so the SGB Cart Border toggle still works on
     * later launches. */
    try_load_sgb_cart_border_cache();

    /* Scan cheats/<game_id>/*.cht. Cart-agnostic -- works on any
     * Game Boy game that has libretro cheat files dropped into
     * the cheats/ folder. */
    gb_cheats_load(g_active_game_id.c_str());
}

/* Helpers for menu handlers to scope a Look setting to the active
 * cart. No-op when no game is loaded (e.g. during the launcher). */
static void set_active_game_pref(const char* key, const std::string& value) {
    if (g_active_game_id.empty() || !key || !*key) return;
    g_per_game_prefs[g_active_game_id][key] = value;
}
static void set_active_game_pref_bool(const char* key, bool value) {
    set_active_game_pref(key, value ? "1" : "0");
}
static void set_active_game_pref_int(const char* key, int value) {
    set_active_game_pref(key, std::to_string(value));
}

void gb_platform_register_context(GBContext* ctx) {
    g_registered_ctx = ctx;
    GBPlatformCallbacks callbacks = {
        .on_audio_sample = on_audio_sample,
        .on_serial_byte = platform_on_serial_byte,
        .load_battery_ram = sdl_load_battery_ram,
        .save_battery_ram = sdl_save_battery_ram,
        .load_rtc_data = sdl_load_rtc_data,
        .save_rtc_data = sdl_save_rtc_data
    };
    gb_set_platform_callbacks(ctx, &callbacks);

    /* Spin up the printer once. The actual output path/prefix is
     * resolved lazily on the first serial byte — register_context runs
     * before gb_context_load_rom, so ctx->rom is still NULL here. */
    if (!g_printer) g_printer = gb_printer_create();
    g_printer_prefix_resolved = false;

    /* gb_context_load_rom auto-enabled the SGB engine based on the cart
     * header. We leave the engine on (so the cart's CheckSGB latches
     * wOnSGB=1 and palette/border packets keep flowing) and only flip
     * the display layers to match the user's prefs. */
    if (ctx && ctx->sgb) {
        gb_sgb_set_display_palettes((GBSgbState*)ctx->sgb, g_sgb_colors_pref);
        gb_sgb_set_display_border((GBSgbState*)ctx->sgb, g_sgb_cart_border_enabled);
    }

    /* Bring up the BGB-protocol link if GB_LINK_LISTEN or GB_LINK_CONNECT
     * is set in the environment. No-op otherwise. */
    gb_serial_link_init_from_env();

    /* Initialize TCP debug server */
    gb_debug_server_set_context(ctx);
    gb_debug_server_init(0); /* default port 4370 */

    /* Opt-in extended view (widescreen): resolve the width request against
     * the game capability. SDL path only — differential/cosim runs never
     * arm, so verify output stays native and byte-identical. */
    gb_ws_arm(ctx);
    if (g_gbws_active) {
        recreate_streaming_texture();
        apply_window_scale_preset();
    }
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

bool gb_platform_consume_restart_requested(void) {
    return false;
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

void gb_platform_set_dump_cycle_frames(const char* frames) { (void)frames; }
void gb_platform_check_cycle_dump(GBContext* ctx) { (void)ctx; }

void gb_platform_set_screenshot_prefix(const char* prefix) { (void)prefix; }

void gb_platform_register_context(GBContext* ctx) { (void)ctx; }

#endif /* GB_HAS_SDL2 */
