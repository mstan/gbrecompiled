/*
 * keybinds.c — Configurable keyboard bindings for Game Boy controls
 *
 * Reads/writes an INI file next to the game executable.
 * Auto-generates with defaults if the file doesn't exist.
 */
#include "keybinds.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stddef.h>

/* ── Default bindings ─────────────────────────────────────────────────────── */

static GBKeybinds s_binds = {
    .a      = SDL_SCANCODE_Z,
    .b      = SDL_SCANCODE_X,
    .select = SDL_SCANCODE_RSHIFT,
    .start  = SDL_SCANCODE_RETURN,
    .up     = SDL_SCANCODE_UP,
    .down   = SDL_SCANCODE_DOWN,
    .left   = SDL_SCANCODE_LEFT,
    .right  = SDL_SCANCODE_RIGHT,
    .turbo  = SDL_SCANCODE_TAB,
};

/* ── Button name mapping ──────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    size_t offset;
} ButtonDef;

static const ButtonDef s_buttons[] = {
    { "a",      offsetof(GBKeybinds, a) },
    { "b",      offsetof(GBKeybinds, b) },
    { "select", offsetof(GBKeybinds, select) },
    { "start",  offsetof(GBKeybinds, start) },
    { "up",     offsetof(GBKeybinds, up) },
    { "down",   offsetof(GBKeybinds, down) },
    { "left",   offsetof(GBKeybinds, left) },
    { "right",  offsetof(GBKeybinds, right) },
    { "turbo",  offsetof(GBKeybinds, turbo) },
    { NULL, 0 }
};

/* ── INI parsing helpers ──────────────────────────────────────────────────── */

static void trim(char *s) {
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    char *start = s;
    while (isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

static SDL_Scancode name_to_scancode(const char *name) {
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    if (sc != SDL_SCANCODE_UNKNOWN) return sc;
    if (strcmp(name, "enter") == 0 || strcmp(name, "return") == 0) return SDL_SCANCODE_RETURN;
    if (strcmp(name, "tab") == 0) return SDL_SCANCODE_TAB;
    if (strcmp(name, "space") == 0) return SDL_SCANCODE_SPACE;
    if (strcmp(name, "lshift") == 0) return SDL_SCANCODE_LSHIFT;
    if (strcmp(name, "rshift") == 0) return SDL_SCANCODE_RSHIFT;
    if (strcmp(name, "backslash") == 0) return SDL_SCANCODE_BACKSLASH;
    if (strcmp(name, "escape") == 0) return SDL_SCANCODE_ESCAPE;
    if (strcmp(name, "backspace") == 0) return SDL_SCANCODE_BACKSPACE;
    return SDL_SCANCODE_UNKNOWN;
}

static const char *scancode_to_name(SDL_Scancode sc) {
    const char *name = SDL_GetScancodeName(sc);
    if (name && name[0]) return name;
    return "Unknown";
}

/* ── File I/O ─────────────────────────────────────────────────────────────── */

static char s_ini_path[512] = {0};

static void derive_ini_path(const char *exe_path) {
    if (!exe_path) {
        strcpy(s_ini_path, "keybinds.ini");
        return;
    }
    const char *slash = NULL, *p = exe_path;
    while (*p) { if (*p == '/' || *p == '\\') slash = p; p++; }
    if (slash) {
        size_t dir_len = (size_t)(slash - exe_path) + 1;
        if (dir_len + 13 < sizeof(s_ini_path)) {
            memcpy(s_ini_path, exe_path, dir_len);
            strcpy(s_ini_path + dir_len, "keybinds.ini");
        }
    } else {
        strcpy(s_ini_path, "keybinds.ini");
    }
}

static void write_defaults(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Game Boy Keybinds\n");
    fprintf(f, "# Edit key names to customize. Uses SDL key names.\n");
    fprintf(f, "# Common: Z, X, Tab, Return, Up, Down, Left, Right\n");
    fprintf(f, "# A-Z letters, Space, Left Shift, Right Shift, Backspace\n\n");
    fprintf(f, "[controls]\n");
    for (const ButtonDef *bd = s_buttons; bd->name; bd++) {
        SDL_Scancode sc = *(SDL_Scancode *)((char *)&s_binds + bd->offset);
        fprintf(f, "%s = %s\n", bd->name, scancode_to_name(sc));
    }
    fclose(f);
    printf("[Keybinds] Generated %s\n", path);
}

static void load_ini(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    int in_controls = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) *end = '\0';
            in_controls = (strcmp(line + 1, "controls") == 0);
            continue;
        }

        if (!in_controls) continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;
        trim(key); trim(val);

        for (char *c = key; *c; c++) *c = (char)tolower((unsigned char)*c);

        for (const ButtonDef *bd = s_buttons; bd->name; bd++) {
            if (strcmp(key, bd->name) == 0) {
                SDL_Scancode sc = name_to_scancode(val);
                if (sc != SDL_SCANCODE_UNKNOWN) {
                    *(SDL_Scancode *)((char *)&s_binds + bd->offset) = sc;
                }
                break;
            }
        }
    }
    fclose(f);
    printf("[Keybinds] Loaded %s\n", path);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void keybinds_init(const char *exe_path) {
    derive_ini_path(exe_path);

    FILE *test = fopen(s_ini_path, "r");
    if (test) {
        fclose(test);
        load_ini(s_ini_path);
    } else {
        write_defaults(s_ini_path);
    }
}

const GBKeybinds *keybinds_get(void) {
    return &s_binds;
}
