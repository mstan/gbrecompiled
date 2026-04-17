/**
 * @file platform_sdl.h
 * @brief SDL2 platform layer for GameBoy runtime
 */

#ifndef GB_PLATFORM_SDL_H
#define GB_PLATFORM_SDL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Joypad state variables (modified by platform layer, read by emulation)
extern uint8_t g_joypad_buttons;
extern uint8_t g_joypad_dpad;

typedef struct GBContext GBContext;

typedef struct GBPlatformTimingInfo {
    double upload_ms;
    double compose_ms;
    double present_ms;
    double total_render_ms;
    double pacing_ms;
    uint32_t pacing_cycles;
} GBPlatformTimingInfo;

typedef enum GBPlatformExitAction {
    GB_PLATFORM_EXIT_QUIT = 0,
    GB_PLATFORM_EXIT_RETURN_TO_LAUNCHER = 1,
} GBPlatformExitAction;

enum {
    GB_PLATFORM_RETURN_TO_LAUNCHER_EXIT_CODE = 64,
};

/**
 * @brief Initialize SDL2 platform (window, renderer)
 * @param scale Initial window scale preset (1-8)
 * @return true on success
 */
bool gb_platform_init(int scale);

/**
 * @brief Register context with platform (sets up callbacks)
 */
void gb_platform_register_context(GBContext* ctx);

/**
 * @brief Enable a headless benchmark mode with no host pacing or UI work.
 */
void gb_platform_set_benchmark_mode(bool enabled);

/**
 * @brief Enable or disable the launcher return action in the runtime menu.
 */
void gb_platform_set_launcher_return_enabled(bool enabled);

/**
 * @brief Report how the SDL runtime requested to exit the current game.
 */
GBPlatformExitAction gb_platform_get_exit_action(void);

/**
 * @brief Shutdown SDL2 platform
 */
void gb_platform_shutdown(void);

/**
 * @brief Process SDL events
 * @return false if quit requested
 */
bool gb_platform_poll_events(GBContext* ctx);

/**
 * @brief Render frame to screen
 */
void gb_platform_render_frame(const uint32_t* framebuffer);

/**
 * @brief Present a framebuffer without advancing guest-frame counters
 */
void gb_platform_present_framebuffer(const uint32_t* framebuffer);

/**
 * @brief Render a blank LCD-off presentation frame without advancing guest frame counters
 */
void gb_platform_render_lcd_off_frame(void);

/**
 * @brief Set input automation script.
 *
 * Legacy entries use "frame:buttons:duration". Cycle-anchored entries use
 * "c<cycle>:buttons:<duration_cycles>".
 */
void gb_platform_set_input_script(const char* script);

/**
 * @brief Record live keyboard/controller input to a replayable script file
 */
void gb_platform_set_input_record_file(const char* path);

/**
 * @brief Set frames to dump screenshots (format: "frame1,frame2,...")
 */
void gb_platform_set_dump_frames(const char* frames);

/**
 * @brief Dump every host present that occurs while one of the selected guest
 * frames is current (format: "frame1,frame2,...")
 */
void gb_platform_set_dump_present_frames(const char* frames);

/**
 * @brief Set filename prefix for screenshots
 */
void gb_platform_set_screenshot_prefix(const char* prefix);

/**
 * @brief Get timing data captured during the most recent render/pacing pass
 */
void gb_platform_get_timing_info(GBPlatformTimingInfo* out);

/**
 * @brief Get joypad state
 * @return Joypad byte (active low)
 */
uint8_t gb_platform_get_joypad(void);

/**
 * @brief Wait for vsync / frame timing
 */
void gb_platform_vsync(uint32_t frame_cycles);

/**
 * @brief Query whether slow-frame presentation smoothing is enabled
 */
bool gb_platform_get_smooth_lcd_transitions(void);

/**
 * @brief Override whether slow-frame presentation smoothing is enabled
 */
void gb_platform_set_smooth_lcd_transitions(bool enabled);

/**
 * @brief Set window title
 */
void gb_platform_set_title(const char* title);

/**
 * @brief Set input script from inline string (frame:buttons:duration,...)
 */
void gb_platform_set_input_script(const char* script);

/**
 * @brief Load input script from file (--script)
 * File format: one line per event, "frame buttons" where buttons is a
 * combination of U/D/L/R/A/B/S/T (or - for none). Active-low hex joypad
 * state is also accepted as "frame 0xHH".
 */
void gb_platform_load_script_file(const char* path);

/**
 * @brief Start recording inputs to a file (--record)
 */
void gb_platform_start_recording(const char* path);

/**
 * @brief Stop recording (called on shutdown)
 */
void gb_platform_stop_recording(void);

void gb_platform_set_dump_frames(const char* frames);
void gb_platform_set_screenshot_prefix(const char* prefix);

#ifdef __cplusplus
}
#endif

#endif /* GB_PLATFORM_SDL_H */
