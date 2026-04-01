/**
 * @file platform_sdl.cpp
 * @brief SDL2 platform implementation for GameBoy runtime with ImGui
 */

#include "platform_sdl.h"
#include "gbrt.h"   /* For GBPlatformCallbacks */
#include "ppu.h"
#include "audio_stats.h"
#include "gbrt_debug.h"
#include "debug_server.h"
extern "C" {
#include "keybinds.h"
}

/* Forward declaration for debug server context setter */
extern "C" void gb_debug_server_set_context(GBContext *ctx);

#ifdef GB_HAS_SDL2
#include <SDL.h>
#include <atomic>
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

/* ============================================================================
 * SDL State
 * ========================================================================== */

static SDL_Window* g_window = NULL;
static SDL_Renderer* g_renderer = NULL;
static SDL_Texture* g_texture = NULL;
static int g_scale = 3;
static uint32_t g_last_frame_time = 0;
static SDL_AudioDeviceID g_audio_device = 0;
static SDL_GameController* g_controller = NULL;
static bool g_vsync = false;  /* VSync OFF - we pace with wall clock for 59.7 FPS */
static bool g_audio_started = false;
static uint32_t g_audio_start_threshold = 0;

/* Performance timing diagnostics */
static uint32_t g_timing_render_total = 0;
static uint32_t g_timing_vsync_total = 0;
static uint32_t g_timing_frame_count = 0;

/* Menu State */
static bool g_show_menu = false;
static int g_speed_percent = 100;
static int g_palette_idx = 0;
static const char* g_palette_names[] = { "Original (Green)", "Black & White (Pocket)", "Amber (Plasma)" };
static const char* g_scale_names[] = { "1x (160x144)", "2x (320x288)", "3x (480x432)", "4x (640x576)", "5x (800x720)", "6x (960x864)", "7x (1120x1008)", "8x (1280x1152)" };
static const uint32_t g_palettes[][4] = {
    { 0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820 }, // Original
    { 0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000 }, // B&W
    { 0xFFFFB000, 0xFFCB4F0E, 0xFF800000, 0xFF330000 }  // Amber
};

/* Joypad state - exported for gbrt.c to access */
volatile uint8_t g_joypad_buttons = 0xFF;  /* Active low: Start, Select, B, A */
volatile uint8_t g_joypad_dpad = 0xFF;     /* Active low: Down, Up, Left, Right */

/* ============================================================================
 * Automation State
 * ========================================================================== */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_SCRIPT_ENTRIES 100
typedef struct {
    uint32_t start_frame;
    uint32_t duration;
    uint8_t dpad;    /* Active LOW mask to apply (0 = Pressed) */
    uint8_t buttons; /* Active LOW mask to apply (0 = Pressed) */
} ScriptEntry;

static ScriptEntry g_input_script[MAX_SCRIPT_ENTRIES];
static int g_script_count = 0;

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
            prev->duration = frame - prev->start_frame;
        }

        ScriptEntry* e = &g_input_script[g_script_count++];
        e->start_frame = frame;
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
static char g_screenshot_prefix[64] = "screenshot";

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

void gb_platform_set_input_script(const char* script) {
    // Format: frame:buttons:duration,...
    if (!script) return;
    
    char* copy = strdup(script);
    char* token = strtok(copy, ",");
    g_script_count = 0;
    
    while (token && g_script_count < MAX_SCRIPT_ENTRIES) {
        uint32_t frame = 0, duration = 0;
        char btn_buf[16] = {0};
        
        if (sscanf(token, "%u:%15[^:]:%u", &frame, btn_buf, &duration) == 3) {
            ScriptEntry* e = &g_input_script[g_script_count++];
            e->start_frame = frame;
            e->duration = duration;
            parse_buttons(btn_buf, &e->dpad, &e->buttons);
            printf("[AUTO] Added input: Frame %u, Btns '%s', Dur %u\n", frame, btn_buf, duration);
        }
        token = strtok(NULL, ",");
    }
    free(copy);
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

void gb_platform_set_screenshot_prefix(const char* prefix) {
    if (prefix) snprintf(g_screenshot_prefix, sizeof(g_screenshot_prefix), "%s", prefix);
}

static void save_ppm(const char* filename, const uint32_t* fb, int width, int height, int frame_count) {
    // Calculate simple hash
    uint32_t hash = 0;
    for (int k = 0; k < width * height; k++) {
        hash = (hash * 33) ^ fb[k];
    }
    printf("[AUTO] Frame %d hash: %08X\n", frame_count, hash);

    FILE* f = fopen(filename, "wb");
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
    printf("[AUTO] Saved screenshot: %s\n", filename);
}


/* ============================================================================
 * Platform Functions
 * ========================================================================== */

void gb_platform_shutdown(void) {
    gb_platform_stop_recording();
    gb_debug_server_shutdown();

    if (g_audio_device) {
        SDL_CloseAudioDevice(g_audio_device);
        g_audio_device = 0;
    }

    if (g_controller) {
        SDL_GameControllerClose(g_controller);
        g_controller = NULL;
    }
    g_audio_started = false;
    g_audio_start_threshold = 0;
    
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

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
    SDL_Quit();
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

/* Last valid sample for smooth underrun handling */
static int16_t g_last_sample_l = 0;
static int16_t g_last_sample_r = 0;

static uint32_t audio_ring_fill_samples(void) {
    uint32_t write_pos = g_audio_write_pos.load(std::memory_order_acquire);
    uint32_t read_pos = g_audio_read_pos.load(std::memory_order_acquire);
    return (write_pos >= read_pos) ? (write_pos - read_pos) : (AUDIO_RING_SIZE - read_pos + write_pos);
}

static void update_audio_stats_from_ring(void) {
    audio_stats_update_buffer(audio_ring_fill_samples(), AUDIO_RING_SIZE, g_audio_device_sample_rate);
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

    if (available >= (uint32_t)samples_needed) {
        /* Normal path — enough samples */
        for (int i = 0; i < samples_needed; i++) {
            out[i * 2]     = g_audio_ring[read_pos * 2];
            out[i * 2 + 1] = g_audio_ring[read_pos * 2 + 1];
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
    g_scale = scale;
    if (g_scale < 1) g_scale = 1;
    if (g_scale > 8) g_scale = 8;

    /* Load configurable keybinds */
    keybinds_init(NULL);

    fprintf(stderr, "[SDL] Initializing SDL...\n");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "[SDL] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    fprintf(stderr, "[SDL] SDL initialized.\n");

    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            g_controller = SDL_GameControllerOpen(i);
            if (g_controller) {
                fprintf(stderr, "[SDL] Controller: %s\n", SDL_GameControllerName(g_controller));
                break;
            }
        }
    }
    
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
        "GB Recompiled",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        GB_SCREEN_WIDTH * g_scale,
        GB_SCREEN_HEIGHT * g_scale,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    
    if (!g_window) {
        fprintf(stderr, "[SDL] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }
    fprintf(stderr, "[SDL] Window created.\n");
    
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
    
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

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

    g_texture = SDL_CreateTexture(
        g_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        GB_SCREEN_WIDTH,
        GB_SCREEN_HEIGHT
    );
    
    if (!g_texture) {
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return false;
    }
    
    g_last_frame_time = SDL_GetTicks();
    
    return true;
}

bool gb_platform_poll_events(GBContext* ctx) {
    /* Debug server: poll TCP commands and block if paused */
    gb_debug_server_poll();
    gb_debug_server_wait_if_paused();

    SDL_Event event;
    uint8_t joyp = ctx ? ctx->io[0x00] : 0xFF;
    bool dpad_selected = !(joyp & 0x10);
    bool buttons_selected = !(joyp & 0x20);
    
    while (SDL_PollEvent(&event)) {
         ImGui_ImplSDL2_ProcessEvent(&event);
         if (event.type == SDL_QUIT) return false;
         if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(g_window))
            return false;

        switch (event.type) {
                case SDL_CONTROLLERAXISMOTION: {
                const int deadzone = 8000;

                if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                    if (event.caxis.value > deadzone) {
                        g_joypad_dpad &= ~0x01;
                        g_joypad_dpad |= 0x02;
                    } else if (event.caxis.value < -deadzone) {
                        g_joypad_dpad &= ~0x02;
                        g_joypad_dpad |= 0x01;
                    } else {
                        g_joypad_dpad |= 0x01;
                        g_joypad_dpad |= 0x02;
                    }
                }

                if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                    if (event.caxis.value > deadzone) {
                        g_joypad_dpad &= ~0x08;
                        g_joypad_dpad |= 0x04;
                    } else if (event.caxis.value < -deadzone) {
                        g_joypad_dpad &= ~0x04;
                        g_joypad_dpad |= 0x08;
                    } else {
                        g_joypad_dpad |= 0x04;
                        g_joypad_dpad |= 0x08;
                    }
                }

                break;
            }

            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP: {
                bool pressed = (event.type == SDL_CONTROLLERBUTTONDOWN);

                switch (event.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_DPAD_UP:
                        if (pressed) g_joypad_dpad &= ~0x04;
                        else g_joypad_dpad |= 0x04;
                        break;

                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                        if (pressed) g_joypad_dpad &= ~0x08;
                        else g_joypad_dpad |= 0x08;
                        break;

                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                        if (pressed) g_joypad_dpad &= ~0x02;
                        else g_joypad_dpad |= 0x02;
                        break;

                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                        if (pressed) g_joypad_dpad &= ~0x01;
                        else g_joypad_dpad |= 0x01;
                        break;

                    case SDL_CONTROLLER_BUTTON_A:
                        if (pressed) g_joypad_buttons &= ~0x01;
                        else g_joypad_buttons |= 0x01;
                        break;

                    case SDL_CONTROLLER_BUTTON_B:
                        if (pressed) g_joypad_buttons &= ~0x02;
                        else g_joypad_buttons |= 0x02;
                        break;

                    case SDL_CONTROLLER_BUTTON_START:
                        if (pressed) g_joypad_buttons &= ~0x08;
                        else g_joypad_buttons |= 0x08;
                        break;

                    case SDL_CONTROLLER_BUTTON_BACK:
                        if (pressed) g_joypad_buttons &= ~0x04;
                        else g_joypad_buttons |= 0x04;
                        break;
                }
            }

            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                bool pressed = (event.type == SDL_KEYDOWN);
                bool trigger = false;
                const GBKeybinds *kb = keybinds_get();
                SDL_Scancode sc = event.key.keysym.scancode;

                /* D-pad */
                if (sc == kb->up) {
                    if (pressed) { g_joypad_dpad &= ~0x04; if (dpad_selected) trigger = true; }
                    else g_joypad_dpad |= 0x04;
                } else if (sc == kb->down) {
                    if (pressed) { g_joypad_dpad &= ~0x08; if (dpad_selected) trigger = true; }
                    else g_joypad_dpad |= 0x08;
                } else if (sc == kb->left) {
                    if (pressed) { g_joypad_dpad &= ~0x02; if (dpad_selected) trigger = true; }
                    else g_joypad_dpad |= 0x02;
                } else if (sc == kb->right) {
                    if (pressed) { g_joypad_dpad &= ~0x01; if (dpad_selected) trigger = true; }
                    else g_joypad_dpad |= 0x01;
                }
                /* Buttons */
                else if (sc == kb->a) {
                    if (pressed) { g_joypad_buttons &= ~0x01; if (buttons_selected) trigger = true; }
                    else g_joypad_buttons |= 0x01;
                } else if (sc == kb->b) {
                    if (pressed) { g_joypad_buttons &= ~0x02; if (buttons_selected) trigger = true; }
                    else g_joypad_buttons |= 0x02;
                } else if (sc == kb->select) {
                    if (pressed) { g_joypad_buttons &= ~0x04; if (buttons_selected) trigger = true; }
                    else g_joypad_buttons |= 0x04;
                } else if (sc == kb->start) {
                    if (pressed) { g_joypad_buttons &= ~0x08; if (buttons_selected) trigger = true; }
                    else g_joypad_buttons |= 0x08;
                }
                /* Turbo */
                else if (sc == kb->turbo) {
                    g_turbo = pressed;
                }
                /* Escape (always hardcoded — menu toggle) */
                else if (sc == SDL_SCANCODE_ESCAPE) {
                    if (pressed) g_show_menu = !g_show_menu;
                    return true;
                }
                
                if (trigger && ctx && event.key.repeat == 0) {
                    ctx->io[0x0F] |= 0x10; /* Request Joypad Interrupt */
                    /* Also wake up HALT state immediately if needed, though handle_interrupts does it */
                    if (ctx->halted) ctx->halted = 0;
                }
                break;
            }
            
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    /* Handle resize if needed or just let SDL/ImGui handle it */
                }
                break;
        }
    }
    
    /* Handle Automation Inputs (script playback) */
    if (g_script_count > 0) {
        /* Reset joypad to "nothing pressed" before applying script state,
         * otherwise released buttons stay latched from prior AND operations */
        g_joypad_dpad = 0xFF;
        g_joypad_buttons = 0xFF;
    }
    for (int i = 0; i < g_script_count; i++) {
        ScriptEntry* e = &g_input_script[i];
        if (g_frame_count >= (int)e->start_frame && g_frame_count < (int)(e->start_frame + e->duration)) {
             // Apply inputs (ANDing masks — works now since we reset to 0xFF above)
             g_joypad_dpad &= e->dpad;
             g_joypad_buttons &= e->buttons;
             if (g_frame_count == (int)e->start_frame) {
                 fprintf(stderr, "[SCRIPT] Frame %d: applying entry %d (dpad=0x%02X btn=0x%02X)\n",
                         g_frame_count, i, e->dpad, e->buttons);
             }
             /* Also write directly to hJoyInput so _Joypad sees it even if
              * ReadJoypad timing doesn't align with script application */
             if (ctx && (e->dpad != 0xFF || e->buttons != 0xFF)) {
                 /* Compute hJoyInput value: active-high combined dpad+buttons */
                 uint8_t joy = 0;
                 if (!(e->dpad & 0x01)) joy |= 0x10; /* Right */
                 if (!(e->dpad & 0x02)) joy |= 0x20; /* Left */
                 if (!(e->dpad & 0x04)) joy |= 0x40; /* Up */
                 if (!(e->dpad & 0x08)) joy |= 0x80; /* Down */
                 if (!(e->buttons & 0x01)) joy |= 0x01; /* A */
                 if (!(e->buttons & 0x02)) joy |= 0x02; /* B */
                 if (!(e->buttons & 0x04)) joy |= 0x04; /* Select */
                 if (!(e->buttons & 0x08)) joy |= 0x08; /* Start */
                 /* Write to hJoyInput ($FFF8) in HRAM */
                 ctx->hram[0x78] = joy;
             }
        }
    }

    /* Record input state (only writes on change) */
    record_frame_input();

    return true;
}



void gb_platform_render_frame(const uint32_t* framebuffer) {
    if (!g_texture || !g_renderer || !framebuffer) {
        DBG_FRAME("Platform render_frame: SKIPPED (null: texture=%d, renderer=%d, fb=%d)",
                  g_texture == NULL, g_renderer == NULL, framebuffer == NULL);
        return;
    }

    /* Turbo: skip rendering most frames, only draw every 4th */
    if (g_turbo && (g_frame_count & 3) != 0) {
        g_frame_count++;
        return;
    }

    /* Debug server: record frame state and check watchpoints */
    gb_debug_server_record_frame();
    gb_debug_server_check_watchpoints();

    g_frame_count++;

    /* Flush battery RAM to disk after no ERAM writes for ~5 seconds.
     * eram_dirty_frame tracks the LAST write, so we debounce properly
     * even if the game writes ERAM frequently (e.g. RTC updates). */
    if (g_ctx && g_ctx->eram_dirty) {
        /* Update last-write timestamp on each dirty frame */
        g_ctx->eram_dirty_frame = g_frame_count;
        g_ctx->eram_dirty = 0;  /* Clear until next write */
    }
    if (g_ctx && g_ctx->eram_dirty_frame != 0 &&
        (uint32_t)(g_frame_count - g_ctx->eram_dirty_frame) > 300) {
        gb_context_save_ram(g_ctx);
        g_ctx->eram_dirty_frame = 0;
    }

    /* Handle Screenshot Dumping */
    for (int i = 0; i < g_dump_count; i++) {
        if (g_dump_frames[i] == (uint32_t)g_frame_count) {
             char filename[128];
             snprintf(filename, sizeof(filename), "%s_%05d.ppm", g_screenshot_prefix, g_frame_count);
             save_ppm(filename, framebuffer, GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT, g_frame_count);
        }
    }
    
    /* Debug: check framebuffer content on first few frames */
    if (g_frame_count <= 3) {
        /* Check if framebuffer has any non-white pixels */
        bool has_content = false;
        uint32_t white = 0xFFE0F8D0;  /* DMG palette color 0 */
        for (int i = 0; i < GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT; i++) {
            if (framebuffer[i] != white) {
                has_content = true;
                break;
            }
        }
        DBG_FRAME("Platform frame %d - has_content=%d, first_pixel=0x%08X",
                  g_frame_count, has_content, framebuffer[0]);
    }
    
    if (g_frame_count % 60 == 0) {
        char title[64];
        snprintf(title, sizeof(title), "GameBoy Recompiled - Frame %d", g_frame_count);
        SDL_SetWindowTitle(g_window, title);
    }
    
    /* Update texture */
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
    
    /* Clear and render */
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);

    // ImGui Frame
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    if (g_show_menu) {
        ImGui::Begin("GameBoy Recompiled", &g_show_menu);
        ImGui::Text("Performance: %.1f FPS", ImGui::GetIO().Framerate);
        int scale_idx = g_scale - 1;
        if (ImGui::Combo("Resolution", &scale_idx, g_scale_names, IM_ARRAYSIZE(g_scale_names))) {
            g_scale = scale_idx + 1;
            SDL_SetWindowSize(g_window, GB_SCREEN_WIDTH * g_scale, GB_SCREEN_HEIGHT * g_scale);
            SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }

        if (ImGui::Checkbox("V-Sync", &g_vsync)) {
            SDL_RenderSetVSync(g_renderer, g_vsync ? 1 : 0);
        }

        ImGui::SliderInt("Speed %", &g_speed_percent, 10, 500);
        if (ImGui::Button("Reset Speed")) g_speed_percent = 100;
        ImGui::Combo("Palette", &g_palette_idx, g_palette_names, IM_ARRAYSIZE(g_palette_names));

        if (ImGui::Button("Reset to Defaults")) {
            g_scale = 3;
            g_speed_percent = 100;
            SDL_SetWindowSize(g_window, GB_SCREEN_WIDTH * g_scale, GB_SCREEN_HEIGHT * g_scale);
            SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            g_palette_idx = 0;
        }

        if (ImGui::Button("Quit")) {
            SDL_Event quit_event;
            quit_event.type = SDL_QUIT;
            SDL_PushEvent(&quit_event);
        }
        ImGui::End();
    }
    /* No overlay by default — press ESC for the debug menu */

    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());

    uint32_t render_start = SDL_GetTicks();
    SDL_RenderPresent(g_renderer);
    g_timing_render_total += SDL_GetTicks() - render_start;
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

void gb_platform_vsync(void) {
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
     */
    static uint64_t next_frame_time = 0;
    static uint64_t frame_remainder = 0;
    const uint64_t gb_frame_cycles = 70224;
    const uint64_t gb_cpu_hz = 4194304;
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t now = SDL_GetPerformanceCounter();
    
    if (next_frame_time == 0) {
        next_frame_time = now;
    }

    uint32_t audio_fill = audio_ring_fill_samples();
    bool audio_starved = g_audio_started && audio_fill < (g_audio_start_threshold / 2);

    if (!audio_starved && now < next_frame_time) {
        uint64_t wait_ticks = next_frame_time - now;
        uint32_t wait_us = (uint32_t)((wait_ticks * 1000000) / freq);
        
        /* Use SDL_Delay for longer waits, busy-wait for precision */
        if (wait_us > 2000) {
            SDL_Delay((wait_us - 1000) / 1000);
        }
        /* Busy-wait the remainder for precision */
        while (SDL_GetPerformanceCounter() < next_frame_time) {
            /* spin */
        }
    }
    
    /* Schedule next frame */
    uint64_t frame_ticks_num = (freq * gb_frame_cycles) + frame_remainder;
    next_frame_time += frame_ticks_num / gb_cpu_hz;
    frame_remainder = frame_ticks_num % gb_cpu_hz;
    
    /* If we fell behind by more than 3 frames, reset (don't try to catch up) */
    uint64_t max_frame_lag = ((freq * gb_frame_cycles * 3) + gb_cpu_hz - 1) / gb_cpu_hz;
    if (SDL_GetPerformanceCounter() > next_frame_time + max_frame_lag) {
        next_frame_time = SDL_GetPerformanceCounter();
        frame_remainder = 0;
    }
    
    update_audio_stats_from_ring();
    audio_stats_tick(SDL_GetTicks64());
    g_last_frame_time = SDL_GetTicks();
}

void gb_platform_set_title(const char* title) {
    if (g_window) {
        SDL_SetWindowTitle(g_window, title);
    }
}

/* ============================================================================
 * Save Data
 * ========================================================================== */

static void sdl_get_save_path(char* buffer, size_t size, const char* rom_name) {
    char* base_path = SDL_GetBasePath();
    if (base_path) {
        // Extract just the filename from rom_name to avoid path traversal issues
        const char* base_name = strrchr(rom_name, '/');
#ifdef _WIN32
        const char* base_name_win = strrchr(rom_name, '\\');
        if (base_name_win > base_name) base_name = base_name_win;
#endif
        if (base_name) {
            base_name++; // Skip separator
        } else {
            base_name = rom_name;
        }

        snprintf(buffer, size, "%s%s.sav", base_path, base_name);
        SDL_free(base_path);
    } else {
        // Fallback to CWD if SDL_GetBasePath fails
        snprintf(buffer, size, "%s.sav", rom_name);
    }
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
    g_ctx = ctx;
    GBPlatformCallbacks callbacks = {
        .on_audio_sample = on_audio_sample,
        .load_battery_ram = sdl_load_battery_ram,
        .save_battery_ram = sdl_save_battery_ram
    };
    gb_set_platform_callbacks(ctx, &callbacks);

    /* Initialize TCP debug server */
    gb_debug_server_set_context(ctx);
    gb_debug_server_init(0); /* default port 4370 */
}

#else  /* !GB_HAS_SDL2 */

/* Stub implementations when SDL2 is not available */

bool gb_platform_init(int scale) {
    (void)scale;
    return false;
}

void gb_platform_shutdown(void) {}

bool gb_platform_poll_events(GBContext* ctx) {
    (void)ctx;
    return true;
}

void gb_platform_render_frame(const uint32_t* framebuffer) {
    (void)framebuffer;
}

uint8_t gb_platform_get_joypad(void) {
    return 0xFF;
}

void gb_platform_vsync(void) {}

void gb_platform_set_title(const char* title) {
    (void)title;
}

void gb_platform_register_context(GBContext* ctx) { (void)ctx; }

#endif /* GB_HAS_SDL2 */
