/*
 * launcher.c — ROM file discovery and caching for GB recompiled games
 *
 * On first run, opens a native file picker to select the ROM.
 * Caches the path in rom.cfg next to the executable for subsequent runs.
 */
#include "launcher.h"
#include "game_extras.h"
#include "gb_sha256.h"
#include "bps_patch.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <commdlg.h>
#  pragma comment(lib, "comdlg32.lib")
#endif

/* ── rom.cfg helpers ──────────────────────────────────────────────────────── */

static char s_cfg_path[512] = {0};
static char s_rom_path[512] = {0};
static char s_exe_dir[512] = {0};        /* dir containing the executable (with trailing sep) */
static char s_expected_sha256[65] = {0};  /* gen-time ROM digest, "" = disabled */
static char s_patch_file[260] = {0};      /* BPS filename (next to exe), "" = none */

void launcher_set_expected_sha256(const char *hex) {
    if (hex && hex[0]) {
        strncpy(s_expected_sha256, hex, sizeof(s_expected_sha256) - 1);
        s_expected_sha256[sizeof(s_expected_sha256) - 1] = '\0';
    } else {
        s_expected_sha256[0] = '\0';
    }
}

void launcher_set_patch_file(const char *filename) {
    if (filename && filename[0]) {
        strncpy(s_patch_file, filename, sizeof(s_patch_file) - 1);
        s_patch_file[sizeof(s_patch_file) - 1] = '\0';
    } else {
        s_patch_file[0] = '\0';
    }
}

/* ── CRC32 ────────────────────────────────────────────────────────────────── */

static unsigned int crc32_table[256];
static int crc32_table_init = 0;

static void crc32_build_table(void) {
    for (unsigned int i = 0; i < 256; i++) {
        unsigned int c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0);
        crc32_table[i] = c;
    }
    crc32_table_init = 1;
}

static unsigned int crc32_compute(const unsigned char *data, unsigned int len) {
    if (!crc32_table_init) crc32_build_table();
    unsigned int crc = 0xFFFFFFFF;
    for (unsigned int i = 0; i < len; i++)
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

void launcher_init(void) {
#ifdef _WIN32
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    char *last_sep = strrchr(exe_path, '\\');
    if (last_sep) *(last_sep + 1) = '\0';
    snprintf(s_exe_dir, sizeof(s_exe_dir), "%s", exe_path);
    snprintf(s_cfg_path, sizeof(s_cfg_path), "%srom.cfg", exe_path);
#else
    s_exe_dir[0] = '\0';  /* current dir */
    strcpy(s_cfg_path, "rom.cfg");
#endif
}

static void rom_cfg_read(char *path_out, int max_len) {
    FILE *f = fopen(s_cfg_path, "r");
    if (!f) { path_out[0] = '\0'; return; }
    if (!fgets(path_out, max_len, f)) path_out[0] = '\0';
    fclose(f);
    int len = (int)strlen(path_out);
    while (len > 0 && (path_out[len-1] == '\n' || path_out[len-1] == '\r'))
        path_out[--len] = '\0';
}

static void rom_cfg_write(const char *rom_path) {
    FILE *f = fopen(s_cfg_path, "w");
    if (!f) return;
    fprintf(f, "%s\n", rom_path);
    fclose(f);
}

/* ── File picker ──────────────────────────────────────────────────────────── */

static int pick_rom_file(char *out, int max_len) {
#ifdef _WIN32
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    out[0] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = NULL;
    ofn.lpstrFilter = "Game Boy ROMs (*.gb;*.gbc)\0*.gb;*.gbc\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = out;
    ofn.nMaxFile    = (DWORD)max_len;
    ofn.lpstrTitle  = "Select Game Boy ROM";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
#else
    (void)out; (void)max_len;
    fprintf(stderr, "[Launcher] No ROM specified and no file picker available on this platform.\n");
    return 0;
#endif
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* ── helpers ──────────────────────────────────────────────────────────────── */

static unsigned char *load_file(const char *path, long *out_sz) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    unsigned char *data = (unsigned char *)malloc(sz > 0 ? (size_t)sz : 1);
    if (!data) { fclose(f); return NULL; }
    if (sz > 0 && fread(data, 1, (size_t)sz, f) != (size_t)sz) { free(data); fclose(f); return NULL; }
    fclose(f);
    *out_sz = sz;
    return data;
}

static int write_file(const char *path, const unsigned char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len;
}

/* Basename without directory or extension, e.g. ".../Foo Bar.gbc" -> "Foo Bar". */
static void path_stem(const char *path, char *out, size_t n) {
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    size_t i = 0;
    for (; base[i] && i + 1 < n; i++) out[i] = base[i];
    out[i] = '\0';
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

static int buffer_sha_matches(const unsigned char *data, size_t len, const char *hex) {
    char got[65];
    gb_sha256_hex(data, len, got);
    return strcmp(got, hex) == 0;
}

/* If `path` already matches the expected SHA, returns 1 and leaves resolved=path.
 * Otherwise, if a BPS patch is configured and applies to this (stock) ROM to
 * produce the expected ROM, writes "<stem>.extended.gbc" next to the exe (plus
 * a copy of the stock ROM as "<stem>.gbc"), copies that path into `resolved`,
 * and returns 1. Returns 0 if neither path works. */
static int resolve_or_patch(const char *path, char *resolved, size_t resolved_sz) {
    long sz = 0;
    unsigned char *rom = load_file(path, &sz);
    if (!rom) return 0;

    if (buffer_sha_matches(rom, (size_t)sz, s_expected_sha256)) {
        free(rom);
        snprintf(resolved, resolved_sz, "%s", path);
        return 1;  /* already the expected (e.g. extended) ROM */
    }
    if (!s_patch_file[0]) { free(rom); return 0; }

    char patch_path[600];
    snprintf(patch_path, sizeof(patch_path), "%s%s", s_exe_dir, s_patch_file);
    long psz = 0;
    unsigned char *patch = load_file(patch_path, &psz);
    if (!patch) { free(rom); return 0; }  /* no patch shipped → not patchable */

    unsigned char *out = NULL; size_t out_len = 0; char err[160];
    int rc = gb_bps_apply(patch, (size_t)psz, rom, (size_t)sz, &out, &out_len, err, sizeof(err));
    free(patch);
    if (rc != 0) {
        fprintf(stderr, "[Launcher] patch did not apply: %s\n", err);
        free(rom);
        return 0;
    }
    if (!buffer_sha_matches(out, out_len, s_expected_sha256)) {
        fprintf(stderr, "[Launcher] patched ROM does not match expected build\n");
        free(out); free(rom);
        return 0;
    }

    char stem[256];
    path_stem(path, stem, sizeof(stem));
    char ext_path[600], stock_copy[600];
    snprintf(ext_path,   sizeof(ext_path),   "%s%s.extended.gbc", s_exe_dir, stem);
    snprintf(stock_copy, sizeof(stock_copy), "%s%s.gbc",          s_exe_dir, stem);
    if (!write_file(ext_path, out, out_len)) {
        fprintf(stderr, "[Launcher] failed to write %s\n", ext_path);
        free(out); free(rom);
        return 0;
    }
    write_file(stock_copy, rom, (size_t)sz);  /* best-effort: keep stock next to exe */
    free(out); free(rom);

    fprintf(stderr, "[Launcher] applied enhancement patch -> %s\n", ext_path);
    snprintf(resolved, resolved_sz, "%s", ext_path);
    return 1;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

/* Returns: 0 invalid, 1 valid as-is, 2 valid after producing a patched file
 * (the resolved path is written into `resolved`). */
static int verify_rom(const char *path, char *resolved, size_t resolved_sz) {
    snprintf(resolved, resolved_sz, "%s", path);

    /* Pull expected CRC(s) from the game's extras.c hooks. */
    int valid_count = 0;
    const uint32_t *valid_list = game_get_valid_crcs(&valid_count);
    uint32_t expected_single = (valid_count == 0) ? game_get_expected_crc32() : 0;
    int have_crc = (valid_count > 0 && valid_list) || expected_single != 0;

    /* No CRC policy → SHA-256 exact-match, with optional BPS auto-patch. CRC
     * hooks take precedence (multi-revision carts keep their valid-list). */
    if (!have_crc) {
        if (!s_expected_sha256[0]) return 1;  /* nothing to verify */
        char patched[512];
        if (resolve_or_patch(path, patched, sizeof(patched))) {
            int changed = strcmp(patched, path) != 0;
            snprintf(resolved, resolved_sz, "%s", patched);
            return changed ? 2 : 1;
        }
#ifdef _WIN32
        MessageBoxA(NULL,
            "ROM SHA-256 mismatch!\n\nThis is not the ROM this build expects,\n"
            "and no enhancement patch could turn it into one.\n\n"
            "Please select the correct (stock) ROM.",
            "Wrong ROM", MB_ICONWARNING | MB_OK);
#endif
        return 0;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    unsigned char *data = (unsigned char *)malloc((size_t)sz);
    if (!data) { fclose(f); return 0; }
    fread(data, 1, (size_t)sz, f);
    fclose(f);

    unsigned int actual = crc32_compute(data, (unsigned int)sz);
    free(data);

    if (valid_count > 0 && valid_list) {
        for (int i = 0; i < valid_count; i++) {
            if (actual == valid_list[i]) return 1;
        }
    } else if (actual == expected_single) {
        return 1;
    }

    char msg[256];
    snprintf(msg, sizeof(msg),
        "ROM CRC32 mismatch!\n\nGot: %08X\n\n"
        "Please select a valid ROM file.",
        actual);
    fprintf(stderr, "[Launcher] %s\n", msg);
#ifdef _WIN32
    MessageBoxA(NULL, msg, "Wrong ROM", MB_ICONWARNING | MB_OK);
#endif
    return 0;
}

const char *launcher_get_rom_path(void) {
    if (s_rom_path[0]) return s_rom_path;

    /* Try cached path */
    char cached[512];
    rom_cfg_read(cached, sizeof(cached));
    if (cached[0] && file_exists(cached)) {
        char resolved[512];
        int rc = verify_rom(cached, resolved, sizeof(resolved));
        if (rc) {
            snprintf(s_rom_path, sizeof(s_rom_path), "%s", resolved);
            if (rc == 2) rom_cfg_write(s_rom_path);  /* cache the patched ROM */
            printf("[Launcher] ROM: %s (cached)\n", s_rom_path);
            return s_rom_path;
        }
    }

    /* Open file picker, loop until valid or cancelled */
    while (1) {
        char picked[512];
        if (!pick_rom_file(picked, sizeof(picked))) {
            s_rom_path[0] = '\0';
            return NULL;
        }
        char resolved[512];
        if (verify_rom(picked, resolved, sizeof(resolved))) {
            snprintf(s_rom_path, sizeof(s_rom_path), "%s", resolved);
            break;
        }
    }

    rom_cfg_write(s_rom_path);
    printf("[Launcher] ROM: %s\n", s_rom_path);
    return s_rom_path;
}

unsigned char *launcher_load_rom(const char *path, unsigned int *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[Launcher] Cannot open ROM: %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    unsigned char *data = (unsigned char *)malloc((size_t)sz);
    if (!data) { fclose(f); return NULL; }
    fread(data, 1, (size_t)sz, f);
    fclose(f);

    *out_size = (unsigned int)sz;
    return data;
}
