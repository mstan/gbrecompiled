/*
 * launcher.c — ROM file discovery and caching for GB recompiled games
 *
 * On first run, opens a native file picker to select the ROM.
 * Caches the path in rom.cfg next to the executable for subsequent runs.
 */
#include "launcher.h"
#include "game_extras.h"
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
    snprintf(s_cfg_path, sizeof(s_cfg_path), "%srom.cfg", exe_path);
#else
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

/* ── Public API ───────────────────────────────────────────────────────────── */

static int verify_rom_crc(const char *path) {
    /* Pull expected CRC(s) from the game's extras.c hooks. */
    int valid_count = 0;
    const uint32_t *valid_list = game_get_valid_crcs(&valid_count);
    uint32_t expected_single = (valid_count == 0) ? game_get_expected_crc32() : 0;

    /* No CRC declared → skip validation entirely. */
    if (valid_count == 0 && expected_single == 0) return 1;

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
    rom_cfg_read(s_rom_path, sizeof(s_rom_path));
    if (s_rom_path[0] && file_exists(s_rom_path) && verify_rom_crc(s_rom_path)) {
        printf("[Launcher] ROM: %s (cached)\n", s_rom_path);
        return s_rom_path;
    }

    /* Open file picker, loop until valid or cancelled */
    s_rom_path[0] = '\0';
    while (1) {
        if (!pick_rom_file(s_rom_path, sizeof(s_rom_path))) {
            return NULL;
        }
        if (verify_rom_crc(s_rom_path)) break;
        s_rom_path[0] = '\0';
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
