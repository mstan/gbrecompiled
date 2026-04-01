#pragma once
#include <stdint.h>
#include <SDL.h>

typedef struct {
    SDL_Scancode a, b, select, start;
    SDL_Scancode up, down, left, right;
    SDL_Scancode turbo;
} GBKeybinds;

/* Initialize keybinds from INI file next to exe. Generates defaults if missing. */
void keybinds_init(const char *exe_path);

/* Get current keybind configuration */
const GBKeybinds *keybinds_get(void);
