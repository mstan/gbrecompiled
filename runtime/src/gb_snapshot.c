#include "gb_snapshot.h"
#include "gbrt.h"
#include "ppu.h"
#include "audio.h"
#include "sgb.h"
#include "ir.h"
#include "gb_widescreen.h"
#include "gb_custom_view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "gb_snapshot_core.inc"

#ifndef GBRT_HLE
#define GBRT_HLE 0
#endif
#define STATE_VERSION 2u
#define STATE_LIMIT (64u * 1024u * 1024u)
#define WRAM_BYTES 0x8000u
#define VRAM_BYTES 0x4000u
#define OAM_BYTES 0xa0u
#define HRAM_BYTES 0x7fu
#define IO_BYTES 0x81u

static const GBSnapshotExtension* state_extension;
bool gb_snapshot_set_extension(const GBSnapshotExtension* extension) {
    if (extension && (!extension->id || !extension->id[0] || strlen(extension->id) >= 64 ||
        !extension->size || !extension->save || !extension->validate || !extension->restore)) return false;
    state_extension = extension;
    return true;
}

/* The core/PPU/APU are opaque same-ABI sections, not a cross-build wire ABI.
 * Changing their semantics requires a schema bump. Never persist a context,
 * callback or allocation address. Header words reject foreign endianness. */
typedef struct {
    uint8_t magic[8];
    uint32_t version, endian, header_size, abi, profile;
    uint32_t rom_size, bios_size, hardware, gba_mode;
    uint32_t core_size, eram_size, ppu_size, apu_size, sgb_size, ir_size;
    uint32_t raw_size, compressed_size, checksum;
    uint8_t rom_hash[32], bios_hash[32];
    uint32_t extension_version, extension_size;
    char extension_id[64];
} GBStateHeader;

_Static_assert(sizeof(((GBContext*)0)->serial_transfer) ==
               sizeof(((GBSavestateCoreState*)0)->serial_transfer),
               "snapshot serial inventory must match the machine");

static bool state_header(GBContext* ctx, GBStateHeader* h) {
    if (!ctx || !ctx->rom || !ctx->rom_size || !ctx->wram || !ctx->vram ||
        !ctx->oam || !ctx->hram || !ctx->io || ctx->rom_size > STATE_LIMIT ||
        ctx->eram_size > STATE_LIMIT || (ctx->eram_size && !ctx->eram)) return false;
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, "GBRBE\r\n", 8);
    h->version = STATE_VERSION;
    h->endian = 0x01020304u;
    h->header_size = sizeof(*h);
#ifdef _MSC_VER
#define GB_STRINGIFY_INNER(x) #x
#define GB_STRINGIFY(x) GB_STRINGIFY_INNER(x)
    const char* compiler = "msvc:" GB_STRINGIFY(_MSC_FULL_VER);
#else
    const char* compiler = __VERSION__;
#endif
    h->abi = (uint32_t)crc32(0, (const Bytef*)compiler, (uInt)strlen(compiler)) ^
             (uint32_t)(sizeof(void*) << 24);
    h->profile = GBRT_HLE | (GBRT_RAM_HLE << 1);
    h->rom_size = (uint32_t)ctx->rom_size;
    h->bios_size = (uint32_t)ctx->boot_rom_size;
    h->hardware = ctx->config.model;
    h->gba_mode = ctx->gba_mode;
    h->core_size = sizeof(GBSavestateCoreState);
    h->eram_size = (uint32_t)ctx->eram_size;
    h->ppu_size = ctx->ppu ? sizeof(GBPPU) : 0;
    h->apu_size = ctx->apu ? (uint32_t)gb_audio_state_size() : 0;
    h->sgb_size = ctx->sgb ? (uint32_t)gb_sgb_state_size() : 0;
    h->ir_size = ctx->ir ? (uint32_t)gb_ir_state_size() : 0;
    if (state_extension) {
        size_t extension_size = state_extension->size(ctx);
        if (!extension_size || extension_size > STATE_LIMIT) return false;
        h->extension_size = (uint32_t)extension_size;
        h->extension_version = state_extension->version;
        strcpy(h->extension_id, state_extension->id);
    }
    size_t raw = h->core_size + (size_t)h->eram_size + WRAM_BYTES + VRAM_BYTES +
        OAM_BYTES + HRAM_BYTES + IO_BYTES + h->ppu_size + h->apu_size + h->sgb_size + h->ir_size + h->extension_size;
    if (raw > STATE_LIMIT) return false;
    h->raw_size = (uint32_t)raw;
    memcpy(h->rom_hash, ctx->rom_sha256, 32);
    memcpy(h->bios_hash, ctx->boot_rom_sha256, 32);
    return true;
}

static void put_bytes(uint8_t** cursor, const void* source, size_t size) {
    if (size) memcpy(*cursor, source, size);
    *cursor += size;
}

bool gb_snapshot_serialize(GBContext* ctx, uint8_t** data, size_t* size) {
    if (!data || !size) return false;
    *data = NULL;
    *size = 0;
    GBStateHeader h;
    if (!state_header(ctx, &h)) return false;
    uint8_t* raw = (uint8_t*)malloc(h.raw_size);
    if (!raw) return false;
    uint8_t* cursor = raw;
    GBSavestateCoreState core;
    gbrt_capture_core_state(ctx, &core);
    put_bytes(&cursor, &core, sizeof(core));
    put_bytes(&cursor, ctx->eram, h.eram_size);
    put_bytes(&cursor, ctx->wram, WRAM_BYTES);
    put_bytes(&cursor, ctx->vram, VRAM_BYTES);
    put_bytes(&cursor, ctx->oam, OAM_BYTES);
    put_bytes(&cursor, ctx->hram, HRAM_BYTES);
    put_bytes(&cursor, ctx->io, IO_BYTES);
    put_bytes(&cursor, ctx->ppu, h.ppu_size);
    bool ok = !h.apu_size || gb_audio_save_state(ctx->apu, cursor, h.apu_size);
    cursor += h.apu_size;
    if (ok && h.sgb_size) ok = gb_sgb_save_state(ctx->sgb, cursor, h.sgb_size);
    cursor += h.sgb_size;
    if (ok && h.ir_size) ok = gb_ir_save_state(ctx->ir, cursor, h.ir_size);
    cursor += h.ir_size;
    if (ok && h.extension_size) ok = state_extension->save(ctx, cursor, h.extension_size);
    uLongf compressed_size = compressBound(h.raw_size);
    uint8_t* packed = ok ? (uint8_t*)malloc(sizeof(h) + compressed_size) : NULL;
    ok = packed && compress2(packed + sizeof(h), &compressed_size, raw, h.raw_size,
                             Z_BEST_SPEED) == Z_OK;
    if (ok) {
        h.compressed_size = (uint32_t)compressed_size;
        h.checksum = (uint32_t)crc32(0, raw, h.raw_size);
        memcpy(packed, &h, sizeof(h));
        *data = packed;
        *size = sizeof(h) + compressed_size;
    } else free(packed);
    free(raw);
    return ok;
}

bool gb_snapshot_deserialize(GBContext* ctx, const uint8_t* data, size_t size) {
    GBStateHeader expected, h;
    if (!data || size < sizeof(h) || !state_header(ctx, &expected)) return false;
    memcpy(&h, data, sizeof(h));
    expected.compressed_size = h.compressed_size;
    expected.checksum = h.checksum;
    if (memcmp(&h, &expected, sizeof(h)) ||
        h.compressed_size > compressBound(h.raw_size) ||
        size - sizeof(h) != h.compressed_size) return false;
    uint8_t* raw = (uint8_t*)malloc(h.raw_size);
    if (!raw) return false;
    uLongf raw_size = h.raw_size;
    bool ok = uncompress(raw, &raw_size, data + sizeof(h), h.compressed_size) == Z_OK &&
        raw_size == h.raw_size && (uint32_t)crc32(0, raw, h.raw_size) == h.checksum;
    GBSavestateCoreState core;
    if (ok) memcpy(&core, raw, sizeof(core));
    if (ok && (memcmp(core.save_id, ctx->save_id, sizeof(core.save_id)) ||
               core.boot_rom_active > 1 || (core.boot_rom_active && !ctx->boot_rom) ||
               core.dma.progress > OAM_BYTES || core.wram_bank > 7 ||
               core.vram_bank > 1)) ok = false;
    const uint8_t* eram = raw + sizeof(core);
    const uint8_t* wram = eram + h.eram_size;
    const uint8_t* vram = wram + WRAM_BYTES;
    const uint8_t* oam = vram + VRAM_BYTES;
    const uint8_t* hram = oam + OAM_BYTES;
    const uint8_t* io = hram + HRAM_BYTES;
    const uint8_t* ppu = io + IO_BYTES;
    const uint8_t* apu = ppu + h.ppu_size;
    const uint8_t* sgb = apu + h.apu_size;
    const uint8_t* ir = sgb + h.sgb_size;
    const uint8_t* extension = ir + h.ir_size;
    if (ok && h.extension_size) ok = state_extension->validate(ctx, extension, h.extension_size);
    /* Stage any component whose validation/allocation can fail before commit. */
    GBSgbState* staged_sgb = NULL;
    if (ok && h.sgb_size) {
        staged_sgb = gb_sgb_create();
        ok = staged_sgb && gb_sgb_load_state(staged_sgb, sgb, h.sgb_size);
    }
    GBIRState* staged_ir = NULL;
    if (ok && h.ir_size) {
        staged_ir = gb_ir_create();
        ok = staged_ir && gb_ir_load_state(staged_ir, ir, h.ir_size);
    }
    if (ok) {
        if (h.eram_size) memcpy(ctx->eram, eram, h.eram_size);
        memcpy(ctx->wram, wram, WRAM_BYTES);
        memcpy(ctx->vram, vram, VRAM_BYTES);
        memcpy(ctx->oam, oam, OAM_BYTES);
        memcpy(ctx->hram, hram, HRAM_BYTES);
        memcpy(ctx->io, io, IO_BYTES);
        if (h.ppu_size) memcpy(ctx->ppu, ppu, h.ppu_size);
        /* APU/IR loads cannot fail after the exact-size/staging checks above. */
        if (h.apu_size) gb_audio_load_state(ctx->apu, apu, h.apu_size);
        if (h.ir_size) gb_ir_load_state(ctx->ir, ir, h.ir_size);
        if (staged_sgb) {
            gb_sgb_set_display_palettes(staged_sgb, gb_sgb_is_display_palettes(ctx->sgb));
            gb_sgb_set_display_border(staged_sgb, gb_sgb_is_display_border(ctx->sgb));
            gb_sgb_destroy(ctx->sgb);
            ctx->sgb = staged_sgb;
            staged_sgb = NULL;
        }
        gbrt_restore_core_state(ctx, &core);
        ctx->debug_dispatch_only = 0;
        if (h.eram_size) ctx->eram_dirty = 1;
        gb_ws_reapply(ctx);
        if (gb_custom_reset) gb_custom_reset(ctx);
        if (h.extension_size) state_extension->restore(ctx, extension, h.extension_size);
    }
    gb_sgb_destroy(staged_sgb);
    gb_ir_destroy(staged_ir);
    free(raw);
    return ok;
}

bool gb_context_save_state_file(GBContext* ctx, const char* path) {
    if (!path || !*path) return false;
    uint8_t* data;
    size_t size;
    if (!gb_snapshot_serialize(ctx, &data, &size)) return false;
    char* temporary = (char*)malloc(strlen(path) + 5);
    if (!temporary) { free(data); return false; }
    sprintf(temporary, "%s.tmp", path);
    FILE* file = fopen(temporary, "wb");
    bool ok = file && fwrite(data, 1, size, file) == size;
    if (file && fclose(file)) ok = false;
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        ok = rename(temporary, path) == 0;
#endif
    }
    if (!ok) remove(temporary);
    free(temporary);
    free(data);
    return ok;
}

bool gb_context_load_state_file(GBContext* ctx, const char* path) {
    if (!path || !*path) return false;
    FILE* file = fopen(path, "rb");
    if (!file) return false;
    bool ok = fseek(file, 0, SEEK_END) == 0;
    long size = ok ? ftell(file) : -1;
    ok = size >= (long)sizeof(GBStateHeader) && size <= (long)STATE_LIMIT &&
         fseek(file, 0, SEEK_SET) == 0;
    uint8_t* data = ok ? (uint8_t*)malloc((size_t)size) : NULL;
    ok = data && fread(data, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    if (ok) ok = gb_snapshot_deserialize(ctx, data, (size_t)size);
    free(data);
    if (ok) gb_rewind_clear(ctx);
    return ok;
}
