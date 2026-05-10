/*
 * See gb_asset_loader.h.
 */
#include "gb_asset_loader.h"
#include "gb_sha1.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(__linux__) || defined(__APPLE__)
#include <libgen.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#define LOG(fmt, ...) fprintf(stderr, "[assets] " fmt "\n", ##__VA_ARGS__)

static bool path_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool ensure_dir(const char* path) {
    if (path_exists(path)) return true;
    /* mkdir -p style: walk components. */
    char buf[1024];
    if (strlen(path) >= sizeof(buf)) return false;
    strcpy(buf, path);
    for (char* p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
                LOG("mkdir(%s) failed: %s", buf, strerror(errno));
                return false;
            }
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
        LOG("mkdir(%s) failed: %s", buf, strerror(errno));
        return false;
    }
    return true;
}

static bool ensure_parent_dir(const char* file_path) {
    char buf[1024];
    if (strlen(file_path) >= sizeof(buf)) return false;
    strcpy(buf, file_path);
    char* slash = strrchr(buf, '/');
    if (!slash) return true; /* no parent to create */
    *slash = '\0';
    return ensure_dir(buf);
}

bool gb_chdir_to_exe_dir(void) {
    char buf[4096];
#if defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        LOG("readlink(/proc/self/exe) failed: %s", strerror(errno));
        return false;
    }
    buf[n] = '\0';
#elif defined(__APPLE__)
    uint32_t cap = (uint32_t)sizeof(buf);
    if (_NSGetExecutablePath(buf, &cap) != 0) {
        LOG("_NSGetExecutablePath failed (need %u bytes)", (unsigned)cap);
        return false;
    }
#else
    /* Windows / unknown: bail out. The caller still gets a sane CWD —
     * just won't auto-chdir to the binary's location. */
    (void)buf;
    LOG("gb_chdir_to_exe_dir: unsupported platform — leaving CWD as-is");
    return false;
#endif
#if defined(__linux__) || defined(__APPLE__)
    char* d = dirname(buf);
    if (chdir(d) != 0) {
        LOG("chdir(%s) failed: %s", d, strerror(errno));
        return false;
    }
    return true;
#endif
}

static bool sha1_check(const uint8_t* data, size_t len, const uint8_t expected[20]) {
    SHA1_CTX ctx;
    SHA1Init(&ctx);
    SHA1Update(&ctx, data, len);
    uint8_t got[20];
    SHA1Final(got, &ctx);
    return memcmp(got, expected, 20) == 0;
}

static void hex(char out[41], const uint8_t in[20]) {
    static const char* h = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        out[i * 2] = h[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = h[in[i] & 0xF];
    }
    out[40] = '\0';
}

static bool assets_complete(const GBGameAssets* game) {
    char path[1024];
    for (uint32_t i = 0; i < game->manifest_count; i++) {
        const GBAssetEntry* e = &game->manifest[i];
        snprintf(path, sizeof(path), "assets/%s/%s", game->game_id, e->path);
        struct stat st;
        if (stat(path, &st) != 0 || st.st_size != (off_t)e->size) {
            return false;
        }
    }
    return true;
}

static bool load_from_assets(const GBGameAssets* game) {
    /* Pre-fill with 0xFF to mirror rgblink -p 0xff padding. */
    memset(game->rom_data, 0xFF, game->rom_size);

    char path[1024];
    for (uint32_t i = 0; i < game->manifest_count; i++) {
        const GBAssetEntry* e = &game->manifest[i];
        snprintf(path, sizeof(path), "assets/%s/%s", game->game_id, e->path);
        FILE* f = fopen(path, "rb");
        if (!f) {
            LOG("fopen(%s) failed: %s", path, strerror(errno));
            return false;
        }
        size_t got = fread(game->rom_data + e->rom_offset, 1, e->size, f);
        fclose(f);
        if (got != e->size) {
            LOG("short read on %s: got %zu / %u", path, got, e->size);
            return false;
        }
    }
    LOG("%s: loaded %u sections from assets/", game->game_id, game->manifest_count);
    return true;
}

static bool extract_from_rom(const GBGameAssets* game) {
    FILE* f = fopen(game->rom_filename, "rb");
    if (!f) {
        LOG("Cannot open ROM '%s': %s", game->rom_filename, strerror(errno));
        LOG("Place a clean copy of your %s ROM at '%s' and try again.",
            game->game_id, game->rom_filename);
        return false;
    }
    size_t got = fread(game->rom_data, 1, game->rom_size, f);
    fclose(f);
    if (got != game->rom_size) {
        LOG("ROM '%s' is %zu bytes, expected %u", game->rom_filename, got, game->rom_size);
        return false;
    }

    if (!sha1_check(game->rom_data, game->rom_size, game->expected_sha1)) {
        char want[41], have[41];
        hex(want, game->expected_sha1);
        SHA1_CTX ctx;
        SHA1Init(&ctx);
        SHA1Update(&ctx, game->rom_data, game->rom_size);
        uint8_t got_sha[20];
        SHA1Final(got_sha, &ctx);
        hex(have, got_sha);
        LOG("ROM '%s' has SHA-1 %s, expected %s — refusing to extract",
            game->rom_filename, have, want);
        return false;
    }
    LOG("%s: ROM verified (sha1 OK), extracting %u sections to assets/%s/",
        game->game_id, game->manifest_count, game->game_id);

    char path[1024];
    for (uint32_t i = 0; i < game->manifest_count; i++) {
        const GBAssetEntry* e = &game->manifest[i];
        snprintf(path, sizeof(path), "assets/%s/%s", game->game_id, e->path);
        if (!ensure_parent_dir(path)) return false;
        FILE* o = fopen(path, "wb");
        if (!o) {
            LOG("fopen(%s, wb) failed: %s", path, strerror(errno));
            return false;
        }
        size_t wrote = fwrite(game->rom_data + e->rom_offset, 1, e->size, o);
        fclose(o);
        if (wrote != e->size) {
            LOG("short write on %s: wrote %zu / %u", path, wrote, e->size);
            return false;
        }
    }
    LOG("%s: extraction complete", game->game_id);
    return true;
}

bool gb_load_assets(const GBGameAssets* game) {
    if (!game || !game->rom_data || game->rom_size == 0) return false;

    if (assets_complete(game)) {
        return load_from_assets(game);
    }

    if (!extract_from_rom(game)) return false;
    /* The rom_data buffer is already populated from extract_from_rom. No need
     * to re-load from assets/ on this run. */
    return true;
}
