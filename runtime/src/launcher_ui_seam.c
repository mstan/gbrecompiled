/*
 * launcher_ui_seam.c — gb-recompiled ↔ recomp-ui pre-boot seam. See
 * launcher_ui_seam.h. Compiled only when RECOMP_LAUNCHER is defined.
 */
#ifdef RECOMP_LAUNCHER

#include "launcher_ui_seam.h"
#include "game_extras.h"
#include "gb_host_paths.h"
#include "debug_server.h"

#include "recomp_launcher.h"   // recomp-ui C ABI
#include "launcher_profile.h"  // launcher_profile_apply()

#include <SDL.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── path helpers ─────────────────────────────────────────────────────────── */

/* Resolve <exe_dir>/<name> into `out`. exe_dir already ends in a separator. */
static void seam_join(char* out, size_t cap, const char* dir, const char* name) {
    snprintf(out, cap, "%s%s", dir ? dir : "", name);
}

/* ── ROM file picker policy ───────────────────────────────────────────────────
 *
 * recomp-ui owns ROM picking end to end: the ABI has no file-dialog callback,
 * so a host steers the picker only through the environment recomp-ui reads
 * (launcher_imgui.cpp: prefer_builtin_file_picker / builtin_picker_initial_path).
 * Three knobs, all honoured here with overwrite=0 so a user or packager who
 * exports one keeps it:
 *
 *   RECOMP_UI_BUILTIN_FILE_PICKER  Linux only. 1 => draw the in-launcher
 *       browser instead of shelling out. Default it ON, because a packaged
 *       Linux build has no reliable native dialog: the AppRun prepends the
 *       bundle's usr/lib to LD_LIBRARY_PATH, so a spawned host zenity loads the
 *       bundle's glib/pcre2 and dies (or worse, blocks) — and on a host with no
 *       zenity/kdialog at all there is nothing to spawn. Measured on the
 *       v0.1.1 AppImage under WSLg: with zenity installed, Browse froze the
 *       launcher's frame loop until the process was killed; without it, the
 *       built-in browser opened. Only the built-in browser works in both.
 *       Windows/macOS keep their native dialog, exactly as the SNES and PSX
 *       hosts do (recomp-ui hard-returns "native available" off Linux).
 *
 *   RECOMP_DISC_HINT      a ROM FILE. The browser opens in its folder with it
 *       pre-selected. Filled from the first .gb/.gbc/.sgb found beside the
 *       program, then in roms/.
 *
 *   RECOMP_APPIMAGE_PATH  a file whose PARENT directory the browser opens in,
 *       used when no ROM was found. Without it recomp-ui falls back to $HOME,
 *       which is the one place a player's ROM is least likely to be — they
 *       dropped it next to the .AppImage, which is where gb_host_state_dir()
 *       points.
 */

static int seam_file_readable(const char* path) {
    FILE* f;
    if (!path || !path[0]) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* Case-insensitive ".gb" / ".gbc" / ".sgb" suffix test — the same set as
 * recomp-ui's gb profile rom_filter (kGbRomPatterns). */
static int seam_is_rom_name(const char* name) {
    static const char* const kExts[] = { ".gb", ".gbc", ".sgb" };
    size_t n = name ? strlen(name) : 0;
    for (size_t i = 0; i < sizeof(kExts) / sizeof(kExts[0]); ++i) {
        size_t e = strlen(kExts[i]);
        if (n <= e) continue;
        const char* tail = name + (n - e);
        size_t j = 0;
        for (; j < e; ++j) {
            char a = tail[j], b = kExts[i][j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
            if (a != b) break;
        }
        if (j == e) return 1;
    }
    return 0;
}

/* First ROM-looking file in `dir` (not recursive), joined into `out`. The
 * lowest name wins so the choice is stable across runs rather than following
 * directory order. Returns 1 when one was found. */
static int seam_find_rom_in_dir(const char* dir, char* out, size_t cap) {
    DIR* d;
    struct dirent* ent;
    char best[512];
    if (!dir || !dir[0] || !out || cap == 0) return 0;
    d = opendir(dir);
    if (!d) return 0;
    best[0] = '\0';
    while ((ent = readdir(d)) != NULL) {
        if (!seam_is_rom_name(ent->d_name)) continue;
        if (!best[0] || strcmp(ent->d_name, best) < 0)
            snprintf(best, sizeof(best), "%s", ent->d_name);
    }
    closedir(d);
    if (!best[0]) return 0;
    snprintf(out, cap, "%s%s%s", dir,
             dir[strlen(dir) - 1] == '/' || dir[strlen(dir) - 1] == '\\' ? "" : "/",
             best);
    return seam_file_readable(out);
}

/* The first ROM a player has put where the program can see it: beside it, then
 * under roms/. Returns 1 and fills `out` when there is one. */
static int seam_find_local_rom(const char* state_dir, char* out, size_t cap) {
    char roms_dir[600];
    /* gb_host_state_dir() returns "" for "the process working directory". */
    const char* base = (state_dir && state_dir[0]) ? state_dir : "./";
    if (seam_find_rom_in_dir(base, out, cap)) return 1;
    seam_join(roms_dir, sizeof(roms_dir), base, "roms");
    return seam_find_rom_in_dir(roms_dir, out, cap);
}

/* Point recomp-ui's file picker at something a player can actually use. Called
 * once, immediately before recomp_launcher_run_window. */
static void seam_arm_file_picker(const char* state_dir) {
#if defined(__linux__)
    SDL_setenv("RECOMP_UI_BUILTIN_FILE_PICKER", "1", 0);
#endif

    char hint[600];
    if (!state_dir) state_dir = "";

    /* 1. a ROM beside the program, 2. one under roms/ — either gives the
     * browser both the right folder and a pre-selected file. */
    if (seam_find_local_rom(state_dir, hint, sizeof(hint))) {
        SDL_setenv("RECOMP_DISC_HINT", hint, 0);
        return;
    }

    /* 3. no ROM anywhere yet: still open beside the program rather than $HOME.
     * Inside an AppImage $APPIMAGE is exactly the file recomp-ui wants; outside
     * one, any path in the state directory names the same parent. */
    {
        const char* appimage = SDL_getenv("APPIMAGE");
        if (appimage && appimage[0]) {
            SDL_setenv("RECOMP_APPIMAGE_PATH", appimage, 0);
        } else if (state_dir[0]) {
            seam_join(hint, sizeof(hint), state_dir, "rom.cfg");
            SDL_setenv("RECOMP_APPIMAGE_PATH", hint, 0);
        }
    }
}

/* ── runtime_prefs.ini read / surgical upsert ─────────────────────────────── */

/* Read a flat "key=value" int from runtime_prefs.ini; `def` if absent. */
static int seam_read_int(const char* path, const char* key, int def) {
    FILE* f = fopen(path, "r");
    if (!f) return def;
    char line[256];
    size_t kl = strlen(key);
    int val = def;
    while (fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') ++s;
        if (strncmp(s, key, kl) == 0) {
            const char* p = s + kl;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p == '=') { val = atoi(p + 1); break; }
        }
    }
    fclose(f);
    return val;
}

/* Does `line` (leading ws) assign flat key `key`? */
static int seam_line_is_key(const char* line, const char* key) {
    const char* i = line;
    while (*i == ' ' || *i == '\t') ++i;
    size_t kl = strlen(key);
    if (strncmp(i, key, kl) != 0) return 0;
    i += kl;
    while (*i == ' ' || *i == '\t') ++i;
    return *i == '=';
}

/* Surgically upsert one "key=value" line into runtime_prefs.ini, preserving
 * every other line (blank lines, keybinds, per-game prefs, audio, …). */
static void seam_upsert(const char* path, const char* key, const char* value) {
    FILE* f = fopen(path, "rb");
    long len = 0;
    char* text = NULL;
    if (f) {
        fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
        text = (char*)malloc((size_t)(len > 0 ? len : 0) + 1);
        if (text) { len = (long)fread(text, 1, (size_t)(len > 0 ? len : 0), f); text[len] = 0; }
        fclose(f);
    }

    int cap = 128, n = 0;
    char** lines = (char**)malloc(sizeof(char*) * cap);
    if (text) {
        char* start = text;
        for (long i = 0; i <= len; ++i) {
            if (i == len || text[i] == '\n') {
                char* end = text + i;
                if (end > start && end[-1] == '\r') end[-1] = 0;
                text[i] = 0;
                if (i == len && start == text + len) break;
                if (n == cap) { cap *= 2; lines = (char**)realloc(lines, sizeof(char*) * cap); }
                lines[n++] = strdup(start);
                start = text + i + 1;
            }
        }
    }

    char assign[256];
    snprintf(assign, sizeof(assign), "%s=%s", key, value);
    int hit = -1;
    for (int i = 0; i < n; ++i) if (seam_line_is_key(lines[i], key)) { hit = i; break; }
    if (hit >= 0) { free(lines[hit]); lines[hit] = strdup(assign); }
    else {
        if (n == cap) { cap += 8; lines = (char**)realloc(lines, sizeof(char*) * cap); }
        lines[n++] = strdup(assign);
    }

    f = fopen(path, "wb");
    if (f) { for (int i = 0; i < n; ++i) { fputs(lines[i], f); fputc('\n', f); } fclose(f); }
    for (int i = 0; i < n; ++i) free(lines[i]);
    free(lines);
    free(text);
}

static void seam_upsert_int(const char* path, const char* key, int value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    seam_upsert(path, key, buf);
}

/* Write the single-line rom.cfg (native path) that launcher_get_rom_path()
 * reads during game init. */
static void seam_write_rom_cfg(const char* path, const char* rom) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n", rom);
    fclose(f);
}

/* ── the seam ─────────────────────────────────────────────────────────────── */

int gb_launcher_preboot(void) {
    /* State-anchored paths (the same directory load_runtime_preferences and
     * launcher.c use). Not SDL_GetBasePath(): inside an AppImage that is the
     * read-only mount, so the launcher wrote rom.cfg and runtime_prefs.ini
     * where nothing could read them back. */
    char exe_dir[1024];
    snprintf(exe_dir, sizeof(exe_dir), "%s", gb_host_state_dir());

    char prefs_path[1152], romcfg_path[1152];
    seam_join(prefs_path,  sizeof(prefs_path),  exe_dir, "runtime_prefs.ini");
    seam_join(romcfg_path, sizeof(romcfg_path), exe_dir, "rom.cfg");

    /* seed the ROM field from the last-used rom.cfg (a single native path line) */
    char initial_rom[1024];
    initial_rom[0] = '\0';
    {
        FILE* rc = fopen(romcfg_path, "r");
        if (rc) {
            if (fgets(initial_rom, sizeof(initial_rom), rc)) {
                size_t n = strlen(initial_rom);
                while (n && (initial_rom[n-1] == '\n' || initial_rom[n-1] == '\r'))
                    initial_rom[--n] = '\0';
            }
            fclose(rc);
        }
    }
    /* No cached ROM (first run, or the player moved theirs): offer the one they
     * dropped beside the program, or in roms/. The launcher then opens with the
     * ROM already resolved and PLAY live, instead of an empty GAME card that
     * makes them browse for a file sitting right there. The identity gate is
     * unchanged — a wrong cart seeded here shows "not verified" and PLAY stays
     * disabled. The Linux AppRun did this for AppImages only, in shell; here
     * every packaging form gets it. */
    if (!seam_file_readable(initial_rom))
        if (!seam_find_local_rom(exe_dir, initial_rom, sizeof(initial_rom)))
            initial_rom[0] = '\0';

    /* "Skip launcher on boot": honor the persisted flag unless forced — but
     * never when the cached ROM has gone. Skipping then hands an unresolvable
     * path to launcher_get_rom_path(), whose own picker exists on Windows
     * only, so a Linux player who moved or renamed their ROM got
     * "No ROM selected -- exiting" and no way back to the launcher that could
     * have browsed for it. The launcher is the only ROM-pick surface, so any
     * run that has no ROM must see it. */
    if (seam_read_int(prefs_path, "launcher.skip", 0) != 0 &&
        seam_file_readable(initial_rom) &&
        !(SDL_getenv("GBRECOMP_LAUNCHER") && SDL_getenv("GBRECOMP_LAUNCHER")[0] == '1')) {
        return GB_LAUNCHER_LAUNCH;   /* boot straight in; nothing shown */
    }

    /* ── seed settings from runtime_prefs.ini ── */
    RecompLauncherCSettings ls;
    memset(&ls, 0, sizeof(ls));
    ls.output_method = 2;   /* OpenGL */
    ls.window_scale  = seam_read_int(prefs_path, "window.scale", 5);
    ls.fullscreen    = seam_read_int(prefs_path, "video.fullscreen", 0);
    ls.linear_filter = seam_read_int(prefs_path, "video.linear_filter", 0);
    /* Screen model folds the DMG palette (0..4) and the Super Game Boy models
     * (5 = colors+border, 6 = colors, no border) into one cycle. Reconstruct the
     * unified index from the split runtime prefs. Keep the SGB indices in sync
     * with kGbScreenKindNames / LNG_GB_SCREEN_KIND_SGB* in recomp-ui gb_profile.h. */
    if (seam_read_int(prefs_path, "sgb.colors", 0)) {
        ls.screen_kind = seam_read_int(prefs_path, "sgb.cart_border", 1) ? 5 : 6;
    } else {
        ls.screen_kind = seam_read_int(prefs_path, "video.palette", 0);
    }
    ls.enable_audio  = 1;
    ls.audio_freq    = 32768;
    ls.volume        = seam_read_int(prefs_path, "audio.volume_percent", 100);
    ls.widescreen    = seam_read_int(prefs_path, "video.widescreen", 0);
    ls.player_src[0] = 1;   /* keyboard */
    ls.skip_launcher = 0;

    /* ── game identity + capabilities ── */
    RecompLauncherCGameInfo gi;
    memset(&gi, 0, sizeof(gi));
    const char* platform = game_get_platform();
    launcher_profile_apply(platform && platform[0] ? platform : "gbc", &gi);
    gi.name = game_get_name();
#if RECOMP_UI_ENABLE_MODS
    gi.mods = game_get_mods(exe_dir);
#endif
    gi.region = "USA";
    /* The runtime derives the .sav name from the cart's save-id / header title,
     * which isn't known until the ROM is loaded (after this preboot). Leave the
     * SAVE row inert (no import/clear against a wrong file) rather than guess a
     * mismatched path; in-game saving is unaffected. */
    gi.sram_path = NULL;

    /* ROM identity gate (advisory badge; launcher_get_rom_path is authoritative).
     * Prefer the multi-revision CRC list, else the single expected CRC. gb uses
     * SHA-256 which the ABI's SHA-1 field can't carry, so SHA-only titles show
     * no CRC and rely on the runtime's own verify. */
    int crc_count = 0;
    const uint32_t* crcs = game_get_valid_crcs(&crc_count);
    if (crc_count > 0 && crcs) {
        gi.expected_crc = crcs[0];
        gi.has_expected_crc = 1;
    } else {
        uint32_t c = game_get_expected_crc32();
        if (c) { gi.expected_crc = c; gi.has_expected_crc = 1; }
    }

    /* Opt-in widescreen: expose the "Widescreen 16:9" toggle (drawn with an
     * EXPERIMENTAL tag by recomp-ui) only for games that opted into the
     * extended view — game_max_view_width() > native 160 (e.g. Megaman Xtreme
     * 2 returns 256). Mirrors how the other ecosystems flag experimental
     * widescreen. The chosen state persists to the video.widescreen pref,
     * which the runtime reads to arm gb_ws. */
    if (game_max_view_width() > 160) gi.widescreen_supported = 1;

    gi.boxart_path   = "assets/img/boxart.tga";  /* staged next to the exe */
    gi.config_path   = prefs_path;   /* hotkeys unused (gb hotkeys_mask == 0) */
    gi.keybinds_path = prefs_path;   /* gb bridge writes keyboard.<btn>.0 here */
    /* recomp-ui's own cached-ROM restore (launcher_model.c) otherwise looks for
     * "rom.cfg" in the cwd and the exe directory. Inside an AppImage the exe
     * directory is the read-only mount and the cwd is whatever the desktop
     * entry chose, so name the state-anchored file the runtime actually reads. */
    gi.rom_cache_path = romcfg_path;

    /* An em dash where titles are UTF-8 end to end. SDL2's X11 backend runs
     * the title through this process's C locale, which cannot convert it:
     * _NET_WM_NAME is never set and window managers show WM_NAME's raw bytes
     * ("Shantae â Launcher"), so Linux gets a plain hyphen. */
    char title[256];
#if defined(_WIN32) || defined(__APPLE__)
    snprintf(title, sizeof(title), "%s \xE2\x80\x94 Launcher",
             gi.name ? gi.name : "Game Boy");
#else
    snprintf(title, sizeof(title), "%s - Launcher",
             gi.name ? gi.name : "Game Boy");
#endif

    char out_rom[1024];
    out_rom[0] = '\0';
    /* Serve the debug protocol for the duration of the launcher own loop. The
     * launcher owns the process here -- no GBContext exists and the per-frame
     * pump has not started -- so a probe would otherwise have nothing to talk
     * to until the game boots. No-op unless GBRECOMP_DEBUG_PORT is set, and
     * the listening socket (plus any connected client) is handed to
     * gb_debug_server_init() afterwards, so one TCP session spans the
     * launcher and the game.
     *
     * Game-agnostic on purpose: it sits in the shared seam every title routes
     * through, not in any game module. */
    seam_arm_file_picker(exe_dir);
    gb_debug_server_preboot_begin();
    int rc = recomp_launcher_run_window(title, &ls, &gi, exe_dir,
                                        initial_rom, out_rom, sizeof(out_rom));
    gb_debug_server_preboot_end();

    if (rc == 1) return GB_LAUNCHER_QUIT;         /* user closed the launcher */
    if (rc != 0) return GB_LAUNCHER_UNAVAILABLE;  /* couldn't init: fall back  */

    /* LAUNCH: persist the chosen settings so load_runtime_preferences() (called
     * moments later in gb_platform_init) picks them up. Keybinds were written
     * live by the gb bridge during the session. */
    seam_upsert_int(prefs_path, "window.scale",         ls.window_scale > 0 ? ls.window_scale : 5);
    /* Tri-state (0 off / 1 borderless / 2 exclusive) — persist the value as
     * chosen in recomp-ui verbatim. Clamp defensively so a future ABI change
     * or stray value can't write something the runtime's own clamp (in
     * platform_sdl.cpp's video.fullscreen pref load) would otherwise have to
     * silently correct. Previously this collapsed to a bool (`? 1 : 0`),
     * which downgraded "Exclusive" (2) back to "Borderless" (1) on every
     * launcher round-trip. */
    int fullscreen_mode = ls.fullscreen;
    if (fullscreen_mode < 0) fullscreen_mode = 0;
    if (fullscreen_mode > 2) fullscreen_mode = 2;
    seam_upsert_int(prefs_path, "video.fullscreen",     fullscreen_mode);
    seam_upsert_int(prefs_path, "video.linear_filter",  ls.linear_filter ? 1 : 0);
    /* Screen model -> split runtime prefs. Models 0..4 are DMG palettes (SGB
     * colorization off); 5/6 are the Super Game Boy models (colors on, border
     * on/off). Keep the DMG palette index stable across an SGB round-trip so
     * switching back to a DMG model restores the previous palette. */
    if (ls.screen_kind >= 5) {
        seam_upsert_int(prefs_path, "sgb.colors",      1);
        seam_upsert_int(prefs_path, "sgb.cart_border", ls.screen_kind == 5 ? 1 : 0);
    } else {
        seam_upsert_int(prefs_path, "video.palette",   ls.screen_kind);
        seam_upsert_int(prefs_path, "sgb.colors",      0);
        /* A DMG model is the plain Game Boy look: no SGB colorization AND no SGB
         * cart border (otherwise the runtime default / a prior SGB choice leaves
         * the border on over a DMG-green screen). */
        seam_upsert_int(prefs_path, "sgb.cart_border", 0);
    }
    seam_upsert_int(prefs_path, "audio.volume_percent", ls.volume);
    seam_upsert_int(prefs_path, "video.widescreen",     ls.widescreen ? 1 : 0);
    seam_upsert_int(prefs_path, "launcher.skip",        ls.skip_launcher ? 1 : 0);

    if (out_rom[0]) seam_write_rom_cfg(romcfg_path, out_rom);

    return GB_LAUNCHER_LAUNCH;
}

#endif /* RECOMP_LAUNCHER */
