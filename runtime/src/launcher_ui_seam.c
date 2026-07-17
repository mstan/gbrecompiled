/*
 * launcher_ui_seam.c — gb-recompiled ↔ recomp-ui pre-boot seam. See
 * launcher_ui_seam.h. Compiled only when RECOMP_LAUNCHER is defined.
 */
#ifdef RECOMP_LAUNCHER

#include "launcher_ui_seam.h"
#include "game_extras.h"

#include "recomp_launcher.h"   // recomp-ui C ABI
#include "launcher_profile.h"  // launcher_profile_apply()

#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── path helpers ─────────────────────────────────────────────────────────── */

/* Resolve <exe_dir>/<name> into `out`. exe_dir already ends in a separator. */
static void seam_join(char* out, size_t cap, const char* dir, const char* name) {
    snprintf(out, cap, "%s%s", dir ? dir : "", name);
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
    /* exe-anchored paths (same base load_runtime_preferences / launcher.c use) */
    char* base = SDL_GetBasePath();
    char exe_dir[1024];
    snprintf(exe_dir, sizeof(exe_dir), "%s", base ? base : "");
    if (base) SDL_free(base);

    char prefs_path[1152], romcfg_path[1152];
    seam_join(prefs_path,  sizeof(prefs_path),  exe_dir, "runtime_prefs.ini");
    seam_join(romcfg_path, sizeof(romcfg_path), exe_dir, "rom.cfg");

    /* "Skip launcher on boot": honor the persisted flag unless forced. */
    if (seam_read_int(prefs_path, "launcher.skip", 0) != 0 &&
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
    ls.screen_kind   = seam_read_int(prefs_path, "video.palette", 0);
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

    char title[256];
    snprintf(title, sizeof(title), "%s \xE2\x80\x94 Launcher",
             gi.name ? gi.name : "Game Boy");

    char out_rom[1024];
    out_rom[0] = '\0';
    int rc = recomp_launcher_run_window(title, &ls, &gi, exe_dir,
                                        initial_rom, out_rom, sizeof(out_rom));

    if (rc == 1) return GB_LAUNCHER_QUIT;         /* user closed the launcher */
    if (rc != 0) return GB_LAUNCHER_UNAVAILABLE;  /* couldn't init: fall back  */

    /* LAUNCH: persist the chosen settings so load_runtime_preferences() (called
     * moments later in gb_platform_init) picks them up. Keybinds were written
     * live by the gb bridge during the session. */
    seam_upsert_int(prefs_path, "window.scale",         ls.window_scale > 0 ? ls.window_scale : 5);
    seam_upsert_int(prefs_path, "video.fullscreen",     ls.fullscreen ? 1 : 0);
    seam_upsert_int(prefs_path, "video.linear_filter",  ls.linear_filter ? 1 : 0);
    seam_upsert_int(prefs_path, "video.palette",        ls.screen_kind);
    seam_upsert_int(prefs_path, "audio.volume_percent", ls.volume);
    seam_upsert_int(prefs_path, "video.widescreen",     ls.widescreen ? 1 : 0);
    seam_upsert_int(prefs_path, "launcher.skip",        ls.skip_launcher ? 1 : 0);

    if (out_rom[0]) seam_write_rom_cfg(romcfg_path, out_rom);

    return GB_LAUNCHER_LAUNCH;
}

#endif /* RECOMP_LAUNCHER */
