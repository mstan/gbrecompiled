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
#include "rewind.h"
#include "color_lut.h"  /* present-time screen-color LUT (opt-in, default raw) */
extern "C" {
#include "keybinds.h"
#include "gb_host_paths.h"
#ifdef RECOMP_LAUNCHER
#include "recomp_runtime_ui.h"
#include "launcher_ui_seam.h"   /* gb_launcher_preboot() — recomp-ui pre-boot seam */
#endif
}

/* Forward declaration for debug server context setter */
extern "C" void gb_debug_server_set_context(GBContext *ctx);

#ifdef GB_HAS_SDL2
#include <SDL.h>
#include <SDL_opengles2.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <ctime>
#include <fstream>
#include <map>
#include <filesystem>
#include <string>
#include <vector>
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#define IMGUI_IMPL_OPENGL_ES2
#include "backends/imgui_impl_opengl3.h"

#include "shader_pipeline.h"
#include "librashader_chain.h"
#include "slang_preset.h"
#include "gl_program_cache.h"
#include "game_extras.h"  /* game_draw_overlay() hook (no-op default in gbrt) */
#include "gb_widescreen.h"
#include "gb_custom_view.h" /* opt-in extended view: render width + arming */

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

/* librashader .slangp preset (see librashader_chain.h). The path is stored
 * relative to the shader folder when it lies inside it, so the folder can move
 * with the executable. Parameter overrides belong to that one preset and are
 * dropped when another is picked. Loads are queued and applied before the next
 * frame is drawn: creating a chain rebinds GL state, which must not happen in
 * the middle of an ImGui frame. */
static std::string g_slang_preset_pref;
static std::string g_slang_dir_pref;
static std::map<std::string, float> g_slang_param_prefs;
/* The preset the menu's edited copy (saved:.builder.slangp) was made from. */
static std::string g_slang_builder_origin;
static std::string g_slang_delete_status;   /* what the last Delete did, until another pick */
static bool g_slang_fill_window = false;
static bool g_slang_load_pending = false;
static bool g_slang_open_section = false;
/* Picked in the menu rather than restored at start-up: a preset that crashed
 * a probe before is tested again only when the user asks for it again. */
static bool g_slang_explicit = false;

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
static int g_swap_interval = 0;  /* last given to SDL_GL_SetSwapInterval */
static bool g_audio_started = false;
static uint32_t g_audio_start_threshold = 0;

/* Performance timing diagnostics */
static double g_timing_render_total = 0.0;
static double g_timing_vsync_total = 0.0;
static uint32_t g_timing_frame_count = 0;
static GBPlatformTimingInfo g_last_timing = {};

/* Menu State: g_show_menu is the settings window; the runtime menu (the one
 * Escape opens) is g_runtime_ui. */
static bool g_show_menu = false;
static bool g_show_overlay = false;
#ifdef RECOMP_LAUNCHER
static RecompRuntimeUi* g_runtime_ui = NULL;
#endif
/* RetroArch's Pause Content When Menu Is Active (menu_pause_libretro, on by
 * default): while a menu is up the game waits in gb_platform_vsync, silent. */
static bool g_menu_pauses_game = true;
/* How a menu covers the game, so a shader's effect shows while it is tuned:
 * Game Dimming is how dark the game behind a menu gets (a black layer, 0 = not
 * dimmed; 60 is about what the runtime menu always had), Menu Opacity how
 * solid the menus' own backgrounds are. */
static constexpr int GB_MENU_DIM_DEFAULT = 60;
static int g_menu_dim_percent = GB_MENU_DIM_DEFAULT;
static int g_menu_opacity_percent = 100;
/* The settings window's last frame had a text field being typed into or a
 * popup (a combo's list, a confirmation) open: Escape is theirs then. */
static bool g_menu_text_input = false;
static bool g_menu_popup_open = false;

static bool menu_open(void) {
#ifdef RECOMP_LAUNCHER
    if (g_runtime_ui && recomp_runtime_ui_is_open(g_runtime_ui)) return true;
#endif
    return g_show_menu;
}

static bool menu_holds_game(void) {
    return g_menu_pauses_game && menu_open();
}

static float menu_opacity(void) {
    return (float)g_menu_opacity_percent / 100.0f;
}

static float menu_dim(void) {
    return (float)g_menu_dim_percent / 100.0f;
}

/* gb_platform_request_window_shot: written at the next present. */
static std::string g_window_shot_path;

/* Restart Game, Return to Launcher and Quit (defined with the save states).
 * g_boot_state is the machine before its first frame. */
static std::vector<uint8_t> g_boot_state;
static bool g_restart_pending = false;
static bool can_restart_game(void);
static void request_restart_game(void);
static void run_pending_restart(void);
static bool launcher_available(void);
static void request_exit(GBPlatformExitAction action);

static int g_speed_percent = 100;
/* Speeds of the Fast Forward (Hold) and Fast Forward (toggle) shortcuts; the
 * toggle was called Max Speed, which its variables and prefs keys (max_speed,
 * speed.max_percent) still say. Either can be Unlimited, no frame limiter at
 * all, as RetroArch's Fast-Forward Rate 0 ("no limit"); it compares above
 * every other speed, and prefs store it as 0. */
static constexpr int GB_SPEED_UNLIMITED = INT_MAX;
static int g_fast_forward_speed_percent = 250;
static int g_max_speed_percent = GB_SPEED_UNLIMITED;
/* RetroArch's "Fast-Forward Frame Skip" (fastforward_frameskip, on by
 * default): above 100% only as many frames are drawn as the display shows. */
static bool g_fast_forward_frameskip = true;
static uint64_t g_frames_skipped = 0;
static constexpr int GB_SHORTCUT_SPEED_MIN_PERCENT = 110;
static constexpr int GB_SHORTCUT_SPEED_MAX_PERCENT = 1000;
/* Sound at speeds other than 100%, RetroArch's fast-forward audio settings:
 * Mute ("Mute When Fast-Forwarding"), Normal Pitch (its default: what the
 * device cannot take in time is dropped) or Sped Up ("Speed Up Audio When
 * Fast-Forwarding"). Below 100% the sound is slowed down unless muted, as in
 * RetroArch's slow motion. */
enum GBSpeedAudioMode { GB_SPEED_AUDIO_MUTE = 0, GB_SPEED_AUDIO_NORMAL_PITCH = 1, GB_SPEED_AUDIO_SPED_UP = 2 };
static int g_speed_audio_mode = GB_SPEED_AUDIO_NORMAL_PITCH;
static const char* g_speed_audio_mode_names[] = { "Mute", "Normal Pitch", "Sped Up" };
static int g_palette_idx = 0;
/* Color correction: 0=Off, 1=GBC (modern LCDs viewing GBC-tinted output),
 * 2=GBA (compensate for AGB LCD's darker / washed-out look). Applied
 * per-pixel during the framebuffer upload. Defaults to Off but gets
 * auto-bumped to a matching mode when the user picks CGB / GBA
 * hardware. Only relevant in CGB/GBA hardware modes — hidden in DMG/SGB. */
static int g_color_correction = 0;
static bool g_smooth_lcd_transitions = true;
/* Preemptive frames (preempt_before_frame): -1 = the game's default. */
static int g_preemptive_frames_pref = -1;
static int preemptive_frames(void);
/* Rewind (rewind_before_frame), RetroArch's settings and defaults but on:
 * Rewind Frames is how many frames apart states are kept (and so how fast
 * holding the key goes back), the buffer how much memory holds them. */
static bool g_rewind_enabled = true;
static int g_rewind_granularity = 1;
static int g_rewind_buffer_mb = 20;
static constexpr int GB_REWIND_MAX_GRANULARITY = 32;
static constexpr int GB_REWIND_MAX_BUFFER_MB = 1024;
static void rewind_free(void);
static bool g_launcher_return_enabled = false;
static bool g_benchmark_mode = false;
/* Headless (GBRECOMP_HEADLESS=1): dummy SDL video/audio, no window, no GL —
 * but UNLIKE plain benchmark mode the SDL event queue is still drained
 * through handle_runtime_event(), so a debug client can inject keystrokes
 * and exercise the real binding layer with nothing on screen. */
static bool g_headless_mode = false;
/* Set by gb_platform_inject_sdl_event(). Forces the next poll to drain the
 * queue even in a mode that normally skips it, so an injected event is
 * never stranded (benchmark mode, or a frame the launcher owns). */
static bool g_injected_event_pending = false;
/* The context the live poll loop last ran with, so the paused pump can hand
 * events to the same handler with the same context. */
static GBContext* g_last_poll_ctx = NULL;
/* Tri-state fullscreen: 0 off (windowed), 1 borderless
 * (SDL_WINDOW_FULLSCREEN_DESKTOP), 2 exclusive (SDL_WINDOW_FULLSCREEN, real
 * display-mode change). Mirrors recomp-ui's universal "video.fullscreen"
 * pref and the SNES/Genesis runtimes' g_config.fullscreen /
 * g_app_config.fullscreen. Boolean "is fullscreen active" checks below use
 * `g_fullscreen_mode != 0` rather than a separate bool so there's a single
 * source of truth. */
static int g_fullscreen_mode = 0;
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
    GB_INPUT_ACTION_REWIND = 9,
    GB_INPUT_ACTION_TOGGLE_MAX_SPEED = 10,
    GB_INPUT_ACTION_SAVE_STATE = 11,
    GB_INPUT_ACTION_LOAD_STATE = 12,
    GB_INPUT_ACTION_PREVIOUS_STATE_SLOT = 13,
    GB_INPUT_ACTION_NEXT_STATE_SLOT = 14,
    GB_INPUT_ACTION_TOGGLE_OVERLAY = 15,
    GB_INPUT_ACTION_TOGGLE_MUTE = 16,
    GB_INPUT_ACTION_TOGGLE_MENU = 17,
    GB_INPUT_ACTION_PAUSE = 18,
    GB_INPUT_ACTION_FRAME_ADVANCE = 19,
    GB_INPUT_ACTION_COUNT = 20,
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
static int g_savestate_slot = 0;
static std::string g_savestate_status;
/* On-screen messages (notify.*): state saves/loads, slot changes, rewinding
 * and fast forward. */
static bool g_notify_save_load = true;
static bool g_notify_slot = true;
static bool g_notify_rewind = true;
static bool g_notify_fast_forward = true;
static std::string g_state_notice;
static uint64_t g_state_notice_ms = 0;
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
static const char* g_fullscreen_mode_names[] = {
    "Off",
    "Borderless",
    "Exclusive",
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
static std::vector<uint32_t> g_custom_framebuffer;   /* sized to the resolved view */
/* The custom view's native picture presented on its own this frame, scaled to
 * the window by gb_custom_native_scaling instead of centered in the view. */
static bool g_native_presented = false;
/* Window pixels per game pixel gb_custom_fit asked for this frame's view
 * (0: the scaling mode decides). */
static double g_custom_present_scale = 0.0;
/* The view's size (the window's size and borders follow it). */
static int view_width(void) {
    return gb_custom_render ? gb_custom_view_width : gb_ws_render_width();
}
static int view_height(void) {
    return gb_custom_render ? gb_custom_view_height : GB_SCREEN_HEIGHT;
}
/* The picture presented this frame: the view (or the part gb_custom_fit
 * kept), or the native picture on its own. */
static int presentation_width(void) {
    if (g_native_presented) return GB_SCREEN_WIDTH;
    return gb_custom_render ? gb_custom_width : gb_ws_render_width();
}
static int presentation_height(void) {
    if (g_native_presented) return GB_SCREEN_HEIGHT;
    return gb_custom_render ? gb_custom_height : GB_SCREEN_HEIGHT;
}
/* Most recent frame handed to the presentation path, published by
 * render_frame_internal() for gb_platform_get_presented_frame(). */
static const uint32_t* g_presented_frame = NULL;
static int g_presented_width = 0;
static int g_presented_height = 0;
static uint32_t g_lcd_off_framebuffer[GB_MAX_FRAMEBUFFER_SIZE];
static uint32_t g_last_guest_framebuffer[GB_MAX_FRAMEBUFFER_SIZE];
static bool g_lcd_off_framebuffer_initialized = false;
static bool g_last_guest_framebuffer_valid = false;
static uint64_t g_present_count = 0;
static GBContext* g_registered_ctx = NULL;
static GBInputBinding g_keyboard_bindings[GB_INPUT_ACTION_COUNT][2] = {};
static GBInputBinding g_controller_bindings[GB_INPUT_ACTION_COUNT][2] = {};
/* Snapshot of the shipped defaults, filled by set_default_input_bindings().
 * sanitise_binding_conflicts() uses it to tell a slot the player explicitly
 * claimed from one that merely still holds its factory value. */
static GBInputBinding g_default_keyboard_bindings[GB_INPUT_ACTION_COUNT][2] = {};
static GBInputBinding g_default_controller_bindings[GB_INPUT_ACTION_COUNT][2] = {};
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
static bool g_debug_fast_forward = false;   /* held by the debug server's `speed` */
static bool g_max_speed_mode = false;
/* User pause / frame advance, for measuring input latency. The emulator waits
 * in gb_platform_vsync() after the finished frame is on screen; each advance
 * lets one more frame through. g_pause_input is the joypad state (dpad low
 * nibble, buttons high, active low) the last advanced frame ran with, and
 * g_pause_input_frames counts consecutive advanced frames that saw it. */
static bool g_user_paused = false;
static int g_frame_advance_pending = 0;
static uint8_t g_pause_input = 0xFF;
static uint32_t g_pause_input_frames = 0;
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
    "Rewind (Hold)",
    "Fast Forward",   /* a toggle; "toggle_max_speed" in prefs, its old name */
    "Save State",
    "Load State",
    "Previous State Slot",
    "Next State Slot",
    "Toggle Overlay",
    "Toggle Mute",
    "Toggle Menu",
    "Pause",
    "Frame Advance",
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
static bool audio_output_should_run(void);
static void on_speed_changed(bool audio_was_running);
static char* trim_ascii(char* text);
static void update_effective_joypad_state(void);
static void save_runtime_preferences(void);
static void binding_to_config_value(const GBInputBinding& binding, char* out, size_t out_size);
static int clear_conflicting_bindings(GBBindingCaptureDevice device,
                                      GBInputAction keep_action,
                                      int keep_slot,
                                      const GBInputBinding& binding,
                                      bool log_drops);
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

#define MAX_SCRIPT_ENTRIES 4096
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

/* Headless is requested purely by environment so every game inherits it without
 * a per-title flag. It implies benchmark mode (no window, no GL, no pacing, no
 * audio device) and additionally keeps the SDL event queue drained, which is
 * what makes an injected keystroke take the ordinary path. */
static bool headless_requested(void) {
    return env_flag_enabled("GBRECOMP_HEADLESS");
}

/* Default fullscreen mode for a fresh install / "Reset to Defaults".
 * Android has no meaningful windowed mode, so it defaults to borderless
 * (matches the pre-tri-state behavior, which always used
 * SDL_WINDOW_FULLSCREEN_DESKTOP there). Desktop defaults to windowed. */
static int platform_default_fullscreen_mode(void) {
#if defined(__ANDROID__)
    return 1;
#else
    return 0;
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
    /* The shortcuts only ever speed the game up. */
    if (g_max_speed_mode && speed_percent < g_max_speed_percent) {
        speed_percent = g_max_speed_percent;
    }
    if (g_fast_forward_active && speed_percent < g_fast_forward_speed_percent) {
        speed_percent = g_fast_forward_speed_percent;
    }
    return speed_percent;
}

/* RetroArch's "Fast forward." while either Fast Forward shortcut is on, with
 * the speed it runs at; empty otherwise. */
static std::string fast_forward_status_text(void) {
    if (!g_max_speed_mode && !g_fast_forward_active) return "";
    const int speed = effective_speed_percent();
    return speed == GB_SPEED_UNLIMITED ? "Fast forward (unlimited)."
                                       : "Fast forward (" + std::to_string(speed) + "%).";
}

/* A shortcut speed as prefs and the debug server give it: 110-1000, or 0 for
 * Unlimited. Anything else leaves it as it is. */
static void set_shortcut_speed(int* percent, long value) {
    if (value == 0) {
        *percent = GB_SPEED_UNLIMITED;
    } else if (value >= GB_SHORTCUT_SPEED_MIN_PERCENT && value <= GB_SHORTCUT_SPEED_MAX_PERCENT) {
        *percent = (int)value;
    }
}

static int shortcut_speed_pref(int percent) {
    return percent == GB_SPEED_UNLIMITED ? 0 : percent;
}

/* V-Sync makes each present wait for the display's refresh, which holds the
 * game at the refresh rate. Above 100% it is off, as RetroArch turns it off
 * while fast-forwarding (driver_set_nonblock_state), so the speed shortcuts
 * work the same with it on or off. */
static void update_swap_interval(void) {
    const int interval = (g_vsync && effective_speed_percent() <= 100) ? 1 : 0;
    if (interval != g_swap_interval) {
        SDL_GL_SetSwapInterval(interval);
        g_swap_interval = interval;
    }
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

    /* One physical input drives exactly one action. Before this slot takes the
     * binding, drop the same physical input from every other action/slot of the
     * same device, so a key the player just claimed cannot keep silently firing
     * whatever it used to be a (usually defaulted) secondary binding for. This
     * is the fix for "rebind A to the A key and jumping also walks left": key A
     * shipped as the secondary binding of LEFT. */
    clear_conflicting_bindings(device, action, slot, binding, true);

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
        case GB_INPUT_ACTION_REWIND: return "rewind";
        case GB_INPUT_ACTION_TOGGLE_MAX_SPEED: return "toggle_max_speed";
        case GB_INPUT_ACTION_SAVE_STATE: return "save_state";
        case GB_INPUT_ACTION_LOAD_STATE: return "load_state";
        case GB_INPUT_ACTION_PREVIOUS_STATE_SLOT: return "previous_state_slot";
        case GB_INPUT_ACTION_NEXT_STATE_SLOT: return "next_state_slot";
        case GB_INPUT_ACTION_TOGGLE_OVERLAY: return "toggle_overlay";
        case GB_INPUT_ACTION_TOGGLE_MUTE: return "toggle_mute";
        case GB_INPUT_ACTION_TOGGLE_MENU: return "toggle_menu";
        case GB_INPUT_ACTION_PAUSE: return "pause";
        case GB_INPUT_ACTION_FRAME_ADVANCE: return "frame_advance";
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

static bool bindings_identical(const GBInputBinding& a, const GBInputBinding& b) {
    return a.kind == b.kind && a.code == b.code;
}

static GBInputBinding* binding_table_for_device(GBBindingCaptureDevice device) {
    if (device == GB_CAPTURE_DEVICE_KEYBOARD) {
        return &g_keyboard_bindings[0][0];
    }
    if (device == GB_CAPTURE_DEVICE_CONTROLLER) {
        return &g_controller_bindings[0][0];
    }
    return NULL;
}

/* Clear `binding` out of every action/slot of `device` except (keep_action,
 * keep_slot), so one physical input drives exactly one action. Returns how many
 * slots were cleared. An unbound / invalid binding is a no-op, so "Clear" never
 * cascades. A physical input is the (kind, code) pair; axis + and axis - on the
 * same axis are deliberately NOT the same input, being opposite directions of
 * one stick that legitimately drive two actions. */
static int clear_conflicting_bindings(GBBindingCaptureDevice device,
                                      GBInputAction keep_action,
                                      int keep_slot,
                                      const GBInputBinding& binding,
                                      bool log_drops) {
    GBInputBinding* table = binding_table_for_device(device);
    if (!table || !binding_is_valid(binding)) {
        return 0;
    }

    const bool keyboard = (device == GB_CAPTURE_DEVICE_KEYBOARD);
    int cleared = 0;
    for (int action = 0; action < GB_INPUT_ACTION_COUNT; action++) {
        for (int slot = 0; slot < 2; slot++) {
            if (action == (int)keep_action && slot == keep_slot) {
                continue;
            }
            GBInputBinding& existing = table[action * 2 + slot];
            if (!bindings_identical(existing, binding)) {
                continue;
            }

            if (log_drops) {
                fprintf(stderr,
                        "[SDL] Input conflict: %s \"%s\" was also bound to %s slot %d; cleared it "
                        "(it now drives %s only)\n",
                        keyboard ? "key" : "controller input",
                        binding_display_label(binding).c_str(),
                        g_input_action_names[action],
                        slot + 1,
                        g_input_action_names[keep_action]);
            }

            existing = make_binding(GB_INPUT_BINDING_NONE, 0);
            if (keyboard) {
                g_keyboard_binding_pressed[action][slot] = false;
            } else {
                g_controller_button_binding_pressed[action][slot] = false;
                g_controller_axis_binding_pressed[action][slot] = false;
            }
            cleared++;
        }
    }
    return cleared;
}

static void set_default_audio_preferences(void) {
    g_audio_output_enabled = true;
    g_audio_muted = false;
    g_audio_latency_ms = 80;
    g_audio_volume_percent = 100;
    g_audio_target_device_name.clear();
    g_speed_audio_mode = GB_SPEED_AUDIO_NORMAL_PITCH;
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
    g_keyboard_bindings[GB_INPUT_ACTION_REWIND][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_R);
    g_keyboard_bindings[GB_INPUT_ACTION_TOGGLE_MAX_SPEED][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_GRAVE);
    g_keyboard_bindings[GB_INPUT_ACTION_SAVE_STATE][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F5);
    g_keyboard_bindings[GB_INPUT_ACTION_LOAD_STATE][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F8);
    g_keyboard_bindings[GB_INPUT_ACTION_PREVIOUS_STATE_SLOT][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F6);
    g_keyboard_bindings[GB_INPUT_ACTION_NEXT_STATE_SLOT][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F7);
    g_keyboard_bindings[GB_INPUT_ACTION_TOGGLE_OVERLAY][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F1);
    g_keyboard_bindings[GB_INPUT_ACTION_TOGGLE_MUTE][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_M);
    g_keyboard_bindings[GB_INPUT_ACTION_TOGGLE_MENU][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_F10);
    g_keyboard_bindings[GB_INPUT_ACTION_PAUSE][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_P);
    g_keyboard_bindings[GB_INPUT_ACTION_FRAME_ADVANCE][0] = make_binding(GB_INPUT_BINDING_KEY, SDL_SCANCODE_O);

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

    memcpy(g_default_keyboard_bindings, g_keyboard_bindings, sizeof(g_default_keyboard_bindings));
    memcpy(g_default_controller_bindings, g_controller_bindings, sizeof(g_default_controller_bindings));

    clear_all_binding_pressed_state();
}

/* Sanitise a bindings table loaded from disk so every physical input appears at
 * most once. Files written before the conflict rule existed can bind one key to
 * two actions -- the defaults fill slot 1 with WASD/JK, and the rebinding UIs
 * used to write slot 0 without clearing anything -- which made the key silently
 * drive both.
 *
 * runtime_prefs.ini carries no edit timestamps and is written in a fixed key
 * order, so "the action the user most recently edited" is NOT derivable from the
 * file. The documented fallback is used instead:
 *   1. a slot holding something OTHER than its factory default beats a slot that
 *      still holds its default (the player claimed the first one on purpose),
 *   2. then slot 0 beats slot 1 (slot 0 is what both rebinding UIs write),
 *   3. then the lower action index wins, purely so the result is deterministic.
 * Everything dropped is logged. */
static int sanitise_binding_conflicts(GBInputBinding table[GB_INPUT_ACTION_COUNT][2],
                                      const GBInputBinding defaults[GB_INPUT_ACTION_COUNT][2],
                                      const char* device_label) {
    int dropped = 0;
    for (int action = 0; action < GB_INPUT_ACTION_COUNT; action++) {
        for (int slot = 0; slot < 2; slot++) {
            /* Copy: the drop loop below can clear this very slot when another
             * slot is the better claimant. */
            const GBInputBinding binding = table[action][slot];
            if (!binding_is_valid(binding)) {
                continue;
            }

            int best_action = action;
            int best_slot = slot;
            bool best_claimed = !bindings_identical(binding, defaults[action][slot]);

            for (int other_action = 0; other_action < GB_INPUT_ACTION_COUNT; other_action++) {
                for (int other_slot = 0; other_slot < 2; other_slot++) {
                    if (other_action == best_action && other_slot == best_slot) {
                        continue;
                    }
                    if (!bindings_identical(table[other_action][other_slot], binding)) {
                        continue;
                    }

                    const bool claimed =
                        !bindings_identical(binding, defaults[other_action][other_slot]);
                    bool wins;
                    if (claimed != best_claimed) {
                        wins = claimed;
                    } else if (other_slot != best_slot) {
                        wins = (other_slot < best_slot);
                    } else {
                        wins = (other_action < best_action);
                    }

                    if (wins) {
                        best_action = other_action;
                        best_slot = other_slot;
                        best_claimed = claimed;
                    }
                }
            }

            for (int other_action = 0; other_action < GB_INPUT_ACTION_COUNT; other_action++) {
                for (int other_slot = 0; other_slot < 2; other_slot++) {
                    if (other_action == best_action && other_slot == best_slot) {
                        continue;
                    }
                    if (!bindings_identical(table[other_action][other_slot], binding)) {
                        continue;
                    }
                    fprintf(stderr,
                            "[SDL] Input conflict in saved prefs: %s \"%s\" drove both %s slot %d "
                            "and %s slot %d; kept %s slot %d, dropped the other\n",
                            device_label,
                            binding_display_label(binding).c_str(),
                            g_input_action_names[best_action],
                            best_slot + 1,
                            g_input_action_names[other_action],
                            other_slot + 1,
                            g_input_action_names[best_action],
                            best_slot + 1);
                    table[other_action][other_slot] = make_binding(GB_INPUT_BINDING_NONE, 0);
                    dropped++;
                }
            }
        }
    }
    return dropped;
}

static int sanitise_all_binding_conflicts(void) {
    int dropped = sanitise_binding_conflicts(g_keyboard_bindings, g_default_keyboard_bindings, "key");
    dropped += sanitise_binding_conflicts(g_controller_bindings, g_default_controller_bindings,
                                          "controller input");
    if (dropped > 0) {
        clear_all_binding_pressed_state();
    }
    return dropped;
}

static std::string runtime_preferences_path(void) {
    /* Co-locate runtime prefs with the other user state (.sav / .rtc /
     * .stateN) so the whole game folder is portable. SDL_GetBasePath() is the
     * wrong anchor inside a container: in an AppImage it resolves to
     * usr/bin in the read-only squashfs, so every setting the player changed
     * vanished on the next launch. gb_host_state_dir() anchors beside the
     * .AppImage / .app and is identical to the base path everywhere else. */
    const char* state_dir = gb_host_state_dir();
    if (state_dir && state_dir[0]) {
        fs::path resolved = fs::path(state_dir) / "runtime_prefs.ini";
        return resolved.lexically_normal().string();
    }
    return fs::path("runtime_prefs.ini").lexically_normal().string();
}

// recomp-ui launcher "Widescreen (experimental)" toggle, mirrored to the
// video.widescreen pref. Drives the opt-in extended view via gb_ws_set_cli_request.
static int g_pref_widescreen = 0;
/* launcher.skip is the launcher's own ("Skip launcher on boot"); carried
 * through so a runtime save does not drop it. -1 = absent. */
static int g_launcher_skip_pref = -1;

/* Rewrite only the keyboard.* / controller.* value lines of an existing prefs
 * file, leaving every other line byte-identical. A full save_runtime_preferences()
 * is not safe this early in gb_platform_init(): the border list, shader list and
 * window state have not been populated yet, so it would drop those keys. */
static void rewrite_binding_lines_in_place(const std::string& path) {
    if (path.empty()) {
        return;
    }

    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        return;
    }
    std::string text;
    char chunk[4096];
    size_t got = 0;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        text.append(chunk, got);
    }
    fclose(file);

    std::string out;
    out.reserve(text.size() + 64);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        const bool last = (eol == std::string::npos);
        std::string body = text.substr(pos, last ? std::string::npos : eol - pos);
        std::string cr;
        if (!body.empty() && body[body.size() - 1] == '\r') {
            cr = "\r";
            body.erase(body.size() - 1);
        }

        const bool keyboard = body.compare(0, 9, "keyboard.") == 0;
        const bool controller = body.compare(0, 11, "controller.") == 0;
        const size_t equals = body.find('=');
        if ((keyboard || controller) && equals != std::string::npos) {
            const std::string key = body.substr(0, equals);
            const size_t dot = key.rfind('.');
            const size_t prefix = keyboard ? 9 : 11;
            if (dot != std::string::npos && dot > prefix) {
                const std::string name = key.substr(prefix, dot - prefix);
                const int slot = atoi(key.c_str() + dot + 1);
                GBInputAction action = GB_INPUT_ACTION_RIGHT;
                if (slot >= 0 && slot < 2 && parse_input_action_name(name.c_str(), &action)) {
                    char value[64];
                    binding_to_config_value(keyboard ? g_keyboard_bindings[action][slot]
                                                     : g_controller_bindings[action][slot],
                                            value, sizeof(value));
                    body = key + "=" + value;
                }
            }
        }

        out += body;
        out += cr;
        if (!last) {
            out += '\n';
        }
        if (last) {
            break;
        }
        pos = eol + 1;
    }

    file = fopen(path.c_str(), "wb");
    if (!file) {
        return;
    }
    fwrite(out.data(), 1, out.size(), file);
    fclose(file);
}

static void load_runtime_preferences(void) {
    set_default_audio_preferences();
    set_default_input_bindings();
    g_savestate_slot = 0;
    g_savestate_status.clear();
    g_notify_save_load = true;
    g_notify_slot = true;
    g_notify_rewind = true;
    g_notify_fast_forward = true;
    g_max_speed_mode = false;
    g_fast_forward_speed_percent = 250;
    g_max_speed_percent = GB_SPEED_UNLIMITED;
    g_fast_forward_frameskip = true;
    g_preemptive_frames_pref = -1;
    g_rewind_enabled = true;
    g_rewind_granularity = 1;
    g_rewind_buffer_mb = 20;
    g_vsync = false;
    g_show_overlay = false;
    g_smooth_lcd_transitions = true;
    g_render_scaling_mode = GB_RENDER_SCALING_PIXEL_PERFECT;
    g_menu_pauses_game = true;
    g_menu_dim_percent = GB_MENU_DIM_DEFAULT;
    g_menu_opacity_percent = 100;
    g_launcher_skip_pref = -1;
    g_per_game_prefs.clear();
    g_slang_param_prefs.clear();
    g_slang_builder_origin.clear();

    const std::string path = runtime_preferences_path();
    if (!path.empty()) {
        FILE* file = fopen(path.c_str(), "r");
        if (file) {
            char line[1024];   /* shader.slang holds a path */
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
                if (strcmp(key, "audio.fast_forward") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 0 && parsed < (long)IM_ARRAYSIZE(g_speed_audio_mode_names)) {
                        g_speed_audio_mode = (int)parsed;
                    }
                    continue;
                }
                if (strcmp(key, "speed.fast_forward_percent") == 0 ||
                    strcmp(key, "speed.max_percent") == 0) {
                    char* end = NULL;
                    long parsed = strtol(value, &end, 10);
                    if (end != value) {
                        set_shortcut_speed(strcmp(key, "speed.max_percent") == 0
                                               ? &g_max_speed_percent
                                               : &g_fast_forward_speed_percent,
                                           parsed);
                    }
                    continue;
                }
                if (strcmp(key, "speed.fast_forward_frameskip") == 0) {
                    g_fast_forward_frameskip = (strcmp(value, "0") != 0);
                    continue;
                }
                /* recomp-ui launcher-controlled display settings (written by
                 * the pre-boot seam; see launcher_ui_seam.c). Parsed here so a
                 * launcher choice is applied on the very next boot — this runs
                 * before the window is created below in gb_platform_init. */
                if (strcmp(key, "window.scale") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 1 && parsed <= 8) {
                        g_scale = (int)parsed;
                        g_windowed_width = GB_SCREEN_WIDTH * g_scale;
                        g_windowed_height = GB_SCREEN_HEIGHT * g_scale;
                        g_game_viewport = {0, 0, g_windowed_width, g_windowed_height};
                    }
                    continue;
                }
                if (strcmp(key, "video.fullscreen") == 0) {
                    /* Tri-state (0 off / 1 borderless / 2 exclusive). Clamp
                     * so a stray/garbled value can't slip a bad SDL flag
                     * combo past set_fullscreen_mode()'s own clamp. Accepts
                     * legacy "0"/"1" bool values from pre-tri-state prefs
                     * files unchanged (0 stays off, 1 stays borderless). */
                    long parsed = strtol(value, NULL, 10);
                    if (parsed < 0) parsed = 0;
                    if (parsed > 2) parsed = 2;
                    g_fullscreen_mode = (int)parsed;
                    continue;
                }
                if (strcmp(key, "emulation.preemptive_frames") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 0 && parsed <= GB_PREEMPT_MAX_FRAMES) {
                        g_preemptive_frames_pref = (int)parsed;
                    }
                    continue;
                }
                if (strcmp(key, "emulation.rewind") == 0) {
                    g_rewind_enabled = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "emulation.rewind_granularity") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 1 && parsed <= GB_REWIND_MAX_GRANULARITY) {
                        g_rewind_granularity = (int)parsed;
                    }
                    continue;
                }
                if (strcmp(key, "emulation.rewind_buffer_mb") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 1 && parsed <= GB_REWIND_MAX_BUFFER_MB) {
                        g_rewind_buffer_mb = (int)parsed;
                    }
                    continue;
                }
                if (strcmp(key, "video.linear_filter") == 0) {
                    g_render_filter_mode = (strcmp(value, "0") != 0)
                        ? GB_RENDER_FILTER_LINEAR : GB_RENDER_FILTER_NEAREST;
                    continue;
                }
                if (strcmp(key, "video.scaling_mode") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 0 && parsed < (long)IM_ARRAYSIZE(g_render_scaling_mode_names)) {
                        g_render_scaling_mode = (GBRenderScalingMode)parsed;
                    }
                    continue;
                }
                if (strcmp(key, "video.vsync") == 0) {
                    g_vsync = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "video.smooth_slow_frames") == 0) {
                    g_smooth_lcd_transitions = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "launcher.skip") == 0) {
                    g_launcher_skip_pref = (strcmp(value, "0") != 0) ? 1 : 0;
                    continue;
                }
                if (strcmp(key, "video.palette") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 0 && parsed < (int)IM_ARRAYSIZE(g_palette_names)) {
                        g_palette_idx = (int)parsed;
                    }
                    continue;
                }
                if (strcmp(key, "video.widescreen") == 0) {
                    g_pref_widescreen = (strcmp(value, "0") != 0);
                    // Feed the opt-in extended-view request; gb_ws_arm() later
                    // resolves it against the game's game_max_view_width(). Games
                    // that haven't opted in stay native regardless.
                    gb_ws_set_cli_request(g_pref_widescreen ? GB_WS_MAX_VIEW_WIDTH : 0);
                    continue;
                }
                if (strcmp(key, "ui.theme") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 0 && parsed <= 2) {
                        g_imgui_theme = (int)parsed;
                    }
                    continue;
                }
                if (strcmp(key, "ui.pause_in_menu") == 0) {
                    g_menu_pauses_game = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "ui.menu_opacity") == 0 || strcmp(key, "ui.menu_dim") == 0) {
                    long parsed = strtol(value, NULL, 10);
                    if (parsed >= 0 && parsed <= 100) {
                        (strcmp(key, "ui.menu_dim") == 0 ? g_menu_dim_percent : g_menu_opacity_percent) =
                            (int)parsed;
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
                if (strcmp(key, "notify.save_load") == 0 ||
                    strcmp(key, "savestate.notify_save_load") == 0 /* earlier name */) {
                    g_notify_save_load = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "notify.slot") == 0 ||
                    strcmp(key, "savestate.notify_slot") == 0 /* earlier name */) {
                    g_notify_slot = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "notify.rewind") == 0) {
                    g_notify_rewind = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "notify.fast_forward") == 0) {
                    g_notify_fast_forward = (strcmp(value, "0") != 0);
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
                if (strcmp(key, "diag.show_overlay") == 0) {
                    g_show_overlay = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strcmp(key, "shader.active") == 0) {
                    g_active_shader_pref = value;
                    continue;
                }
                if (strcmp(key, "shader.slang") == 0) {
                    g_slang_preset_pref = value;
                    g_slang_load_pending = true;
                    continue;
                }
                if (strcmp(key, "shader.slang_dir") == 0) {
                    g_slang_dir_pref = value;
                    continue;
                }
                if (strcmp(key, "shader.slang_builder_origin") == 0) {
                    g_slang_builder_origin = value;
                    continue;
                }
                if (strcmp(key, "shader.slang_fill_window") == 0) {
                    g_slang_fill_window = (strcmp(value, "0") != 0);
                    continue;
                }
                if (strncmp(key, "shader.slang_param.", 19) == 0 && key[19]) {
                    g_slang_param_prefs[key + 19] = strtof(value, NULL);
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

    /* Files written before the one-key-one-action rule existed can bind the same
     * physical input to two actions. Fix the in-memory tables, then write the
     * corrected binding lines back so the file itself is sanitised too. */
    if (sanitise_all_binding_conflicts() > 0) {
        rewrite_binding_lines_in_place(path);
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
    fprintf(file, "audio.fast_forward=%d\n", g_speed_audio_mode);
    fprintf(file, "speed.fast_forward_percent=%d\n", shortcut_speed_pref(g_fast_forward_speed_percent));
    fprintf(file, "speed.max_percent=%d\n", shortcut_speed_pref(g_max_speed_percent));
    fprintf(file, "speed.fast_forward_frameskip=%d\n", g_fast_forward_frameskip ? 1 : 0);
    /* recomp-ui launcher-controlled display settings (round-trip with the
     * pre-boot seam; see launcher_ui_seam.c). */
    fprintf(file, "window.scale=%d\n", g_scale);
    fprintf(file, "video.fullscreen=%d\n", g_fullscreen_mode);
    fprintf(file, "video.linear_filter=%d\n",
            g_render_filter_mode == GB_RENDER_FILTER_LINEAR ? 1 : 0);
    fprintf(file, "video.scaling_mode=%d\n", (int)g_render_scaling_mode);
    fprintf(file, "video.vsync=%d\n", g_vsync ? 1 : 0);
    fprintf(file, "video.smooth_slow_frames=%d\n", g_smooth_lcd_transitions ? 1 : 0);
    fprintf(file, "video.palette=%d\n", g_palette_idx);
    fprintf(file, "video.widescreen=%d\n", g_pref_widescreen ? 1 : 0);
    if (g_preemptive_frames_pref >= 0) {
        fprintf(file, "emulation.preemptive_frames=%d\n", g_preemptive_frames_pref);
    }
    fprintf(file, "emulation.rewind=%d\n", g_rewind_enabled ? 1 : 0);
    fprintf(file, "emulation.rewind_granularity=%d\n", g_rewind_granularity);
    fprintf(file, "emulation.rewind_buffer_mb=%d\n", g_rewind_buffer_mb);
    fprintf(file, "ui.theme=%d\n", g_imgui_theme);
    fprintf(file, "ui.pause_in_menu=%d\n", g_menu_pauses_game ? 1 : 0);
    fprintf(file, "ui.menu_dim=%d\n", g_menu_dim_percent);
    fprintf(file, "ui.menu_opacity=%d\n", g_menu_opacity_percent);
    fprintf(file, "savestate.slot=%d\n", g_savestate_slot);
    fprintf(file, "notify.save_load=%d\n", g_notify_save_load ? 1 : 0);
    fprintf(file, "notify.slot=%d\n", g_notify_slot ? 1 : 0);
    fprintf(file, "notify.rewind=%d\n", g_notify_rewind ? 1 : 0);
    fprintf(file, "notify.fast_forward=%d\n", g_notify_fast_forward ? 1 : 0);
    fprintf(file, "border.enabled=%d\n", g_border_enabled ? 1 : 0);
    fprintf(file, "sgb.colors=%d\n", g_sgb_colors_pref ? 1 : 0);
    fprintf(file, "sgb.cart_border=%d\n", g_sgb_cart_border_enabled ? 1 : 0);
    fprintf(file, "diag.show_fps=%d\n", g_show_fps ? 1 : 0);
    fprintf(file, "diag.show_overlay=%d\n", g_show_overlay ? 1 : 0);
    if (g_launcher_skip_pref >= 0) {
        fprintf(file, "launcher.skip=%d\n", g_launcher_skip_pref);
    }
    if (!g_active_shader_pref.empty()) {
        fprintf(file, "shader.active=%s\n", g_active_shader_pref.c_str());
    }
    fprintf(file, "shader.slang=%s\n", g_slang_preset_pref.c_str());
    if (!g_slang_dir_pref.empty()) {
        fprintf(file, "shader.slang_dir=%s\n", g_slang_dir_pref.c_str());
    }
    if (!g_slang_builder_origin.empty()) {
        fprintf(file, "shader.slang_builder_origin=%s\n", g_slang_builder_origin.c_str());
    }
    fprintf(file, "shader.slang_fill_window=%d\n", g_slang_fill_window ? 1 : 0);
    for (const auto& [name, value] : g_slang_param_prefs) {
        fprintf(file, "shader.slang_param.%s=%.9g\n", name.c_str(), (double)value);
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

/* RetroArch's state messages (saved / loaded / state slot), bottom left for a
 * few seconds; `shown` is the matching notify.* setting. A failed
 * save or load passes true, so it always shows. The Esc menu's footer shows
 * the message while that menu is open, the settings window has its own
 * status line. */
static void post_state_notice(const std::string& text, bool shown) {
#if defined(RECOMP_LAUNCHER) && defined(RECOMP_RUNTIME_UI_HAS_STATUS)
    if (g_runtime_ui && recomp_runtime_ui_is_open(g_runtime_ui)) {
        recomp_runtime_ui_set_status(g_runtime_ui, text.c_str());
        return;
    }
#endif
    if (!shown || menu_open()) return;
    g_state_notice = text;
    g_state_notice_ms = SDL_GetTicks64();
}

/* "State slot: 3 (empty)" / "State slot: 3 (saved Sep 25 13:35)". */
static std::string savestate_slot_summary(const GBContext* ctx, int slot) {
    std::string text = "State slot: " + std::to_string(slot + 1);
    std::string path;
    if (!savestate_slot_exists(ctx, slot, &path)) return text + " (empty)";
    std::error_code ec;
    const fs::file_time_type written = fs::last_write_time(fs::path(path), ec);
    if (ec) return text;
    const auto system_written = std::chrono::system_clock::now() +
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            written - fs::file_time_type::clock::now());
    const time_t when = std::chrono::system_clock::to_time_t(system_written);
    const struct tm* local = localtime(&when);
    char stamp[32];
    if (!local || !strftime(stamp, sizeof(stamp), "%b %d %H:%M", local)) return text;
    return text + " (saved " + stamp + ")";
}

static void select_savestate_slot(const GBContext* ctx, int slot) {
    g_savestate_slot = (slot % GB_SAVESTATE_SLOT_COUNT + GB_SAVESTATE_SLOT_COUNT) % GB_SAVESTATE_SLOT_COUNT;
    g_savestate_status = "Selected slot " + std::to_string(g_savestate_slot + 1);
    save_runtime_preferences();
    post_state_notice(savestate_slot_summary(ctx, g_savestate_slot), g_notify_slot);
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

static uint8_t current_joypad_input(void) {
    return (uint8_t)((g_joypad_dpad & 0x0F) | ((g_joypad_buttons & 0x0F) << 4));
}

static void set_user_paused(bool paused) {
    if (g_user_paused == paused) {
        return;
    }
    g_user_paused = paused;
    g_frame_advance_pending = 0;
    g_pause_input = current_joypad_input();
    g_pause_input_frames = 0;
    /* audio_output_should_run() is false while paused: this drops the queued
     * samples and pauses the device, and on resume it refills before playing. */
    reset_audio_output_buffer(true);
}

static void update_runtime_action_state(GBContext* ctx) {
    const int previous_effective_speed = effective_speed_percent();
    const bool audio_was_running = audio_output_should_run();

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
                    select_savestate_slot(ctx, g_savestate_slot - 1);
                    break;

                case GB_INPUT_ACTION_NEXT_STATE_SLOT:
                    select_savestate_slot(ctx, g_savestate_slot + 1);
                    break;

                case GB_INPUT_ACTION_TOGGLE_OVERLAY:
                    g_show_overlay = !g_show_overlay;
                    save_runtime_preferences();
                    break;

                case GB_INPUT_ACTION_TOGGLE_MUTE:
                    g_audio_muted = !g_audio_muted;
                    save_runtime_preferences();
                    break;

                case GB_INPUT_ACTION_TOGGLE_MENU:
                    g_show_menu = !g_show_menu;
                    break;

                case GB_INPUT_ACTION_PAUSE:
                    set_user_paused(!g_user_paused);
                    break;

                case GB_INPUT_ACTION_FRAME_ADVANCE:
                    /* While running, stop at the end of the current frame. */
                    if (g_user_paused) {
                        g_frame_advance_pending++;
                    } else {
                        set_user_paused(true);
                    }
                    break;

                case GB_INPUT_ACTION_FAST_FORWARD:
                case GB_INPUT_ACTION_REWIND:   /* held; rewind_before_frame() reads it */
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

    g_fast_forward_active = g_runtime_action_pressed[GB_INPUT_ACTION_FAST_FORWARD] || g_debug_fast_forward;

    if (effective_speed_percent() != previous_effective_speed) {
        on_speed_changed(audio_was_running);
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
    /* While a menu is up, hide all joypad activity from the cart, so keys
     * that navigate the menu (or type into it) never move the player or
     * open the cart's own menu, also with Pause in Menu off. Active-low:
     * 0xFF = nothing pressed. */
    if (menu_open()) {
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

/* Called wherever a menu may have opened or closed. Every binding starts
 * released either way, as RetroArch blocks input across a menu toggle: what
 * was held when the menu opened must not stay held for the game, and what was
 * pressed in the menu must not reach it afterwards. The game's sound stops
 * and starts with the game. */
static void menu_visibility_changed(void) {
    static bool was_open = false;
    const bool open = menu_open();
    if (open == was_open) return;
    was_open = open;
    clear_all_binding_pressed_state();
    update_effective_joypad_state();
    update_runtime_action_state(g_registered_ctx);
    if (g_menu_pauses_game) reset_audio_output_buffer(true);
    if (!open) {
        g_menu_text_input = false;
        g_menu_popup_open = false;
    }
}

static void set_menu_pauses_game(bool pauses) {
    if (g_menu_pauses_game == pauses) return;
    g_menu_pauses_game = pauses;
    reset_audio_output_buffer(true);   /* the sound stops or starts with the game */
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
    if (view_width() != GB_SCREEN_WIDTH || view_height() != GB_SCREEN_HEIGHT) return false;
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
    if ((g_border_enabled && g_border_texture && view_width() == GB_SCREEN_WIDTH && view_height() == GB_SCREEN_HEIGHT) || sgb_cart_border_active()) {
        return GB_BORDER_FULL_W;
    }
    return view_width();   /* 160 unless the extended view is armed */
}

static int border_content_height(void) {
    if ((g_border_enabled && g_border_texture && view_width() == GB_SCREEN_WIDTH && view_height() == GB_SCREEN_HEIGHT) || sgb_cart_border_active()) {
        return GB_BORDER_FULL_H;
    }
    return view_height();
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
    /* Borders the player dropped in live beside the .exe / .AppImage / .app
     * (state), then any shipped with the program (asset payload). */
    for (const char* dir : { gb_host_state_dir(), gb_host_asset_dir() }) {
        if (dir && dir[0]) candidates.push_back(fs::path(dir) / GB_BORDER_DIR_NAME);
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

/* ---- librashader presets ------------------------------------------------ */

/* UTF-8 whatever the C++ standard (u8string is std::u8string from C++20),
 * because that is what librashader takes a path as. */
static std::string path_utf8(const fs::path& p) {
    const auto s = p.u8string();
    std::string out(s.begin(), s.end());
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

/* The folder the preset picker lists: the menu's choice, else
 * GBRECOMP_SHADER_DIR, else shaders/ beside the executable. */
static std::string slang_shader_dir(void) {
    if (!g_slang_dir_pref.empty()) return g_slang_dir_pref;
    const char* env = SDL_getenv("GBRECOMP_SHADER_DIR");
    if (env && env[0]) return env;
    std::error_code ec;
    for (const char* base : { gb_host_asset_dir(), gb_host_state_dir() }) {
        const fs::path candidate = fs::u8path(base ? base : "") / "shaders";
        if (fs::is_directory(candidate, ec)) return path_utf8(candidate);
    }
    return path_utf8(fs::u8path(gb_host_asset_dir()) / "shaders");
}

/* Presets saved from the preset builder live in shader_presets/ beside the
 * game's other state, and their setting reads "saved:<file>". The builder's
 * working copy, which Apply writes and loads, is the hidden
 * saved:.builder.slangp. */
static constexpr const char* SLANG_SAVED_PREFIX = "saved:";
static const std::string g_slang_builder_pref = std::string(SLANG_SAVED_PREFIX) + ".builder.slangp";

static std::string slang_saved_dir(void) {
    char path[1152];
    gb_host_state_path("shader_presets", path, sizeof(path));
    return path_utf8(fs::u8path(path));
}

static bool slang_pref_is_saved(const std::string& pref) {
    return pref.rfind(SLANG_SAVED_PREFIX, 0) == 0;
}

static std::string slang_preset_path(const std::string& pref) {
    if (slang_pref_is_saved(pref)) {
        return path_utf8(fs::u8path(slang_saved_dir()) / fs::u8path(pref.substr(strlen(SLANG_SAVED_PREFIX))));
    }
    if (pref.empty() || fs::u8path(pref).is_absolute()) return pref;
    return path_utf8(fs::u8path(slang_shader_dir()) / fs::u8path(pref));
}

/* What the menu calls a preset setting. */
static std::string slang_pref_label(const std::string& pref) {
    if (pref.empty()) return "Off";
    if (pref == g_slang_builder_pref) {
        return g_slang_builder_origin.empty() || g_slang_builder_origin == g_slang_builder_pref
                   ? "Built in the menu (not saved)"
                   : slang_pref_label(g_slang_builder_origin) + ", edited (not saved)";
    }
    if (!slang_pref_is_saved(pref)) return pref;
    std::string name = pref.substr(strlen(SLANG_SAVED_PREFIX));
    if (name.size() > 7 && name.compare(name.size() - 7, 7, ".slangp") == 0) name.resize(name.size() - 7);
    return "Saved: " + name;
}

/* Every .slangp and .slang under the shader folder, relative to it and
 * '/'-separated, and the presets saved from the builder (as settings). */
static std::vector<std::string> g_slang_presets;
static std::vector<std::string> g_slang_shaders;
static std::vector<std::string> g_slang_saved;
/* The Preset list: the edited copy while there is one, the saved presets,
 * then the folder's. */
static std::vector<std::string> g_slang_choices;
static std::string g_slang_scanned_dir;
static bool g_slang_scanned = false;

static bool less_ci(const std::string& a, const std::string& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower((unsigned char)x) < std::tolower((unsigned char)y);
    });
}

static bool has_extension_ci(const std::string& leaf, const char* ext) {
    const size_t n = strlen(ext);
    if (leaf.size() <= n) return false;
    for (size_t i = 0; i < n; i++) {
        if (std::tolower((unsigned char)leaf[leaf.size() - n + i]) != ext[i]) return false;
    }
    return true;
}

static void slang_rebuild_choices(void) {
    g_slang_choices.clear();
    std::error_code ec;
    if (fs::exists(fs::u8path(slang_preset_path(g_slang_builder_pref)), ec)) {
        g_slang_choices.push_back(g_slang_builder_pref);
    }
    g_slang_choices.insert(g_slang_choices.end(), g_slang_saved.begin(), g_slang_saved.end());
    g_slang_choices.insert(g_slang_choices.end(), g_slang_presets.begin(), g_slang_presets.end());
}

static void slang_scan_presets(void) {
    g_slang_presets.clear();
    g_slang_shaders.clear();
    g_slang_saved.clear();
    g_slang_scanned_dir = slang_shader_dir();
    g_slang_scanned = true;
    const fs::path root = fs::u8path(g_slang_scanned_dir);
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::string leaf = path_utf8(it->path().filename());
        if (!leaf.empty() && leaf[0] == '.') {   /* .git and friends */
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        if (has_extension_ci(leaf, ".slangp")) {
            g_slang_presets.push_back(path_utf8(it->path().lexically_relative(root)));
        } else if (has_extension_ci(leaf, ".slang")) {
            g_slang_shaders.push_back(path_utf8(it->path().lexically_relative(root)));
        }
    }
    for (fs::directory_iterator saved(fs::u8path(slang_saved_dir()), ec); !ec && saved != fs::directory_iterator();
         saved.increment(ec)) {
        const std::string leaf = path_utf8(saved->path().filename());
        if (!leaf.empty() && leaf[0] != '.' && has_extension_ci(leaf, ".slangp")) {
            g_slang_saved.push_back(SLANG_SAVED_PREFIX + leaf);
        }
    }
    std::sort(g_slang_presets.begin(), g_slang_presets.end(), less_ci);
    std::sort(g_slang_shaders.begin(), g_slang_shaders.end(), less_ci);
    std::sort(g_slang_saved.begin(), g_slang_saved.end(), less_ci);
    slang_rebuild_choices();
}

static void slang_cancel_probe(void);
static void slang_build_load(const std::string& pref);

/* A preset picked in the menu, which the preset editor then shows. */
static void slang_select(const std::string& pref) {
    slang_cancel_probe();
    g_slang_preset_pref = pref;
    g_slang_param_prefs.clear();
    g_slang_load_pending = true;
    g_slang_explicit = true;
    g_slang_delete_status.clear();
    if (!pref.empty()) slang_build_load(pref);
    save_runtime_preferences();
}

/* Probe results (see gb_lrs_probe_start), one line per preset file in the
 * state folder: "ok|ok-unoptimized|failed <mtime> <key>" (slang_probe_key).
 * A preset is probed again when its file changes. */
struct SlangProbe {
    long long mtime;
    GBLrsProbeResult result;   /* OK, OK_UNOPTIMIZED or FAILED */
};
static std::map<std::string, SlangProbe> g_slang_probe_cache;
static bool g_slang_probe_cache_loaded = false;
static const std::pair<GBLrsProbeResult, const char*> k_slang_probe_names[] = {
    { GB_LRS_PROBE_OK, "ok" },
    { GB_LRS_PROBE_OK_UNOPTIMIZED, "ok-unoptimized" },
    { GB_LRS_PROBE_FAILED, "failed" },
};

static std::string slang_probe_cache_path(void) {
    char path[1152];
    gb_host_state_path("librashader_probe.txt", path, sizeof(path));
    return path;
}

static void slang_probe_cache_load(void) {
    if (g_slang_probe_cache_loaded) return;
    g_slang_probe_cache_loaded = true;
    std::ifstream in(fs::u8path(slang_probe_cache_path()));
    std::string result, preset;
    long long mtime = 0;
    while (in >> result >> mtime && std::getline(in >> std::ws, preset)) {
        for (const auto& [value, name] : k_slang_probe_names) {
            if (result == name) g_slang_probe_cache[preset] = { mtime, value };
        }
    }
}

static void slang_probe_cache_save(void) {
    std::ofstream out(fs::u8path(slang_probe_cache_path()), std::ios::trunc);
    for (const auto& [preset, entry] : g_slang_probe_cache) {
        for (const auto& [value, name] : k_slang_probe_names) {
            if (entry.result == value) out << name << ' ' << entry.mtime << ' ' << preset << '\n';
        }
    }
}

static long long slang_file_stamp(const std::string& path) {
    std::error_code ec;
    const auto stamp = fs::last_write_time(fs::u8path(path), ec);
    return ec ? 0 : (long long)stamp.time_since_epoch().count();
}

/* A preset's key in the probe cache: relative to the shader folder when it
 * lies inside it, so the verdicts outlive the folder moving (an AppImage mounts
 * it somewhere new on every start); the path itself otherwise. */
static std::string slang_probe_key(const std::string& path) {
    const std::string dir = path_utf8(fs::u8path(slang_shader_dir())) + "/";
    if (path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0) return path.substr(dir.size());
    return path;
}

/* The cached verdict for `path`, SKIPPED when there is none. */
static GBLrsProbeResult slang_probe_result(const std::string& path) {
    slang_probe_cache_load();
    const auto it = g_slang_probe_cache.find(slang_probe_key(path));
    return it == g_slang_probe_cache.end() ? GB_LRS_PROBE_SKIPPED : it->second.result;
}

static bool slang_probe_failed(const std::string& path) {
    return slang_probe_result(path) == GB_LRS_PROBE_FAILED;
}

/* The preset that is actually running, so a choice that fails to load can
 * fall back to it instead of being saved as the setting. */
static std::string g_slang_running_pref;
/* The preset a background probe is compiling, "" when none is. */
static std::string g_slang_probing_pref;
static void slang_cancel_probe(void) {
    if (g_slang_probing_pref.empty()) return;
    gb_lrs_probe_cancel();
    g_slang_probing_pref.clear();
}

static void slang_revert(void) {
    /* Keep the setting while librashader itself is missing, so the preset
     * comes back with the library; otherwise go back to what is running (the
     * menu shows why) along with that preset's parameter overrides. */
    if (!gb_lrs_available() || g_slang_preset_pref == g_slang_running_pref) return;
    g_slang_preset_pref = g_slang_running_pref;
    g_slang_param_prefs.clear();
    for (int i = 0; i < gb_lrs_param_count(); i++) {
        const GBLrsParam* p = gb_lrs_param(i);
        if (p->value != p->initial) g_slang_param_prefs[p->name] = p->value;
    }
    save_runtime_preferences();
}

static void slang_load_now(void) {
    const std::string path = slang_preset_path(g_slang_preset_pref);
    if (!gb_lrs_load(path.c_str(), slang_probe_result(path) == GB_LRS_PROBE_OK_UNOPTIMIZED)) {
        slang_revert();
        return;
    }
    g_slang_running_pref = g_slang_preset_pref;
    for (const auto& [name, value] : g_slang_param_prefs) {
        const int index = gb_lrs_find_param(name.c_str());
        if (index >= 0) gb_lrs_set_param(index, value);
    }
}

static void slang_apply_pending(void) {
    if (!g_gl_context) return;
    if (!g_slang_probing_pref.empty()) {
        const GBLrsProbeResult result = gb_lrs_probe_poll();
        if (result == GB_LRS_PROBE_RUNNING) return;
        const std::string path = slang_preset_path(g_slang_probing_pref);
        g_slang_probing_pref.clear();
        if (result != GB_LRS_PROBE_SKIPPED) {
            g_slang_probe_cache[slang_probe_key(path)] = { slang_file_stamp(path), result };
            slang_probe_cache_save();
        }
        if (result == GB_LRS_PROBE_FAILED) slang_revert();
        else slang_load_now();
        return;
    }
    if (!g_slang_load_pending) return;
    g_slang_load_pending = false;
    const bool explicit_pick = g_slang_explicit;
    g_slang_explicit = false;

    const std::string path = slang_preset_path(g_slang_preset_pref);
    if (!path.empty() && gb_lrs_available()) {
        slang_probe_cache_load();
        const auto it = g_slang_probe_cache.find(slang_probe_key(path));
        const bool known = it != g_slang_probe_cache.end() && it->second.mtime == slang_file_stamp(path);
        const bool failed = known && it->second.result == GB_LRS_PROBE_FAILED;
        if (failed && !explicit_pick) {
            gb_lrs_set_error((slang_pref_label(g_slang_preset_pref) + " failed a test run before, so it was "
                              "not loaded. Pick it again to retest it.").c_str());
            slang_revert();
            return;
        }
        if ((!known || failed) && gb_lrs_probe_start(path.c_str()) == GB_LRS_PROBE_RUNNING) {
            g_slang_probing_pref = g_slang_preset_pref;
            return;
        }
    }
    slang_load_now();
}

/* Live parameter change; the override is remembered only while it differs
 * from what the preset loaded with. Saved when the slider is let go. */
static void slang_set_param(int index, float value) {
    const GBLrsParam* p = gb_lrs_param(index);
    if (!p || !gb_lrs_set_param(index, value)) return;
    if (p->value == p->initial) g_slang_param_prefs.erase(p->name);
    else g_slang_param_prefs[p->name] = p->value;
}

static bool contains_ci(const std::string& hay, const std::string& needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
    }) != hay.end();
}

/* Game Dimming and Menu Opacity: the menus redraw with them while they are
 * dragged; saved when let go. */
static void menu_look_sliders(void) {
    auto slider = [](const char* label, int* percent, const char* tip) {
        const bool changed = ImGui::SliderInt(label, percent, 0, 100, "%d", ImGuiSliderFlags_NoInput);
        if (ImGui::IsItemDeactivatedAfterEdit() || (changed && !ImGui::IsItemActive())) {
            save_runtime_preferences();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    slider("Game Dimming %", &g_menu_dim_percent,
           "How dark the game gets behind a menu. 0 leaves it as it is,\n"
           "so a shader preset's effect shows while you tune it.");
    slider("Menu Opacity %", &g_menu_opacity_percent,
           "How solid the menus' own backgrounds are. Lower it too to\n"
           "see the game through the menu itself.");
}

static bool slang_probe_failed(const std::string& path);

/* A combo over `items` with a filter field (every word must match, any case),
 * showing each through `label`; the picked index, or -1. With `off`, an
 * "Off" row on top picks -2. With `mark_failed`, the rows on screen say which
 * presets failed a test run. */
static int slang_filtered_combo(const char* id, const char* preview, char* filter, size_t filter_size,
                                const std::vector<std::string>& items, const std::string& current,
                                std::string (*label)(const std::string&), bool off, bool mark_failed) {
    if (!ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightLarge)) return -1;
    int picked = -1;
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "Filter, e.g. \"crt\" or \"handheld gbc\"", filter, filter_size);
    std::vector<std::string> words;
    for (const char* p = filter; *p;) {
        while (*p == ' ') p++;
        const char* start = p;
        while (*p && *p != ' ') p++;
        if (p > start) words.emplace_back(start, p);
    }
    std::vector<int> shown;
    std::vector<std::string> labels;
    int selected_row = -1;
    for (int i = 0; i < (int)items.size(); i++) {
        std::string text = label(items[(size_t)i]);
        bool match = true;
        for (const std::string& w : words) match = match && contains_ci(text, w);
        if (!match) continue;
        if (items[(size_t)i] == current) selected_row = (int)shown.size();
        shown.push_back(i);
        labels.push_back(std::move(text));
    }
    if (off && ImGui::Selectable("Off", current.empty())) picked = -2;
    ImGuiListClipper clipper;
    clipper.Begin((int)shown.size());
    if (selected_row >= 0) clipper.IncludeItemByIndex(selected_row);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
            const bool selected = row == selected_row;
            const int index = shown[(size_t)row];
            const bool failed = mark_failed && slang_probe_failed(slang_preset_path(items[(size_t)index]));
            ImGui::PushID(row);
            if (ImGui::Selectable(failed ? (labels[(size_t)row] + "  (failed a test run)").c_str()
                                         : labels[(size_t)row].c_str(),
                                  selected)) {
                picked = index;
            }
            ImGui::PopID();
            if (selected && ImGui::IsWindowAppearing()) {
                ImGui::SetItemDefaultFocus();
                ImGui::SetScrollHereY(0.5f);
            }
        }
    }
    ImGui::EndCombo();
    return picked;
}

static std::string slang_plain_label(const std::string& name) {
    return name;
}

/* ---- Preset editor ----
 * RetroArch's Shader menu as a list, holding the passes of the preset that
 * was picked: shader passes added one at a time and whole presets appended or
 * prepended (slang_preset.h), each pass's filter, scale and wrap settings.
 * Apply writes the chain to the hidden saved:.builder.slangp and loads that
 * (through the same probe as any other preset), so the picked preset's own
 * file is never changed; Save writes a preset of the user's own to
 * shader_presets/ with the parameters as tuned. */
static SlangPreset g_slang_build;
static std::string g_slang_build_source;     /* the preset the chain came from, "" for none */
static bool g_slang_build_loaded = false;    /* has shown a preset since start-up */
static bool g_slang_build_edited = false;    /* differs from its source */
static bool g_slang_build_dirty = false;     /* changed since it was applied */
static bool g_slang_build_auto = true;       /* apply changes as they are made */
static bool g_slang_build_open = false;      /* Edit Preset was pressed */
static uint64_t g_slang_build_changed_ms = 0;
static std::string g_slang_build_status;
static char g_slang_build_name[96] = "";

static void slang_build_changed(void) {
    g_slang_build_edited = true;
    g_slang_build_dirty = true;
    g_slang_build_changed_ms = SDL_GetTicks64();
    g_slang_build_status.clear();
}

/* The parameters as tuned now, as the preset's own values. */
static void slang_build_add_tuning(SlangPreset* preset) {
    for (const auto& [name, value] : g_slang_param_prefs) {
        char text[32];
        snprintf(text, sizeof(text), "%.9g", (double)value);
        slang_preset_set_parameter(preset, name, text);
    }
}

/* Show `pref`'s passes; the edited copy shows what it was made from. The
 * name to save under starts as that preset's own. */
static void slang_build_load(const std::string& pref) {
    g_slang_build = SlangPreset{};
    g_slang_build_status.clear();
    std::string error;
    if (!pref.empty() && !slang_preset_read(slang_preset_path(pref), &g_slang_build, &error)) {
        g_slang_build_status = error;
    }
    g_slang_build_loaded = true;
    g_slang_build_dirty = false;
    g_slang_build_edited = pref == g_slang_builder_pref;
    g_slang_build_source = g_slang_build_edited ? g_slang_builder_origin : pref;
    const std::string& source = g_slang_build_source;
    const size_t skip = slang_pref_is_saved(source) ? strlen(SLANG_SAVED_PREFIX) : 0;
    snprintf(g_slang_build_name, sizeof(g_slang_build_name), "%s",
             path_utf8(fs::u8path(source.substr(skip)).stem()).c_str());
}

static void slang_build_apply(void) {
    std::string error;
    if (!slang_preset_write(slang_preset_path(g_slang_builder_pref), g_slang_build, &error)) {
        g_slang_build_status = error;
        return;
    }
    slang_cancel_probe();
    /* Parameter tuning carries over by name. */
    g_slang_preset_pref = g_slang_builder_pref;
    g_slang_builder_origin = g_slang_build_source;
    g_slang_load_pending = true;
    g_slang_explicit = true;
    g_slang_build_dirty = false;
    slang_rebuild_choices();
    save_runtime_preferences();
}

/* A file name from what was typed: no path characters, no .slangp. */
static std::string slang_file_name(const char* text) {
    std::string name;
    for (const char* p = text; *p; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c >= 32 && !strchr("<>:\"/\\|?*", c)) name += (char)c;
    }
    const size_t b = name.find_first_not_of(" .");
    name = b == std::string::npos ? "" : name.substr(b, name.find_last_not_of(" .") - b + 1);
    if (has_extension_ci(name, ".slangp")) name.resize(name.size() - 7);
    return name;
}

static void slang_build_save(const std::string& name) {
    const std::string pref = SLANG_SAVED_PREFIX + name + ".slangp";
    SlangPreset preset = g_slang_build;
    slang_build_add_tuning(&preset);
    std::string error;
    const std::string path = slang_preset_path(pref);
    if (!slang_preset_write(path, preset, &error)) {
        g_slang_build_status = error;
        return;
    }
    slang_cancel_probe();
    g_slang_preset_pref = pref;
    g_slang_param_prefs.clear();   /* in the preset now */
    g_slang_load_pending = true;
    g_slang_explicit = true;
    g_slang_build_source = pref;
    g_slang_build_edited = false;
    g_slang_build_dirty = false;
    g_slang_build_status = "Saved " + path + ".";
    if (std::find(g_slang_saved.begin(), g_slang_saved.end(), pref) == g_slang_saved.end()) {
        g_slang_saved.push_back(pref);
        std::sort(g_slang_saved.begin(), g_slang_saved.end(), less_ci);
        slang_rebuild_choices();
    }
    save_runtime_preferences();
}

/* A shader path as the list shows it: relative to the shader folder when it
 * is in it. */
static std::string slang_display_path(const std::string& path) {
    const fs::path rel = fs::u8path(path).lexically_relative(fs::u8path(g_slang_scanned_dir));
    const std::string text = path_utf8(rel);
    return rel.empty() || text.rfind("..", 0) == 0 ? path : text;
}

static std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

/* One pass's settings, as a .slangp gives them; "Preset default" leaves one
 * unset, for librashader's default. Returns true on a change. */
static bool draw_slang_pass_settings(SlangPresetPass& pass) {
    bool changed = false;
    auto choice = [&](const char* label, const char* key, const char* const* names, const char* const* values,
                      int count, const char* tip) {
        const std::string current = lower_ascii(pass.get(key));
        int index = 0;
        for (int i = 1; i < count; i++) {
            if (current == values[i]) index = i;
        }
        if (ImGui::Combo(label, &index, names, count)) {
            pass.set(key, values[index]);
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    auto flag = [&](const char* label, const char* key, const char* tip) {
        const std::string current = lower_ascii(pass.get(key));
        bool on = current == "true" || current == "1";
        if (ImGui::Checkbox(label, &on)) {
            pass.set(key, on ? "true" : "");
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };

    {
        static const char* const names[] = { "Preset default", "Nearest", "Linear" };
        const std::string current = lower_ascii(pass.get("filter_linear"));
        int index = current.empty() ? 0 : (current == "true" || current == "1") ? 2 : 1;
        if (ImGui::Combo("Filter", &index, names, 3)) {
            pass.set("filter_linear", index == 0 ? "" : index == 2 ? "true" : "false");
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How the next pass samples this pass's output.");
    }
    static const char* const scale_names[] = { "Preset default", "Source (its input)", "Viewport (the screen)",
                                               "Absolute (pixels)" };
    static const char* const scale_values[] = { "", "source", "viewport", "absolute" };
    choice("Scale Type", "scale_type", scale_names, scale_values, 4,
           "What this pass's output size is relative to. Preset default: its input.");
    const std::string scale_type = lower_ascii(pass.get("scale_type"));
    if (scale_type == "absolute") {
        int pixels = atoi(pass.get("scale").c_str());
        if (pixels <= 0) pixels = 1;
        if (ImGui::InputInt("Size (pixels)", &pixels, 1, 16)) {
            pass.set("scale", std::to_string(std::max(1, pixels)));
            changed = true;
        }
    } else if (!scale_type.empty()) {
        const std::string text = pass.get("scale");
        float scale = text.empty() ? 1.0f : (float)atof(text.c_str());
        if (ImGui::InputFloat("Scale", &scale, 0.5f, 1.0f, "%.2f")) {
            char value[32];
            snprintf(value, sizeof(value), "%g", (double)std::max(0.01f, scale));
            pass.set("scale", value);
            changed = true;
        }
    }
    if (scale_type.empty() && !(pass.get("scale_type_x") + pass.get("scale_type_y")).empty()) {
        ImGui::TextDisabled("Scaled separately in X and Y by its preset (kept as it is).");
    }
    static const char* const wrap_names[] = { "Preset default", "Clamp to border", "Clamp to edge", "Repeat",
                                              "Mirrored repeat" };
    static const char* const wrap_values[] = { "", "clamp_to_border", "clamp_to_edge", "repeat",
                                               "mirrored_repeat" };
    choice("Wrap Mode", "wrap_mode", wrap_names, wrap_values, 5,
           "What the next pass reads outside this pass's output.");
    flag("Float Framebuffer", "float_framebuffer", "Keep this pass's output in floating point.");
    ImGui::SameLine();
    flag("sRGB Framebuffer", "srgb_framebuffer", "Keep this pass's output in sRGB.");
    ImGui::SameLine();
    flag("Mipmap Input", "mipmap_input", "Give this pass mipmaps of its input.");
    int frame_count_mod = atoi(pass.get("frame_count_mod").c_str());
    if (ImGui::InputInt("Frame Count Mod", &frame_count_mod, 1, 10)) {
        frame_count_mod = std::max(0, frame_count_mod);
        pass.set("frame_count_mod", frame_count_mod ? std::to_string(frame_count_mod) : "");
        changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The frame count this pass sees wraps at this number (0: never).");
    char alias[64];
    snprintf(alias, sizeof(alias), "%s", pass.get("alias").c_str());
    if (ImGui::InputText("Alias", alias, sizeof(alias), ImGuiInputTextFlags_CharsNoBlank)) {
        pass.set("alias", alias);
        changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The name later passes read this pass's output by.");
    return changed;
}

static void draw_slang_builder(void) {
    if (!g_slang_build_loaded) slang_build_load(g_slang_preset_pref);
    char label[96];
    const size_t passes = g_slang_build.passes.size();
    snprintf(label, sizeof(label), "Edit Preset (%zu pass%s)###slang_builder", passes, passes == 1 ? "" : "es");
    if (g_slang_build_open) ImGui::SetNextItemOpen(true);
    const bool open = ImGui::TreeNode(label);
    if (g_slang_build_open) {
        ImGui::SetScrollHereY(0.0f);
        g_slang_build_open = false;
    }
    if (!open) return;
    ImGui::PushTextWrapPos(0.0f);
    const std::string& source = g_slang_build_source;
    if (source.empty()) {
        ImGui::Text("Editing: a preset of your own%s", passes ? " (not saved)" : "");
    } else {
        ImGui::Text("Editing: %s%s", slang_pref_label(source).c_str(), g_slang_build_edited ? ", changed" : "");
    }
    ImGui::TextDisabled("The passes of the preset you pick. Change their settings, reorder or remove them, "
                        "or add shader passes and whole presets, as RetroArch's Shader menu does. Changes "
                        "run as a copy, so the preset's own file stays as it is until you save.");
    ImGui::BeginDisabled(!g_slang_build_edited || source.empty());
    if (ImGui::Button("Undo Changes")) slang_select(source);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Go back to the preset as it is saved.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Start Empty")) {
        g_slang_build = SlangPreset{};
        g_slang_build_source.clear();
        g_slang_build_name[0] = '\0';
        slang_build_changed();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove every pass, to build a chain from nothing.");

    int remove = -1, move_from = -1, move_to = -1;
    for (int i = 0; i < (int)g_slang_build.passes.size(); i++) {
        SlangPresetPass& pass = g_slang_build.passes[(size_t)i];
        ImGui::PushID(i);
        ImGui::BeginDisabled(i == 0);
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) move_from = i, move_to = i - 1;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 == (int)g_slang_build.passes.size());
        if (ImGui::ArrowButton("##down", ImGuiDir_Down)) move_from = i, move_to = i + 1;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Remove")) remove = i;
        ImGui::SameLine();
        std::error_code ec;
        const bool missing = !fs::exists(fs::u8path(pass.shader), ec);
        if (ImGui::TreeNode("##pass", "%d. %s%s", i + 1, slang_display_path(pass.shader).c_str(),
                            missing ? "  (file missing)" : "")) {
            if (draw_slang_pass_settings(pass)) slang_build_changed();
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (move_from >= 0) {
        std::swap(g_slang_build.passes[(size_t)move_from], g_slang_build.passes[(size_t)move_to]);
        slang_build_changed();
    }
    if (remove >= 0) {
        g_slang_build.passes.erase(g_slang_build.passes.begin() + remove);
        slang_build_changed();
    }
    if (g_slang_build.passes.empty()) {
        ImGui::TextDisabled("No passes yet: add a shader pass or a whole preset below.");
    }

    static char shader_filter[128] = "", append_filter[128] = "", prepend_filter[128] = "";
    const int shader = slang_filtered_combo("Add Shader Pass", "Pick a .slang shader...", shader_filter,
                                            sizeof(shader_filter), g_slang_shaders, "", slang_plain_label,
                                            false, false);
    if (shader >= 0) {
        SlangPresetPass pass;
        pass.shader = path_utf8(fs::u8path(g_slang_scanned_dir) / fs::u8path(g_slang_shaders[(size_t)shader]));
        g_slang_build.passes.push_back(std::move(pass));
        slang_build_changed();
    }
    for (const bool before : { false, true }) {
        const int picked = slang_filtered_combo(before ? "Prepend Preset" : "Append Preset",
                                                before ? "Put a preset's passes first..."
                                                       : "Add a preset's passes after these...",
                                                before ? prepend_filter : append_filter,
                                                sizeof(append_filter), g_slang_choices, "", slang_pref_label,
                                                false, false);
        if (picked < 0) continue;
        SlangPreset other;
        std::string error;
        if (!slang_preset_read(slang_preset_path(g_slang_choices[(size_t)picked]), &other, &error)) {
            g_slang_build_status = error;
            continue;
        }
        slang_preset_combine(&g_slang_build, other, before);
        slang_build_changed();
    }

    ImGui::Spacing();
    /* Whether the chain here is what the preset setting loads. */
    const bool on = g_slang_preset_pref == g_slang_builder_pref
                        ? !g_slang_build_dirty
                        : !g_slang_build_edited && !source.empty() && g_slang_preset_pref == source;
    ImGui::Checkbox("Apply Changes Automatically", &g_slang_build_auto);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Load the chain a moment after each change. Each one compiles in the\n"
                          "background first; the current look stays until it is ready.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(g_slang_build.passes.empty() || on);
    if (ImGui::Button("Apply")) slang_build_apply();
    ImGui::EndDisabled();
    if (g_slang_build_auto && g_slang_build_dirty && !g_slang_build.passes.empty() &&
        SDL_GetTicks64() - g_slang_build_changed_ms >= 400 && !ImGui::IsAnyItemActive()) {
        slang_build_apply();
    }
    if (g_slang_build_dirty && !g_slang_build.passes.empty()) {
        ImGui::TextDisabled("Not applied yet.");
    } else if (!on && !g_slang_build.passes.empty()) {
        ImGui::TextDisabled("Not on now: Apply turns this chain on.");
    } else if (on && g_slang_build_edited) {
        ImGui::TextDisabled("Your changes are on, not saved yet.");
    }

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
    ImGui::InputTextWithHint("##save_name", "Name to save it under", g_slang_build_name, sizeof(g_slang_build_name));
    ImGui::SameLine();
    const std::string name = slang_file_name(g_slang_build_name);
    std::error_code ec;
    const bool exists = !name.empty() &&
                        fs::exists(fs::u8path(slang_preset_path(SLANG_SAVED_PREFIX + name + ".slangp")), ec);
    ImGui::BeginDisabled(name.empty() || g_slang_build.passes.empty());
    if (ImGui::Button(exists ? "Overwrite Preset" : "Save Preset")) slang_build_save(name);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Presets are saved to %s (a folder's own presets are never overwritten) and are "
                        "listed first under Preset.", slang_saved_dir().c_str());
    if (!g_slang_build_status.empty()) ImGui::TextWrapped("%s", g_slang_build_status.c_str());
    ImGui::PopTextWrapPos();
    ImGui::TreePop();
}

/* ---- Deleting presets of your own ----
 * Only the ones in shader_presets/; a folder's presets are never touched.
 * Deleted files go to the Recycle Bin or Trash where there is one
 * (gb_host_trash). */
static std::vector<std::string> g_slang_delete;   /* what the open confirmation would delete */

static bool slang_delete_saved(const std::string& pref) {
    if (!slang_pref_is_saved(pref)) return false;
    const std::string path = slang_preset_path(pref);
    if (g_slang_probing_pref == pref) slang_cancel_probe();
    std::error_code ec;
    const bool gone = gb_host_has_trash() ? gb_host_trash(path.c_str()) != 0 : fs::remove(fs::u8path(path), ec);
    if (!gone) return false;
    g_slang_saved.erase(std::remove(g_slang_saved.begin(), g_slang_saved.end(), pref), g_slang_saved.end());
    if (g_slang_probe_cache.erase(slang_probe_key(path))) slang_probe_cache_save();
    if (g_slang_preset_pref == pref) slang_select("");
    if (g_slang_builder_origin == pref) g_slang_builder_origin.clear();
    /* The editor keeps changes made to it, as a chain of your own. */
    if (g_slang_build_source == pref) {
        if (g_slang_build_edited) g_slang_build_source.clear();
        else slang_build_load("");
    }
    return true;
}

static void slang_delete_confirmed(void) {
    size_t deleted = 0;
    std::string failed;
    for (const std::string& pref : g_slang_delete) {
        if (slang_delete_saved(pref)) deleted++;
        else failed += (failed.empty() ? "" : ", ") + slang_pref_label(pref);
    }
    slang_rebuild_choices();
    save_runtime_preferences();
    g_slang_delete_status = "Deleted " + std::to_string(deleted) + (deleted == 1 ? " preset" : " presets");
    if (deleted && gb_host_has_trash()) g_slang_delete_status += std::string(" (in the ") + gb_host_trash_name() + ")";
    g_slang_delete_status += ".";
    if (!failed.empty()) g_slang_delete_status += " Could not delete " + failed + ".";
    g_slang_delete.clear();
}

static void draw_slang_delete_confirmation(void) {
    if (!g_slang_delete.empty() && !ImGui::IsPopupOpen("###slang_delete")) ImGui::OpenPopup("###slang_delete");
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    const bool one = g_slang_delete.size() == 1;
    if (!ImGui::BeginPopupModal(one ? "Delete this preset?###slang_delete" : "Delete your presets?###slang_delete",
                                NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        g_slang_delete.clear();
        return;
    }
    if (one) {
        ImGui::Text("Delete %s?", slang_pref_label(g_slang_delete[0]).c_str());
    } else {
        ImGui::Text("Delete all %zu presets you saved?", g_slang_delete.size());
    }
    if (gb_host_has_trash()) {
        ImGui::Text("Deleted presets go to the %s, where they can be restored.", gb_host_trash_name());
    } else {
        ImGui::TextUnformatted("This can't be undone.");
    }
    ImGui::TextDisabled("The presets in the shader folder stay as they are.");
    if (ImGui::Button("Delete")) {
        slang_delete_confirmed();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
        g_slang_delete.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static void draw_librashader_settings(void) {
    if (g_slang_open_section) ImGui::SetNextItemOpen(true);
    const bool open = ImGui::CollapsingHeader("Shader Presets (librashader)");
    if (g_slang_open_section) {
        ImGui::SetScrollHereY(0.0f);
        g_slang_open_section = false;
    }
    if (!open) return;

    ImGui::PushTextWrapPos(0.0f);
    if (!gb_lrs_available()) {
        ImGui::TextDisabled("%s.", gb_lrs_status());
#if defined(_WIN32)
        ImGui::TextDisabled("Presets need librashader.dll (built with its OpenGL runtime) beside the executable.");
#else
        ImGui::TextDisabled("Presets need librashader.so (built with its OpenGL runtime) beside the executable.");
#endif
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::TextDisabled("RetroArch .slangp presets. While one is on it replaces the Shader setting above.");

    if (!g_slang_scanned || g_slang_scanned_dir != slang_shader_dir()) slang_scan_presets();

    static char filter[128] = "";
    const std::string preview = slang_pref_label(g_slang_preset_pref);
    const int picked = slang_filtered_combo("Preset", preview.c_str(), filter, sizeof(filter), g_slang_choices,
                                            g_slang_preset_pref, slang_pref_label, true, true);
    if (picked == -2) slang_select("");
    else if (picked >= 0) slang_select(g_slang_choices[(size_t)picked]);
    ImGui::TextDisabled("%zu presets in %s%s", g_slang_presets.size(), g_slang_scanned_dir.c_str(),
                        g_slang_saved.empty() ? "" : ", and the ones you saved");

    if (!g_slang_preset_pref.empty()) {
        if (ImGui::Button("Edit Preset")) {
            /* Unless the editor holds changes, show the preset that is on. */
            if (!g_slang_build_edited && g_slang_build_source != g_slang_preset_pref) {
                slang_build_load(g_slang_preset_pref);
            }
            g_slang_build_open = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open this preset's passes below, to change how it is set up.");
        ImGui::SameLine();
        if (ImGui::Button("Reload")) g_slang_load_pending = true;
        ImGui::SameLine();
        if (ImGui::Button("Turn Off")) slang_select("");
        if (slang_pref_is_saved(g_slang_preset_pref) && g_slang_preset_pref != g_slang_builder_pref) {
            ImGui::SameLine();
            if (ImGui::Button("Delete Preset")) g_slang_delete = { g_slang_preset_pref };
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete this preset of yours.");
        }
    }
    if (ImGui::Button("Rescan Folder")) slang_scan_presets();
    if (!g_slang_saved.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Delete All My Presets")) g_slang_delete = g_slang_saved;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Delete every preset you saved, leaving the folder's own.");
        }
    }
    draw_slang_delete_confirmation();
    if (!g_slang_delete_status.empty()) ImGui::TextDisabled("%s", g_slang_delete_status.c_str());

    if (ImGui::Checkbox("Preset draws the whole window", &g_slang_fill_window)) {
        save_runtime_preferences();
    }
    ImGui::TextDisabled("For bezel presets (Mega Bezel, koko-aio) that draw a television around the "
                        "picture. Others stretch to the window with this on.");

    static char dir_buf[512] = "";
    static bool dir_synced = false;
    if (!dir_synced) {
        snprintf(dir_buf, sizeof(dir_buf), "%s", g_slang_dir_pref.c_str());
        dir_synced = true;
    }
    if (ImGui::InputTextWithHint("Shader folder", "shaders beside the game", dir_buf, sizeof(dir_buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        g_slang_dir_pref = dir_buf;
        g_slang_scanned = false;
        save_runtime_preferences();
    }
    ImGui::TextDisabled("Any folder of .slangp presets, such as RetroArch's shaders_slang. "
                        "Press Enter to apply; leave empty for the default.");

    if (!g_slang_probing_pref.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f),
                           "Compiling %s in the background (%.0f s). The current look stays until it is "
                           "ready; the largest presets take about a minute the first time.",
                           slang_pref_label(g_slang_probing_pref).c_str(), gb_lrs_probe_seconds());
    } else if (gb_lrs_error()[0]) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", gb_lrs_error());
    }
    if (gb_lrs_unoptimized()) {
        ImGui::TextDisabled("Compiled with the HLSL optimizer off: Microsoft's shader compiler crashes on "
                            "%s otherwise.", slang_pref_label(g_slang_running_pref).c_str());
    }
    ImGui::PopTextWrapPos();

    const int count = gb_lrs_param_count();
    char label[64];
    snprintf(label, sizeof(label), "Parameters (%d)###slang_params", count);
    if (gb_lrs_active() && count > 0 && ImGui::TreeNode(label)) {
        if (ImGui::Button("Reset All")) {
            for (int i = 0; i < count; i++) gb_lrs_set_param(i, gb_lrs_param(i)->initial);
            g_slang_param_prefs.clear();
            save_runtime_preferences();
        }
        for (int i = 0; i < count; i++) {
            const GBLrsParam* p = gb_lrs_param(i);
            if (p->maximum <= p->minimum) {   /* a heading: the range leaves nothing to set */
                ImGui::TextDisabled("%s", p->description);
                continue;
            }
            ImGui::PushID(i);
            float value = p->value;
            const float step = p->step;
            const char* format = step >= 1.0f ? "%.0f" : step >= 0.1f ? "%.1f" : step >= 0.01f ? "%.2f" : "%.3f";
            if (ImGui::SliderFloat(p->description, &value, p->minimum, p->maximum, format)) {
                if (step > 0.0f) value = p->minimum + std::round((value - p->minimum) / step) * step;
                slang_set_param(i, value);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) save_runtime_preferences();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("%s (default %g)", p->name, (double)p->initial);
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    draw_slang_builder();
}

/* The largest whole scale at which content fits the window, at least 1. */
static int largest_whole_scale(int window_w, int window_h, int content_w, int content_h) {
    const int scale = std::min(window_w / content_w, window_h / content_h);
    return scale < 1 ? 1 : scale;
}

static void update_game_viewport(void) {
    /* The custom view's native picture on its own has its own scaling, and
     * the part of the view gb_custom_fit kept may ask for a scale. Borders
     * surround a 160 x 144 view only. */
    const bool border = !g_native_presented && border_content_width() != view_width();
    const int content_w = border ? border_content_width() : presentation_width();
    const int content_h = border ? border_content_height() : presentation_height();
    int mode = (int)g_render_scaling_mode, whole_scale = 0;
    if (g_native_presented && gb_custom_native_scaling >= 0) {
        if (gb_custom_native_scaling == GB_CUSTOM_NATIVE_WHOLE_SCALE) {
            whole_scale = std::max(gb_custom_native_scale, 1);
        } else if (gb_custom_native_scaling < (int)IM_ARRAYSIZE(g_render_scaling_mode_names)) {
            mode = gb_custom_native_scaling;
        }
    }

    int window_w = 0;
    int window_h = 0;
    if (g_window) {
        SDL_GetWindowSize(g_window, &window_w, &window_h);
    } else if (!gb_custom_render) {
        g_game_viewport.x = 0;
        g_game_viewport.y = 0;
        g_game_viewport.w = content_w * g_scale;
        g_game_viewport.h = content_h * g_scale;
        return;
    } else {
        /* No window (benchmark runs): the windowed size the custom view
         * resolves against, so the debug server's `window` reports its rect. */
        window_w = g_windowed_width;
        window_h = g_windowed_height;
    }
    if (window_w <= 0) window_w = content_w;
    if (window_h <= 0) window_h = content_h;

    int viewport_w = window_w;
    int viewport_h = window_h;

    if (!g_native_presented && !border && g_custom_present_scale > 0.0) {
        viewport_w = round_to_int(content_w * g_custom_present_scale);
        viewport_h = round_to_int(content_h * g_custom_present_scale);
        mode = -1;
    }
    if (whole_scale > 0) {
        /* A chosen whole scale, down to the largest that fits the window. */
        whole_scale = std::min(whole_scale, largest_whole_scale(window_w, window_h, content_w, content_h));
        viewport_w = content_w * whole_scale;
        viewport_h = content_h * whole_scale;
        mode = -1;
    }

    switch (mode) {
        case -1:
            break;
        case GB_RENDER_SCALING_PIXEL_PERFECT: {
            const int integer_scale = largest_whole_scale(window_w, window_h, content_w, content_h);
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
    /* An expanded view can be thousands of pixels: a window scale it would
     * overflow the screen at steps down to the largest that fits (at least 1). */
    int scale = g_scale;
    SDL_Rect usable;
    if (gb_custom_render && g_window &&
        SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(g_window), &usable) == 0) {
        while (scale > 1 && (border_content_width() * scale > usable.w ||
                             border_content_height() * scale > usable.h)) {
            --scale;
        }
    }
    g_windowed_width = border_content_width() * scale;
    g_windowed_height = border_content_height() * scale;

    if (g_window && g_fullscreen_mode == 0) {
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

/* Apply a fullscreen mode (0 off / 1 borderless / 2 exclusive). Handles all
 * three transition directions, including switching directly between the two
 * fullscreen modes (borderless <-> exclusive) without passing back through
 * windowed. */
static void set_fullscreen_mode(int mode) {
    if (mode < 0 || mode > 2) {
        mode = 0;
    }
    if (!g_window || g_fullscreen_mode == mode) {
        return;
    }

    if (mode == 0) {
        if (SDL_SetWindowFullscreen(g_window, 0) == 0) {
            g_fullscreen_mode = 0;
            SDL_SetWindowSize(g_window, g_windowed_width, g_windowed_height);
            SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
    } else {
        if (g_fullscreen_mode == 0) {
            /* Leaving windowed mode: remember the windowed size so turning
             * fullscreen back off (or switching to a smaller display) can
             * restore it. Only capture on the windowed->fullscreen edge —
             * not when hopping between the two fullscreen modes. */
            SDL_GetWindowSize(g_window, &g_windowed_width, &g_windowed_height);
        }
        Uint32 flag = (mode == 2) ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_FULLSCREEN_DESKTOP;
        if (SDL_SetWindowFullscreen(g_window, flag) == 0) {
            g_fullscreen_mode = mode;
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
                 presentation_width(), presentation_height(), 0,
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
    g_vsync = false;   /* update_swap_interval() applies it */
    g_show_overlay = false;
    g_max_speed_mode = false;
    g_fast_forward_speed_percent = 250;
    g_max_speed_percent = GB_SPEED_UNLIMITED;
    g_fast_forward_frameskip = true;
    g_render_scaling_mode = GB_RENDER_SCALING_PIXEL_PERFECT;
    g_render_filter_mode = GB_RENDER_FILTER_NEAREST;
    update_render_filter();

    const int want_fullscreen_mode = platform_default_fullscreen_mode();
    if (g_window) {
        if (g_fullscreen_mode != want_fullscreen_mode) {
            set_fullscreen_mode(want_fullscreen_mode);
        }
        if (g_fullscreen_mode == 0) {
            apply_window_scale_preset();
        } else {
            update_game_viewport();
        }
    } else {
        g_fullscreen_mode = want_fullscreen_mode;
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

#ifdef RECOMP_LAUNCHER
static const char* const g_runtime_scaling_choices[] = {
    "Pixel Perfect", "Aspect Fit", "Aspect Fill", "Stretch",
};
static const char* const g_runtime_color_choices[] = { "Off", "GBC", "GBA" };
static const RecompRuntimeUiItem g_runtime_extra_items[] = {
    { "gbc.scaling_mode", "Graphics", "Scaling mode",
      "Choose integer pixels, preserve/fill aspect, or stretch.",
      RECOMP_RUNTIME_UI_CHOICE, 0, 3, 1,
      g_runtime_scaling_choices, 4, NULL },
    { "gbc.color_correction", "Graphics", "Color correction",
      "Match raw output, the Game Boy Color LCD, or the GBA LCD.",
      RECOMP_RUNTIME_UI_CHOICE, 0, 2, 1,
      g_runtime_color_choices, 3, NULL },
    { RECOMP_RUNTIME_UI_KEY_LCD_GHOSTING, "Graphics", "LCD shader",
      "Simulate the response and subpixels of the original LCD.",
      RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, NULL, 0, NULL },
    { "gbc.shader_preset", "Graphics", "Shader presets",
      "Pick, build and tune RetroArch .slangp presets.",
      RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, NULL, 0, NULL },
    /* Short descriptions: the row's -/+ buttons sit over the end of them. */
    { "gbc.menu_dim", "Display", "Game dimming",
      "How dark the game gets behind a menu.",
      RECOMP_RUNTIME_UI_INT, 0, 100, 10, NULL, 0, NULL },
    { "gbc.menu_opacity", "Display", "Menu opacity",
      "How solid the menus' backgrounds are.",
      RECOMP_RUNTIME_UI_INT, 0, 100, 10, NULL, 0, NULL },
    { "gbc.state_slot", "System", "State slot",
      "The slot Save state and Load state use.",
      RECOMP_RUNTIME_UI_INT, 1, GB_SAVESTATE_SLOT_COUNT, 1, NULL, 0, NULL },
    { "gbc.advanced", "System", "Advanced settings",
      "Open Game Boy-specific palettes, borders, link, cheats, and controls.",
      RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, NULL, 0, NULL },
    { "gbc.pause_in_menu", "System", "Pause in menu",
      "Hold the game while a menu is open.",
      RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, NULL, 0, NULL },
    { "gbc.restart", "System", "Restart game",
      "Back to the boot, keeping saves. Press twice.",
      RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, NULL, 0, NULL },
    { "gbc.return_to_launcher", "System", "Return to launcher",
      "Close the game, open the launcher. Press twice.",
      RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, NULL, 0, NULL },
    { "gbc.quit", "System", "Quit game",
      "Close the game. Press twice.",
      RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, NULL, 0, NULL },
    { "gbc.notify_save_load", "Notifications", "Save and load",
      "A state saved or loaded. Failures always show.",
      RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, NULL, 0, NULL },
    { "gbc.notify_slot", "Notifications", "State slot",
      "The slot picked, and whether it holds a state.",
      RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, NULL, 0, NULL },
    { "gbc.notify_rewind", "Notifications", "Rewind",
      "Rewinding, and the buffer running out.",
      RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, NULL, 0, NULL },
    { "gbc.notify_fast_forward", "Notifications", "Fast forward",
      "Fast forward on, and its speed.",
      RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, NULL, 0, NULL },
};

/* Leaving the game from this menu takes a second press within three seconds;
 * the first one says so in the footer (with a recomp-ui that has no footer
 * message, one press does). */
static bool runtime_ui_confirmed(const char* key, const char* prompt) {
#ifndef RECOMP_RUNTIME_UI_HAS_STATUS
    (void)key; (void)prompt;
    return true;
#else
    static std::string pending;
    static uint64_t pending_ms = 0;
    const uint64_t now = SDL_GetTicks64();
    if (pending == key && now - pending_ms <= 3000) {
        pending.clear();
        return true;
    }
    pending = key;
    pending_ms = now;
    recomp_runtime_ui_set_status(g_runtime_ui, prompt);
    return false;
#endif
}

static int runtime_ui_get(void*, const RecompRuntimeUiItem* item, int* out) {
    if (!item || !out) return 0;
    if (strcmp(item->key, RECOMP_RUNTIME_UI_KEY_FULLSCREEN) == 0) *out = g_fullscreen_mode;
    else if (strcmp(item->key, RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE) == 0) *out = g_scale;
    else if (strcmp(item->key, RECOMP_RUNTIME_UI_KEY_LINEAR_FILTER) == 0) *out = g_render_filter_mode == GB_RENDER_FILTER_LINEAR;
    else if (strcmp(item->key, RECOMP_RUNTIME_UI_KEY_AUDIO) == 0) *out = !g_audio_muted;
    else if (strcmp(item->key, RECOMP_RUNTIME_UI_KEY_VOLUME) == 0) *out = (int)std::min(g_audio_volume_percent, 100u);
    else if (strcmp(item->key, "gbc.scaling_mode") == 0) *out = (int)g_render_scaling_mode;
    else if (strcmp(item->key, "gbc.color_correction") == 0) *out = g_color_correction;
    else if (strcmp(item->key, "gbc.menu_dim") == 0) *out = g_menu_dim_percent;
    else if (strcmp(item->key, "gbc.menu_opacity") == 0) *out = g_menu_opacity_percent;
    else if (strcmp(item->key, "gbc.pause_in_menu") == 0) *out = g_menu_pauses_game;
    else if (strcmp(item->key, "gbc.state_slot") == 0) *out = g_savestate_slot + 1;
    else if (strcmp(item->key, "gbc.notify_save_load") == 0) *out = g_notify_save_load;
    else if (strcmp(item->key, "gbc.notify_slot") == 0) *out = g_notify_slot;
    else if (strcmp(item->key, "gbc.notify_rewind") == 0) *out = g_notify_rewind;
    else if (strcmp(item->key, "gbc.notify_fast_forward") == 0) *out = g_notify_fast_forward;
    else if (strcmp(item->key, RECOMP_RUNTIME_UI_KEY_LCD_GHOSTING) == 0) {
        const int active = g_shader_pipeline ? gb_shader_pipeline_active(g_shader_pipeline) : -1;
        const char* name = active >= 0 ? gb_shader_pipeline_name(g_shader_pipeline, active) : NULL;
        *out = name && strcmp(name, "lcd") == 0;
    } else return 0;
    return 1;
}

static int runtime_ui_set(void*, const RecompRuntimeUiItem* item, int value) {
    if (!item) return 0;
    if (strcmp(item->key, RECOMP_RUNTIME_UI_KEY_FULLSCREEN) == 0) set_fullscreen_mode(value);
    else if (strcmp(item->key, RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE) == 0) {
        g_scale = std::max(1, std::min(value, 8));
        apply_window_scale_preset();
    } else if (strcmp(item->key, RECOMP_RUNTIME_UI_KEY_LINEAR_FILTER) == 0) {
        g_render_filter_mode = value ? GB_RENDER_FILTER_LINEAR : GB_RENDER_FILTER_NEAREST;
        update_render_filter();
    } else if (strcmp(item->key, RECOMP_RUNTIME_UI_KEY_AUDIO) == 0) {
        g_audio_muted = !value;
        reset_audio_output_buffer(true);
    } else if (strcmp(item->key, RECOMP_RUNTIME_UI_KEY_VOLUME) == 0) {
        g_audio_volume_percent = (uint32_t)std::max(0, std::min(value, 100));
    } else if (strcmp(item->key, "gbc.scaling_mode") == 0) {
        g_render_scaling_mode = (GBRenderScalingMode)std::max(0, std::min(value, 3));
        update_game_viewport();
    } else if (strcmp(item->key, "gbc.color_correction") == 0) {
        g_color_correction = std::max(0, std::min(value, 2));
        set_active_game_pref_int("color_correction", g_color_correction);
    } else if (strcmp(item->key, "gbc.menu_dim") == 0) {
        g_menu_dim_percent = std::max(0, std::min(value, 100));
    } else if (strcmp(item->key, "gbc.menu_opacity") == 0) {
        g_menu_opacity_percent = std::max(0, std::min(value, 100));
    } else if (strcmp(item->key, "gbc.pause_in_menu") == 0) {
        set_menu_pauses_game(value != 0);
    } else if (strcmp(item->key, "gbc.state_slot") == 0) {
        /* Saves the preference and puts the slot's summary in the footer,
         * which "Saved" would replace. */
        select_savestate_slot(g_registered_ctx, std::max(1, std::min(value, GB_SAVESTATE_SLOT_COUNT)) - 1);
        return 0;
    } else if (strcmp(item->key, "gbc.notify_save_load") == 0) {
        g_notify_save_load = value != 0;
    } else if (strcmp(item->key, "gbc.notify_slot") == 0) {
        g_notify_slot = value != 0;
    } else if (strcmp(item->key, "gbc.notify_rewind") == 0) {
        g_notify_rewind = value != 0;
    } else if (strcmp(item->key, "gbc.notify_fast_forward") == 0) {
        g_notify_fast_forward = value != 0;
    } else if (strcmp(item->key, RECOMP_RUNTIME_UI_KEY_LCD_GHOSTING) == 0) {
        if (!g_shader_pipeline || !gb_shader_pipeline_set_active_by_name(
                g_shader_pipeline, value ? "lcd" : "sharp")) return 0;
        g_active_shader_pref = value ? "lcd" : "sharp";
        set_active_game_pref("shader", g_active_shader_pref);
    } else return 0;
    return 1;
}

static int runtime_ui_action(void*, const RecompRuntimeUiItem* item) {
    if (!item) return 0;
    if (strcmp(item->key, RECOMP_RUNTIME_UI_KEY_RESUME) == 0) {
        recomp_runtime_ui_close(g_runtime_ui);
    } else if (strcmp(item->key, RECOMP_RUNTIME_UI_KEY_SAVE_STATE) == 0) {
        /* The footer keeps the message save/load put there ("Done" would
         * replace it). */
        save_savestate_slot(g_registered_ctx, g_savestate_slot);
        return 0;
    } else if (strcmp(item->key, RECOMP_RUNTIME_UI_KEY_LOAD_STATE) == 0) {
        load_savestate_slot(g_registered_ctx, g_savestate_slot);
        return 0;
    } else if (strcmp(item->key, "gbc.advanced") == 0) {
        recomp_runtime_ui_close(g_runtime_ui);
        g_show_menu = true;
    } else if (strcmp(item->key, "gbc.shader_preset") == 0) {
        recomp_runtime_ui_close(g_runtime_ui);
        g_show_menu = true;
        g_slang_open_section = true;
    } else if (strcmp(item->key, "gbc.restart") == 0) {
        if (!runtime_ui_confirmed(item->key, "Press again to restart")) return 0;
        request_restart_game();
    } else if (strcmp(item->key, "gbc.return_to_launcher") == 0) {
        if (!runtime_ui_confirmed(item->key, "Press again for the launcher")) return 0;
        request_exit(GB_PLATFORM_EXIT_RETURN_TO_LAUNCHER);
    } else if (strcmp(item->key, "gbc.quit") == 0) {
        if (!runtime_ui_confirmed(item->key, "Press again to quit")) return 0;
        request_exit(GB_PLATFORM_EXIT_QUIT);
    } else return 0;
    return 1;
}

static int runtime_ui_enabled(void*, const RecompRuntimeUiItem* item) {
    if (strcmp(item->key, "gbc.restart") == 0) return can_restart_game();
    if (strcmp(item->key, "gbc.return_to_launcher") == 0) return launcher_available();
    if (strcmp(item->key, RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE) == 0)
        return g_fullscreen_mode == 0;
    if (strcmp(item->key, "gbc.color_correction") == 0)
        return g_active_hardware_mode_pref == GB_HARDWARE_MODE_CGB ||
               g_active_hardware_mode_pref == GB_HARDWARE_MODE_GBA;
    if (strcmp(item->key, RECOMP_RUNTIME_UI_KEY_LCD_GHOSTING) == 0)
        return g_shader_pipeline != NULL && !gb_lrs_active();   /* a preset replaces it */
    return 1;
}

static void runtime_ui_save(void*) { save_runtime_preferences(); }

static void create_runtime_ui(void) {
    RecompRuntimeUiStandardConfig config = {};
    config.menu.title = "Game Boy Recompiled";
    config.menu.subtitle = "Runtime settings";
    config.menu.theme = "gbc";
    config.menu.accept_label = "A / Enter";
    config.menu.back_label = "B / Esc";
    config.menu.callbacks = { NULL, runtime_ui_get, runtime_ui_set,
                              runtime_ui_action, runtime_ui_enabled,
                              runtime_ui_save,
                              [](void*, int) { menu_visibility_changed(); } };
    config.features = RECOMP_RUNTIME_UI_STANDARD_FULLSCREEN |
                      RECOMP_RUNTIME_UI_STANDARD_WINDOW_SCALE |
                      RECOMP_RUNTIME_UI_STANDARD_LINEAR_FILTER |
                      RECOMP_RUNTIME_UI_STANDARD_AUDIO |
                      RECOMP_RUNTIME_UI_STANDARD_VOLUME |
                      RECOMP_RUNTIME_UI_STANDARD_RESUME |
                      RECOMP_RUNTIME_UI_STANDARD_SAVE_STATE |
                      RECOMP_RUNTIME_UI_STANDARD_LOAD_STATE;
    config.window_scale_max = 8;
    config.extra_items = g_runtime_extra_items;
    config.extra_item_count = sizeof(g_runtime_extra_items) / sizeof(g_runtime_extra_items[0]);
    g_runtime_ui = recomp_runtime_ui_create_standard(&config);
}
#endif

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

static std::string action_hint_label(GBInputAction action) {
    for (int slot = 0; slot < 2; slot++) {
        if (binding_is_valid(g_keyboard_bindings[action][slot])) {
            return binding_display_label(g_keyboard_bindings[action][slot]);
        }
    }
    for (int slot = 0; slot < 2; slot++) {
        if (binding_is_valid(g_controller_bindings[action][slot])) {
            return binding_display_label(g_controller_bindings[action][slot]);
        }
    }
    return "Unbound";
}

static void draw_pause_overlay(void) {
    static const char* const names[8] = { "Right", "Left", "Up", "Down", "A", "B", "Select", "Start" };
    const uint8_t input = current_joypad_input();
    std::string held;
    for (int bit = 0; bit < 8; bit++) {
        if (!(input & (1u << bit))) {
            held += held.empty() ? names[bit] : std::string(" + ") + names[bit];
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    const float pad = 8.0f * io.FontGlobalScale;
    ImGui::SetNextWindowPos(ImVec2(pad, pad), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("##pause_overlay", NULL,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoInputs)) {
        ImGui::Text("Paused - %s: resume, %s: advance%s%s%s",
                    action_hint_label(GB_INPUT_ACTION_PAUSE).c_str(),
                    action_hint_label(GB_INPUT_ACTION_FRAME_ADVANCE).c_str(),
                    g_rewind_enabled ? ", " : "",
                    g_rewind_enabled ? action_hint_label(GB_INPUT_ACTION_REWIND).c_str() : "",
                    g_rewind_enabled ? ": back" : "");
        ImGui::Text("Held: %s", held.empty() ? "nothing" : held.c_str());
        ImGui::Text("Frames run with this input: %u",
                    input == g_pause_input ? g_pause_input_frames : 0u);
    }
    ImGui::End();
}

/* RetroArch's messages, bottom left, each above the ones drawn before it:
 * `stacked` is the height those took, and the new total is returned. */
static float draw_corner_message(const char* id, const char* text, float alpha, float stacked) {
    ImGuiIO& io = ImGui::GetIO();
    const float pad = 8.0f * io.FontGlobalScale;
    const float bottom = io.DisplaySize.y - pad - stacked;
    float height = 0.0f;
    ImGui::SetNextWindowPos(ImVec2(pad, bottom), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    if (ImGui::Begin(id, NULL,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoInputs)) {
        ImGui::TextUnformatted(text);
        height = ImGui::GetWindowHeight();
    }
    ImGui::End();
    ImGui::PopStyleVar();
    return height > 0.0f ? stacked + height + pad * 0.5f : stacked;
}

/* The rewind messages while the key is held, RetroArch's fast-forward
 * indicator while a Fast Forward shortcut is on, and the latest state message
 * (post_state_notice) for three seconds with the last half second fading, as
 * RetroArch's 180-frame messages. notify.rewind / notify.fast_forward turn
 * the first two off. */
static const char* rewind_status_text(void);
static std::string fast_forward_status_text(void);
static void draw_corner_messages(void) {
    static constexpr uint64_t kShowMs = 3000, kFadeMs = 500;
    float stacked = 0.0f;
    if (const char* rewind_text = rewind_status_text(); rewind_text && g_notify_rewind) {
        stacked = draw_corner_message("##rewind_overlay", rewind_text, 1.0f, stacked);
    }
    if (const std::string fast_forward = fast_forward_status_text(); !fast_forward.empty() && g_notify_fast_forward) {
        stacked = draw_corner_message("##fast_forward_overlay", fast_forward.c_str(), 1.0f, stacked);
    }
    if (g_state_notice.empty()) return;
    const uint64_t age = SDL_GetTicks64() - g_state_notice_ms;
    if (age >= kShowMs) {
        g_state_notice.clear();
        return;
    }
    const float alpha = age + kFadeMs > kShowMs ? (float)(kShowMs - age) / (float)kFadeMs : 1.0f;
    draw_corner_message("##state_notice", g_state_notice.c_str(), alpha, stacked);
}

/* RetroArch's fast-forward frame skip (video_driver_frame), ported: while the
 * game runs above 100% (RetroArch: nonblocking, which is also when V-Sync is
 * off here), the time between frames accumulates and a frame is drawn only
 * once it reaches the display's refresh period. The first frame's interval is
 * ignored (no rubber band at the start), a frame drawn with a whole spare
 * interval in hand drops it (so an outside frame limiter cannot pull the
 * speed down to 1x), and a runaway accumulator, when the host cannot keep up,
 * is reset. The menu and pause always draw. Deviation: RetroArch keeps the
 * interval in 16 bits, which wraps after 65 ms; here it saturates. */
static bool fast_forward_frame_due(void) {
    static uint64_t last_time;
    static int32_t accumulator;
    static int8_t nonblock_active;
    const uint64_t new_time = SDL_GetPerformanceCounter() * 1000000ull / SDL_GetPerformanceFrequency();
    bool render = true;
    if (g_fast_forward_frameskip && effective_speed_percent() > 100 && !menu_open() && !g_user_paused) {
        int refresh = 60;
        SDL_DisplayMode mode;
        const int display = g_window ? SDL_GetWindowDisplayIndex(g_window) : 0;
        if (SDL_GetCurrentDisplayMode(display < 0 ? 0 : display, &mode) == 0 && mode.refresh_rate > 0) {
            refresh = mode.refresh_rate;
        }
        const int32_t previous = accumulator;
        const int32_t delta = (int32_t)std::min<uint64_t>(new_time - last_time, 65535);
        const int32_t target = (int32_t)(1000000.0f / (float)refresh);
        if (!nonblock_active) {
            nonblock_active = -1;
        } else if (nonblock_active < 0) {
            nonblock_active = 1;
        }
        if (nonblock_active > 0) {
            accumulator += delta;
        }
        render = accumulator >= target;
        if (render) {
            accumulator -= target;
            if (previous - accumulator >= delta) {
                accumulator -= delta;
            }
            if (accumulator < 0 || accumulator > target) {
                accumulator = 0;
            }
        }
    } else {
        nonblock_active = 0;
        accumulator = 0;
    }
    last_time = new_time;
    return render;
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

    /* A skipped frame is still counted above; one picked for a dump is drawn. */
    const bool dump_frame =
        (count_guest_frame &&
         frame_is_selected_for_dump(g_dump_frames, g_dump_count, (uint32_t)g_frame_count)) ||
        frame_is_selected_for_dump(g_dump_present_frames, g_dump_present_count, (uint32_t)g_frame_count);
    if (!g_benchmark_mode && g_gl_context && !fast_forward_frame_due() && !dump_frame) {
        g_frames_skipped++;
        g_last_timing.upload_ms = 0.0;
        g_last_timing.compose_ms = 0.0;
        g_last_timing.present_ms = 0.0;
        g_last_timing.total_render_ms = 0.0;
        return;
    }

    if (gb_custom_render && g_registered_ctx) {
        int ww = g_windowed_width, wh = g_windowed_height;
        if (g_window) SDL_GetWindowSize(g_window, &ww, &wh);
        int width, height;
        const int whole_pixels = g_render_scaling_mode == GB_RENDER_SCALING_PIXEL_PERFECT;
        gb_custom_resolve_size(ww, wh, whole_pixels, &width, &height);
        const bool was_native = g_native_presented;
        const int shown_width = presentation_width(), shown_height = presentation_height();
        const double shown_scale = g_custom_present_scale;
        gb_custom_view_width = width;
        gb_custom_view_height = height;
        /* The game may show less of the world than the view (a room smaller
         * than it), at a scale of its own. */
        double scale = 0.0;
        if (gb_custom_fit) {
            gb_custom_fit(g_registered_ctx, ww, wh, whole_pixels, &width, &height, &scale);
            width = std::max(GB_SCREEN_WIDTH, std::min(width, gb_custom_view_width));
            height = std::max(GB_SCREEN_HEIGHT, std::min(height, gb_custom_view_height));
        }
        gb_custom_width = width;
        gb_custom_height = height;
        g_custom_present_scale = scale > 0.0 ? scale : 0.0;
        g_custom_framebuffer.resize((size_t)width * height);
        const int native_width = gb_ws_render_width();
        /* A frame the game declines shows the native picture: on its own and
         * scaled to the window, or centered in the view. Frames that are not
         * new guest frames (the LCD is off, or the last one presented again)
         * keep the last frame's choice, so a blank LCD never switches it. */
        static int applied_scaling = GB_CUSTOM_NATIVE_IN_VIEW, applied_scale = 1;
        const bool setting_changed = gb_custom_native_scaling != applied_scaling ||
                                     gb_custom_native_scale != applied_scale;
        applied_scaling = gb_custom_native_scaling;
        applied_scale = gb_custom_native_scale;
        const bool drawn = gb_custom_draw_frame(g_registered_ctx, g_custom_framebuffer.data(), width,
                                                framebuffer);
        if (count_guest_frame || setting_changed) {
            g_native_presented = !drawn && gb_custom_native_scaling != GB_CUSTOM_NATIVE_IN_VIEW &&
                                 native_width == GB_SCREEN_WIDTH;
        }
        if (!g_native_presented) {
            if (!drawn) {
                gb_custom_center_native(g_custom_framebuffer.data(), width, framebuffer, native_width);
            }
            framebuffer = g_custom_framebuffer.data();
        }
        if (presentation_width() != shown_width || presentation_height() != shown_height ||
            g_native_presented != was_native || setting_changed) {
            recreate_streaming_texture();
            update_game_viewport();
            /* History and feedback passes would blend pictures of both sizes. */
            gb_lrs_clear_history();
        } else if (g_custom_present_scale != shown_scale) {
            update_game_viewport();
        }
    }

    /* Publish what the user is about to see. Deliberately placed after the
     * compositor and before the benchmark/headless early-out below, so the
     * `screenshot` debug command captures the *presented* frame (wide when the
     * custom view is armed) in windowed and headless runs alike. Both possible
     * sources (g_custom_framebuffer and the PPU's rgb_framebuffer) are stable
     * allocations, so a pointer is enough; it names the most recent frame. */
    g_presented_frame = framebuffer;
    g_presented_width = presentation_width();
    g_presented_height = presentation_height();

    if (count_guest_frame) {
        /* Handle Screenshot Dumping */
        if (frame_is_selected_for_dump(g_dump_frames, g_dump_count, (uint32_t)g_frame_count)) {
            char filename[128];
            snprintf(filename, sizeof(filename), "%s_%05d.ppm", g_screenshot_prefix, g_frame_count);
            save_ppm(filename, framebuffer, presentation_width(), presentation_height(), g_frame_count);
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
        save_ppm(filename, framebuffer, presentation_width(), presentation_height(), g_frame_count);
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
        for (int i = 0; i < presentation_width() * presentation_height(); i++) {
            if (framebuffer[i] != white) {
                has_content = true;
                break;
            }
        }
        DBG_FRAME("Platform frame %d - has_content=%d, first_pixel=0x%08X",
                  g_frame_count, has_content, framebuffer[0]);
    }

    /* Upload framebuffer to the GL game texture. The PPU stores pixels
     * as 0xAARRGGBB (CPU-side ARGB), which on a little-endian host is
     * BGRA in memory. GL ES 2.0 doesn't reliably expose GL_BGRA, so
     * shuffle into RGBA byte order on the way in. The optional palette
     * override (Original / Pocket / Plasma) is applied during the same
     * pass since we're touching every pixel anyway. */
    double upload_start_ms = sdl_now_ms();
    const size_t frame_pixels = (size_t)presentation_width() * presentation_height();
    static std::vector<uint32_t> s_upload_buf;
    s_upload_buf.resize(frame_pixels);
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
            static std::vector<uint32_t> s_lut_buf;
            s_lut_buf.resize(frame_pixels);
            color_lut_map_argb8888(g_color_lut, framebuffer, s_lut_buf.data(),
                                   presentation_width(), presentation_height());
            src = s_lut_buf.data();
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

        /* Neither palette nor correction (the usual case): the swap alone,
         * four pixels at a time. A 1920x1080 view is 2 million pixels. */
        size_t first = 0;
        if (!pal && !cc) {
            uint32_t* dst = s_upload_buf.data();
#if defined(__SSE2__)
            const __m128i alpha_green = _mm_set1_epi32((int)0xFF00FF00u);
            for (; first + 4 <= frame_pixels; first += 4) {
                const __m128i c = _mm_loadu_si128((const __m128i*)(src + first));
                /* 00RR00BB -> 00BB00RR: swap the 16-bit halves. */
                __m128i rb = _mm_andnot_si128(alpha_green, c);
                rb = _mm_shufflehi_epi16(_mm_shufflelo_epi16(rb, 0xB1), 0xB1);
                _mm_storeu_si128((__m128i*)(dst + first), _mm_or_si128(_mm_and_si128(c, alpha_green), rb));
            }
#endif
            for (; first < frame_pixels; first++) {
                const uint32_t c = src[first];
                dst[first] = (c & 0xFF00FF00u) | ((c >> 16) & 0xFFu) | ((c & 0xFFu) << 16);
            }
        }
        for (size_t i = first; i < frame_pixels; i++) {
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
                    presentation_width(), presentation_height(),
                    GL_RGBA, GL_UNSIGNED_BYTE, s_upload_buf.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    g_last_timing.upload_ms = sdl_now_ms() - upload_start_ms;

    /* A preset picked in last frame's menu compiles here, before this frame
     * binds anything of its own. */
    slang_apply_pending();

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
    if (g_native_presented) {
        /* The custom view's native picture has no border of its own. */
    } else if (sgb_cart_border_active()) {
        active_border = g_sgb_cart_border_texture;
    } else if (g_border_enabled && g_border_texture && view_width() == GB_SCREEN_WIDTH && view_height() == GB_SCREEN_HEIGHT) {
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
        /* The game image at (x, y, w, h). With a librashader preset the chain
         * renders it at exactly w x h and the result is copied 1:1 with the
         * passthrough; otherwise the built-in shader samples the game texture.
         * Blending is off for the copy: presets do not all write alpha 1. */
        auto draw_game = [&](int x, int y, int w, int h, int src_w, int src_h) {
            const GLuint shaded = gb_lrs_active()
                ? gb_lrs_render(g_game_tex, presentation_width(), presentation_height(), w, h)
                : 0;
            if (!shaded) {
                gb_shader_pipeline_draw(g_shader_pipeline, g_game_tex, src_w, src_h,
                                        x, y, w, h, draw_w, draw_h);
                return;
            }
            glViewport(0, 0, draw_w, draw_h);
            glDisable(GL_BLEND);
            gb_shader_pipeline_set_active_by_name(g_shader_pipeline, "sharp");
            gb_shader_pipeline_draw(g_shader_pipeline, shaded, w, h, x, y, w, h, draw_w, draw_h);
            if (active_shader >= 0) {
                gb_shader_pipeline_set_active(g_shader_pipeline, active_shader);
            }
            glEnable(GL_BLEND);
        };
        if (gb_lrs_active() && g_slang_fill_window) {
            /* Bezel presets draw a whole television around the picture, so
             * they get the window rather than the game rect (and no border). */
            draw_game(0, 0, draw_w, draw_h, presentation_width(), presentation_height());
        } else if (active_border) {
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
            draw_game(gb_x, gb_y, gb_w, gb_h, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT);
        } else {
            draw_game(vp_x, vp_y, vp_w, vp_h, presentation_width(), presentation_height());
        }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    ImGuiIO& imgui_io = ImGui::GetIO();
    imgui_io.FontGlobalScale = settings_ui_scale_for_size(imgui_io.DisplaySize);

    g_menu_text_input = false;
    g_menu_popup_open = false;
    if (g_show_menu) {
        /* Game Dimming, under the window (whose own background is Menu
         * Opacity); at the defaults the game shows about as little as it did
         * under the window's old fixed 96%. */
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            ImVec2(0.0f, 0.0f), imgui_io.DisplaySize,
            IM_COL32(0, 0, 0, (int)(255.0f * menu_dim() + 0.5f)));
        const float ui_scale = imgui_io.FontGlobalScale;
        const float footer_height = ImGui::GetFrameHeightWithSpacing() *
                                    (g_launcher_return_enabled ? 3.8f : 3.0f);

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(imgui_io.DisplaySize, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.96f * menu_opacity());
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
        const bool audio_was_running = audio_output_should_run();
        const int previous_speed = effective_speed_percent();
        ImGui::SliderInt("Speed %", &g_speed_percent, 10, 500, "%d", ImGuiSliderFlags_NoInput);
        if (ImGui::Button("Reset Speed")) {
            g_speed_percent = 100;
            g_max_speed_mode = false;
        }
        ImGui::SameLine();
        if (effective_speed_percent() == GB_SPEED_UNLIMITED) {
            ImGui::TextDisabled("Effective: Unlimited");
        } else {
            ImGui::TextDisabled("Effective: %d%%", effective_speed_percent());
        }
        if (g_fast_forward_active) {
            ImGui::TextDisabled("Fast Forward (Hold) is held.");
        } else if (g_max_speed_mode) {
            ImGui::TextDisabled("Fast Forward is on.");
        }
        /* In steps of 10%, which a 110-1000 slider is too fine to hit, and
         * one step past 1000% for Unlimited. */
        auto shortcut_speed_slider = [](const char* label, int* percent) {
            constexpr int unlimited_step = GB_SHORTCUT_SPEED_MAX_PERCENT + 10;
            int value = *percent == GB_SPEED_UNLIMITED ? unlimited_step : *percent;
            if (ImGui::SliderInt(label, &value, GB_SHORTCUT_SPEED_MIN_PERCENT, unlimited_step,
                                 value >= unlimited_step ? "Unlimited" : "%d",
                                 ImGuiSliderFlags_NoInput)) {
                value = (value + 5) / 10 * 10;
                if (value >= unlimited_step) {
                    value = GB_SPEED_UNLIMITED;
                }
                if (value != *percent) {
                    *percent = value;
                    save_runtime_preferences();
                }
            }
        };
        shortcut_speed_slider("Fast Forward Speed %", &g_max_speed_percent);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Speed while Fast Forward is on; %s turns it on and off.\n"
                              "Unlimited runs the game as fast as this PC can.",
                              action_hint_label(GB_INPUT_ACTION_TOGGLE_MAX_SPEED).c_str());
        }
        shortcut_speed_slider("Fast Forward (Hold) Speed %", &g_fast_forward_speed_percent);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Speed while %s is held.\n"
                              "Unlimited runs the game as fast as this PC can.",
                              action_hint_label(GB_INPUT_ACTION_FAST_FORWARD).c_str());
        }
        if (ImGui::Checkbox("Fast-Forward Frame Skip", &g_fast_forward_frameskip)) {
            save_runtime_preferences();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Above 100%%, draw only as many frames as the display shows,\n"
                              "as RetroArch's Fast-Forward Frame Skip does. The game still\n"
                              "runs every frame; the time goes to running it faster.");
        }
        if (effective_speed_percent() != previous_speed) {
            on_speed_changed(audio_was_running);
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
        bool pause_in_menu = g_menu_pauses_game;
        if (ImGui::Checkbox("Pause Game in Menu", &pause_in_menu)) {
            set_menu_pauses_game(pause_in_menu);
            save_runtime_preferences();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Hold the game (and its sound) while this or the Escape menu is open.\n"
                              "Off, the game keeps running behind the menu, still without your input.");
        }
        menu_look_sliders();

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
        {
            const bool audio_was_running = audio_output_should_run();
            if (ImGui::Combo("Fast-Forward Audio", &g_speed_audio_mode, g_speed_audio_mode_names,
                             IM_ARRAYSIZE(g_speed_audio_mode_names))) {
                on_speed_changed(audio_was_running);
                save_runtime_preferences();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Sound while the game runs faster than 100%%, as RetroArch offers it:\n"
                                  "Normal Pitch drops what cannot be played in time, so it skips;\n"
                                  "Sped Up plays all of it faster and higher. Below 100%% the sound\n"
                                  "plays slower and lower unless this is Mute.");
            }
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

        draw_librashader_settings();

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
            select_savestate_slot(g_registered_ctx, savestate_slot - 1);
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

        /* Notifications: the messages bottom left (draw_corner_messages). */
        ImGui::Spacing();
        ImGui::TextDisabled("Notifications");
        struct NotifyToggle { const char* label; bool* value; const char* tip; };
        const NotifyToggle notify_toggles[] = {
            { "Save/Load", &g_notify_save_load,
              "Say on screen when a state is saved or loaded. A failed save or load always shows." },
            { "Slot Changes", &g_notify_slot,
              "Say on screen which state slot the Previous/Next Slot keys picked, and whether it holds a state." },
            { "Rewind", &g_notify_rewind, "Say on screen while rewinding, and when the rewind buffer runs out." },
            { "Fast Forward", &g_notify_fast_forward,
              "Say on screen while either Fast Forward shortcut is on, and at what speed." },
        };
        /* Two to a row, the second column past the longest first-column label. */
        const ImGuiStyle& notify_style = ImGui::GetStyle();
        const float notify_column = ImGui::GetCursorPosX() + ImGui::GetFrameHeight() + notify_style.ItemInnerSpacing.x +
                                    ImGui::CalcTextSize("Save/Load").x + notify_style.ItemSpacing.x * 4.0f;
        for (size_t i = 0; i < sizeof(notify_toggles) / sizeof(notify_toggles[0]); i++) {
            if (i % 2) ImGui::SameLine(notify_column);
            if (ImGui::Checkbox(notify_toggles[i].label, notify_toggles[i].value)) {
                save_runtime_preferences();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", notify_toggles[i].tip);
            }
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
         * Cheats: libretro .cht files dropped into cheats/ (see
         * gb_cheats_load) are loaded on cart launch, or again with
         * Reload Cheats. GameShark codes apply per-frame
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
            static int cheat_page = 0;
            if (gb_cheats_count() == 0) {
                ImGui::PushTextWrapPos(0.0f);
                if (gb_cheats_game_id()[0]) {
                    ImGui::TextDisabled("No cheats found. Put a libretro-database .cht file in %s "
                                        "(or in its %s folder) and press Reload Cheats.",
                                        gb_cheats_dir(), gb_cheats_game_id());
                } else {
                    ImGui::TextDisabled("No cheats found. Put a libretro-database .cht file in %s "
                                        "and press Reload Cheats.", gb_cheats_dir());
                }
                ImGui::PopTextWrapPos();
                if (ImGui::Button("Reload Cheats")) {
                    gb_cheats_reload(g_registered_ctx);
                    cheat_page = 0;
                }
            } else {
            static char cheat_filter[64] = "";
            const int   cheats_per_page = 10;

            int total  = gb_cheats_count();
            int active = 0;
            for (int i = 0; i < total; i++) {
                if (gb_cheats_get(i)->enabled) active++;
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("Loaded %d cheats from %s  (active: %d)",
                                total, gb_cheats_dir(), active);
            ImGui::PopTextWrapPos();
            if (ImGui::InputText("Search##cheats",
                                 cheat_filter, sizeof(cheat_filter))) {
                /* Reset to page 1 on filter change so the user isn't
                 * stranded on a page that no longer has matches. */
                cheat_page = 0;
            }
            if (ImGui::Button("Disable All##cheats")) {
                gb_cheats_disable_all(g_registered_ctx);
            }
            ImGui::SameLine();
            if (ImGui::Button("Reload Cheats")) {
                gb_cheats_reload(g_registered_ctx);
                cheat_page = 0;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Turn every cheat off and read the .cht files again.");
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
            int fullscreen_mode = g_fullscreen_mode;
            if (ImGui::Combo("Fullscreen",
                             &fullscreen_mode,
                             g_fullscreen_mode_names,
                             IM_ARRAYSIZE(g_fullscreen_mode_names))) {
                set_fullscreen_mode(fullscreen_mode);
                save_runtime_preferences();
            }
            int scaling_mode = (int)g_render_scaling_mode;
            if (ImGui::Combo("Scaling Mode",
                             &scaling_mode,
                             g_render_scaling_mode_names,
                             IM_ARRAYSIZE(g_render_scaling_mode_names))) {
                g_render_scaling_mode = (GBRenderScalingMode)scaling_mode;
                update_game_viewport();
                save_runtime_preferences();
            }
            int filter_mode = (int)g_render_filter_mode;
            if (ImGui::Combo("Scale Filter",
                             &filter_mode,
                             g_render_filter_names,
                             IM_ARRAYSIZE(g_render_filter_names))) {
                g_render_filter_mode = (GBRenderFilterMode)filter_mode;
                update_render_filter();
                save_runtime_preferences();
            }

            if (g_fullscreen_mode == 0) {
                int scale_idx = g_scale - 1;
                if (ImGui::Combo("Window Size",
                                 &scale_idx,
                                 g_scale_names,
                                 IM_ARRAYSIZE(g_scale_names))) {
                    g_scale = scale_idx + 1;
                    apply_window_scale_preset();
                    save_runtime_preferences();
                }
            } else {
                ImGui::TextDisabled("Window Size disabled while fullscreen.");
            }
            if (ImGui::Checkbox("V-Sync", &g_vsync)) {   /* update_swap_interval() applies it */
                save_runtime_preferences();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Off while the game runs faster than 100%%, so fast forward is not\n"
                                  "held to the display's refresh rate.");
            }
            if (ImGui::Checkbox("Show Overlay", &g_show_overlay)) {
                save_runtime_preferences();
            }
            if (ImGui::Checkbox("Smooth Slow Frames", &g_smooth_lcd_transitions)) {
                save_runtime_preferences();
            }
            int preempt_frames = preemptive_frames();
            if (ImGui::SliderInt("Preemptive Frames", &preempt_frames, 0, GB_PREEMPT_MAX_FRAMES)) {
                g_preemptive_frames_pref = preempt_frames;
                save_runtime_preferences();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Replays each input change into this many past frames, as RetroArch\n"
                                  "does, so the game responds that many frames sooner. Set it no\n"
                                  "higher than the game's own delay, or the first frames of a\n"
                                  "response are skipped.");
            }
            if (ImGui::Checkbox("Rewind", &g_rewind_enabled)) {
                if (!g_rewind_enabled) rewind_free();
                save_runtime_preferences();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Hold %s to run the game backwards, as in RetroArch. Recent frames\n"
                                  "are kept in memory as the game runs; holding the key goes back\n"
                                  "through them, and letting go plays on from there.",
                                  action_hint_label(GB_INPUT_ACTION_REWIND).c_str());
            }
            if (g_rewind_enabled) {
                if (ImGui::SliderInt("Rewind Frames", &g_rewind_granularity, 1, GB_REWIND_MAX_GRANULARITY)) {
                    save_runtime_preferences();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Keeps one frame in this many: the buffer reaches back further,\n"
                                      "and rewinding goes back that many frames at a time.");
                }
                /* A new size starts an empty buffer, so it applies on release. */
                static int buffer_mb = 0;
                static bool buffer_dragged = false;
                if (!buffer_dragged) buffer_mb = g_rewind_buffer_mb;
                ImGui::SliderInt("Rewind Buffer (MB)", &buffer_mb, 10, GB_REWIND_MAX_BUFFER_MB, "%d",
                                 ImGuiSliderFlags_NoInput);
                buffer_dragged = ImGui::IsItemActive();
                if (ImGui::IsItemDeactivatedAfterEdit() && buffer_mb != g_rewind_buffer_mb) {
                    g_rewind_buffer_mb = buffer_mb;
                    rewind_free();   /* the next frame starts the new one */
                    save_runtime_preferences();
                }
                GBPlatformRewindInfo rewind_info;
                gb_platform_get_rewind_info(&rewind_info);
                const double fps = 4194304.0 / 70224.0;
                const double held_s = rewind_info.states * g_rewind_granularity / fps;
                if (rewind_info.capacity && rewind_info.used && rewind_info.states > 60) {
                    /* Patches so far, extended to the whole buffer. */
                    const double full = (double)rewind_info.capacity / rewind_info.used *
                                        (rewind_info.states - 1) * g_rewind_granularity / fps;
                    ImGui::TextDisabled("Holds %.0f s, room for about %.0f s (%.1f of %d MB)",
                                        held_s, full > held_s ? full : held_s,
                                        rewind_info.used / 1048576.0, g_rewind_buffer_mb);
                } else {
                    ImGui::TextDisabled("Holds %.0f s", held_s);
                }
            }

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
            if (effective_speed_percent() != 100 && g_speed_audio_mode == GB_SPEED_AUDIO_MUTE) {
                ImGui::TextDisabled("Audio paused while Speed %% != 100 (Fast-Forward Audio: Mute).");
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
            ImGui::TextDisabled("Shortcuts: fast forward, rewind, savestates, overlay, mute, menu, pause.");

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

        /* Leaving the game asks first: progress since the game last saved
         * would go. Restart Game runs in place (run_pending_restart). */
        enum { LEAVE_NONE, LEAVE_RESTART, LEAVE_LAUNCHER, LEAVE_QUIT };
        static int leave = LEAVE_NONE;
        const bool offer_launcher = launcher_available();
        const int footer_button_count = offer_launcher ? 4 : 3;
        const float footer_button_width =
            (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * (footer_button_count - 1)) /
            (float)footer_button_count;
        if (ImGui::Button("Resume", ImVec2(footer_button_width, 0.0f))) {
            g_show_menu = false;
        }
        ImGui::SameLine();
        if (!can_restart_game()) ImGui::BeginDisabled();
        if (ImGui::Button("Restart Game", ImVec2(footer_button_width, 0.0f))) leave = LEAVE_RESTART;
        if (!can_restart_game()) ImGui::EndDisabled();
        if (offer_launcher) {
            ImGui::SameLine();
            if (ImGui::Button("Return to Launcher", ImVec2(footer_button_width, 0.0f))) leave = LEAVE_LAUNCHER;
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(footer_button_width, 0.0f))) leave = LEAVE_QUIT;

        if (leave != LEAVE_NONE && !ImGui::IsPopupOpen("###leave_game")) {
            ImGui::OpenPopup("###leave_game");
        }
        const char* const leave_title = leave == LEAVE_RESTART  ? "Restart the game?###leave_game"
                                      : leave == LEAVE_LAUNCHER ? "Return to the launcher?###leave_game"
                                                                : "Quit the game?###leave_game";
        ImGui::SetNextWindowPos(ImVec2(imgui_io.DisplaySize.x * 0.5f, imgui_io.DisplaySize.y * 0.5f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal(leave_title, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(leave == LEAVE_RESTART
                                       ? "The game starts over from the boot. Its own saves are kept."
                                       : "Anything the game has not saved is lost.");
            const char* const confirm = leave == LEAVE_RESTART  ? "Restart"
                                      : leave == LEAVE_LAUNCHER ? "Return to Launcher"
                                                                : "Quit";
            if (ImGui::Button(confirm)) {
                if (leave == LEAVE_RESTART) request_restart_game();
                else request_exit(leave == LEAVE_LAUNCHER ? GB_PLATFORM_EXIT_RETURN_TO_LAUNCHER
                                                          : GB_PLATFORM_EXIT_QUIT);
                leave = LEAVE_NONE;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
                leave = LEAVE_NONE;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        } else {
            leave = LEAVE_NONE;
        }

        /* For the next events: whether Escape / B belong to a field or popup. */
        g_menu_text_input = imgui_io.WantTextInput;
        g_menu_popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ||
                            ImGui::IsAnyItemActive();
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

    if (g_user_paused && !menu_open()) {
        draw_pause_overlay();
    }
    if (!menu_open()) {
        draw_corner_messages();
    }

#ifdef RECOMP_LAUNCHER
#ifdef RECOMP_RUNTIME_UI_HAS_BACKDROP
    recomp_runtime_ui_set_backdrop(g_runtime_ui, menu_dim(), menu_opacity());
#endif
    recomp_runtime_ui_render_imgui(g_runtime_ui);
#endif
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    g_last_timing.compose_ms = sdl_now_ms() - compose_start_ms;
    /* The menus' own buttons (Resume, the confirmations) open and close them. */
    menu_visibility_changed();

    /* GBRECOMP_WINDOW_SHOTS=1: frames picked by --dump-frames are also written
     * as the composed window, <prefix>_window_<frame>.ppm. The other captures
     * read the guest framebuffer, so this is the only one that shows shaders,
     * borders and menus. */
    if (count_guest_frame &&
        frame_is_selected_for_dump(g_dump_frames, g_dump_count, (uint32_t)g_frame_count) &&
        env_flag_enabled("GBRECOMP_WINDOW_SHOTS")) {
        std::vector<uint8_t> rgba((size_t)draw_w * (size_t)draw_h * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glReadPixels(0, 0, draw_w, draw_h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        std::vector<uint32_t> argb((size_t)draw_w * (size_t)draw_h);
        for (int y = 0; y < draw_h; y++) {
            const uint8_t* src = &rgba[(size_t)(draw_h - 1 - y) * (size_t)draw_w * 4];   /* GL rows are bottom-up */
            for (int x = 0; x < draw_w; x++, src += 4) {
                argb[(size_t)y * (size_t)draw_w + (size_t)x] =
                    0xFF000000u | ((uint32_t)src[0] << 16) | ((uint32_t)src[1] << 8) | src[2];
            }
        }
        char filename[160];
        snprintf(filename, sizeof(filename), "%s_window_%05d.ppm", g_screenshot_prefix, g_frame_count);
        save_ppm(filename, argb.data(), draw_w, draw_h, g_frame_count);
    }
    if (!g_window_shot_path.empty()) {
        std::vector<uint8_t> rgba((size_t)draw_w * (size_t)draw_h * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glReadPixels(0, 0, draw_w, draw_h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 0xFF;
        const int stride = draw_w * 4;   /* GL rows are bottom-up: write from the last one */
        if (!stbi_write_png(g_window_shot_path.c_str(), draw_w, draw_h, 4,
                            rgba.data() + (size_t)(draw_h - 1) * (size_t)stride, -stride)) {
            fprintf(stderr, "[SDL] Could not write %s\n", g_window_shot_path.c_str());
        }
        g_window_shot_path.clear();
    }

    update_swap_interval();
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
    rewind_free();   /* a restarted game starts a new one, as RetroArch does per content */
    close_input_record_file();
    close_audio_output_device();
    g_audio_output_devices.clear();
    gb_debug_server_shutdown();

    clear_controller_state();
    g_binding_capture_active = false;
    g_binding_capture_device = GB_CAPTURE_DEVICE_NONE;
    
    if (ImGui::GetCurrentContext() != NULL) {
#ifdef RECOMP_LAUNCHER
        recomp_runtime_ui_destroy(g_runtime_ui);
        g_runtime_ui = NULL;
#endif
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
    }

    /* Tear down GL-owned objects before the context goes away. */
    if (g_gl_context) {
        gb_lrs_shutdown();
    }
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
    gb_before_first_frame = NULL;
    g_boot_state.clear();
    g_restart_pending = false;
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
           !g_user_paused &&
           !menu_holds_game() &&
           (effective_speed_percent() == 100 || g_speed_audio_mode != GB_SPEED_AUDIO_MUTE);
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

static void audio_ring_push(int16_t left, int16_t right) {
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

/* ---- Audio at other speeds ----
 * RetroArch's fast-forward and slow-motion sound (audio_driver_flush), ported.
 * At 100% samples go to the device as they are made. Faster, Normal Pitch
 * keeps them as they are and drops what arrives while the buffer holds its
 * target, as RetroArch's non-blocking write loses what the device cannot take;
 * Sped Up resamples them to the speed the game really runs at, measured as
 * audio_driver_fastforward_ratio_mult does, so they reach the device as fast
 * as it plays them. Slower, both stretch them the same way. Dynamic rate
 * control (RetroArch's audio_rate_control_delta, +-0.5%) holds a resampled
 * buffer near its target against the measurement's small errors. */
struct SpeedAudio {
    uint64_t last_counter = 0;    /* when the last frame ended; 0 = measure afresh */
    double avg_wall = 0.0;        /* seconds a frame took, averaged */
    double avg_expected = 0.0;    /* seconds those frames last at 100% */
    double step = 1.0;            /* game samples per device sample */
    double phase = 0.0;
    int32_t sum_l = 0;
    int32_t sum_r = 0;
    int count = 0;
    int16_t prev_l = 0;
    int16_t prev_r = 0;
};
static SpeedAudio g_speed_audio;
static constexpr double GB_SPEED_AUDIO_AVG_FRAMES = 16.0;   /* AUDIO_FF_EXP_AVG_SAMPLES */
static constexpr double GB_AUDIO_RATE_CONTROL_DELTA = 0.005;
static constexpr double GB_SPEED_AUDIO_MIN_STEP = 0.0625;   /* AUDIO_MIN_RATIO */
static constexpr double GB_SPEED_AUDIO_MAX_STEP = 16.0;     /* AUDIO_MAX_RATIO */

static double clamp_speed_audio_step(double step) {
    return step < GB_SPEED_AUDIO_MIN_STEP ? GB_SPEED_AUDIO_MIN_STEP
         : step > GB_SPEED_AUDIO_MAX_STEP ? GB_SPEED_AUDIO_MAX_STEP : step;
}

/* The set speed's step, which Unlimited starts at the most of. */
static double set_speed_audio_step(void) {
    return clamp_speed_audio_step(effective_speed_percent() / 100.0);
}

/* Starts measuring again from the set speed. */
static void speed_audio_reset(void) {
    g_speed_audio.last_counter = 0;
    g_speed_audio.step = set_speed_audio_step();
    g_speed_audio.sum_l = g_speed_audio.sum_r = 0;
    g_speed_audio.count = 0;
}

/* The sound plays on through a speed change unless it stops or starts. */
static void on_speed_changed(bool audio_was_running) {
    speed_audio_reset();
    if (audio_output_should_run() != audio_was_running) {
        reset_audio_output_buffer(true);
    }
}

/* After each frame's pacing: moving averages of how long frames take and how
 * long they last at 100%, seeded with the set speed. */
static void speed_audio_frame(uint32_t frame_cycles) {
    SpeedAudio& s = g_speed_audio;
    const int speed_percent = effective_speed_percent();
    if (speed_percent == 100 || !audio_output_should_run()) {
        s.last_counter = 0;
        return;
    }
    const uint64_t now = SDL_GetPerformanceCounter();
    const double expected = (double)(frame_cycles ? frame_cycles : 70224u) / 4194304.0;
    if (s.last_counter == 0) {
        s.avg_expected = expected;
        s.avg_wall = expected / set_speed_audio_step();
    } else {
        const double wall = (double)(now - s.last_counter) / (double)SDL_GetPerformanceFrequency();
        const double keep = (GB_SPEED_AUDIO_AVG_FRAMES - 1.0) / GB_SPEED_AUDIO_AVG_FRAMES;
        s.avg_wall = s.avg_wall * keep + wall / GB_SPEED_AUDIO_AVG_FRAMES;
        s.avg_expected = s.avg_expected * keep + expected / GB_SPEED_AUDIO_AVG_FRAMES;
    }
    s.last_counter = now;
    s.step = clamp_speed_audio_step(s.avg_wall > 0.0 ? s.avg_expected / s.avg_wall : 1.0);
}

static void speed_audio_sample(int16_t left, int16_t right) {
    SpeedAudio& s = g_speed_audio;
    if (effective_speed_percent() > 100 && g_speed_audio_mode == GB_SPEED_AUDIO_NORMAL_PITCH) {
        if (audio_ring_fill_samples() < g_audio_start_threshold) {
            audio_ring_push(left, right);
        } else {
            audio_stats_samples_dropped(1);
        }
        return;
    }
    double step = s.step;
    if (g_audio_started && g_audio_start_threshold > 0) {
        double direction = ((double)audio_ring_fill_samples() - (double)g_audio_start_threshold) /
                           (double)g_audio_start_threshold;
        direction = direction < -1.0 ? -1.0 : direction > 1.0 ? 1.0 : direction;
        step *= 1.0 + GB_AUDIO_RATE_CONTROL_DELTA * direction;
    }
    if (step >= 1.0) {
        /* Fewer samples: each is the average of those it stands for. */
        s.sum_l += left;
        s.sum_r += right;
        s.count++;
        s.phase += 1.0;
        if (s.phase >= step) {
            s.phase -= step;
            audio_ring_push((int16_t)(s.sum_l / s.count), (int16_t)(s.sum_r / s.count));
            s.sum_l = s.sum_r = 0;
            s.count = 0;
        }
    } else {
        /* More samples: between the previous one and this one. */
        for (; s.phase < 1.0; s.phase += step) {
            audio_ring_push((int16_t)(s.prev_l + (left - s.prev_l) * s.phase),
                            (int16_t)(s.prev_r + (right - s.prev_r) * s.phase));
        }
        s.phase -= 1.0;
    }
    s.prev_l = left;
    s.prev_r = right;
}

static void on_audio_sample(GBContext* ctx, int16_t left, int16_t right) {
    (void)ctx;
    if (!audio_output_should_run()) return;
    if (effective_speed_percent() == 100) {
        audio_ring_push(left, right);
    } else {
        speed_audio_sample(left, right);
    }
}

/* Pre-boot launcher (recomp-ui). Interactive runs only: skip benchmark,
 * scripted replay, and any frame-dump/headless mode. Runs BEFORE the SDL_Init +
 * load_runtime_preferences + window creation in gb_platform_init(), so the
 * settings + keybinds it writes to runtime_prefs.ini are read moments later,
 * and the ROM it writes to rom.cfg is picked up in game init.
 *
 * gb_platform_init() calls this itself, which is all a single-body project
 * needs. A multi-body project (gb_body.h) calls it explicitly first: the Mods
 * page decides which body boots, and that body decides the GBConfig and the
 * save id, both of which are needed before gb_context_create(). Runs at most
 * once, so the gb_platform_init() call is then a no-op. Returns 1 if the
 * launcher actually ran, 0 if it was skipped. */
extern "C" int gb_platform_preboot_launcher(void) {
    static int s_preboot_done = 0;
    if (s_preboot_done) return 0;
    s_preboot_done = 1;

#ifdef RECOMP_LAUNCHER
    g_headless_mode = g_headless_mode || headless_requested();
    g_benchmark_mode = g_benchmark_mode || g_headless_mode ||
                       env_flag_enabled("GBRECOMP_BENCHMARK");
    if (!g_benchmark_mode &&
        g_script_count == 0 && g_dump_count == 0 &&
        g_dump_present_count == 0 && g_dump_cycle_count == 0 &&
        !env_flag_enabled("GBRECOMP_NO_LAUNCHER")) {
        if (gb_launcher_preboot() == GB_LAUNCHER_QUIT) {
            /* user closed the launcher window: clean exit, no game boot */
            exit(0);
        }
        return 1;
    }
#endif
    return 0;
}

bool gb_platform_init(int scale) {
    g_headless_mode = g_headless_mode || headless_requested();
    g_benchmark_mode = g_benchmark_mode || g_headless_mode ||
                       env_flag_enabled("GBRECOMP_BENCHMARK");
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
    g_fullscreen_mode = platform_default_fullscreen_mode();
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

    gb_platform_preboot_launcher();

    /* Load configurable keybinds */
    keybinds_init(NULL);

    if (g_benchmark_mode) {
        if (!SDL_getenv("SDL_VIDEODRIVER")) {
            SDL_setenv("SDL_VIDEODRIVER", "dummy", 0);
        }
        if (!SDL_getenv("SDL_AUDIODRIVER")) {
            SDL_setenv("SDL_AUDIODRIVER", "dummy", 0);
        }
        /* Headless additionally needs SDL_INIT_EVENTS: the queue is what a
         * debug client injects into and gb_platform_poll_events() drains. */
        const Uint32 subsystems =
            SDL_INIT_TIMER | (g_headless_mode ? (Uint32)SDL_INIT_EVENTS : 0u);
        if (SDL_Init(subsystems) < 0) {
            fprintf(stderr, "[SDL] SDL_Init failed in %s mode: %s\n",
                    g_headless_mode ? "headless" : "benchmark", SDL_GetError());
            return false;
        }
        if (g_headless_mode) {
            fprintf(stderr, "[SDL] headless: no window, event queue live\n");
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
#if defined(_WIN32)
    /* Same DPI awareness as the launcher (launcher_platform_open), which sets
     * it when it runs first; without it (launcher skipped) Windows bitmap-
     * scales the window at 125% and up, so whole-pixel scaling blurs and the
     * desktop reads smaller than it is. Default priority keeps any earlier
     * choice. */
    SDL_SetHintWithPriority(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2", SDL_HINT_DEFAULT);
    SDL_SetHintWithPriority(SDL_HINT_WINDOWS_DPI_SCALING, "0", SDL_HINT_DEFAULT);
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "[SDL] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    /* Without any sound output (no PipeWire/PulseAudio/ALSA on a Linux box,
     * say) the game still runs, silently: the audio code checks
     * audio_subsystem_available(). */
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "[SDL] No audio output (%s); running without sound.\n", SDL_GetError());
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

    /* GBRECOMP_HIDDEN_WINDOW: a real GL context in a window that is never
     * shown, for automated runs that must render (shader checks) without
     * putting a window on the desktop. */
    /* Copied: on Windows SDL_getenv returns one buffer that every later
     * SDL_getenv (SDL's own hint lookups included) overwrites. */
    const char* const shader_probe_env = SDL_getenv("GBRECOMP_SHADER_PROBE");
    const std::string shader_probe_path = shader_probe_env ? shader_probe_env : "";
    const char* const shader_probe = shader_probe_env ? shader_probe_path.c_str() : NULL;
    if (shader_probe) {
        g_fullscreen_mode = 0;   /* a probe must never touch the display mode */
    }
    const Uint32 window_visibility =
        (shader_probe || env_flag_enabled("GBRECOMP_HIDDEN_WINDOW")) ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN;
    g_window = SDL_CreateWindow(
        "GB Recompiled",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        g_windowed_width,
        g_windowed_height,
        window_visibility | SDL_WINDOW_OPENGL |
        (g_fullscreen_mode == 2 ? SDL_WINDOW_FULLSCREEN :
         g_fullscreen_mode == 1 ? SDL_WINDOW_FULLSCREEN_DESKTOP :
                                   SDL_WINDOW_RESIZABLE)
    );

    if (!g_window) {
        fprintf(stderr, "[SDL] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }
    {
        SDL_DisplayMode desktop;
        float hdpi = 0.0f;
        const int display = SDL_GetWindowDisplayIndex(g_window);
        if (SDL_GetDesktopDisplayMode(display, &desktop) == 0 &&
            SDL_GetDisplayDPI(display, NULL, &hdpi, NULL) == 0) {
            fprintf(stderr, "[SDL] Window created on a %dx%d desktop (%.0f dpi).\n",
                    desktop.w, desktop.h, hdpi);
        } else {
            fprintf(stderr, "[SDL] Window created.\n");
        }
    }
    SDL_SetWindowMinimumSize(g_window, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT);

    fprintf(stderr, "[SDL] Creating GL context...\n");
    /* Ask for ES 3.1 first: librashader compiles presets to the context's GLSL
     * ES version, and 3.00 rejects constructs common in slang presets (arrays
     * of arrays, in crt-geom among others). ANGLE answers an ES 2.0 request
     * with 3.0 but only gives 3.1 when asked. Everything else here is ES 2.0
     * code, which a 3.x context runs unchanged; fall back to 2.0 if 3.1 is
     * refused. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    g_gl_context = SDL_GL_CreateContext(g_window);
    if (!g_gl_context) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        g_gl_context = SDL_GL_CreateContext(g_window);
    }
    if (!g_gl_context) {
        fprintf(stderr, "[SDL] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return false;
    }
    SDL_GL_MakeCurrent(g_window, g_gl_context);
    /* Before any program is linked: ANGLE looks D3DCompile up on first use. */
    gb_lrs_hook_shader_compiler();

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

    /* Compiled programs persist in shader_cache/ (see gl_program_cache.h). */
    {
        char cache_dir[1152];
        gb_host_state_path("shader_cache", cache_dir, sizeof(cache_dir));
        gb_gl_program_cache_install(cache_dir);
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
    /* With the other settings, not in whatever folder the game was started
     * from (an AppImage or a Steam shortcut often starts in $HOME). */
    static char imgui_ini[1152];
    io.IniFilename = gb_host_state_path("imgui.ini", imgui_ini, sizeof(imgui_ini));
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    apply_imgui_theme(g_imgui_theme);

    /* GLES 2 backend — `#version 100` GLSL, no VAOs, runs anywhere. */
    ImGui_ImplSDL2_InitForOpenGL(g_window, g_gl_context);
    ImGui_ImplOpenGL3_Init("#version 100");

#ifdef RECOMP_LAUNCHER
    create_runtime_ui();
#endif

    /* Build the post-process shader pipeline now that GL is current. */
    g_shader_pipeline = gb_shader_pipeline_create();
    if (g_shader_pipeline) {
        if (!gb_shader_pipeline_set_active_by_name(g_shader_pipeline,
                                                    g_active_shader_pref.c_str())) {
            gb_shader_pipeline_set_active_by_name(g_shader_pipeline, "sharp");
        }
    }

    /* librashader is optional: without it the menu says why and nothing else
     * changes. GBRECOMP_SHADER_PRESET overrides the saved preset for this run
     * ("" turns it off). */
    gb_lrs_init();
    if (shader_probe) {
        /* A child started by gb_lrs_probe: compile the preset, render, and go,
         * before the ROM, saves or preferences are touched. */
        std::_Exit(gb_lrs_probe_child(shader_probe));
    }
    if (const char* preset = SDL_getenv("GBRECOMP_SHADER_PRESET")) {
        g_slang_preset_pref = preset;
        g_slang_param_prefs.clear();
    }
    g_slang_load_pending = !g_slang_preset_pref.empty();

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

/* A press of Toggle Menu (or the controller's Guide button). */
static bool event_is_menu_toggle(const SDL_Event* event) {
    if (event->type == SDL_KEYDOWN && event->key.repeat == 0) {
        for (int slot = 0; slot < 2; slot++) {
            if (binding_matches_scancode(g_keyboard_bindings[GB_INPUT_ACTION_TOGGLE_MENU][slot],
                                         event->key.keysym.scancode)) {
                return true;
            }
        }
    }
    if (event->type == SDL_CONTROLLERBUTTONDOWN) {
        if (event->cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) return true;
        for (int slot = 0; slot < 2; slot++) {
            if (binding_matches_controller_button(g_controller_bindings[GB_INPUT_ACTION_TOGGLE_MENU][slot],
                                                  event->cbutton.button)) {
                return true;
            }
        }
    }
    return false;
}

static bool event_is_game_input(const SDL_Event* event) {
    switch (event->type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
        case SDL_CONTROLLERAXISMOTION:
            return true;
        default:
            return false;
    }
}

static void close_menus(void) {
    g_show_menu = false;
#ifdef RECOMP_LAUNCHER
    recomp_runtime_ui_close(g_runtime_ui);
#endif
}

#ifdef RECOMP_LAUNCHER
static bool handle_shared_runtime_ui_event(const SDL_Event* event) {
    if (!g_runtime_ui || !event) return false;

    /* Toggle Menu closes whichever menu is up (not while typing: F10 is a
     * key like any other then). */
    if (menu_open() && !g_menu_text_input && event_is_menu_toggle(event)) {
        close_menus();
        return true;
    }

    /* The settings window's Back: to the runtime menu, unless a text field,
     * a combo's list or a popup takes Escape / B (ImGui closes those). */
    const bool back_key = (event->type == SDL_KEYDOWN &&
                           (event->key.keysym.scancode == SDL_SCANCODE_ESCAPE ||
                            event->key.keysym.scancode == SDL_SCANCODE_AC_BACK)) ||
                          (event->type == SDL_CONTROLLERBUTTONDOWN &&
                           event->cbutton.button == SDL_CONTROLLER_BUTTON_B);
    if (back_key && g_show_menu && !recomp_runtime_ui_is_open(g_runtime_ui)) {
        const bool repeat = event->type == SDL_KEYDOWN && event->key.repeat != 0;
        if (!g_menu_text_input && !g_menu_popup_open && !repeat) {
            g_show_menu = false;
            recomp_runtime_ui_open(g_runtime_ui);
        }
        return true;
    }

    if (event->type == SDL_CONTROLLERBUTTONDOWN &&
        event->cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
        if (recomp_runtime_ui_is_open(g_runtime_ui))
            recomp_runtime_ui_close(g_runtime_ui);
        else {
            g_show_menu = false;
            recomp_runtime_ui_open(g_runtime_ui);
        }
        return true;
    }

    if ((event->type == SDL_KEYDOWN || event->type == SDL_KEYUP)) {
        const int pressed = event->type == SDL_KEYDOWN;
        const int repeat = pressed ? event->key.repeat : 0;
        RecompRuntimeUiInput input;
        bool mapped = true;
        switch (event->key.keysym.scancode) {
            case SDL_SCANCODE_ESCAPE:
            case SDL_SCANCODE_AC_BACK: input = RECOMP_RUNTIME_UI_INPUT_BACK; break;
            case SDL_SCANCODE_UP: input = RECOMP_RUNTIME_UI_INPUT_UP; break;
            case SDL_SCANCODE_DOWN: input = RECOMP_RUNTIME_UI_INPUT_DOWN; break;
            case SDL_SCANCODE_LEFT: input = RECOMP_RUNTIME_UI_INPUT_LEFT; break;
            case SDL_SCANCODE_RIGHT: input = RECOMP_RUNTIME_UI_INPUT_RIGHT; break;
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_SPACE: input = RECOMP_RUNTIME_UI_INPUT_ACCEPT; break;
            default: mapped = false; break;
        }
        if (mapped) {
            if (recomp_runtime_ui_is_open(g_runtime_ui)) {
                recomp_runtime_ui_handle_input(g_runtime_ui, input, pressed, repeat);
                return true;
            }
            if (input == RECOMP_RUNTIME_UI_INPUT_BACK) {
                if (pressed && !repeat) {
                    g_show_menu = false;
                    recomp_runtime_ui_open(g_runtime_ui);
                }
                return true;
            }
            /* Overlay closed: Enter/Space/arrows belong to the game (Start and
             * the d-pad on the default bindings). Swallowing them here made
             * every GB title unplayable from the keyboard. Fall through. */
        }
        return recomp_runtime_ui_is_open(g_runtime_ui) != 0;
    }

    if (event->type == SDL_CONTROLLERBUTTONDOWN ||
        event->type == SDL_CONTROLLERBUTTONUP) {
        const int pressed = event->type == SDL_CONTROLLERBUTTONDOWN;
        RecompRuntimeUiInput input;
        bool mapped = true;
        switch (event->cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP: input = RECOMP_RUNTIME_UI_INPUT_UP; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: input = RECOMP_RUNTIME_UI_INPUT_DOWN; break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: input = RECOMP_RUNTIME_UI_INPUT_LEFT; break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: input = RECOMP_RUNTIME_UI_INPUT_RIGHT; break;
            case SDL_CONTROLLER_BUTTON_A: input = RECOMP_RUNTIME_UI_INPUT_ACCEPT; break;
            case SDL_CONTROLLER_BUTTON_B: input = RECOMP_RUNTIME_UI_INPUT_BACK; break;
            default: mapped = false; break;
        }
        if (mapped && recomp_runtime_ui_is_open(g_runtime_ui)) {
            recomp_runtime_ui_handle_input(g_runtime_ui, input, pressed, 0);
            return true;
        }
        return recomp_runtime_ui_is_open(g_runtime_ui) != 0;
    }

    return false;
}
#endif

static bool handle_runtime_event_inner(const SDL_Event* event, GBContext* ctx);

static bool handle_runtime_event(const SDL_Event* event, GBContext* ctx) {
    const bool keep_running = handle_runtime_event_inner(event, ctx);
    menu_visibility_changed();
    return keep_running;
}

static bool handle_runtime_event_inner(const SDL_Event* event, GBContext* ctx) {
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
#ifdef RECOMP_LAUNCHER
    if (handle_shared_runtime_ui_event(event)) {
        return true;
    }
#endif
    /* A menu has the keyboard and the controller: nothing reaches the game or
     * fires a shortcut, whether it moves through the menu or is typed into a
     * search field there. */
    if (menu_open() && event_is_game_input(event)) {
#ifndef RECOMP_LAUNCHER
        const bool back = event->type == SDL_KEYDOWN && event->key.repeat == 0 &&
                          (event->key.keysym.scancode == SDL_SCANCODE_ESCAPE ||
                           event->key.keysym.scancode == SDL_SCANCODE_AC_BACK) &&
                          !g_menu_popup_open;
        if (!g_menu_text_input && (back || event_is_menu_toggle(event))) {
            close_menus();
        }
#endif
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
                if (g_fullscreen_mode == 0) {
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

/* ---- Debug-server event injection -------------------------------------- *
 * One entry point for every synthetic input. It pushes a real SDL_Event into
 * the real queue, so the code under test (binding capture, binding resolution,
 * the runtime UI hook, the joypad mapping, ImGui) is byte-for-byte the code a
 * physical keystroke runs. Nothing here consults the OS foreground window, so
 * it works headless (SDL_VIDEODRIVER=dummy) and in an unfocused window alike. */

/* The window an injected event should be addressed to. In the game that is the
 * runtime's own window; during the pre-boot launcher the runtime has none yet,
 * so fall back to the only window SDL has open. An id of 0 is left alone --
 * handlers that compare against a window id treat it as "not mine". */
static Uint32 injection_window_id(void) {
    if (g_window) return SDL_GetWindowID(g_window);
    SDL_Window* w = SDL_GetKeyboardFocus();
    if (!w) w = SDL_GetMouseFocus();
    if (!w) {
        /* Unfocused / headless: SDL window ids are small and allocated from 1,
         * so scan a short range rather than require focus we deliberately do
         * not take. */
        for (Uint32 id = 1; id <= 16 && !w; ++id) w = SDL_GetWindowFromID(id);
    }
    return w ? SDL_GetWindowID(w) : 0;
}

extern "C" bool gb_platform_is_headless(void) { return g_headless_mode; }

extern "C" void gb_platform_pump_paused_events(void) {
    if (g_benchmark_mode && !g_headless_mode && !g_injected_event_pending) {
        /* Plain benchmark mode has no window and nothing to pump. */
        return;
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        /* Historic pause-loop escape hatches, kept verbatim. */
        if (event.type == SDL_QUIT) exit(0);
        if (event.type == SDL_KEYDOWN &&
            event.key.keysym.sym == SDLK_ESCAPE) exit(0);
        if (ImGui::GetCurrentContext() != NULL) {
            ImGui_ImplSDL2_ProcessEvent(&event);
        }
        /* Same handler the running loop uses: an injected key resolves through
         * the real binding tables even with the guest halted. */
        if (!handle_runtime_event(&event, g_last_poll_ctx)) exit(0);
    }
    g_injected_event_pending = false;
}

extern "C" int gb_platform_inject_sdl_event(const void* sdl_event) {
    if (!sdl_event) return 0;
    SDL_Event ev = *(const SDL_Event*)sdl_event;
    const Uint32 wid = injection_window_id();

    switch (ev.type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (!ev.key.windowID) ev.key.windowID = wid;
            ev.key.state = (ev.type == SDL_KEYDOWN) ? SDL_PRESSED : SDL_RELEASED;
            if (!ev.key.timestamp) ev.key.timestamp = SDL_GetTicks();
            break;
        case SDL_MOUSEMOTION:
            if (!ev.motion.windowID) ev.motion.windowID = wid;
            if (!ev.motion.timestamp) ev.motion.timestamp = SDL_GetTicks();
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (!ev.button.windowID) ev.button.windowID = wid;
            ev.button.state =
                (ev.type == SDL_MOUSEBUTTONDOWN) ? SDL_PRESSED : SDL_RELEASED;
            if (!ev.button.timestamp) ev.button.timestamp = SDL_GetTicks();
            break;
        case SDL_MOUSEWHEEL:
            if (!ev.wheel.windowID) ev.wheel.windowID = wid;
            if (!ev.wheel.timestamp) ev.wheel.timestamp = SDL_GetTicks();
            break;
        case SDL_TEXTINPUT:
            if (!ev.text.windowID) ev.text.windowID = wid;
            if (!ev.text.timestamp) ev.text.timestamp = SDL_GetTicks();
            break;
        default:
            break;
    }

    /* Keep SDL's own cursor position in step, for a UI that samples
     * SDL_GetMouseState() rather than reading the event. Skipped for a
     * GB_PLATFORM_SYNTHETIC_MOUSE_ID pointer: warping drags the REAL cursor
     * across the user's desktop, which is exactly what a headless probe is
     * supposed to stop doing. */
    if (ev.type == SDL_MOUSEMOTION && wid &&
        ev.motion.which != GB_PLATFORM_SYNTHETIC_MOUSE_ID) {
        SDL_Window* w = SDL_GetWindowFromID(wid);
        if (w) SDL_WarpMouseInWindow(w, ev.motion.x, ev.motion.y);
    }

    if (SDL_PushEvent(&ev) <= 0) {
        fprintf(stderr, "[SDL] inject failed: %s\n", SDL_GetError());
        return 0;
    }
    g_injected_event_pending = true;
    return 1;
}

/* Debug-server input override, applied last so it beats keyboard,
 * controller, the --input script and the in-game menu gate: a client that
 * asked for a button explicitly is driving the joypad.
 *
 * This has to land on g_joypad_dpad/g_joypad_buttons. The override used to
 * exist only inside gb_platform_get_joypad(), but gbrt.c's JOYP read
 * (gb_read8 at 0xFF00) reads the two globals directly and never calls that
 * accessor -- so set_input over TCP silently did nothing, which is why game
 * modules grew their own input commands on top of the script route. */
static void apply_debug_input_override(GBContext* ctx) {
    static uint8_t s_prev_debug_dpad = 0xFF;
    static uint8_t s_prev_debug_buttons = 0xFF;
    const uint8_t joyp = ctx ? ctx->io[0x00] : 0xFF;
    const bool dpad_selected = !(joyp & 0x10);
    const bool buttons_selected = !(joyp & 0x20);
    const int override = gb_debug_server_get_input_override();
    uint8_t debug_dpad = 0xFF;
    uint8_t debug_buttons = 0xFF;
    if (override >= 0) {
        /* Mask is active high, 0=R 1=L 2=U 3=D 4=A 5=B 6=Select 7=Start;
         * the joypad nibbles are active low. */
        debug_dpad    = (uint8_t)(0xF0 | (~override & 0x0F));
        debug_buttons = (uint8_t)(0xF0 | ((~(override >> 4)) & 0x0F));
        g_joypad_dpad = debug_dpad;
        g_joypad_buttons = debug_buttons;
    }
    const uint8_t new_debug_dpad =
        (uint8_t)(s_prev_debug_dpad & (uint8_t)(~debug_dpad) & 0x0F);
    const uint8_t new_debug_buttons =
        (uint8_t)(s_prev_debug_buttons & (uint8_t)(~debug_buttons) & 0x0F);
    if (ctx && ((new_debug_dpad && dpad_selected) ||
                (new_debug_buttons && buttons_selected))) {
        request_joypad_interrupt(ctx);
    }
    s_prev_debug_dpad = debug_dpad;
    s_prev_debug_buttons = debug_buttons;
}

bool gb_platform_poll_events(GBContext* ctx) {
    g_last_poll_ctx = ctx;
    /* Drain inbound BGB packets and apply them to the live serial state. */
    gb_serial_link_tick(ctx);

    /* Cheats (libretro .cht): iterate enabled GameShark codes and
     * write each value into RAM. No-op when nothing is enabled. */
    gb_cheats_tick(ctx);

    /* Debug server: poll TCP commands and block if paused */
    gb_debug_server_poll();
    gb_debug_server_wait_if_paused();

    game_on_frame(ctx);

    SDL_Event event;

    /* Drain the queue whenever there is anything to drain. Headless runs have
     * no window but still carry injected events, and plain benchmark mode skips
     * the drain for speed — so an injection forces one pass either way. A
     * debug-injected event therefore always reaches handle_runtime_event(), the
     * same function a real keystroke reaches, with no focus and no window. */
    const bool drain_events =
        !g_benchmark_mode || g_headless_mode || g_injected_event_pending;
    g_injected_event_pending = false;
    if (drain_events) {
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
    /* Also here for runs that never pace a frame (benchmark mode). */
    run_pending_restart();

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
    apply_debug_input_override(ctx);
    record_manual_input_state(current_cycles);

    return true;
}

/* Handles the events that arrived while gb_platform_vsync() slept. Without
 * this they would wait for gb_platform_poll_events(), which runs only after
 * the next frame has been emulated, so a press made during the sleep (most of
 * every frame) reached the game a frame late. */
static void handle_events_before_frame(void) {
    SDL_Event event;
    bool handled = false;
    while (SDL_PollEvent(&event)) {
        if (ImGui::GetCurrentContext() != NULL) {
            ImGui_ImplSDL2_ProcessEvent(&event);
        }
        if (!handle_runtime_event(&event, g_registered_ctx)) {
            /* Quit: let gb_platform_poll_events() see it and end the loop. */
            SDL_Event quit_event = {};
            quit_event.type = SDL_QUIT;
            SDL_PushEvent(&quit_event);
            break;
        }
        handled = true;
    }
    if (handled) {
        apply_debug_input_override(g_registered_ctx);
        record_manual_input_state(g_registered_ctx ? g_registered_ctx->cycles : 0);
    }
}



void gb_platform_render_frame(const uint32_t* framebuffer) {
    /* Debug server: record frame state and check watchpoints */
    gb_debug_server_record_frame();
    gb_debug_server_check_watchpoints();

    if (g_registered_ctx) game_post_frame(g_registered_ctx);

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
        /* Override bits: 0=Right,1=Left,2=Up,3=Down,4=A,5=B,6=Select,7=Start
         * (active high). Both joypad nibbles are active low and live in bits
         * 0-3, so fold the two halves of the mask together the same way the
         * combined return below does -- the old code returned ~mask verbatim,
         * which put A/B/Select/Start in the high nibble where no caller looks. */
        const uint8_t dpad    = (uint8_t)(0xF0 | (~override & 0x0F));
        const uint8_t buttons = (uint8_t)(0xF0 | ((~(override >> 4)) & 0x0F));
        return (uint8_t)(dpad & buttons);
    }
    /* Return combined state based on P1 register selection */
    /* Caller should AND with the appropriate selection bits */
    return g_joypad_buttons & g_joypad_dpad;
}

/* Holds the emulator while the user has it paused or a menu is open (with
 * Pause in Menu on). The frame just finished is already presented; this keeps
 * re-presenting it so the menus stay usable, and keeps handling events so
 * buttons held now are what the next frame reads. Returns true if it held,
 * once the menus are closed and the user resumes, advances one frame or steps
 * one back with Rewind. */
static bool paused_rewind_step_due(void);
static bool wait_while_user_paused(void) {
    if (!g_user_paused && !menu_holds_game()) {
        return false;
    }
    uint64_t last_present_ms = 0;
    while (menu_holds_game() ||
           (g_user_paused && g_frame_advance_pending == 0 && !paused_rewind_step_due())) {
        const uint64_t now_ms = SDL_GetTicks64();
        const uint64_t next_present_ms = last_present_ms + 16;
        SDL_Event event;
        if (SDL_WaitEventTimeout(&event, now_ms < next_present_ms ? (int)(next_present_ms - now_ms) : 0)) {
            do {
                if (ImGui::GetCurrentContext() != NULL) {
                    ImGui_ImplSDL2_ProcessEvent(&event);
                }
                if (!handle_runtime_event(&event, g_registered_ctx)) {
                    /* Quit: let gb_platform_poll_events() see it and end the loop. */
                    SDL_Event quit_event = {};
                    quit_event.type = SDL_QUIT;
                    SDL_PushEvent(&quit_event);
                    return true;
                }
            } while (SDL_PollEvent(&event));
        }
        gb_debug_server_poll();
        if (!menu_holds_game() &&
            (!g_user_paused || g_frame_advance_pending > 0 || paused_rewind_step_due())) {
            break;
        }
        if (SDL_GetTicks64() >= next_present_ms) {
            /* A state load drops the cached frame; show the loaded one instead. */
            const uint32_t* fb = g_last_guest_framebuffer_valid ? g_last_guest_framebuffer
                               : g_registered_ctx ? gb_get_framebuffer(g_registered_ctx) : NULL;
            render_frame_internal(fb, false);
            last_present_ms = SDL_GetTicks64();
        }
    }
    if (g_user_paused && g_frame_advance_pending > 0) {
        g_frame_advance_pending--;
        const uint8_t input = current_joypad_input();
        g_pause_input_frames = (input == g_pause_input) ? g_pause_input_frames + 1 : 1;
        g_pause_input = input;
    }
    return true;
}

/* ---- Preemptive frames ----
 * RetroArch's model (runahead.c, preempt_run), ported. A ring keeps the
 * machine state from before each of the last N frames. Before a frame runs,
 * the joypad is compared with the one the previous frame read; when it changed
 * and the ring is full, the oldest state is loaded and those N frames run again
 * with the new input, without sound or picture, re-saving the ring on the way.
 * The state before the coming frame then replaces the oldest entry, and the
 * main loop runs and shows that frame as usual. The game sees each change N
 * frames before it was made, so a response that takes the game N or more
 * frames appears N frames sooner; nothing else about the game changes. A state
 * load empties the ring (RetroArch's CMD_EVENT_PREEMPT_RESET_BUFFER); it
 * refills after N frames. */
struct Preempt {
    uint8_t* buffer = nullptr;   /* frames * state_size */
    size_t state_size = 0;
    int frames = 0;
    uint64_t frame_count = 0;    /* states saved since the ring was emptied */
    int start_ptr = 0;           /* oldest state */
    uint8_t joypad = 0;          /* what the last frame read */
};
static Preempt g_preempt;
static uint64_t g_preempt_replays = 0;

static int preemptive_frames(void) {
    const int n = g_preemptive_frames_pref >= 0 ? g_preemptive_frames_pref
                                                : game_default_preemptive_frames();
    return n < 0 ? 0 : n > GB_PREEMPT_MAX_FRAMES ? GB_PREEMPT_MAX_FRAMES : n;
}

static void preempt_reset(void) {
    g_preempt.frame_count = 0;
}

static void preempt_free(void) {
    free(g_preempt.buffer);
    g_preempt = Preempt();
}

uint64_t gb_platform_preempt_replays(void) {
    return g_preempt_replays;
}

/* One frame as the main loop runs it, in the same slices so it ends on the
 * same instruction, without polling or presenting. False when it did not
 * finish (the LCD stayed off). */
static bool preempt_run_frame(GBContext* ctx) {
    gb_reset_frame(ctx);
    ctx->stopped = 0;
    for (int slice = 0; !ctx->frame_done; slice++) {
        if (slice == 8) return false;
        gb_run_cycles(ctx, g_smooth_lcd_transitions ? 70224u : 0xFFFFFFFFu);
    }
    g_preempt_replays++;
    return true;
}

/* Called at the end of gb_platform_vsync, after this frame's input is in. */
static void preempt_before_frame(void) {
    GBContext* ctx = g_registered_ctx;
    /* A scripted input run expects each input on its own frame. */
    const int frames = (ctx && g_script_count == 0) ? preemptive_frames() : 0;
    if (!frames) {
        if (g_preempt.buffer) preempt_free();
        return;
    }
    /* Only whole frames can be run again; pacing slices of a frame the LCD
     * keeps off empty the ring. */
    if (!ctx->frame_done) {
        preempt_reset();
        return;
    }
    const size_t size = gb_state_size(ctx);
    if (!g_preempt.buffer || frames != g_preempt.frames || size != g_preempt.state_size) {
        preempt_free();
        if (size > SIZE_MAX / (size_t)frames ||
            !(g_preempt.buffer = (uint8_t*)malloc(size * (size_t)frames))) {
            return;
        }
        g_preempt.state_size = size;
        g_preempt.frames = frames;
        g_preempt.joypad = current_joypad_input();
    }
    auto slot = [](int i) { return g_preempt.buffer + (size_t)i * g_preempt.state_size; };
    auto next = [frames](int i) { return (i + 1) % frames; };

    const uint8_t joypad = current_joypad_input();
    const bool dirty = joypad != g_preempt.joypad;
    g_preempt.joypad = joypad;
    if (dirty && g_preempt.frame_count >= (uint64_t)frames) {
        /* Those frames were already heard; run them again silently. */
        const auto on_audio_sample = ctx->callbacks.on_audio_sample;
        ctx->callbacks.on_audio_sample = NULL;
        gb_state_load(ctx, slot(g_preempt.start_ptr));
        bool finished = preempt_run_frame(ctx);
        for (int replay = next(g_preempt.start_ptr);
             finished && replay != g_preempt.start_ptr; replay = next(replay)) {
            gb_state_save(ctx, slot(replay));
            finished = preempt_run_frame(ctx);
        }
        ctx->callbacks.on_audio_sample = on_audio_sample;
        if (!finished) {
            preempt_reset();
            return;
        }
    }
    gb_state_save(ctx, slot(g_preempt.start_ptr));
    g_preempt.start_ptr = next(g_preempt.start_ptr);
    g_preempt.frame_count++;
}

/* ---- Rewind ----
 * RetroArch's rewind (state_manager_check_rewind), ported; the buffer is
 * rewind.c. Before a frame runs, its state goes into the buffer, one every
 * Rewind Frames frames. While the Rewind key is held, the newest state is
 * taken out and loaded instead and the frame runs from it, so each frame
 * shown is an older one; its sound is collected backwards and played before
 * the next. The first step back passes over the state of the frame already
 * on screen (RetroArch shows that frame again). States leave out the picture
 * (gb_state_save_no_picture): the frame run from one draws it again. Loading
 * one empties the preemptive frames' ring, as any state load does in
 * RetroArch; it refills once the key is let go. */
enum GBRewindStatus { GB_REWIND_IDLE, GB_REWIND_BACK, GB_REWIND_AT_OLDEST };
static GBRewind* g_rewind = nullptr;
static size_t g_rewind_state_size = 0;
static size_t g_rewind_buffer_bytes = 0;
static int g_rewind_granularity_count = 0;
static bool g_rewind_newest_on_screen = false;  /* pushed just before the frame shown */
static bool g_rewind_frame_is_reversed = false; /* this frame's sound is collected */
static GBRewindStatus g_rewind_status = GB_REWIND_IDLE;
static int g_rewind_debug_frames = 0;           /* debug server `rewind` */
static uint64_t g_rewind_paused_step_ms = 0;
/* One frame's sound, stereo, written from the end backwards (RetroArch's
 * rewind_buf); room for a held frame twice the usual length. */
static constexpr size_t GB_REWIND_AUDIO_SIZE = 8192;
static int16_t g_rewind_audio[GB_REWIND_AUDIO_SIZE];
static size_t g_rewind_audio_ptr = GB_REWIND_AUDIO_SIZE;
static void (*g_rewind_audio_sink)(GBContext*, int16_t, int16_t) = nullptr;

static void rewind_free(void) {
    gb_rewind_free(g_rewind);
    g_rewind = nullptr;
    g_rewind_state_size = 0;
    g_rewind_buffer_bytes = 0;
    g_rewind_newest_on_screen = false;
    g_rewind_status = GB_REWIND_IDLE;
}

static bool rewind_held(void) {
    return g_runtime_action_pressed[GB_INPUT_ACTION_REWIND] || g_rewind_debug_frames > 0;
}

static void on_audio_sample_rewind(GBContext* ctx, int16_t left, int16_t right) {
    (void)ctx;
    if (g_rewind_audio_ptr < 2) return;
    g_rewind_audio[--g_rewind_audio_ptr] = right;
    g_rewind_audio[--g_rewind_audio_ptr] = left;
}

/* RetroArch's audio_driver_frame_is_reverse: the frame just run went back, so
 * its sound goes out backwards now. */
static void rewind_end_reversed_frame(GBContext* ctx) {
    if (!g_rewind_frame_is_reversed) return;
    g_rewind_frame_is_reversed = false;
    ctx->callbacks.on_audio_sample = g_rewind_audio_sink;
    for (size_t i = g_rewind_audio_ptr; g_rewind_audio_sink && i + 1 < GB_REWIND_AUDIO_SIZE; i += 2) {
        g_rewind_audio_sink(ctx, g_rewind_audio[i], g_rewind_audio[i + 1]);
    }
    g_rewind_audio_ptr = GB_REWIND_AUDIO_SIZE;
}

/* With the key held, loads the state the coming frame runs from and returns
 * true. Called before the frame's preemptive frames, which it replaces. */
static bool rewind_step_back(void) {
    GBContext* ctx = g_registered_ctx;
    if (!ctx) return false;
    /* Only between whole frames; slices of a frame the LCD keeps off wait. */
    if (!ctx->frame_done) return false;
    rewind_end_reversed_frame(ctx);
    const bool held = rewind_held();
    if (g_rewind_debug_frames > 0) g_rewind_debug_frames--;
    if (!g_rewind_enabled) {
        if (g_rewind) rewind_free();
        g_rewind_status = GB_REWIND_IDLE;
        return false;
    }

    const size_t size = gb_state_size(ctx);
    const size_t buffer = (size_t)g_rewind_buffer_mb << 20;
    if (size != g_rewind_state_size || buffer != g_rewind_buffer_bytes) {
        rewind_free();
        g_rewind = gb_rewind_new(size, buffer);
        g_rewind_state_size = size;   /* not retried until one changes */
        g_rewind_buffer_bytes = buffer;
        g_rewind_granularity_count = g_rewind_granularity - 1;   /* keep this frame's */
        if (!g_rewind) {
            fprintf(stderr, "[REWIND] Could not keep %zu-byte states in %d MB\n",
                    size, g_rewind_buffer_mb);
        }
    }
    if (!g_rewind || !held) {
        g_rewind_status = GB_REWIND_IDLE;
        return false;
    }

    const void* state = nullptr;
    bool popped = gb_rewind_pop(g_rewind, &state);
    if (!state) {   /* nothing kept yet: the frame runs on */
        g_rewind_status = GB_REWIND_IDLE;
        return false;
    }
    if (popped && g_rewind_newest_on_screen) {
        gb_rewind_pop(g_rewind, &state);   /* at the oldest, state stays */
    }
    g_rewind_newest_on_screen = false;
    gb_state_load_no_picture(ctx, state);
    preempt_reset();
    /* core_set_rewind_callbacks: collect the frame's sound; none at the
     * oldest, where the same frame runs again. */
    g_rewind_frame_is_reversed = true;
    g_rewind_audio_ptr = GB_REWIND_AUDIO_SIZE;
    g_rewind_audio_sink = ctx->callbacks.on_audio_sample;
    ctx->callbacks.on_audio_sample = popped ? on_audio_sample_rewind : NULL;
    g_rewind_status = popped ? GB_REWIND_BACK : GB_REWIND_AT_OLDEST;
    if (g_user_paused) g_rewind_paused_step_ms = SDL_GetTicks64();
    return true;
}

/* Keeps the state the coming frame runs from, after preemptive frames made
 * it. */
static void rewind_push(void) {
    GBContext* ctx = g_registered_ctx;
    if (!ctx || !g_rewind || !ctx->frame_done) return;
    g_rewind_newest_on_screen = false;
    if (++g_rewind_granularity_count < g_rewind_granularity) return;
    g_rewind_granularity_count = 0;
    gb_state_save_no_picture(ctx, gb_rewind_push_where(g_rewind));
    gb_rewind_push_do(g_rewind);
    g_rewind_newest_on_screen = true;
}

static const char* rewind_status_text(void) {
    if (!rewind_held()) return NULL;
    switch (g_rewind_status) {
        case GB_REWIND_BACK: return "Rewinding.";
        case GB_REWIND_AT_OLDEST: return "Reached end of rewind buffer.";
        case GB_REWIND_IDLE:
        default: return NULL;
    }
}

/* Holding Rewind while paused steps back once per refresh, as RetroArch runs
 * one frame per refresh while it rewinds paused. */
static bool paused_rewind_step_due(void) {
    /* Leaving the pause runs a frame, so only with a state to go back to. */
    return g_rewind && gb_rewind_has_state(g_rewind) && rewind_held() &&
           SDL_GetTicks64() >= g_rewind_paused_step_ms + 16;
}

void gb_platform_rewind_hold(int frames) {
    g_rewind_debug_frames = frames > 0 ? frames : 0;
}

void gb_platform_get_rewind_info(GBPlatformRewindInfo* out) {
    if (!out) return;
    *out = GBPlatformRewindInfo{};
    out->enabled = g_rewind_enabled;
    out->state_size = g_rewind_state_size;
    if (g_rewind) {
        out->states = gb_rewind_entries(g_rewind);
        out->used = gb_rewind_used(g_rewind);
        out->capacity = gb_rewind_capacity(g_rewind);
    }
}

void gb_platform_set_speed(int fast_forward, int max_speed, int vsync, int percent,
                           int fast_forward_percent, int max_percent) {
    const int previous_speed = effective_speed_percent();
    const bool audio_was_running = audio_output_should_run();
    if (fast_forward >= 0) g_debug_fast_forward = fast_forward != 0;
    if (max_speed >= 0) g_max_speed_mode = max_speed != 0;
    if (vsync >= 0) g_vsync = vsync != 0;
    if (percent >= 10 && percent <= 500) g_speed_percent = percent;   /* the menu's range */
    set_shortcut_speed(&g_fast_forward_speed_percent, fast_forward_percent);
    set_shortcut_speed(&g_max_speed_percent, max_percent);
    g_fast_forward_active = g_runtime_action_pressed[GB_INPUT_ACTION_FAST_FORWARD] || g_debug_fast_forward;
    if (effective_speed_percent() != previous_speed) {
        on_speed_changed(audio_was_running);
    }
}

void gb_platform_get_speed_info(GBPlatformSpeedInfo* out) {
    if (!out) return;
    out->effective_percent = shortcut_speed_pref(effective_speed_percent());
    out->fast_forward_percent = shortcut_speed_pref(g_fast_forward_speed_percent);
    out->max_percent = shortcut_speed_pref(g_max_speed_percent);
    out->guest_fps = g_guest_fps;
    out->fast_forward = g_fast_forward_active;
    out->max_speed = g_max_speed_mode;
    out->vsync = g_vsync;
    out->swap_interval = g_swap_interval;
    out->audio_mode = g_speed_audio_mode;
    out->audio_step = g_speed_audio.step;
    out->present_ms = g_last_timing.present_ms;
    out->frameskip = g_fast_forward_frameskip;
    out->frames_skipped = g_frames_skipped;
}

void gb_platform_set_window(int width, int height, int scaling_mode) {
    if (scaling_mode >= 0 && scaling_mode < (int)IM_ARRAYSIZE(g_render_scaling_mode_names)) {
        g_render_scaling_mode = (GBRenderScalingMode)scaling_mode;
    }
    if (width > 0 && height > 0 && g_fullscreen_mode == 0) {
        g_windowed_width = width;
        g_windowed_height = height;
        if (g_window) SDL_SetWindowSize(g_window, width, height);
    }
    update_game_viewport();
}

void gb_platform_get_window_info(GBPlatformWindowInfo* out) {
    if (!out) return;
    out->window_width = g_windowed_width;
    out->window_height = g_windowed_height;
    if (g_window) SDL_GetWindowSize(g_window, &out->window_width, &out->window_height);
    out->view_width = view_width();
    out->view_height = view_height();
    out->native_presented = g_native_presented;
    out->picture_width = presentation_width();
    out->picture_height = presentation_height();
    out->game_x = g_game_viewport.x;
    out->game_y = g_game_viewport.y;
    out->game_width = g_game_viewport.w;
    out->game_height = g_game_viewport.h;
    out->scaling_mode = (int)g_render_scaling_mode;
    out->fullscreen = g_fullscreen_mode;
}

void gb_platform_set_menu(const char* open, int pause_in_menu, int dim_percent, int opacity_percent) {
    if (pause_in_menu >= 0) set_menu_pauses_game(pause_in_menu != 0);
    if (dim_percent >= 0 && dim_percent <= 100) g_menu_dim_percent = dim_percent;
    if (opacity_percent >= 0 && opacity_percent <= 100) g_menu_opacity_percent = opacity_percent;
    if (open) {
        close_menus();
        if (strcmp(open, "settings") == 0 || strcmp(open, "shaders") == 0) {
            g_show_menu = true;
            g_slang_open_section = strcmp(open, "shaders") == 0;
        } else if (strcmp(open, "main") == 0) {
#ifdef RECOMP_LAUNCHER
            recomp_runtime_ui_open(g_runtime_ui);
#else
            g_show_menu = true;
#endif
        }
    }
    menu_visibility_changed();
}

void gb_platform_get_menu_info(GBPlatformMenuInfo* out) {
    if (!out) return;
    *out = GBPlatformMenuInfo{};
#ifdef RECOMP_LAUNCHER
    out->main_open = g_runtime_ui && recomp_runtime_ui_is_open(g_runtime_ui);
#endif
    out->settings_open = g_show_menu;
    out->game_held = menu_holds_game();
    out->pause_in_menu = g_menu_pauses_game;
    out->dim_percent = g_menu_dim_percent;
    out->opacity_percent = g_menu_opacity_percent;
}

bool gb_platform_restart_game(void) {
    if (!can_restart_game()) return false;
    request_restart_game();
    return true;
}

bool gb_platform_leave_game(const char* to) {
    if (!to) return false;
    if (strcmp(to, "quit") == 0) {
        request_exit(GB_PLATFORM_EXIT_QUIT);
    } else if (strcmp(to, "launcher") == 0 && launcher_available()) {
        request_exit(GB_PLATFORM_EXIT_RETURN_TO_LAUNCHER);
    } else {
        return false;
    }
    return true;
}

bool gb_platform_request_window_shot(const char* path) {
    if (!g_window || !g_gl_context || !path || !path[0]) return false;
    g_window_shot_path = path;
    return true;
}

/* Called at the end of gb_platform_vsync, after this frame's input is in. */
static void before_frame(void) {
    run_pending_restart();
    if (rewind_step_back()) return;
    preempt_before_frame();
    rewind_push();
}

void gb_platform_vsync(uint32_t frame_cycles) {
    static uint64_t next_frame_time = 0;
    static uint64_t frame_remainder = 0;
    if (g_benchmark_mode || g_app_suspended) {
        g_last_timing.pacing_cycles = (frame_cycles > 0) ? frame_cycles : 70224u;
        g_last_timing.pacing_ms = 0.0;
        speed_audio_reset();
        return;
    }
    if (wait_while_user_paused()) {
        /* Time stood still for the game: pace on from now instead of racing
         * to catch up, so an advanced frame runs immediately. */
        next_frame_time = SDL_GetPerformanceCounter();
        frame_remainder = 0;
        speed_audio_reset();
        g_last_timing.pacing_cycles = (frame_cycles > 0) ? frame_cycles : 70224u;
        g_last_timing.pacing_ms = 0.0;
        g_last_frame_time = SDL_GetTicks();
        before_frame();
        return;
    }
    if (g_turbo) {
        /* Skip frame pacing entirely — run as fast as possible */
        update_audio_stats_from_ring();
        audio_stats_tick(SDL_GetTicks64());
        speed_audio_frame(frame_cycles);
        g_last_frame_time = SDL_GetTicks();
        before_frame();
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
    const uint64_t gb_frame_cycles = (frame_cycles > 0) ? (uint64_t)frame_cycles : 70224ull;
    const uint64_t gb_cpu_hz = 4194304;
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t now = SDL_GetPerformanceCounter();
    uint32_t speed_percent = (uint32_t)effective_speed_percent();
    /* Unlimited has no frame limiter (RetroArch's Fast-Forward Rate 0): no
     * wait, and a limited speed paces on from the frame it starts on. */
    const bool unlimited = speed_percent == (uint32_t)GB_SPEED_UNLIMITED;
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
    /* Only at 100%: at other speeds the sound follows the game's real speed
     * (speed_audio_frame), so running early to feed it would feed back. */
    bool audio_starved = speed_percent == 100 && audio_output_should_run() && g_audio_started &&
                         audio_fill < g_audio_low_watermark;

    if (!unlimited && !audio_starved && now < target_frame_time) {
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
    if (unlimited || now > target_frame_time + max_frame_lag) {
        next_frame_time = now;
        frame_remainder = 0;
    }

    update_audio_stats_from_ring();
    audio_stats_tick(SDL_GetTicks64());
    speed_audio_frame(frame_cycles);
    g_last_timing.pacing_cycles = (uint32_t)gb_frame_cycles;
    g_last_timing.pacing_ms = sdl_now_ms() - pacing_start_ms;
    g_timing_vsync_total += g_last_timing.pacing_ms;
    g_timing_frame_count++;
    g_last_frame_time = SDL_GetTicks();

    handle_events_before_frame();
    before_frame();
}

bool gb_platform_get_smooth_lcd_transitions(void) {
    return g_smooth_lcd_transitions;
}

void gb_platform_set_smooth_lcd_transitions(bool enabled) {
    g_smooth_lcd_transitions = enabled;
}

void gb_platform_set_title(const char* title) {
    if (g_window && !g_benchmark_mode) {
        const char* game_name = game_get_name();
        SDL_SetWindowTitle(g_window, (game_name && game_name[0]) ? game_name : title);
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
    const char* state_dir = gb_host_state_dir();
    if (state_dir && state_dir[0]) {
        fs::path resolved = fs::path(state_dir) / filename;
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
    const bool success = gb_platform_save_state_path(ctx, filename);
    set_savestate_status("Save", slot, success, filename);
    const std::string number = std::to_string(slot + 1);
    post_state_notice(success ? "Saved state to slot " + number + "."
                              : "Failed to save state to slot " + number + ".",
                      g_notify_save_load || !success);
    return success;
}

static bool load_savestate_slot(GBContext* ctx, int slot) {
    if (!ctx) {
        set_savestate_status("Load", slot, false, "No active game context");
        return false;
    }

    char filename[512];
    sdl_get_savestate_path(filename, sizeof(filename), ctx, slot);
    const bool present = savestate_slot_exists(ctx, slot, NULL);
    const bool success = present && gb_platform_load_state_path(ctx, filename);
    set_savestate_status("Load", slot, success, present ? filename : "Slot is empty");
    const std::string number = std::to_string(slot + 1);
    post_state_notice(success   ? "Loaded state from slot " + number + "."
                      : present ? "Failed to load state from slot " + number + "."
                                : "State slot " + number + " is empty.",
                      g_notify_save_load || !success);
    return success;
}

/* Host-side resets that must follow any state load, whichever route took it:
 * the audio ring holds samples generated for the abandoned timeline, the
 * cached guest framebuffer belongs to the abandoned frame, and the present
 * counter has to resync to the restored frame count. gb_custom_reset() and
 * gb_ws_reapply() already run inside gb_context_load_state_file(). */
static void apply_post_load_host_state(GBContext* ctx) {
    /* Preemptive frames' states are the old timeline's. Paused, run enough
     * frames to refill them, as RetroArch runs one after a paused load, so the
     * next frame advance already responds a frame sooner. */
    preempt_reset();
    if (g_user_paused) {
        g_frame_advance_pending += preemptive_frames();
    }
    /* Rewind keeps its states, so a load can be rewound past; the frame on
     * screen is the loaded one now, so the first step back lands on the
     * newest state, the one before the frame shown until now. */
    g_rewind_newest_on_screen = false;
    reset_audio_output_buffer(true);
    gb_lrs_clear_history();   /* a preset's previous frames are the old timeline's */
    g_last_guest_framebuffer_valid = false;
    g_present_count = ctx ? ctx->completed_frames : 0;
    g_last_frame_time = SDL_GetTicks();
}

/* ---- Restart Game ----
 * The machine as it was before its first frame (gb_before_first_frame) is
 * kept, and Restart Game loads it back, as RetroArch's Restart resets the
 * core in place: the window, the settings, rewind and the preset stay, and it
 * happens between frames like a state load. The cart's battery RAM is the
 * current one (a power cycle keeps the save), written to disk first. */
static void keep_boot_state(GBContext* ctx) {
    if (ctx != g_registered_ctx || !g_boot_state.empty()) return;
    g_boot_state.resize(gb_state_size(ctx));
    gb_state_save(ctx, g_boot_state.data());
}

static bool can_restart_game(void) {
    return g_registered_ctx && g_boot_state.size() == gb_state_size(g_registered_ctx);
}

static void request_restart_game(void) {
    g_restart_pending = can_restart_game();
    close_menus();
}

static void run_pending_restart(void) {
    GBContext* ctx = g_registered_ctx;
    if (!g_restart_pending || !ctx) return;
    g_restart_pending = false;
    if (!can_restart_game()) return;
    if (ctx->eram && ctx->eram_size) gb_context_save_ram(ctx);
    const std::vector<uint8_t> eram(ctx->eram, ctx->eram + ctx->eram_size);
    const auto rtc = ctx->rtc;
    gb_state_load(ctx, g_boot_state.data());
    if (!eram.empty()) memcpy(ctx->eram, eram.data(), eram.size());
    ctx->rtc = rtc;
    if (gb_custom_reset) gb_custom_reset(ctx);
    apply_post_load_host_state(ctx);
    g_savestate_status = "Restarted the game.";
    fprintf(stderr, "[SDL] Restarted the game\n");
}

/* ---- Quit / Return to Launcher ----
 * A launcher that started this game (gb_platform_set_launcher_return_enabled)
 * takes exit code 64 back. The pre-boot launcher runs inside this program,
 * before the game, so returning to it starts the program again with the
 * launcher asked for, once this process has saved and closed (atexit runs
 * after main has destroyed the context, which writes the battery RAM). */
static void relaunch_with_launcher(void) {
    if (!gb_host_relaunch("GBRECOMP_LAUNCHER", "1", "GBRECOMP_NO_LAUNCHER")) {
        fprintf(stderr, "[SDL] Could not start the launcher again\n");
    }
}

static bool launcher_available(void) {
#ifdef RECOMP_LAUNCHER
    return g_launcher_return_enabled || gb_host_can_relaunch();
#else
    return g_launcher_return_enabled;
#endif
}

static void request_exit(GBPlatformExitAction action) {
    g_exit_action = action;
    if (action == GB_PLATFORM_EXIT_RETURN_TO_LAUNCHER && !g_launcher_return_enabled) {
        static bool relaunch_registered = false;
        if (!relaunch_registered) atexit(relaunch_with_launcher);
        relaunch_registered = true;
    }
    SDL_Event quit_event = {};
    quit_event.type = SDL_QUIT;
    SDL_PushEvent(&quit_event);
}

bool gb_platform_savestate_slot_path(const GBContext* ctx, int slot,
                                     char* out, size_t out_size) {
    if (!ctx || !out || out_size == 0) {
        return false;
    }
    char filename[512];
    sdl_get_savestate_path(filename, sizeof(filename), ctx, slot);
    if (strlen(filename) + 1 > out_size) {
        out[0] = '\0';
        return false;
    }
    snprintf(out, out_size, "%s", filename);
    return true;
}

bool gb_platform_save_state_path(GBContext* ctx, const char* path) {
    if (!ctx || !path || !path[0]) {
        return false;
    }
    return gb_context_save_state_file(ctx, path);
}

bool gb_platform_load_state_path(GBContext* ctx, const char* path) {
    if (!ctx || !path || !path[0]) {
        return false;
    }
    if (!gb_context_load_state_file(ctx, path)) {
        return false;
    }
    apply_post_load_host_state(ctx);
    return true;
}

bool gb_platform_get_presented_frame(const uint32_t** pixels, int* width, int* height) {
    if (!g_presented_frame || g_presented_width <= 0 || g_presented_height <= 0) {
        return false;
    }
    if (pixels) *pixels = g_presented_frame;
    if (width)  *width  = g_presented_width;
    if (height) *height = g_presented_height;
    return true;
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
    const int game_pref = game_default_hardware_mode();
    if (game_pref > GB_HARDWARE_MODE_AUTO && game_pref <= GB_HARDWARE_MODE_GBA) {
        return static_cast<GBHardwareModePref>(game_pref);
    }
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
        /* SGB colors / cart border are NOT restored from the per-game pref here:
         * the launcher's "Screen model" choice (written to runtime_prefs.ini as
         * sgb.colors / sgb.cart_border and applied by load_runtime_preferences)
         * is authoritative at launch. An in-game SGB toggle still takes effect
         * live and persists to runtime_prefs.ini, so it is respected during the
         * session and reflected the next time the launcher opens — but it never
         * shadows a fresh launcher choice at boot. */
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

    /* This game's cheats: cheats/<game_id>/*.cht and cheats/<game_id>*.cht.
     * Cart-agnostic -- works on any Game Boy game that has libretro cheat
     * files dropped into the cheats/ folder. */
    gb_cheats_load(g_active_game_id.c_str(), false);
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
    /* Restart Game's starting point, kept before the first frame runs. */
    g_boot_state.clear();
    gb_before_first_frame = keep_boot_state;
    GBPlatformCallbacks callbacks = {
        .on_audio_sample = on_audio_sample,
        .on_serial_byte = platform_on_serial_byte,
        .load_battery_ram = sdl_load_battery_ram,
        .save_battery_ram = sdl_save_battery_ram,
        .load_rtc_data = sdl_load_rtc_data,
        .save_rtc_data = sdl_save_rtc_data
    };
    gb_set_platform_callbacks(ctx, &callbacks);

    /* A build of one game has no game id (gb_platform_set_game_id, which
     * loads the game's own cheats, is for builds that load a game), so
     * every .cht in cheats/ is its, and cheats/<save id>/ too. */
    gb_cheats_load(ctx ? ctx->save_id : "", true);

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
    game_on_init(ctx);
    if (!gb_custom_render) gb_ws_arm(ctx);
    if (gb_custom_render) {
        /* A view that fills the window starts shaped like the screen; the
         * window scale below then sizes the window around it. */
        int ww = g_windowed_width, wh = g_windowed_height;
        SDL_DisplayMode desktop;
        if (gb_custom_requested_width < 0 && g_window &&
            SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(g_window), &desktop) == 0) {
            ww = desktop.w;
            wh = desktop.h;
        }
        gb_custom_resolve_size(ww, wh, g_render_scaling_mode == GB_RENDER_SCALING_PIXEL_PERFECT,
                               &gb_custom_width, &gb_custom_height);
        gb_custom_view_width = gb_custom_width;
        gb_custom_view_height = gb_custom_height;
    }
    if (g_gbws_active || gb_custom_render) {
        recreate_streaming_texture();
        apply_window_scale_preset();
    }
}

#else  /* !GB_HAS_SDL2 */

/* Stub implementations when SDL2 is not available */

extern "C" int gb_platform_preboot_launcher(void) { return 0; }

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

bool gb_platform_is_headless(void) { return false; }
int gb_platform_inject_sdl_event(const void* sdl_event) { (void)sdl_event; return 0; }
void gb_platform_pump_paused_events(void) {}

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
uint64_t gb_platform_preempt_replays(void) { return 0; }
void gb_platform_rewind_hold(int frames) { (void)frames; }
void gb_platform_get_rewind_info(GBPlatformRewindInfo* out) {
    if (out) *out = GBPlatformRewindInfo{};
}
void gb_platform_set_speed(int fast_forward, int max_speed, int vsync, int percent,
                           int fast_forward_percent, int max_percent) {
    (void)fast_forward; (void)max_speed; (void)vsync; (void)percent;
    (void)fast_forward_percent; (void)max_percent;
}
void gb_platform_get_speed_info(GBPlatformSpeedInfo* out) {
    if (out) *out = GBPlatformSpeedInfo{};
}
void gb_platform_set_window(int width, int height, int scaling_mode) {
    (void)width; (void)height; (void)scaling_mode;
}
void gb_platform_get_window_info(GBPlatformWindowInfo* out) {
    if (out) *out = GBPlatformWindowInfo{};
}
void gb_platform_set_menu(const char* open, int pause_in_menu, int dim_percent, int opacity_percent) {
    (void)open; (void)pause_in_menu; (void)dim_percent; (void)opacity_percent;
}
void gb_platform_get_menu_info(GBPlatformMenuInfo* out) {
    if (out) *out = GBPlatformMenuInfo{};
}
bool gb_platform_restart_game(void) { return false; }
bool gb_platform_leave_game(const char* to) { (void)to; return false; }
bool gb_platform_request_window_shot(const char* path) { (void)path; return false; }

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
