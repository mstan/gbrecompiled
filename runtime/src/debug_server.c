/*
 * debug_server.c -- TCP debug server for GB recomp (shared runtime)
 *
 * Single-threaded, non-blocking TCP server polled once per frame.
 * JSON-over-newline protocol on localhost:4370.
 *
 * Game-specific commands dispatched via game_handle_debug_cmd() hook.
 * Game-specific frame data filled via game_fill_frame_record() hook.
 *
 * Modeled after:
 *   nesrecomp/runner/src/debug_server.c
 *   psxrecomp/runner/src/debug_server.c
 */
#include "debug_server.h"
#include "game_extras.h"
#include "gbrt.h"
#include "gb_platform_compat.h"
#include "gb_body.h"
#include "ppu.h"
#include "platform_sdl.h"
#include "gb_custom_view.h"
#include "gb_widescreen.h"
/* Declarations only — gb_printer.c owns STB_IMAGE_WRITE_IMPLEMENTATION. */
#include "stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#ifdef GB_HAS_SDL2
#include <SDL.h>
#endif

/* ---- Extern: the GBContext pointer (set by game_on_init or platform) ---- */
static GBContext *s_ctx = NULL;

void gb_debug_server_set_context(GBContext *ctx) { s_ctx = ctx; }

/* ---- Server state ---- */
static sock_t s_listen  = SOCK_INVALID;
static sock_t s_client  = SOCK_INVALID;
static int    s_port    = 4370;

#define RECV_BUF_SIZE 8192
static char s_recv_buf[RECV_BUF_SIZE];
static int  s_recv_len = 0;

/* ---- Outbound queue ----
 * The client socket is non-blocking, so send() can accept only part of a
 * message (or none) whenever the kernel send buffer is full — which happens
 * as soon as any async event source outruns the client's reader. The old code
 * issued two unchecked send() calls per line (payload, then "\n") and dropped
 * whatever the kernel refused, so a full buffer silently truncated a line or
 * ate its terminator, concatenating two JSON objects on one line and
 * desynchronizing every subsequent response. Queue whole lines instead: a
 * message is either fully enqueued or fully dropped, never split. */
#define SEND_BUF_SIZE (1 << 20)
static char     s_send_buf[SEND_BUF_SIZE];
static uint32_t s_send_head = 0;   /* next byte to transmit */
static uint32_t s_send_tail = 0;   /* next free byte */
static uint64_t s_send_drops = 0;  /* whole messages dropped for lack of room */
static uint64_t s_send_drops_reported = 0;

static uint32_t send_queue_used(void)
{
    return (s_send_tail >= s_send_head)
         ? (s_send_tail - s_send_head)
         : (SEND_BUF_SIZE - s_send_head + s_send_tail);
}

/* One byte is kept free so head==tail always means "empty". */
static uint32_t send_queue_free(void)
{
    return SEND_BUF_SIZE - 1 - send_queue_used();
}

static void send_queue_reset(void)
{
    s_send_head = 0;
    s_send_tail = 0;
    s_send_drops = 0;
    s_send_drops_reported = 0;
}

static void send_queue_push(const char *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        s_send_buf[s_send_tail] = data[i];
        s_send_tail = (s_send_tail + 1) & (SEND_BUF_SIZE - 1);
    }
}

/* Drain as much of the queue as the socket will take. Partial sends advance
 * the head; EWOULDBLOCK stops the drain until the next poll. */
static void send_queue_flush(void)
{
    while (s_client != SOCK_INVALID && s_send_head != s_send_tail) {
        uint32_t end = (s_send_tail > s_send_head) ? s_send_tail : SEND_BUF_SIZE;
        int chunk = (int)(end - s_send_head);
        int n = send(s_client, s_send_buf + s_send_head, chunk, 0);
        if (n > 0) {
            s_send_head = (s_send_head + (uint32_t)n) & (SEND_BUF_SIZE - 1);
            continue;
        }
        if (n == 0) break;
        {
            int err = sock_error();
#ifdef _WIN32
            if (err == WSAEWOULDBLOCK) break;
#else
            if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) break;
#endif
            fprintf(stderr, "[debug] send error %d, dropping client\n", err);
            sock_close(s_client);
            s_client = SOCK_INVALID;
            send_queue_reset();
            return;
        }
    }
}

/* ---- Pause / step ---- */
static volatile int s_paused     = 0;
static int          s_step_count = 0;
static uint32_t     s_run_to     = 0;

/* ---- Input override ----
 * s_input_override is an active-high button mask consumed by
 * gb_platform_get_joypad(); -1 means "no override, the real joypad wins".
 * s_input_frames > 0 makes the override transient: record_frame() counts it
 * down and clears the override when it hits zero (the `press` command). A
 * value of 0 with an active override means "held until cleared"
 * (set_input / hold / release). */
static int s_input_override = -1;
static int s_input_frames   = 0;

/* ---- Frame counter (maintained by record_frame) ---- */
static uint64_t s_frame_count = 0;

/* ---- Ring buffer ---- */
static GBFrameRecord s_frame_history[GB_FRAME_HISTORY_CAP];
static uint64_t      s_history_count = 0;

/* ---- Watchpoints ---- */
#define MAX_WATCHPOINTS 8
typedef struct {
    uint16_t addr;
    uint8_t  prev_val;
    int      active;
} Watchpoint;
static Watchpoint s_watchpoints[MAX_WATCHPOINTS];

/* ---- Last function tracking ---- */
static char s_last_func[32] = "(none)";

void gb_debug_server_set_last_func(const char *name) {
    if (!name) name = "(none)";
    strncpy(s_last_func, name, sizeof(s_last_func) - 1);
    s_last_func[sizeof(s_last_func) - 1] = '\0';
}

/* ---- Platform helpers ---- */
static void set_nonblocking(sock_t s)
{
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

/* ---- JSON helpers (hand-parsed, no library) ---- */

static const char *json_get_str(const char *json, const char *key,
                                 char *out, int out_sz)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < out_sz - 1)
            out[i++] = *p++;
        out[i] = '\0';
        return out;
    }
    {
        int i = 0;
        while (*p && *p != ',' && *p != '}' && *p != ' ' && i < out_sz - 1)
            out[i++] = *p++;
        out[i] = '\0';
        return out;
    }
}

static int json_get_int(const char *json, const char *key, int def)
{
    char buf[64];
    if (!json_get_str(json, key, buf, sizeof(buf))) return def;
    return atoi(buf);
}

static uint32_t hex_to_u32(const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    return (uint32_t)strtoul(s, NULL, 16);
}

/* ---- Send helpers (public API) ---- */

void gb_debug_server_send_line(const char *json)
{
    if (s_client == SOCK_INVALID) return;

    uint32_t len = (uint32_t)strlen(json);
    if (len == 0) return;

    /* Whole-line atomicity: a message that does not fit is dropped entirely,
     * so the stream stays newline-synchronized no matter how far behind the
     * reader is. Drops are counted and reported once there is room again. */
    if (len + 1 > send_queue_free()) {
        s_send_drops++;
        return;
    }
    send_queue_push(json, len);
    send_queue_push("\n", 1);

    if (s_send_drops != s_send_drops_reported) {
        char note[96];
        int n = snprintf(note, sizeof(note),
                         "{\"event\":\"dropped\",\"messages\":%llu}\n",
                         (unsigned long long)s_send_drops);
        if (n > 0 && (uint32_t)n <= send_queue_free()) {
            send_queue_push(note, (uint32_t)n);
            s_send_drops_reported = s_send_drops;
        }
    }
}

void gb_debug_server_send_fmt(const char *fmt, ...)
{
    char buf[16384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gb_debug_server_send_line(buf);
}

#define send_line  gb_debug_server_send_line
#define send_fmt   gb_debug_server_send_fmt

static void send_ok(int id)
{
    send_fmt("{\"id\":%d,\"ok\":true}", id);
}

static void send_err(int id, const char *msg)
{
    send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"%s\"}", id, msg);
}

/* ---- RAM access helpers ---- */

static uint8_t read_byte(uint16_t addr)
{
    if (!s_ctx) return 0;
    return gb_read8(s_ctx, addr);
}

static void write_byte(uint16_t addr, uint8_t val)
{
    if (!s_ctx) return;
    gb_write8(s_ctx, addr, val);
}

/* Host-side read with EXPLICIT bank selection, straight out of the backing
 * arrays. Unlike read_byte() this never enters gb_read8, so it cannot be
 * intercepted by a game module's gb_custom_read_override, cannot trip a
 * watchpoint and cannot advance any emulated state -- and it can reach banks
 * the guest does not currently have mapped (VRAM bank 1's BG attribute map,
 * WRAM banks 2-7, any ROM/ERAM bank). Pass -1 for a bank to use the live one. */
static uint8_t peek_byte(uint16_t addr, int rom_bank, int ram_bank,
                         int wram_bank, int vram_bank)
{
    GBContext *c = s_ctx;
    if (!c) return 0;
    if (rom_bank  < 0) rom_bank  = (int)c->rom_bank;
    if (ram_bank  < 0) ram_bank  = (int)c->ram_bank;
    if (wram_bank < 0) wram_bank = (int)c->wram_bank;
    if (vram_bank < 0) vram_bank = (int)c->vram_bank;
    if (wram_bank == 0) wram_bank = 1;   /* SVBK 0 aliases bank 1 */
    if (addr < 0x4000u)
        return c->rom && addr < c->rom_size ? c->rom[addr] : 0;
    if (addr < 0x8000u) {
        size_t o = (size_t)rom_bank * 0x4000u + (addr - 0x4000u);
        return c->rom && o < c->rom_size ? c->rom[o] : 0;
    }
    if (addr < 0xA000u) {
        size_t o = (size_t)(vram_bank & 1) * VRAM_SIZE + (addr - 0x8000u);
        return c->vram ? c->vram[o] : 0;
    }
    if (addr < 0xC000u) {
        size_t o = (size_t)ram_bank * 0x2000u + (addr - 0xA000u);
        return c->eram && o < c->eram_size ? c->eram[o] : 0;
    }
    if (addr < 0xD000u) return c->wram ? c->wram[addr - 0xC000u] : 0;
    if (addr < 0xE000u)
        return c->wram ? c->wram[(size_t)(wram_bank & 7) * 0x1000u + (addr - 0xD000u)] : 0;
    if (addr >= 0xFE00u && addr < 0xFEA0u) return c->oam ? c->oam[addr - 0xFE00u] : 0;
    if (addr >= 0xFF00u && addr < 0xFF80u) return c->io ? c->io[addr - 0xFF00u] : 0;
    if (addr >= 0xFF80u && addr < 0xFFFFu) return c->hram ? c->hram[addr - 0xFF80u] : 0;
    return 0;
}

/* {"cmd":"peek","addr":"0xD000","len":256,"wram_bank":2} -- see peek_byte().
 * Chunked like dump_ram (256 bytes per line) so one call can pull a whole
 * tilemap or attribute map. */
static void handle_peek(int id, const char *json)
{
    char addr_str[32];
    if (!s_ctx) { send_err(id, "no context"); return; }
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint16_t addr = (uint16_t)hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 256);
    if (len < 1) len = 1;
    if (len > 8192) len = 8192;
    int rom_bank  = json_get_int(json, "rom_bank",  -1);
    int ram_bank  = json_get_int(json, "ram_bank",  -1);
    int wram_bank = json_get_int(json, "wram_bank", -1);
    int vram_bank = json_get_int(json, "vram_bank", -1);

    int offset = 0;
    while (offset < len) {
        int chunk = len - offset;
        if (chunk > 256) chunk = 256;
        char hex[513];
        for (int i = 0; i < chunk; i++)
            snprintf(hex + i * 2, 3, "%02x",
                     peek_byte((uint16_t)(addr + offset + i),
                               rom_bank, ram_bank, wram_bank, vram_bank));
        send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%04X\",\"offset\":%d,"
                 "\"len\":%d,\"total\":%d,\"hex\":\"%s\"}",
                 id, (unsigned)((addr + offset) & 0xFFFF), offset, chunk, len, hex);
        offset += chunk;
    }
}

/* ---- Command handlers ---- */

static void handle_ping(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%llu}",
             id, (unsigned long long)s_frame_count);
}

static void handle_frame(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%llu,\"last_func\":\"%s\"}",
             id, (unsigned long long)s_frame_count, s_last_func);
}

static void handle_get_registers(int id, const char *json)
{
    (void)json;
    if (!s_ctx) { send_err(id, "no context"); return; }

    gb_pack_flags(s_ctx);
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"A\":\"0x%02X\",\"F\":\"0x%02X\","
             "\"B\":\"0x%02X\",\"C_reg\":\"0x%02X\","
             "\"D\":\"0x%02X\",\"E\":\"0x%02X\","
             "\"H_reg\":\"0x%02X\",\"L\":\"0x%02X\","
             "\"SP\":\"0x%04X\",\"PC\":\"0x%04X\","
             "\"Z\":%d,\"N\":%d,\"H\":%d,\"C\":%d,"
             "\"IME\":%d,"
             "\"rom_bank\":%d,\"ram_bank\":%d,"
             "\"frame\":%llu}",
             id,
             s_ctx->a, s_ctx->f,
             s_ctx->b, s_ctx->c,
             s_ctx->d, s_ctx->e,
             s_ctx->h, s_ctx->l,
             s_ctx->sp, s_ctx->pc,
             s_ctx->f_z, s_ctx->f_n, s_ctx->f_h, s_ctx->f_c,
             s_ctx->ime,
             s_ctx->rom_bank, s_ctx->ram_bank,
             (unsigned long long)s_frame_count);
}

static void handle_read_ram(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint16_t addr = (uint16_t)hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 1);
    if (len < 1) len = 1;
    if (len > 256) len = 256;

    char hex[513];
    for (int i = 0; i < len; i++)
        snprintf(hex + i * 2, 3, "%02x", read_byte(addr + (uint16_t)i));

    send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%04X\",\"len\":%d,\"hex\":\"%s\"}",
             id, addr, len, hex);
}

static void handle_dump_ram(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint16_t addr = (uint16_t)hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 256);
    if (len < 1) len = 1;
    if (len > 8192) len = 8192;

    int offset = 0;
    while (offset < len) {
        int chunk = len - offset;
        if (chunk > 256) chunk = 256;
        char hex[513];
        for (int i = 0; i < chunk; i++)
            snprintf(hex + i * 2, 3, "%02x", read_byte(addr + (uint16_t)(offset + i)));
        send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%04X\",\"offset\":%d,\"len\":%d,\"hex\":\"%s\"}",
                 id, addr + offset, offset, chunk, hex);
        offset += chunk;
    }
}

static void handle_write_ram(int id, const char *json)
{
    char addr_str[32], hex_str[1024];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    if (!json_get_str(json, "hex", hex_str, sizeof(hex_str))) {
        /* Try single value */
        char val_str[32];
        if (!json_get_str(json, "val", val_str, sizeof(val_str))) {
            send_err(id, "missing hex or val");
            return;
        }
        uint16_t addr = (uint16_t)hex_to_u32(addr_str);
        write_byte(addr, (uint8_t)hex_to_u32(val_str));
        send_ok(id);
        return;
    }
    uint16_t addr = (uint16_t)hex_to_u32(addr_str);
    int hlen = (int)strlen(hex_str);
    for (int i = 0; i + 1 < hlen; i += 2) {
        char byte_hex[3] = { hex_str[i], hex_str[i+1], '\0' };
        write_byte(addr + (uint16_t)(i / 2), (uint8_t)strtoul(byte_hex, NULL, 16));
    }
    send_ok(id);
}

static void handle_read_oam(int id, const char *json)
{
    if (!s_ctx || !s_ctx->oam) { send_err(id, "no context"); return; }

    int index = json_get_int(json, "index", -1);
    if (index >= 0 && index < 40) {
        int off = index * 4;
        send_fmt("{\"id\":%d,\"ok\":true,\"index\":%d,"
                 "\"y\":%d,\"x\":%d,\"tile\":%d,\"flags\":\"0x%02X\"}",
                 id, index,
                 s_ctx->oam[off], s_ctx->oam[off+1],
                 s_ctx->oam[off+2], s_ctx->oam[off+3]);
    } else {
        /* Dump all 40 sprites as hex (160 bytes) */
        char hex[321];
        for (int i = 0; i < 160; i++)
            snprintf(hex + i * 2, 3, "%02x", s_ctx->oam[i]);
        send_fmt("{\"id\":%d,\"ok\":true,\"count\":40,\"hex\":\"%s\"}", id, hex);
    }
}

static void handle_read_vram(int id, const char *json)
{
    if (!s_ctx || !s_ctx->vram) { send_err(id, "no context"); return; }

    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint16_t addr = (uint16_t)hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 16);
    if (len < 1) len = 1;
    if (len > 256) len = 256;

    /* VRAM is at 0x8000-0x9FFF */
    char hex[513];
    for (int i = 0; i < len; i++) {
        uint16_t vaddr = addr + (uint16_t)i;
        uint8_t v = 0;
        if (vaddr >= 0x8000 && vaddr < 0xA000)
            v = s_ctx->vram[vaddr - 0x8000];
        snprintf(hex + i * 2, 3, "%02x", v);
    }

    send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%04X\",\"len\":%d,\"hex\":\"%s\"}",
             id, addr, len, hex);
}

static void handle_read_io(int id, const char *json)
{
    if (!s_ctx) { send_err(id, "no context"); return; }

    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint16_t addr = (uint16_t)hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 1);
    if (len < 1) len = 1;
    if (len > 128) len = 128;

    char hex[257];
    for (int i = 0; i < len; i++) {
        uint16_t ioaddr = addr + (uint16_t)i;
        uint8_t v = 0;
        if (ioaddr >= 0xFF00 && ioaddr <= 0xFF7F && s_ctx->io)
            v = s_ctx->io[ioaddr - 0xFF00];
        snprintf(hex + i * 2, 3, "%02x", v);
    }

    send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%04X\",\"len\":%d,\"hex\":\"%s\"}",
             id, addr, len, hex);
}

static void handle_hw_state(int id, const char *json)
{
    (void)json;
    if (!s_ctx) { send_err(id, "no context"); return; }

    /* Which hardware model is actually live, which recompiled body is running
     * it, and -- on CGB -- both 64-byte palette RAMs, read straight out of the
     * GBPPU. Deliberately NOT a BCPS/BCPD poke sequence: writing the palette
     * index register to walk the RAM would permanently move the guest's own
     * index, and BCPD reads back 0xFF during mode 3, so a probe paused at an
     * arbitrary cycle would silently record garbage. This is one non-mutating
     * read of state the runtime already keeps. */
    const GBPPU *ppu = (const GBPPU *)s_ctx->ppu;
    const GBBody *body = gb_body_active();
    int cgb = s_ctx->config.model == GB_MODEL_CGB;

    char bg_hex[129], obj_hex[129];
    bg_hex[0] = obj_hex[0] = '\0';
    if (cgb && ppu) {
        for (int i = 0; i < 64; i++) {
            snprintf(bg_hex + i * 2, 3, "%02X", ppu->bg_palette_ram[i]);
            snprintf(obj_hex + i * 2, 3, "%02X", ppu->obj_palette_ram[i]);
        }
    }

    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"model\":\"%s\",\"cgb\":%d,\"cgb_compat\":%d,"
             "\"body\":\"%s\",\"rom_size\":%u,\"mbc\":\"0x%02X\","
             "\"bgpi\":\"0x%02X\",\"obpi\":\"0x%02X\","
             "\"bg_palette\":\"%s\",\"obj_palette\":\"%s\"}",
             id,
             s_ctx->config.model == GB_MODEL_CGB ? "cgb"
                 : (s_ctx->config.model == GB_MODEL_SGB ? "sgb" : "dmg"),
             cgb ? 1 : 0,
             s_ctx->config.cgb_compatibility_mode ? 1 : 0,
             body && body->id ? body->id : "",
             (unsigned)s_ctx->rom_size,
             s_ctx->mbc_type,
             ppu ? (unsigned)ppu->bgpi : 0u,
             ppu ? (unsigned)ppu->obpi : 0u,
             bg_hex, obj_hex);
}

static void handle_ppu_state(int id, const char *json)
{
    (void)json;
    if (!s_ctx || !s_ctx->io) { send_err(id, "no context"); return; }

    uint8_t *io = s_ctx->io;
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"LCDC\":\"0x%02X\",\"STAT\":\"0x%02X\","
             "\"SCY\":%d,\"SCX\":%d,"
             "\"LY\":%d,\"LYC\":%d,"
             "\"WY\":%d,\"WX\":%d,"
             "\"BGP\":\"0x%02X\",\"OBP0\":\"0x%02X\",\"OBP1\":\"0x%02X\"}",
             id,
             io[0x40], io[0x41],
             io[0x42], io[0x43],
             io[0x44], io[0x45],
             io[0x4A], io[0x4B],
             io[0x47], io[0x48], io[0x49]);
}

static void handle_mapper_state(int id, const char *json)
{
    (void)json;
    if (!s_ctx) { send_err(id, "no context"); return; }

    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"rom_bank\":%d,\"ram_bank\":%d,"
             "\"mbc_type\":%d,\"ram_enabled\":%d,\"mbc_mode\":%d}",
             id,
             s_ctx->rom_bank, s_ctx->ram_bank,
             s_ctx->mbc_type, s_ctx->ram_enabled, s_ctx->mbc_mode);
}

static void handle_watch(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint16_t addr = (uint16_t)hex_to_u32(addr_str);

    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        if (!s_watchpoints[i].active) {
            s_watchpoints[i].addr = addr;
            s_watchpoints[i].prev_val = read_byte(addr);
            s_watchpoints[i].active = 1;
            send_fmt("{\"id\":%d,\"ok\":true,\"slot\":%d,\"addr\":\"0x%04X\"}",
                     id, i, addr);
            return;
        }
    }
    send_err(id, "all watchpoint slots full (max 8)");
}

static void handle_unwatch(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint16_t addr = (uint16_t)hex_to_u32(addr_str);

    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        if (s_watchpoints[i].active && s_watchpoints[i].addr == addr) {
            s_watchpoints[i].active = 0;
            send_ok(id);
            return;
        }
    }
    send_err(id, "watchpoint not found");
}

/* Button spellings accepted anywhere a `buttons` argument is taken.
 *
 * Two forms, distinguished by the leading characters:
 *   - a hex mask, "0x30" or "30"  (bit 0 R, 1 L, 2 U, 3 D, 4 A, 5 B,
 *                                  6 Select, 7 Start -- active high)
 *   - a letter string, "AB", "RS", "-"  using the same letters the --input
 *     script route and platform_sdl.cpp's parse_buttons()/write_buttons() use:
 *     R L U D A B S(tart) T(selecT).
 * Returns the mask, or -1 if the string is not a valid spelling. "-" and ""
 * both mean "no buttons" (mask 0). */
static int parse_button_mask(const char *s)
{
    if (!s) return -1;
    if (s[0] == '\0' || (s[0] == '-' && s[1] == '\0')) return 0;

    /* Hex form: an explicit 0x prefix, or an all-hex-digit string that
     * contains at least one character that is not a button letter. */
    int all_letters = 1;
    for (const char *p = s; *p; p++) {
        if (!strchr("RLUDABST", *p)) { all_letters = 0; break; }
    }
    if (!all_letters || (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
        const char *p = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? s + 2 : s;
        if (!*p) return -1;
        for (const char *q = p; *q; q++) {
            if (!strchr("0123456789abcdefABCDEF", *q)) return -1;
        }
        return (int)(hex_to_u32(s) & 0xFFu);
    }

    int mask = 0;
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case 'R': mask |= 0x01; break;
            case 'L': mask |= 0x02; break;
            case 'U': mask |= 0x04; break;
            case 'D': mask |= 0x08; break;
            case 'A': mask |= 0x10; break;
            case 'B': mask |= 0x20; break;
            case 'T': mask |= 0x40; break;  /* selecT */
            case 'S': mask |= 0x80; break;  /* Start */
            default:  return -1;
        }
    }
    return mask;
}

/* Render a mask back in the letter spelling, for echoing in replies. */
static void button_mask_to_str(int mask, char *out, int out_sz)
{
    static const char k_letters[8] = { 'R', 'L', 'U', 'D', 'A', 'B', 'T', 'S' };
    int n = 0;
    for (int i = 0; i < 8 && n < out_sz - 1; i++) {
        if (mask & (1 << i)) out[n++] = k_letters[i];
    }
    if (n == 0 && out_sz > 1) out[n++] = '-';
    out[n] = '\0';
}

static void reply_input(int id, const char *cmd)
{
    char letters[16];
    int mask = s_input_override < 0 ? 0 : s_input_override;
    button_mask_to_str(mask, letters, sizeof(letters));
    send_fmt("{\"id\":%d,\"ok\":true,\"cmd\":\"%s\",\"buttons\":\"%s\","
             "\"mask\":%d,\"frames\":%d,\"frame\":%llu}",
             id, cmd, s_input_override < 0 ? "-" : letters,
             s_input_override, s_input_frames,
             (unsigned long long)s_frame_count);
}

static void handle_set_input(int id, const char *json)
{
    char val_str[32];
    if (!json_get_str(json, "buttons", val_str, sizeof(val_str))) {
        send_err(id, "missing buttons");
        return;
    }
    int mask = parse_button_mask(val_str);
    if (mask < 0) { send_err(id, "bad buttons"); return; }
    s_input_override = mask;
    s_input_frames   = 0;   /* held until cleared */
    reply_input(id, "set_input");
}

static void handle_clear_input(int id, const char *json)
{
    (void)json;
    s_input_override = -1;
    s_input_frames   = 0;
    reply_input(id, "clear_input");
}

/* Transient press: hold `buttons` for `frames` guest frames (default 1), then
 * release automatically. Counted down in gb_debug_server_record_frame(), which
 * runs once per presented guest frame, so N frames means N frames of the
 * emulated joypad -- not N host frames and not N poll cycles. */
static void handle_press(int id, const char *json)
{
    char val_str[32];
    if (!json_get_str(json, "buttons", val_str, sizeof(val_str))) {
        send_err(id, "missing buttons");
        return;
    }
    int mask = parse_button_mask(val_str);
    if (mask < 0) { send_err(id, "bad buttons"); return; }
    int frames = json_get_int(json, "frames", 1);
    if (frames < 1) frames = 1;
    s_input_override = mask;
    s_input_frames   = frames;
    reply_input(id, "press");
}

/* Incremental variants of set_input: add / remove buttons from the held mask
 * without having to restate the whole thing. Both cancel any pending `press`
 * countdown, because the caller is asking for an explicitly held state. */
static void handle_hold(int id, const char *json)
{
    char val_str[32];
    if (!json_get_str(json, "buttons", val_str, sizeof(val_str))) {
        send_err(id, "missing buttons");
        return;
    }
    int mask = parse_button_mask(val_str);
    if (mask < 0) { send_err(id, "bad buttons"); return; }
    s_input_override = (s_input_override < 0 ? 0 : s_input_override) | mask;
    s_input_frames   = 0;
    reply_input(id, "hold");
}

static void handle_release(int id, const char *json)
{
    char val_str[32];
    if (!json_get_str(json, "buttons", val_str, sizeof(val_str))) {
        send_err(id, "missing buttons");
        return;
    }
    int mask = parse_button_mask(val_str);
    if (mask < 0) { send_err(id, "bad buttons"); return; }
    if (s_input_override >= 0) {
        s_input_override &= ~mask;
        /* Releasing every held button hands the joypad back to the user
         * rather than pinning it at "nothing pressed" forever. */
        if (s_input_override == 0) s_input_override = -1;
    }
    s_input_frames = 0;
    reply_input(id, "release");
}

/* ---- Save states ----
 *
 * Both commands take either {"path":"..."} or {"slot":N}. A slot resolves
 * through gb_platform_savestate_slot_path(), i.e. the exact file the in-game
 * F5/F8 keys use ("<save_id>.state<N+1>" beside the executable), so a state
 * the user saved by hand loads over TCP and vice versa.
 *
 * Frame boundary: every command is dispatched from gb_debug_server_poll(),
 * which the platform calls from gb_platform_poll_events() once per guest
 * frame, between frames -- never from inside a recompiled body. State swaps
 * therefore land at the same point the F5/F8 keys take effect. */
static int resolve_state_path(const char *json, char *out, int out_sz,
                              const char **err)
{
    if (json_get_str(json, "path", out, out_sz) && out[0]) return 1;

    char slot_str[32];
    if (json_get_str(json, "slot", slot_str, sizeof(slot_str)) && slot_str[0]) {
        int slot = atoi(slot_str);
        if (!s_ctx) { *err = "no context"; return 0; }
        if (!gb_platform_savestate_slot_path(s_ctx, slot, out, (size_t)out_sz)) {
            *err = "slot path failed";
            return 0;
        }
        return 1;
    }
    *err = "missing path or slot";
    return 0;
}

static void handle_save_state(int id, const char *json)
{
    char path[512];
    const char *err = "bad request";
    if (!s_ctx) { send_err(id, "no context"); return; }
    if (!resolve_state_path(json, path, (int)sizeof(path), &err)) {
        send_err(id, err);
        return;
    }
    if (!gb_platform_save_state_path(s_ctx, path)) {
        send_err(id, "save failed");
        return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\",\"frame\":%llu}",
             id, path, (unsigned long long)s_frame_count);
}

static void handle_load_state(int id, const char *json)
{
    char path[512];
    const char *err = "bad request";
    if (!s_ctx) { send_err(id, "no context"); return; }
    if (!resolve_state_path(json, path, (int)sizeof(path), &err)) {
        send_err(id, err);
        return;
    }
    /* gb_platform_load_state_path() runs the same post-load hooks the in-game
     * F8 path runs: gb_ws_reapply() + gb_custom_reset() inside
     * gb_context_load_state_file(), then the audio-ring reset, the
     * guest-framebuffer cache invalidation and the present-counter resync.
     * Without the custom reset the compositor keeps deriving margins from the
     * abandoned timeline and the next frame composes wrong. */
    if (!gb_platform_load_state_path(s_ctx, path)) {
        send_err(id, "load failed");
        return;
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\",\"frame\":%llu}",
             id, path, (unsigned long long)s_frame_count);
}

static void handle_save_slot_path(int id, const char *json)
{
    if (!s_ctx) { send_err(id, "no context"); return; }
    int slot = json_get_int(json, "slot", 0);
    char path[512];
    if (!gb_platform_savestate_slot_path(s_ctx, slot, path, sizeof(path))) {
        send_err(id, "slot path failed");
        return;
    }
    FILE *f = fopen(path, "rb");
    int exists = f != NULL;
    if (f) fclose(f);
    send_fmt("{\"id\":%d,\"ok\":true,\"slot\":%d,\"path\":\"%s\",\"exists\":%s}",
             id, slot, path, exists ? "true" : "false");
}

/* ---- Screenshot ----
 *
 * Writes the PRESENTED frame: the composited custom/wide frame when a
 * compositor is armed, the native 160x144 framebuffer otherwise. The platform
 * publishes that frame from render_frame_internal(); if it has never presented
 * (no platform, or a headless run that has not reached its first present) we
 * compose one here through the same custom render hook so the reply is still
 * what the user would see. */
static uint32_t s_shot_buf[GB_CUSTOM_FRAME_SIZE];

static int compose_presented_fallback(const uint32_t **out, int *w, int *h)
{
    if (!s_ctx || !s_ctx->ppu) return 0;
    const uint32_t *native = ((GBPPU *)s_ctx->ppu)->rgb_framebuffer;
    const int native_w = gb_ws_render_width();
    *h = GB_SCREEN_HEIGHT;

    if (!gb_custom_render) {
        *out = native;
        *w = native_w;
        return 1;
    }

    int width = gb_custom_width > 0 ? gb_custom_width : native_w;
    if (width > GB_CUSTOM_MAX_WIDTH) width = GB_CUSTOM_MAX_WIDTH;
    if (!gb_custom_render(s_ctx, s_shot_buf, width, native)) {
        /* Compositor declined this frame -- pillarbox the native image the
         * same way the platform does, so the reply matches the screen. */
        for (int i = 0; i < width * GB_SCREEN_HEIGHT; i++) s_shot_buf[i] = 0xFF000000u;
        for (int y = 0; y < GB_SCREEN_HEIGHT; y++)
            memcpy(s_shot_buf + (size_t)y * width + (width - native_w) / 2,
                   native + (size_t)y * native_w,
                   (size_t)native_w * sizeof(uint32_t));
    }
    *out = s_shot_buf;
    *w = width;
    return 1;
}

static int path_ends_with_png(const char *path)
{
    size_t n = strlen(path);
    if (n < 4) return 0;
    const char *e = path + n - 4;
    return (e[0] == '.' &&
            (e[1] == 'p' || e[1] == 'P') &&
            (e[2] == 'n' || e[2] == 'N') &&
            (e[3] == 'g' || e[3] == 'G'));
}

static int write_ppm(const char *path, const uint32_t *fb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    uint8_t *row = (uint8_t *)malloc((size_t)w * 3);
    if (!row) { fclose(f); return 0; }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = fb[(size_t)y * w + x];   /* 0xAARRGGBB */
            row[x * 3 + 0] = (uint8_t)(p >> 16);
            row[x * 3 + 1] = (uint8_t)(p >> 8);
            row[x * 3 + 2] = (uint8_t)(p);
        }
        fwrite(row, 1, (size_t)w * 3, f);
    }
    free(row);
    fclose(f);
    return 1;
}

static int write_png(const char *path, const uint32_t *fb, int w, int h)
{
    /* stb wants tightly packed RGB(A) bytes; the framebuffer is host-order
     * 0xAARRGGBB, so repack to RGB triples (alpha is always opaque here). */
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * 3);
    if (!rgb) return 0;
    for (int i = 0; i < w * h; i++) {
        uint32_t p = fb[i];
        rgb[i * 3 + 0] = (uint8_t)(p >> 16);
        rgb[i * 3 + 1] = (uint8_t)(p >> 8);
        rgb[i * 3 + 2] = (uint8_t)(p);
    }
    int ok = stbi_write_png(path, w, h, 3, rgb, w * 3);
    free(rgb);
    return ok;
}

static void handle_screenshot(int id, const char *json)
{
    char path[512];
    if (!json_get_str(json, "path", path, sizeof(path)) || !path[0]) {
        snprintf(path, sizeof(path), "gb_shot_%05llu.ppm",
                 (unsigned long long)s_frame_count);
    }

    const uint32_t *fb = NULL;
    int w = 0, h = 0;
    /* {"recompose":1} forces a fresh pass through the custom render hook
     * instead of reusing the last presented frame. The presented frame is the
     * honest answer to "what is on screen", so it is the default; recompose
     * exists for callers that want the compositor re-run against the current
     * VRAM/OAM (the old sml2_capture semantics). */
    int recompose = json_get_int(json, "recompose", 0);
    int source_is_present = 0;
    if (!recompose) {
        source_is_present = gb_platform_get_presented_frame(&fb, &w, &h) ? 1 : 0;
    }
    if (!source_is_present && !compose_presented_fallback(&fb, &w, &h)) {
        send_err(id, "no frame available");
        return;
    }
    if (!fb || w <= 0 || h <= 0) { send_err(id, "no frame available"); return; }

    int ok = path_ends_with_png(path) ? write_png(path, fb, w, h)
                                      : write_ppm(path, fb, w, h);
    if (!ok) { send_err(id, "write failed"); return; }

    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\",\"width\":%d,\"height\":%d,"
             "\"frame\":%llu,\"source\":\"%s\"}",
             id, path, w, h, (unsigned long long)s_frame_count,
             source_is_present ? "presented" : "composed");
}

static void handle_pause(int id, const char *json)
{
    (void)json;
    s_paused = 1;
    send_fmt("{\"id\":%d,\"ok\":true,\"paused\":true,\"frame\":%llu}",
             id, (unsigned long long)s_frame_count);
}

static void handle_continue(int id, const char *json)
{
    (void)json;
    s_paused = 0;
    s_step_count = 0;
    s_run_to = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"paused\":false}", id);
}

static void handle_step(int id, const char *json)
{
    int n = json_get_int(json, "count", 1);
    if (n < 1) n = 1;
    s_step_count = n;
    s_paused = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"stepping\":%d}", id, n);
}

static void handle_run_to_frame(int id, const char *json)
{
    int target = json_get_int(json, "frame", 0);
    if (target <= (int)s_frame_count) {
        send_err(id, "target frame already passed");
        return;
    }
    s_run_to = (uint32_t)target;
    s_paused = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"running_to\":%d}", id, target);
}

/* Always-on interpreter-fallback ring: totals plus the top entry sites, kept
 * by gbrt_note_dispatch_fallback / gbrt_note_interpreter_session on every run
 * (Release included). Queried here rather than streamed, so a late-joining
 * client still sees every site recorded since boot. */
static void handle_interp_fallbacks(int id, const char *json)
{
    (void)json;
    if (!s_ctx) { send_err(id, "no context"); return; }

    char sites[4096];
    int off = 0;
    sites[0] = '\0';
    for (int i = 0; i < GBRT_INTERPRETER_HOTSPOT_CAPACITY; i++) {
        const GBInterpreterHotspot *h = &s_ctx->interpreter_hotspots[i];
        if (h->entries == 0) continue;
        int n = snprintf(sites + off, sizeof(sites) - (size_t)off,
                         "%s{\"bank\":%u,\"addr\":\"0x%04X\",\"entries\":%llu,"
                         "\"instructions\":%llu,\"cycles\":%llu,\"last_frame\":%llu}",
                         off ? "," : "",
                         (unsigned)h->bank, (unsigned)h->addr,
                         (unsigned long long)h->entries,
                         (unsigned long long)h->instructions,
                         (unsigned long long)h->cycles,
                         (unsigned long long)h->last_frame);
        if (n <= 0 || (size_t)n >= sizeof(sites) - (size_t)off) break;
        off += n;
    }

    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"total_fallbacks\":%llu,\"total_entries\":%llu,"
             "\"total_instructions\":%llu,\"total_cycles\":%llu,"
             "\"frame_fallbacks\":%u,"
             "\"frame_first\":\"%03X:%04X\",\"frame_last\":\"%03X:%04X\","
             "\"unimplemented_opcode\":%u,"
             "\"sites\":[%s]}",
             id,
             (unsigned long long)s_ctx->total_dispatch_fallbacks,
             (unsigned long long)s_ctx->total_interpreter_entries,
             (unsigned long long)s_ctx->total_interpreter_instructions,
             (unsigned long long)s_ctx->total_interpreter_cycles,
             s_ctx->frame_dispatch_fallbacks,
             (unsigned)s_ctx->frame_first_fallback_bank, s_ctx->frame_first_fallback_addr,
             (unsigned)s_ctx->frame_last_fallback_bank, s_ctx->frame_last_fallback_addr,
             (unsigned)(s_ctx->has_unimplemented_interpreter_opcode
                        ? s_ctx->last_unimplemented_opcode : 0),
             sites);
}

/* ---- Ring buffer queries ---- */

static void handle_history(int id, const char *json)
{
    (void)json;
    uint64_t oldest = (s_history_count > GB_FRAME_HISTORY_CAP)
                    ? s_history_count - GB_FRAME_HISTORY_CAP : 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"count\":%llu,\"oldest\":%llu,\"newest\":%llu}",
             id,
             (unsigned long long)s_history_count,
             (unsigned long long)oldest,
             (unsigned long long)(s_history_count > 0 ? s_history_count - 1 : 0));
}

static void handle_get_frame(int id, const char *json)
{
    int f = json_get_int(json, "frame", -1);
    if (f < 0) { send_err(id, "missing frame"); return; }

    uint64_t oldest = (s_history_count > GB_FRAME_HISTORY_CAP)
                    ? s_history_count - GB_FRAME_HISTORY_CAP : 0;
    if ((uint64_t)f < oldest || (uint64_t)f >= s_history_count) {
        send_err(id, "frame not in buffer");
        return;
    }

    uint32_t idx = (uint32_t)f % GB_FRAME_HISTORY_CAP;
    const GBFrameRecord *r = &s_frame_history[idx];
    if (r->frame_number != (uint32_t)f) {
        send_err(id, "frame record mismatch");
        return;
    }

    char gd_hex[33];
    for (int i = 0; i < 16; i++)
        snprintf(gd_hex + i * 2, 3, "%02x", r->game_data[i]);

    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"frame\":%u,"
             "\"cpu\":{\"A\":\"0x%02X\",\"F\":\"0x%02X\","
             "\"B\":\"0x%02X\",\"C\":\"0x%02X\","
             "\"D\":\"0x%02X\",\"E\":\"0x%02X\","
             "\"H\":\"0x%02X\",\"L\":\"0x%02X\","
             "\"SP\":\"0x%04X\",\"PC\":\"0x%04X\",\"IME\":%d},"
             "\"ppu\":{\"LCDC\":\"0x%02X\",\"STAT\":\"0x%02X\","
             "\"SCY\":%d,\"SCX\":%d,\"LY\":%d,\"WY\":%d,\"WX\":%d},"
             "\"rom_bank\":%d,\"ram_bank\":%d,"
             "\"joypad\":\"0x%02X\",\"cycles\":%u,"
             "\"game_data\":\"%s\","
             "\"last_func\":\"%s\"}",
             id, r->frame_number,
             r->cpu_a, r->cpu_f, r->cpu_b, r->cpu_c,
             r->cpu_d, r->cpu_e, r->cpu_h, r->cpu_l,
             r->cpu_sp, r->cpu_pc, r->cpu_ime,
             r->lcdc, r->stat, r->scy, r->scx, r->ly, r->wy, r->wx,
             r->rom_bank, r->ram_bank,
             r->joypad, r->cycles,
             gd_hex,
             r->last_func);
}

static void handle_frame_range(int id, const char *json)
{
    int start = json_get_int(json, "start", -1);
    int end   = json_get_int(json, "end", -1);
    if (start < 0 || end < 0) { send_err(id, "missing start/end"); return; }
    if (end - start + 1 > 200) { send_err(id, "max 200 frames per request"); return; }

    uint64_t oldest = (s_history_count > GB_FRAME_HISTORY_CAP)
                    ? s_history_count - GB_FRAME_HISTORY_CAP : 0;

    char *buf = (char *)malloc(200 * 256 + 256);
    if (!buf) { send_err(id, "alloc failed"); return; }

    int pos = snprintf(buf, 64, "{\"id\":%d,\"ok\":true,\"frames\":[", id);
    int first = 1;

    for (int f = start; f <= end; f++) {
        if (!first) buf[pos++] = ',';
        first = 0;

        if ((uint64_t)f < oldest || (uint64_t)f >= s_history_count) {
            pos += snprintf(buf + pos, 128, "{\"frame\":%d,\"available\":false}", f);
            continue;
        }

        uint32_t idx = (uint32_t)f % GB_FRAME_HISTORY_CAP;
        const GBFrameRecord *r = &s_frame_history[idx];
        if (r->frame_number != (uint32_t)f) {
            pos += snprintf(buf + pos, 128, "{\"frame\":%d,\"available\":false}", f);
            continue;
        }

        char gd_hex[33];
        for (int i = 0; i < 16; i++)
            snprintf(gd_hex + i * 2, 3, "%02x", r->game_data[i]);

        pos += snprintf(buf + pos, 256,
            "{\"frame\":%u,\"bank\":%d,\"joy\":\"0x%02X\","
            "\"game_data\":\"%s\"}",
            r->frame_number, r->rom_bank, r->joypad, gd_hex);
    }

    pos += snprintf(buf + pos, 8, "]}");
    send_line(buf);
    free(buf);
}

static void handle_frame_timeseries(int id, const char *json)
{
    int start = json_get_int(json, "start", -1);
    int end   = json_get_int(json, "end", -1);
    if (start < 0 || end < 0) { send_err(id, "missing start/end"); return; }
    if (end - start + 1 > 200) { send_err(id, "max 200 frames per request"); return; }

    uint64_t oldest = (s_history_count > GB_FRAME_HISTORY_CAP)
                    ? s_history_count - GB_FRAME_HISTORY_CAP : 0;

    char *buf = (char *)malloc(200 * 320 + 256);
    if (!buf) { send_err(id, "alloc failed"); return; }

    int pos = snprintf(buf, 64, "{\"id\":%d,\"ok\":true,\"ts\":[", id);
    int first = 1;

    for (int f = start; f <= end; f++) {
        if (!first) buf[pos++] = ',';
        first = 0;

        if ((uint64_t)f < oldest || (uint64_t)f >= s_history_count) {
            pos += snprintf(buf + pos, 32, "null");
            continue;
        }

        uint32_t idx = (uint32_t)f % GB_FRAME_HISTORY_CAP;
        const GBFrameRecord *r = &s_frame_history[idx];
        if (r->frame_number != (uint32_t)f) {
            pos += snprintf(buf + pos, 32, "null");
            continue;
        }

        char gd_hex[33];
        for (int i = 0; i < 16; i++)
            snprintf(gd_hex + i * 2, 3, "%02x", r->game_data[i]);

        pos += snprintf(buf + pos, 320,
            "{\"f\":%u,\"a\":%d,\"sp\":%d,\"pc\":%d,"
            "\"lcdc\":%d,\"ly\":%d,\"scx\":%d,\"scy\":%d,"
            "\"bk\":%d,\"joy\":%d,\"cyc\":%u,\"gd\":\"%s\"}",
            r->frame_number,
            r->cpu_a, r->cpu_sp, r->cpu_pc,
            r->lcdc, r->ly, r->scx, r->scy,
            r->rom_bank, r->joypad, r->cycles,
            gd_hex);
    }

    pos += snprintf(buf + pos, 8, "]}");
    send_line(buf);
    free(buf);
}

static void handle_quit(int id, const char *json)
{
    (void)json;
    send_ok(id);
    gb_debug_server_shutdown();
    exit(0);
}

/* ---- Delegation seam for game command handlers (see debug_server.h) ---- */

int gb_debug_server_save_state(int id, const char *json)
{
    handle_save_state(id, json ? json : "{}");
    return 1;
}

int gb_debug_server_load_state(int id, const char *json)
{
    handle_load_state(id, json ? json : "{}");
    return 1;
}

int gb_debug_server_screenshot(int id, const char *json)
{
    handle_screenshot(id, json ? json : "{}");
    return 1;
}

/* ---- Command dispatch ---- */

typedef void (*CmdHandler)(int id, const char *json);
typedef struct {
    const char *name;
    CmdHandler  handler;
} CmdEntry;

static const CmdEntry s_commands[] = {
    { "ping",              handle_ping },
    { "frame",             handle_frame },
    { "get_registers",     handle_get_registers },
    { "read_ram",          handle_read_ram },
    { "dump_ram",          handle_dump_ram },
    { "peek",              handle_peek },
    { "write_ram",         handle_write_ram },
    { "read_oam",          handle_read_oam },
    { "read_vram",         handle_read_vram },
    { "read_io",           handle_read_io },
    { "ppu_state",         handle_ppu_state },
    { "hw_state",          handle_hw_state },
    { "interp_fallbacks",  handle_interp_fallbacks },
    { "mapper_state",      handle_mapper_state },
    { "watch",             handle_watch },
    { "unwatch",           handle_unwatch },
    { "set_input",         handle_set_input },
    { "clear_input",       handle_clear_input },
    { "press",             handle_press },
    { "hold",              handle_hold },
    { "release",           handle_release },
    { "save_state",        handle_save_state },
    { "load_state",        handle_load_state },
    { "save_slot_path",    handle_save_slot_path },
    { "screenshot",        handle_screenshot },
    { "pause",             handle_pause },
    { "continue",          handle_continue },
    { "step",              handle_step },
    { "run_to_frame",      handle_run_to_frame },
    { "history",           handle_history },
    { "get_frame",         handle_get_frame },
    { "frame_range",       handle_frame_range },
    { "frame_timeseries",  handle_frame_timeseries },
    { "quit",              handle_quit },
    { NULL, NULL }
};

static void process_command(const char *line)
{
    char cmd[64];
    if (!json_get_str(line, "cmd", cmd, sizeof(cmd))) {
        strncpy(cmd, line, sizeof(cmd) - 1);
        cmd[sizeof(cmd) - 1] = '\0';
        int len = (int)strlen(cmd);
        while (len > 0 && (cmd[len-1] == '\r' || cmd[len-1] == ' '))
            cmd[--len] = '\0';
    }

    int id = json_get_int(line, "id", 0);

    for (const CmdEntry *e = s_commands; e->name; e++) {
        if (strcmp(cmd, e->name) == 0) {
            e->handler(id, line);
            return;
        }
    }

    /* Try game-specific command handler */
    if (game_handle_debug_cmd(cmd, id, line))
        return;

    send_err(id, "unknown command");
}

/* ---- Public API ---- */

void gb_debug_server_init(int port)
{
    const char *configured_port = getenv("GBRECOMP_DEBUG_PORT");
    if (port <= 0 && configured_port) {
        long value = strtol(configured_port, NULL, 10);
        if (value > 0 && value <= 65535) port = (int)value;
    }
    if (port > 0) s_port = port;

    gb_net_global_init();

    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen == SOCK_INVALID) {
        fprintf(stderr, "[debug] Failed to create socket\n");
        return;
    }

    int yes = 1;
    setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)s_port);

    if (bind(s_listen, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[debug] Failed to bind port %d\n", s_port);
        sock_close(s_listen);
        s_listen = SOCK_INVALID;
        return;
    }

    listen(s_listen, 1);
    set_nonblocking(s_listen);

    memset(s_frame_history, 0, sizeof(s_frame_history));
    s_history_count = 0;
    memset(s_watchpoints, 0, sizeof(s_watchpoints));

    fprintf(stderr, "[debug] TCP server listening on 127.0.0.1:%d\n", s_port);
}

void gb_debug_server_poll(void)
{
    if (s_listen == SOCK_INVALID) return;

    /* Accept new client if none connected */
    if (s_client == SOCK_INVALID) {
        struct sockaddr_in caddr;
#ifdef _WIN32
        int clen = sizeof(caddr);
#else
        socklen_t clen = sizeof(caddr);
#endif
        sock_t c = accept(s_listen, (struct sockaddr *)&caddr, &clen);
        if (c != SOCK_INVALID) {
            s_client = c;
            set_nonblocking(s_client);
            s_recv_len = 0;
            s_recv_buf[0] = '\0';
            send_queue_reset();
            fprintf(stderr, "[debug] Client connected\n");
        }
        return;
    }

    /* Drain anything queued since the last poll before reading commands, so a
     * client that alternates request/response never waits a frame for a reply
     * that is already formatted. */
    send_queue_flush();
    if (s_client == SOCK_INVALID) return;

    /* Receive data */
    int space = RECV_BUF_SIZE - s_recv_len - 1;
    if (space > 0) {
        int n = recv(s_client, s_recv_buf + s_recv_len, space, 0);
        if (n > 0) {
            s_recv_len += n;
            s_recv_buf[s_recv_len] = '\0';
        } else if (n == 0) {
            fprintf(stderr, "[debug] Client disconnected\n");
            sock_close(s_client);
            s_client = SOCK_INVALID;
            send_queue_reset();
            s_paused = 0;
            s_input_override = -1;
            s_input_frames = 0;
            return;
        } else {
            int err = sock_error();
#ifdef _WIN32
            if (err != WSAEWOULDBLOCK) {
#else
            if (err != EAGAIN && err != EWOULDBLOCK) {
#endif
                fprintf(stderr, "[debug] recv error %d, dropping client\n", err);
                sock_close(s_client);
                s_client = SOCK_INVALID;
                send_queue_reset();
                s_paused = 0;
                s_input_override = -1;
                s_input_frames = 0;
                return;
            }
        }
    }

    /* Process complete lines */
    char *nl;
    while ((nl = strchr(s_recv_buf, '\n')) != NULL) {
        *nl = '\0';
        if (nl > s_recv_buf && *(nl - 1) == '\r')
            *(nl - 1) = '\0';
        if (s_recv_buf[0] != '\0')
            process_command(s_recv_buf);
        int consumed = (int)(nl - s_recv_buf) + 1;
        s_recv_len -= consumed;
        memmove(s_recv_buf, nl + 1, s_recv_len + 1);
    }

    /* Push the responses this poll produced. */
    send_queue_flush();
}

void gb_debug_server_record_frame(void)
{
    uint32_t idx = (uint32_t)(s_frame_count % GB_FRAME_HISTORY_CAP);
    GBFrameRecord *r = &s_frame_history[idx];

    r->frame_number = (uint32_t)s_frame_count;

    if (s_ctx) {
        gb_pack_flags(s_ctx);
        r->cpu_a = s_ctx->a;
        r->cpu_f = s_ctx->f;
        r->cpu_b = s_ctx->b;
        r->cpu_c = s_ctx->c;
        r->cpu_d = s_ctx->d;
        r->cpu_e = s_ctx->e;
        r->cpu_h = s_ctx->h;
        r->cpu_l = s_ctx->l;
        r->cpu_sp = s_ctx->sp;
        r->cpu_pc = s_ctx->pc;
        r->cpu_ime = s_ctx->ime;

        /* PPU from I/O registers */
        if (s_ctx->io) {
            r->lcdc = s_ctx->io[0x40];
            r->stat = s_ctx->io[0x41];
            r->scy  = s_ctx->io[0x42];
            r->scx  = s_ctx->io[0x43];
            r->ly   = s_ctx->io[0x44];
            r->wy   = s_ctx->io[0x4A];
            r->wx   = s_ctx->io[0x4B];
        }

        r->rom_bank = s_ctx->rom_bank;
        r->ram_bank = s_ctx->ram_bank;
        r->joypad   = s_ctx->io ? s_ctx->io[0x00] : 0;
        r->cycles   = s_ctx->cycles;
    }

    /* Game-specific data */
    memset(r->game_data, 0, sizeof(r->game_data));
    game_fill_frame_record(r);

    /* Last function */
    strncpy(r->last_func, s_last_func, sizeof(r->last_func) - 1);
    r->last_func[sizeof(r->last_func) - 1] = '\0';

    s_history_count = s_frame_count + 1;
    s_frame_count++;

    /* Transient `press`: this frame consumed one of the requested frames.
     * Release on the last one so the button is down for exactly N frames. */
    if (s_input_frames > 0) {
        s_input_frames--;
        if (s_input_frames == 0) s_input_override = -1;
    }

    /* Step mode: count down and re-pause */
    if (s_step_count > 0) {
        s_step_count--;
        if (s_step_count == 0) {
            s_paused = 1;
            send_fmt("{\"event\":\"step_done\",\"frame\":%llu}",
                     (unsigned long long)s_frame_count);
        }
    }

    /* Run-to-frame */
    if (s_run_to > 0 && s_frame_count >= s_run_to) {
        s_paused = 1;
        s_run_to = 0;
        send_fmt("{\"event\":\"run_to_done\",\"frame\":%llu}",
                 (unsigned long long)s_frame_count);
    }
}

void gb_debug_server_wait_if_paused(void)
{
    while (s_paused) {
        gb_debug_server_poll();

#ifdef GB_HAS_SDL2
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) exit(0);
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) exit(0);
        }
        SDL_Delay(5);
#else
        /* No SDL — just a short sleep */
#ifdef _WIN32
        Sleep(5);
#else
        usleep(5000);
#endif
#endif
    }
}

void gb_debug_server_check_watchpoints(void)
{
    if (s_client == SOCK_INVALID) return;

    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        if (!s_watchpoints[i].active) continue;
        uint8_t cur = read_byte(s_watchpoints[i].addr);
        if (cur != s_watchpoints[i].prev_val) {
            send_fmt("{\"event\":\"watchpoint\","
                     "\"addr\":\"0x%04X\",\"old\":\"0x%02X\",\"new\":\"0x%02X\","
                     "\"frame\":%llu}",
                     s_watchpoints[i].addr,
                     s_watchpoints[i].prev_val, cur,
                     (unsigned long long)s_frame_count);
            s_watchpoints[i].prev_val = cur;
        }
    }
}

void gb_debug_server_shutdown(void)
{
    if (s_client != SOCK_INVALID) {
        /* Best-effort drain so the reply to `quit` actually reaches the
         * client. Bounded: a wedged reader must not wedge the exit path. */
        for (int spin = 0; spin < 200 && s_client != SOCK_INVALID
                        && s_send_head != s_send_tail; spin++) {
            send_queue_flush();
            if (s_send_head == s_send_tail) break;
#ifdef _WIN32
            Sleep(1);
#else
            usleep(1000);
#endif
        }
    }
    if (s_client != SOCK_INVALID) {
        sock_close(s_client);
        s_client = SOCK_INVALID;
        send_queue_reset();
    }
    if (s_listen != SOCK_INVALID) {
        sock_close(s_listen);
        s_listen = SOCK_INVALID;
    }
#ifdef _WIN32
    WSACleanup();
#endif
    fprintf(stderr, "[debug] Server shut down\n");
}

int gb_debug_server_is_connected(void)
{
    return s_client != SOCK_INVALID;
}

int gb_debug_server_get_input_override(void)
{
    return s_input_override;
}
