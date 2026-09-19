/*
 * launcher.c — ROM file discovery and caching for GB recompiled games
 *
 * On first run, opens a native file picker to select the ROM.
 * Caches the path in rom.cfg next to the executable for subsequent runs.
 */
#include "launcher.h"
#include "game_extras.h"
#include "gb_host_paths.h"
#include "gb_sha256.h"
#include "bps_patch.h"
#include <dirent.h>
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
static char s_state_dir[512] = {0};      /* writable, user-visible (with trailing sep) */
static char s_asset_dir[512] = {0};      /* read-only payload shipped with the exe */
static char s_expected_sha256[65] = {0};  /* gen-time ROM digest, "" = disabled */
static char s_patch_file[260] = {0};      /* BPS filename (next to exe), "" = none */
static char s_last_error[256] = {0};      /* reason for the most recent failure */
/* A rejected ROM normally warrants a message box / a stderr line: the user
 * chose that file. The beside-the-program scan below tries every cart in a
 * folder, so its rejections are routine and must stay silent — one dialog per
 * unrelated .gb sitting next to the exe is not a diagnostic, it is a wall. */
static int  s_verify_quiet = 0;

const char *launcher_last_error(void) { return s_last_error; }

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
    /* rom.cfg is user state: it must land beside the .AppImage / .app, not
     * inside a read-only container, and not wherever the process happened to
     * be started from. gb_host_paths.c owns that policy for every platform —
     * before it existed this was the exe directory on Windows and the process
     * working directory everywhere else, so a Linux build launched from a
     * desktop entry cached its ROM path into $HOME. */
    snprintf(s_state_dir, sizeof(s_state_dir), "%s", gb_host_state_dir());
    snprintf(s_asset_dir, sizeof(s_asset_dir), "%s", gb_host_asset_dir());
    gb_host_state_path("rom.cfg", s_cfg_path, sizeof(s_cfg_path));
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
    /* No native dialog off Windows, and none is wanted: ROM picking belongs to
     * the recomp-ui launcher, which draws its own in-app browser on Linux
     * (launcher_ui_seam.c arms it). This path is only reached by a build with
     * no launcher at all, so say what the player can do about it instead of
     * "no file picker available", which named a thing they cannot install. */
    (void)out; (void)max_len;
    fprintf(stderr,
            "[Launcher] No ROM selected. Put a .gb/.gbc file beside the program "
            "(or in roms/), or write its full path as the first line of %s\n",
            s_cfg_path[0] ? s_cfg_path : "rom.cfg");
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

/* Resolve a file shipped WITH the program (a BPS patch today). It normally
 * sits next to the executable, which inside an AppImage means inside the
 * read-only mount — so the asset directory is tried first, and the state
 * directory (next to the .AppImage / .app) second, which also lets a user drop
 * their own patch beside the program without unpacking it. */
static void resolve_payload_file(const char *name, char *out, size_t out_size) {
    FILE *f;
    gb_host_asset_path(name, out, out_size);
    f = fopen(out, "rb");
    if (f) { fclose(f); return; }
    if (strcmp(s_state_dir, s_asset_dir) == 0) return;
    gb_host_state_path(name, out, out_size);
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
    resolve_payload_file(s_patch_file, patch_path, sizeof(patch_path));
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
    snprintf(ext_path,   sizeof(ext_path),   "%s%s.extended.gbc", s_state_dir, stem);
    snprintf(stock_copy, sizeof(stock_copy), "%s%s.gbc",          s_state_dir, stem);
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

int launcher_image_matches_sha256(const unsigned char *rom, unsigned int rom_len,
                                  const char *sha256_hex) {
    if (!rom || !sha256_hex || !sha256_hex[0]) return 0;
    return buffer_sha_matches(rom, (size_t)rom_len, sha256_hex);
}

/* Multi-body path: derive one body's image from the user's ROM entirely in
 * memory. Never touches the disk, never changes what the CRC gate accepts —
 * the user supplies (and the gate validates) the stock cart either way. */
int launcher_apply_patch_in_memory(const char *patch_filename,
                                   const unsigned char *rom, unsigned int rom_len,
                                   unsigned char **out, unsigned int *out_len,
                                   const char *expect_sha256) {
    s_last_error[0] = '\0';
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!patch_filename || !patch_filename[0] || !rom || !out || !out_len) {
        snprintf(s_last_error, sizeof(s_last_error), "internal: bad patch request");
        return 0;
    }

    char patch_path[600];
    resolve_payload_file(patch_filename, patch_path, sizeof(patch_path));
    long psz = 0;
    unsigned char *patch = load_file(patch_path, &psz);
    if (!patch) {
        snprintf(s_last_error, sizeof(s_last_error),
                 "patch file not found next to the executable: %s", patch_filename);
        return 0;
    }

    unsigned char *patched = NULL;
    size_t patched_len = 0;
    char err[160] = {0};
    int rc = gb_bps_apply(patch, (size_t)psz, rom, (size_t)rom_len,
                          &patched, &patched_len, err, sizeof(err));
    free(patch);
    if (rc != 0) {
        snprintf(s_last_error, sizeof(s_last_error),
                 "%s does not apply to this ROM: %s", patch_filename, err);
        return 0;
    }
    if (expect_sha256 && expect_sha256[0] &&
        !buffer_sha_matches(patched, patched_len, expect_sha256)) {
        free(patched);
        snprintf(s_last_error, sizeof(s_last_error),
                 "%s produced an image this build was not compiled from", patch_filename);
        return 0;
    }

    *out = patched;
    *out_len = (unsigned int)patched_len;
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
    snprintf(s_last_error, sizeof(s_last_error), "ROM CRC32 mismatch (got %08X)", actual);
    fprintf(stderr, "[Launcher] %s\n", msg);
#ifdef _WIN32
    MessageBoxA(NULL, msg, "Wrong ROM", MB_ICONWARNING | MB_OK);
#endif
    return 0;
}

/* Case-insensitive ".gb" / ".gbc" / ".sgb" suffix test. */
static int is_rom_name(const char *name) {
    static const char *const exts[] = { ".gb", ".gbc", ".sgb" };
    size_t n = name ? strlen(name) : 0;
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t e = strlen(exts[i]);
        if (n <= e) continue;
        const char *tail = name + (n - e);
        size_t j = 0;
        for (; j < e; j++) {
            char a = tail[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
            if (a != exts[i][j]) break;
        }
        if (j == e) return 1;
    }
    return 0;
}

/* First ROM in `dir` that this build accepts, resolved into `resolved`.
 * Lowest filename first, so the choice is stable rather than directory-order.
 * verify_rom is the gate, so a folder of unrelated carts cannot be picked up
 * by accident — only the one this binary was recompiled from. Past
 * ROM_SCAN_MAX candidates in one directory the scan stops looking; that is a
 * ROM library, not a game folder, and the launcher's Browse is the surface for
 * one of those. */
#define ROM_SCAN_MAX 256
static int find_acceptable_rom_in_dir(const char *dir, char *resolved, size_t resolved_sz) {
    DIR *d;
    struct dirent *ent;
    static char names[ROM_SCAN_MAX][256];
    int count = 0;
    if (!dir || !dir[0]) return 0;
    d = opendir(dir);
    if (!d) return 0;
    while ((ent = readdir(d)) != NULL && count < (int)(sizeof(names) / sizeof(names[0]))) {
        if (!is_rom_name(ent->d_name)) continue;
        snprintf(names[count], sizeof(names[count]), "%s", ent->d_name);
        count++;
    }
    closedir(d);
    for (int i = 1; i < count; i++) {
        char tmp[256];
        int j = i;
        snprintf(tmp, sizeof(tmp), "%s", names[i]);
        while (j > 0 && strcmp(names[j - 1], tmp) > 0) {
            snprintf(names[j], sizeof(names[j]), "%s", names[j - 1]);
            j--;
        }
        snprintf(names[j], sizeof(names[j]), "%s", tmp);
    }
    for (int i = 0; i < count; i++) {
        char candidate[600];
        size_t dn = strlen(dir);
        snprintf(candidate, sizeof(candidate), "%s%s%s", dir,
                 (dn && (dir[dn - 1] == '/' || dir[dn - 1] == '\\')) ? "" : "/",
                 names[i]);
        int ok;
        s_verify_quiet = 1;
        ok = verify_rom(candidate, resolved, resolved_sz);
        s_verify_quiet = 0;
        if (ok) return 1;
    }
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

    /* A ROM the player dropped beside the program, or under roms/. The Linux
     * AppRun already seeded rom.cfg this way for AppImages only and only in
     * shell; doing it here gives the Windows zip, the plain tarball and the
     * .app bundle the same behaviour, and covers an AppImage started without
     * its AppRun. */
    {
        char resolved[512];
        char roms_dir[600];
        /* gb_host_state_dir() returns "" for "the process working directory". */
        const char *base = s_state_dir[0] ? s_state_dir : "./";
        snprintf(roms_dir, sizeof(roms_dir), "%sroms", base);
        if (find_acceptable_rom_in_dir(base, resolved, sizeof(resolved)) ||
            find_acceptable_rom_in_dir(roms_dir, resolved, sizeof(resolved))) {
            snprintf(s_rom_path, sizeof(s_rom_path), "%s", resolved);
            rom_cfg_write(s_rom_path);
            printf("[Launcher] ROM: %s (found beside the program)\n", s_rom_path);
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
