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

// Joypad state variables
extern uint8_t g_joypad_buttons;
extern uint8_t g_joypad_dpad;

typedef struct GBContext GBContext;

/**
 * @brief Initialize SDL2 platform (window, renderer)
 * @param scale Window scale factor (1-4)
 * @return true on success
 */
bool gb_platform_init(int scale);

/**
 * @brief Register context with platform (sets up callbacks)
 */
void gb_platform_register_context(GBContext* ctx);

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
 * @brief Get joypad state
 * @return Joypad byte (active low)
 */
uint8_t gb_platform_get_joypad(void);

/**
 * @brief Wait for vsync / frame timing
 */
void gb_platform_vsync(void);

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
