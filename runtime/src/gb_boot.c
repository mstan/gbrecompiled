#include "gb_boot.h"
#include "gb_host_paths.h"
#include "gb_sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !GBRT_HLE
void gbrt_bios_dmg__gb_dispatch(GBContext*, uint16_t);
void gbrt_bios_cgb__gb_dispatch(GBContext*, uint16_t);
extern const char gbrt_bios_dmg__expected_sha256[];
extern const char gbrt_bios_cgb__expected_sha256[];

static bool native_bios_matches(const GBContext* ctx) {
    const bool cgb = ctx->config.model == GB_MODEL_CGB;
    const char* expected = cgb ? gbrt_bios_cgb__expected_sha256 : gbrt_bios_dmg__expected_sha256;
    const char* hex = "0123456789abcdef";
    if (!ctx->boot_rom || ctx->boot_rom_size != (cgb ? 0x900u : 0x100u)) return false;
    for (size_t i = 0; i < 32; ++i) {
        if (expected[i * 2] != hex[ctx->boot_rom_sha256[i] >> 4] ||
            expected[i * 2 + 1] != hex[ctx->boot_rom_sha256[i] & 15]) return false;
    }
    return true;
}
#endif

const char* gb_boot_implementation(void) { return GBRT_HLE ? "HLE" : "LLE"; }

bool gb_boot_verify_file(const char* path, bool cgb) {
#if GBRT_HLE
    (void)path; (void)cgb;
    return false;
#else
    if (!path || !*path) return false;
    FILE* file = fopen(path, "rb");
    if (!file) return false;
    uint8_t image[0x901];
    size_t size = fread(image, 1, sizeof(image), file);
    bool ok = !ferror(file) && size == (cgb ? 0x900u : 0x100u);
    fclose(file);
    char hash[65];
    if (ok) {
        gb_sha256_hex(image, size, hash);
        ok = !strcmp(hash, cgb ? gbrt_bios_cgb__expected_sha256 : gbrt_bios_dmg__expected_sha256);
    }
    return ok;
#endif
}

bool gb_boot_resolve_path(bool cgb, char* path, size_t capacity) {
    if (!path || !capacity) return false;
    char cache[4096];
    gb_host_state_path(cgb ? "bios_cgb.cfg" : "bios_dmg.cfg", cache, sizeof(cache));
    FILE* file = fopen(cache, "r");
    path[0] = '\0';
    if (file) {
        if (fgets(path, (int)capacity, file)) path[strcspn(path, "\r\n")] = '\0';
        fclose(file);
        if (gb_boot_verify_file(path, cgb)) return true;
    }
    gb_host_state_path(cgb ? "bios/cgb_boot.bin" : "bios/dmg_boot.bin", path, capacity);
    return gb_boot_verify_file(path, cgb);
}

bool gb_boot_remember_path(const char* path) {
    bool cgb = gb_boot_verify_file(path, true);
    if (!cgb && !gb_boot_verify_file(path, false)) return false;
    char cache[4096];
    gb_host_state_path(cgb ? "bios_cgb.cfg" : "bios_dmg.cfg", cache, sizeof(cache));
    FILE* file = fopen(cache, "w");
    if (!file) return false;
    bool ok = fprintf(file, "%s\n", path) > 0;
    return fclose(file) == 0 && ok;
}

bool gb_boot_start(GBContext* ctx, const char* path) {
    if (!ctx || !ctx->rom) return false;
#if GBRT_HLE
    (void)path;
    gb_context_reset(ctx, true);
#else
    char automatic_path[4096];
    if (!path || !*path) {
        gb_boot_resolve_path(ctx->config.model == GB_MODEL_CGB, automatic_path, sizeof(automatic_path));
        path = automatic_path;
    }
    FILE* file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "[BOOT] LLE requires the real BIOS at %s (or --boot-rom PATH)\n", path);
        return false;
    }
    uint8_t image[0x901];
    size_t size = fread(image, 1, sizeof(image), file);
    bool read_ok = !ferror(file);
    fclose(file);
    if (!read_ok || !gb_context_load_boot_rom(ctx, image, size) || !native_bios_matches(ctx)) {
        fprintf(stderr, "[BOOT] BIOS differs from this build's native recompilation: %s\n", path);
        return false;
    }
    gb_context_reset(ctx, false);
#endif
    fprintf(stderr, "[BOOT] implementation=%s model=%s pc=%04X\n",
            gb_boot_implementation(), ctx->config.model == GB_MODEL_CGB ? "CGB" : "DMG", ctx->pc);
    return true;
}

bool gb_boot_dispatch(GBContext* ctx, uint16_t addr) {
    if (!ctx->boot_rom_active ||
        !(addr < 0x100 || (ctx->boot_rom_size == 0x900 && addr >= 0x200 && addr < 0x900))) return false;
#if GBRT_HLE
    /* A direct context/oracle caller can explicitly map a BIOS even in an
     * HLE build. Preserve that LLE API using the real instruction engine. */
    gb_interpret(ctx, addr);
#else
    if (!native_bios_matches(ctx)) {
        fprintf(stderr, "[BOOT] mapped BIOS has no matching native body\n");
        exit(EXIT_FAILURE);
    }
    if (ctx->config.model == GB_MODEL_CGB) gbrt_bios_cgb__gb_dispatch(ctx, addr);
    else gbrt_bios_dmg__gb_dispatch(ctx, addr);
#endif
    return true;
}
