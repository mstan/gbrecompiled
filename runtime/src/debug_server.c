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

/* Find the VALUE of `key` in a flat JSON object.
 *
 * A plain strstr for "key" also matches the key name appearing as somebody
 * else's string value -- {"type":"key","key":"Right"} used to resolve
 * `key` against the `type` value and hand back an empty string. So only accept
 * a match that is followed by a colon and preceded by an object/member start.
 * Escapes are still not processed (see docs/DEBUG_SERVER.md). */
static const char *json_get_str(const char *json, const char *key,
                                 char *out, int out_sz)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const size_t plen = strlen(pattern);
    const char *p = json;
    for (;;) {
        p = strstr(p, pattern);
        if (!p) return NULL;
        /* Preceded by the start of a member: { or , (whitespace allowed). */
        const char *before = p;
        while (before > json && (before[-1] == ' ' || before[-1] == '\t' ||
                                 before[-1] == '\n' || before[-1] == '\r'))
            --before;
        const int member_start = (before == json) ||
                                 (before[-1] == '{') || (before[-1] == ',');
        /* Followed by a colon: it is a key, not a value. */
        const char *after = p + plen;
        while (*after == ' ' || *after == '\t') ++after;
        if (member_start && *after == ':') { p = after; break; }
        p += plen;
    }
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

/* Escape a string for embedding in a JSON reply. Paths are the reason this
 * exists: sdl_get_persistent_path() hands back native Windows paths, and an
 * unescaped "C:\Users\..." makes the whole reply line invalid JSON -- the
 * client's parser then fails on a reply that reports success. Handles the two
 * characters that must be escaped plus control bytes; the rest passes through
 * (UTF-8 is legal raw in JSON strings). Truncates rather than overflowing. */
static void json_escape(const char *in, char *out, int out_sz)
{
    int o = 0;
    if (out_sz <= 0) return;
    for (const unsigned char *p = (const unsigned char *)in; *p && o < out_sz - 1; p++) {
        if (*p == '"' || *p == '\\') {
            if (o + 2 > out_sz - 1) break;
            out[o++] = '\\';
            out[o++] = (char)*p;
        } else if (*p < 0x20) {
            if (o + 6 > out_sz - 1) break;
            o += snprintf(out + o, out_sz - o, "\\u%04x", *p);
        } else {
            out[o++] = (char)*p;
        }
    }
    out[o] = '\0';
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
    char esc[1100];
    json_escape(path, esc, sizeof(esc));
    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\",\"frame\":%llu}",
             id, esc, (unsigned long long)s_frame_count);
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
    char esc[1100];
    json_escape(path, esc, sizeof(esc));
    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\",\"frame\":%llu}",
             id, esc, (unsigned long long)s_frame_count);
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
    char esc[1100];
    json_escape(path, esc, sizeof(esc));
    send_fmt("{\"id\":%d,\"ok\":true,\"slot\":%d,\"path\":\"%s\",\"exists\":%s}",
             id, slot, esc, exists ? "true" : "false");
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

    char esc[1100];
    json_escape(path, esc, sizeof(esc));
    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\",\"width\":%d,\"height\":%d,"
             "\"frame\":%llu,\"source\":\"%s\"}",
             id, esc, w, h, (unsigned long long)s_frame_count,
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

/* ---- Synthetic SDL events (sdl_event / key / mouse) --------------------
 *
 * These build a real SDL_Event and hand it to gb_platform_inject_sdl_event(),
 * which pushes it into the very queue gb_platform_poll_events() drains. The
 * binding-capture code, the binding-resolution tables, the runtime-UI hook and
 * the joypad mapping therefore see exactly what a physical key produces --
 * unlike `press`/`set_input`, which override the resolved joypad mask and would
 * pass even with the binding layer broken.
 *
 * No window focus is needed, in a normal window or under SDL_VIDEODRIVER=dummy
 * (GBRECOMP_HEADLESS=1), because nothing on that path reads the OS foreground
 * window or SDL_GetKeyboardState(). */

#ifdef GB_HAS_SDL2

/* "left"/"1" .. "x2"/"5"; 0 means "not a button name". */
static int sdl_event_button_code(const char *name)
{
    if (!name || !name[0]) return SDL_BUTTON_LEFT;
    if (!strcmp(name, "left")   || !strcmp(name, "1")) return SDL_BUTTON_LEFT;
    if (!strcmp(name, "middle") || !strcmp(name, "2")) return SDL_BUTTON_MIDDLE;
    if (!strcmp(name, "right")  || !strcmp(name, "3")) return SDL_BUTTON_RIGHT;
    if (!strcmp(name, "x1")     || !strcmp(name, "4")) return SDL_BUTTON_X1;
    if (!strcmp(name, "x2")     || !strcmp(name, "5")) return SDL_BUTTON_X2;
    return 0;
}

/* Resolve the scancode from either {"scancode":N} or {"key":"D"} (any name
 * SDL_GetScancodeName() prints). Returns SDL_SCANCODE_UNKNOWN when neither
 * is usable. */
static SDL_Scancode sdl_event_scancode(const char *json)
{
    const int code = json_get_int(json, "scancode", -1);
    if (code > SDL_SCANCODE_UNKNOWN && code < SDL_NUM_SCANCODES)
        return (SDL_Scancode)code;
    char name[64];
    if (json_get_str(json, "key", name, sizeof(name)) && name[0])
        return SDL_GetScancodeFromName(name);
    return SDL_SCANCODE_UNKNOWN;
}

/* A synthetic pointer move. `warp` (default 0) also drags the HOST cursor via
 * SDL_WarpMouseInWindow -- off by default precisely so a probe never disturbs
 * the machine it runs on. UIs that read the event (ImGui, Clay) need no warp;
 * only a UI that polls SDL_GetMouseState() does. */
static int sdl_event_push_motion(const char *json, int x, int y)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_MOUSEMOTION;
    ev.motion.x = x;
    ev.motion.y = y;
    ev.motion.state = (Uint32)json_get_int(json, "buttons_held", 0);
    if (!json_get_int(json, "warp", 0)) {
        /* Tell the platform layer not to warp: a non-zero `which` marks this as
         * a synthetic device, which gb_platform_inject_sdl_event() honours. */
        ev.motion.which = GB_PLATFORM_SYNTHETIC_MOUSE_ID;
    }
    return gb_platform_inject_sdl_event(&ev);
}

static void sdl_event_common(int id, const char *json, const char *default_type)
{
    char type[32];
    if (!json_get_str(json, "type", type, sizeof(type)) || !type[0])
        snprintf(type, sizeof(type), "%s", default_type ? default_type : "key");

    /* `mouse` with no explicit type: a button event when one is named,
     * otherwise a bare pointer move. */
    if (!strcmp(type, "mouse")) {
        char btn[16];
        snprintf(type, sizeof(type), "%s",
                 json_get_str(json, "button", btn, sizeof(btn)) ? "mouse_button"
                                                                : "mouse_move");
    }

    int pushed = 0;

    if (!strcmp(type, "key")) {
        const SDL_Scancode sc = sdl_event_scancode(json);
        if (sc <= SDL_SCANCODE_UNKNOWN || sc >= SDL_NUM_SCANCODES) {
            send_err(id, "sdl_event: need scancode:N or a valid key name");
            return;
        }
        const int down   = json_get_int(json, "down", 1);
        const int repeat = json_get_int(json, "repeat", 0);
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        ev.key.repeat = (Uint8)(down ? (repeat ? 1 : 0) : 0);
        ev.key.keysym.scancode = sc;
        ev.key.keysym.sym = SDL_GetKeyFromScancode(sc);
        ev.key.keysym.mod = (Uint16)json_get_int(json, "mod", 0);
        pushed = gb_platform_inject_sdl_event(&ev);
        const char *label = SDL_GetScancodeName(sc);
        send_fmt("{\"id\":%d,\"ok\":%s,\"type\":\"key\",\"scancode\":%d,"
                 "\"key\":\"%s\",\"down\":%d,\"repeat\":%d,\"pushed\":%d,"
                 "\"frame\":%llu}",
                 id, pushed ? "true" : "false", (int)sc,
                 (label && label[0]) ? label : "", down ? 1 : 0,
                 (down && repeat) ? 1 : 0, pushed,
                 (unsigned long long)s_frame_count);
        return;
    }

    if (!strcmp(type, "mouse_move")) {
        const int x = json_get_int(json, "x", 0);
        const int y = json_get_int(json, "y", 0);
        pushed = sdl_event_push_motion(json, x, y);
        send_fmt("{\"id\":%d,\"ok\":%s,\"type\":\"mouse_move\",\"x\":%d,"
                 "\"y\":%d,\"pushed\":%d,\"frame\":%llu}",
                 id, pushed ? "true" : "false", x, y, pushed,
                 (unsigned long long)s_frame_count);
        return;
    }

    if (!strcmp(type, "mouse_button")) {
        char btn[16];
        btn[0] = '\0';
        json_get_str(json, "button", btn, sizeof(btn));
        const int button = sdl_event_button_code(btn);
        if (button == 0) {
            send_err(id, "sdl_event: button must be left/middle/right/x1/x2");
            return;
        }
        const int down   = json_get_int(json, "down", 1);
        const int clicks = json_get_int(json, "clicks", 1);
        const int x = json_get_int(json, "x", -1);
        const int y = json_get_int(json, "y", -1);
        /* A click at a named point is a move THEN a button, the order a real
         * pointer produces -- otherwise the UI resolves the hit against
         * wherever the pointer last was. */
        if (x >= 0 && y >= 0) pushed += sdl_event_push_motion(json, x, y);
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        ev.button.button = (Uint8)button;
        ev.button.clicks = (Uint8)(clicks > 0 ? clicks : 1);
        ev.button.x = (x >= 0) ? x : 0;
        ev.button.y = (y >= 0) ? y : 0;
        pushed += gb_platform_inject_sdl_event(&ev);
        send_fmt("{\"id\":%d,\"ok\":%s,\"type\":\"mouse_button\",\"button\":%d,"
                 "\"down\":%d,\"x\":%d,\"y\":%d,\"pushed\":%d,\"frame\":%llu}",
                 id, pushed ? "true" : "false", button, down ? 1 : 0, x, y,
                 pushed, (unsigned long long)s_frame_count);
        return;
    }

    if (!strcmp(type, "mouse_wheel")) {
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = SDL_MOUSEWHEEL;
        ev.wheel.x = json_get_int(json, "x", 0);
        ev.wheel.y = json_get_int(json, "y", 0);
        ev.wheel.preciseX = (float)ev.wheel.x;
        ev.wheel.preciseY = (float)ev.wheel.y;
        pushed = gb_platform_inject_sdl_event(&ev);
        send_fmt("{\"id\":%d,\"ok\":%s,\"type\":\"mouse_wheel\",\"x\":%d,"
                 "\"y\":%d,\"pushed\":%d,\"frame\":%llu}",
                 id, pushed ? "true" : "false", ev.wheel.x, ev.wheel.y, pushed,
                 (unsigned long long)s_frame_count);
        return;
    }

    if (!strcmp(type, "text")) {
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = SDL_TEXTINPUT;
        if (!json_get_str(json, "text", ev.text.text, sizeof(ev.text.text))) {
            send_err(id, "sdl_event: text requires a text argument");
            return;
        }
        pushed = gb_platform_inject_sdl_event(&ev);
        char esc[256];
        json_escape(ev.text.text, esc, sizeof(esc));
        send_fmt("{\"id\":%d,\"ok\":%s,\"type\":\"text\",\"text\":\"%s\","
                 "\"pushed\":%d,\"frame\":%llu}",
                 id, pushed ? "true" : "false", esc, pushed,
                 (unsigned long long)s_frame_count);
        return;
    }

    send_err(id, "sdl_event: type must be key/mouse_move/mouse_button/"
                 "mouse_wheel/text");
}

static void handle_sdl_event(int id, const char *json) { sdl_event_common(id, json, "key"); }
static void handle_key_event(int id, const char *json) { sdl_event_common(id, json, "key"); }
static void handle_mouse_event(int id, const char *json) { sdl_event_common(id, json, "mouse"); }

#else  /* !GB_HAS_SDL2 */

static void handle_sdl_event(int id, const char *json)
{
    (void)json;
    send_err(id, "sdl_event requires an SDL build");
}
static void handle_key_event(int id, const char *json)   { handle_sdl_event(id, json); }
static void handle_mouse_event(int id, const char *json) { handle_sdl_event(id, json); }

#endif /* GB_HAS_SDL2 */

/* ---- Pre-boot listener (the recomp-ui launcher phase) ------------------
 *
 * The launcher runs its own SDL loop BEFORE the game exists, so the ordinary
 * once-per-guest-frame pump never runs and a probe has nothing to talk to. This
 * serves the same protocol from a small thread for the duration of the launcher
 * and then hands the listening socket -- and any connected client -- to
 * gb_debug_server_init(), so one TCP session spans launcher and game.
 *
 * Only the commands that mean anything without a GBContext are accepted while
 * it is up; everything else answers "not available until the game boots".
 *
 * Off unless GBRECOMP_DEBUG_PORT names a port: a normal player run must never
 * open a socket before the game does.
 *
 * Thread safety: the main thread is inside the launcher's own loop and does not
 * touch any server state until gb_debug_server_preboot_end() has joined this
 * thread, so the server's buffers are owned by exactly one thread at a time.
 * The only cross-thread call is SDL_PushEvent(), which SDL documents as safe
 * from any thread and which wakes the launcher's SDL_WaitEventTimeout(). */

/* Defined with the rest of the socket setup, below. */
static int server_bind(int port);

static int s_preboot_mode = 0;      /* restrict the command set while up */
#ifdef GB_HAS_SDL2
static SDL_Thread *s_preboot_thread = NULL;
static volatile int s_preboot_stop = 0;
#endif

static int preboot_command_allowed(const char *cmd)
{
    static const char *const allowed[] = {
        "ping", "help", "frame", "sdl_event", "key", "mouse", "quit", NULL
    };
    for (int i = 0; allowed[i]; i++)
        if (strcmp(cmd, allowed[i]) == 0) return 1;
    return 0;
}

#ifdef GB_HAS_SDL2
static int SDLCALL preboot_thread_main(void *unused)
{
    (void)unused;
    while (!s_preboot_stop) {
        gb_debug_server_poll();
        SDL_Delay(2);
    }
    return 0;
}
#endif

void gb_debug_server_preboot_begin(void)
{
    const char *configured_port = getenv("GBRECOMP_DEBUG_PORT");
    if (!configured_port || !configured_port[0]) return;   /* opt-in only */
    if (s_listen != SOCK_INVALID) return;                  /* already up */

    long value = strtol(configured_port, NULL, 10);
    if (value <= 0 || value > 65535) return;
    s_port = (int)value;

    if (!server_bind(s_port)) return;
    s_preboot_mode = 1;

#ifdef GB_HAS_SDL2
    s_preboot_stop = 0;
    s_preboot_thread = SDL_CreateThread(preboot_thread_main, "gbdbg-preboot", NULL);
    if (!s_preboot_thread) {
        fprintf(stderr, "[debug] pre-boot pump thread failed: %s\n", SDL_GetError());
        s_preboot_mode = 0;
        return;
    }
#endif
    fprintf(stderr, "[debug] pre-boot listener on 127.0.0.1:%d (launcher phase)\n",
            s_port);
}

void gb_debug_server_preboot_end(void)
{
    if (!s_preboot_mode) return;
#ifdef GB_HAS_SDL2
    s_preboot_stop = 1;
    if (s_preboot_thread) {
        SDL_WaitThread(s_preboot_thread, NULL);
        s_preboot_thread = NULL;
    }
#endif
    s_preboot_mode = 0;
    fprintf(stderr, "[debug] pre-boot listener handed over to the game\n");
}

/* ---- Command dispatch ---- */

typedef void (*CmdHandler)(int id, const char *json);
typedef struct {
    const char *name;
    const char *summary;   /* one-line description, served by `help` */
    CmdHandler  handler;
} CmdEntry;

static void handle_help(int id, const char *json);

static const CmdEntry s_commands[] = {
    { "ping",              "connectivity check; returns the current frame",                              handle_ping },
    { "help",              "list every built-in command with a one-line summary",                        handle_help },
    { "frame",             "current frame number and last reported function",                            handle_frame },
    { "get_registers",     "SM83 register + flag state, bank and frame",                                 handle_get_registers },
    { "read_ram",          "read up to 256 bytes through gb_read8 (live banks, side effects)",           handle_read_ram },
    { "dump_ram",          "streamed hex dump through gb_read8, 256 bytes per line",                     handle_dump_ram },
    { "peek",              "read backing arrays directly with explicit ROM/ERAM/WRAM/VRAM banks",        handle_peek },
    { "write_ram",         "debug poke: one byte (val) or a run (hex)",                                  handle_write_ram },
    { "read_oam",          "one decoded sprite (index) or all 160 OAM bytes",                            handle_read_oam },
    { "read_vram",         "read 0x8000-0x9FFF from the current VRAM bank",                              handle_read_vram },
    { "read_io",           "read 0xFF00-0xFF7F I/O registers",                                           handle_read_io },
    { "ppu_state",         "LCDC/STAT/scroll/window/palette registers",                                  handle_ppu_state },
    { "hw_state",          "model, CGB mode, active body, MBC and CGB palette RAM",                      handle_hw_state },
    { "interp_fallbacks",  "always-on interpreter-fallback ring: totals and per-site counts",            handle_interp_fallbacks },
    { "mapper_state",      "current banks, MBC type, RAM enable and mode",                               handle_mapper_state },
    { "watch",             "watch a byte; changes arrive as watchpoint events (max 8)",                  handle_watch },
    { "unwatch",           "drop a watchpoint",                                                          handle_unwatch },
    { "set_input",         "set the held button mask absolutely (letters RLUDABST or hex)",              handle_set_input },
    { "clear_input",       "drop the override and hand the joypad back to the user",                     handle_clear_input },
    { "press",             "hold buttons for N guest frames (default 1), then auto-release",             handle_press },
    { "hold",              "add buttons to the held mask",                                               handle_hold },
    { "release",           "remove buttons from the held mask",                                          handle_release },
    { "save_state",        "save state to {path} or the runtime's own {slot:N} file",                    handle_save_state },
    { "load_state",        "load state from {path} or {slot:N}, with the in-game post-load hooks",       handle_load_state },
    { "save_slot_path",    "resolve the <save_id>.stateN file for a slot without touching it",           handle_save_slot_path },
    { "screenshot",        "write the presented frame; .png gives PNG, anything else PPM",               handle_screenshot },
    { "pause",             "pause at the next frame boundary",                                           handle_pause },
    { "continue",          "resume; also cancels a pending step / run_to_frame",                         handle_continue },
    { "step",              "run N frames then re-pause; emits step_done",                                handle_step },
    { "run_to_frame",      "resume and pause at an absolute frame; emits run_to_done",                   handle_run_to_frame },
    { "history",           "frame-ring window: count, oldest, newest",                                   handle_history },
    { "get_frame",         "one historical frame record (CPU + PPU + banks + game data)",                handle_get_frame },
    { "frame_range",       "a span of historical frames, max 200",                                       handle_frame_range },
    { "frame_timeseries",  "compact per-frame timeseries over a span, max 200",                          handle_frame_timeseries },
    { "sdl_event",         "synthesize an SDL event (key/mouse/text) into the real event queue",   handle_sdl_event },
    { "key",               "sdl_event shorthand: {scancode|key, down, repeat}",                    handle_key_event },
    { "mouse",             "sdl_event shorthand: {x,y} moves and {button,down} clicks",            handle_mouse_event },
    { "quit",              "reply, flush, and exit the runner",                                          handle_quit },
    { NULL, NULL, NULL }
};

/* Discoverability: an undocumented command is a command nobody uses. `help`
 * serves the same table the dispatcher uses, so it can never drift from what
 * the binary actually accepts. Full argument/reply detail is in
 * docs/DEBUG_SERVER.md. */
static void handle_help(int id, const char *json)
{
    (void)json;
    char *buf = (char *)malloc(16384);
    if (!buf) { send_err(id, "alloc failed"); return; }
    int pos = snprintf(buf, 16384, "{\"id\":%d,\"ok\":true,\"commands\":[", id);
    for (int i = 0; s_commands[i].name; i++) {
        pos += snprintf(buf + pos, 16384 - pos,
                        "%s{\"name\":\"%s\",\"summary\":\"%s\"}",
                        i ? "," : "", s_commands[i].name, s_commands[i].summary);
    }
    snprintf(buf + pos, 16384 - pos,
             "],\"docs\":\"gb-recompiled/docs/DEBUG_SERVER.md\",\"note\":"
             "\"unknown commands fall through to game_handle_debug_cmd()\"}");
    send_line(buf);
    free(buf);
}

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

    /* The launcher phase has no GBContext; refuse the rest by name rather
     * than let a handler read a NULL context. */
    if (s_preboot_mode && !preboot_command_allowed(cmd)) {
        send_err(id, "not available until the game boots (pre-boot launcher)");
        return;
    }

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

/* Bind + listen on `port`. Shared by the pre-boot listener and the in-game
 * server so both open the socket exactly the same way. Returns 1 on success. */
static int server_bind(int port)
{
    gb_net_global_init();

    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen == SOCK_INVALID) {
        fprintf(stderr, "[debug] Failed to create socket\n");
        return 0;
    }

    int yes = 1;
    setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    if (bind(s_listen, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[debug] Failed to bind port %d\n", port);
        sock_close(s_listen);
        s_listen = SOCK_INVALID;
        return 0;
    }

    listen(s_listen, 1);
    set_nonblocking(s_listen);
    return 1;
}

void gb_debug_server_init(int port)
{
    const char *configured_port = getenv("GBRECOMP_DEBUG_PORT");
    if (port <= 0 && configured_port) {
        long value = strtol(configured_port, NULL, 10);
        if (value > 0 && value <= 65535) port = (int)value;
    }
    if (port > 0) s_port = port;

    memset(s_frame_history, 0, sizeof(s_frame_history));
    s_history_count = 0;
    memset(s_watchpoints, 0, sizeof(s_watchpoints));

    /* The pre-boot listener may already own the port -- and a client with it.
     * Adopt both rather than rebind, so a probe that drove the launcher keeps
     * the same connection into the game. */
    if (s_listen != SOCK_INVALID) {
        fprintf(stderr, "[debug] TCP server adopted the pre-boot listener on "
                        "127.0.0.1:%d (client %s)\n",
                s_port, s_client != SOCK_INVALID ? "connected" : "none");
        return;
    }

    if (!server_bind(s_port)) return;

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
        /* Hand the queue to the platform instead of discarding it. The old
         * drain threw every event away, so a key injected with `sdl_event`
         * (or pressed by hand) while paused never reached the binding layer;
         * SDL_QUIT and Escape keep their meaning inside the pump. */
        gb_platform_pump_paused_events();
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
