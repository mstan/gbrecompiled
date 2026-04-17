/*
 * main.c -- Peanut-GB Debug Emulator
 *
 * Ground truth Game Boy emulator with TCP debug server for comparison
 * against the recompiled native build. Uses the same JSON-over-TCP
 * protocol on port 4371 (one port above the recomp runtime at 4370).
 *
 * Download peanut_gb.h from:
 *   https://raw.githubusercontent.com/deltabeard/Peanut-GB/master/peanut_gb.h
 * Place it in this directory.
 *
 * Usage:
 *   gb-debug-emu [--port N] [--headless] [--frames N] rom.gb
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

/* ---- Platform sockets ---- */
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define sock_close closesocket
   static int sock_error(void) { return WSAGetLastError(); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
   typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define sock_close close
   static int sock_error(void) { return errno; }
#endif

#include <SDL.h>

/* ---- Peanut-GB configuration ---- */
#define ENABLE_SOUND 0
#define PEANUT_GB_IS_LITTLE_ENDIAN 1

/* Include Peanut-GB (single header) */
#include "peanut_gb.h"

/* ---- ROM storage ---- */
static uint8_t *g_rom_data = NULL;
static size_t   g_rom_size = 0;
static uint8_t  g_cart_ram[0x8000]; /* 32KB max cart RAM */

/* ---- Peanut-GB callbacks ---- */
uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    if (addr < g_rom_size)
        return g_rom_data[addr];
    return 0xFF;
}

uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    if (addr < sizeof(g_cart_ram))
        return g_cart_ram[addr];
    return 0xFF;
}

void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val)
{
    (void)gb;
    if (addr < sizeof(g_cart_ram))
        g_cart_ram[addr] = val;
}

void gb_error(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t val)
{
    (void)gb;
    const char *err_str;
    switch (gb_err) {
    case GB_INVALID_OPCODE: err_str = "invalid opcode"; break;
    case GB_INVALID_READ:   err_str = "invalid read";   break;
    case GB_INVALID_WRITE:  err_str = "invalid write";  break;
    default:                err_str = "unknown";         break;
    }
    fprintf(stderr, "[emu] Error: %s at 0x%04X\n", err_str, val);
}

/* ---- Frame buffer ---- */
#define GB_LCD_WIDTH  160
#define GB_LCD_HEIGHT 144
static uint32_t g_framebuffer[GB_LCD_WIDTH * GB_LCD_HEIGHT];

/* DMG palette (green tones) */
static const uint32_t g_palette[4] = {
    0xFFE0F8D0, /* White */
    0xFF88C070, /* Light */
    0xFF346856, /* Dark */
    0xFF081820  /* Black */
};

void lcd_draw_line(struct gb_s *gb, const uint8_t pixels[GB_LCD_WIDTH],
                   const uint_fast8_t line)
{
    (void)gb;
    if (line >= GB_LCD_HEIGHT) return;
    for (int x = 0; x < GB_LCD_WIDTH; x++) {
        g_framebuffer[line * GB_LCD_WIDTH + x] = g_palette[pixels[x] & 3];
    }
}

/* ---- TCP Debug Server (minimal, matching protocol) ---- */

#define EMU_FRAME_HISTORY_CAP 36000
#define RECV_BUF_SIZE 8192

typedef struct {
    uint32_t frame_number;
    uint8_t  cpu_a, cpu_f;
    uint8_t  cpu_b, cpu_c;
    uint8_t  cpu_d, cpu_e;
    uint8_t  cpu_h, cpu_l;
    uint16_t cpu_sp, cpu_pc;
    uint8_t  lcdc, stat;
    uint8_t  scy, scx;
    uint8_t  ly;
    uint16_t rom_bank;
    uint8_t  joypad;
} EmuFrameRecord;

static sock_t emu_listen  = SOCK_INVALID;
static sock_t emu_client  = SOCK_INVALID;
static int    emu_port    = 4371;
static char   emu_recv_buf[RECV_BUF_SIZE];
static int    emu_recv_len = 0;
static volatile int emu_paused = 0;
static int    emu_step_count = 0;
static uint32_t emu_run_to = 0;
static uint64_t emu_frame_count = 0;
static int    emu_input_override = -1;

static EmuFrameRecord emu_frame_history[EMU_FRAME_HISTORY_CAP];
static uint64_t       emu_history_count = 0;

static struct gb_s *g_gb = NULL;  /* Set after init */

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

static void emu_send_line(const char *json)
{
    if (emu_client == SOCK_INVALID) return;
    int len = (int)strlen(json);
    send(emu_client, json, len, 0);
    send(emu_client, "\n", 1, 0);
}

static void emu_send_fmt(const char *fmt, ...)
{
    char buf[16384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emu_send_line(buf);
}

/* Minimal JSON parser */
static const char *emu_json_get_str(const char *json, const char *key,
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

static int emu_json_get_int(const char *json, const char *key, int def)
{
    char buf[64];
    if (!emu_json_get_str(json, key, buf, sizeof(buf))) return def;
    return atoi(buf);
}

static uint32_t emu_hex_to_u32(const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    return (uint32_t)strtoul(s, NULL, 16);
}

static void emu_process_command(const char *line)
{
    char cmd[64];
    if (!emu_json_get_str(line, "cmd", cmd, sizeof(cmd))) {
        strncpy(cmd, line, sizeof(cmd) - 1);
        cmd[sizeof(cmd) - 1] = '\0';
    }
    int id = emu_json_get_int(line, "id", 0);

    if (strcmp(cmd, "ping") == 0) {
        emu_send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%llu,\"source\":\"peanut-gb\"}",
                     id, (unsigned long long)emu_frame_count);
    }
    else if (strcmp(cmd, "get_registers") == 0) {
        if (!g_gb) { emu_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"no gb\"}", id); return; }
        emu_send_fmt("{\"id\":%d,\"ok\":true,"
                     "\"A\":\"0x%02X\",\"F\":\"0x%02X\","
                     "\"B\":\"0x%02X\",\"C_reg\":\"0x%02X\","
                     "\"D\":\"0x%02X\",\"E\":\"0x%02X\","
                     "\"H_reg\":\"0x%02X\",\"L\":\"0x%02X\","
                     "\"SP\":\"0x%04X\",\"PC\":\"0x%04X\","
                     "\"rom_bank\":%d,"
                     "\"frame\":%llu,\"source\":\"peanut-gb\"}",
                     id,
                     g_gb->cpu_reg.a, g_gb->cpu_reg.f.reg,
                     g_gb->cpu_reg.bc.bytes.b, g_gb->cpu_reg.bc.bytes.c,
                     g_gb->cpu_reg.de.bytes.d, g_gb->cpu_reg.de.bytes.e,
                     g_gb->cpu_reg.hl.bytes.h, g_gb->cpu_reg.hl.bytes.l,
                     g_gb->cpu_reg.sp.reg, g_gb->cpu_reg.pc.reg,
                     g_gb->selected_rom_bank,
                     (unsigned long long)emu_frame_count);
    }
    else if (strcmp(cmd, "read_ram") == 0) {
        char addr_str[32];
        if (!emu_json_get_str(line, "addr", addr_str, sizeof(addr_str))) {
            emu_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"missing addr\"}", id);
            return;
        }
        uint16_t addr = (uint16_t)emu_hex_to_u32(addr_str);
        int len = emu_json_get_int(line, "len", 1);
        if (len < 1) len = 1;
        if (len > 256) len = 256;
        char hex[513];
        for (int i = 0; i < len; i++)
            snprintf(hex + i * 2, 3, "%02x", __gb_read(g_gb, addr + i));
        emu_send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%04X\",\"len\":%d,\"hex\":\"%s\",\"source\":\"peanut-gb\"}",
                     id, addr, len, hex);
    }
    else if (strcmp(cmd, "ppu_state") == 0) {
        emu_send_fmt("{\"id\":%d,\"ok\":true,"
                     "\"LCDC\":\"0x%02X\",\"STAT\":\"0x%02X\","
                     "\"SCY\":%d,\"SCX\":%d,"
                     "\"LY\":%d,"
                     "\"source\":\"peanut-gb\"}",
                     id,
                     g_gb->hram_io[IO_LCDC], g_gb->hram_io[IO_STAT],
                     g_gb->hram_io[IO_SCY], g_gb->hram_io[IO_SCX],
                     g_gb->hram_io[IO_LY]);
    }
    else if (strcmp(cmd, "pause") == 0) {
        emu_paused = 1;
        emu_send_fmt("{\"id\":%d,\"ok\":true,\"paused\":true,\"frame\":%llu}",
                     id, (unsigned long long)emu_frame_count);
    }
    else if (strcmp(cmd, "continue") == 0) {
        emu_paused = 0;
        emu_step_count = 0;
        emu_run_to = 0;
        emu_send_fmt("{\"id\":%d,\"ok\":true,\"paused\":false}", id);
    }
    else if (strcmp(cmd, "step") == 0) {
        int n = emu_json_get_int(line, "count", 1);
        if (n < 1) n = 1;
        emu_step_count = n;
        emu_paused = 0;
        emu_send_fmt("{\"id\":%d,\"ok\":true,\"stepping\":%d}", id, n);
    }
    else if (strcmp(cmd, "history") == 0) {
        uint64_t oldest = (emu_history_count > EMU_FRAME_HISTORY_CAP)
                        ? emu_history_count - EMU_FRAME_HISTORY_CAP : 0;
        emu_send_fmt("{\"id\":%d,\"ok\":true,\"count\":%llu,\"oldest\":%llu,\"newest\":%llu}",
                     id,
                     (unsigned long long)emu_history_count,
                     (unsigned long long)oldest,
                     (unsigned long long)(emu_history_count > 0 ? emu_history_count - 1 : 0));
    }
    else if (strcmp(cmd, "get_frame") == 0) {
        int f = emu_json_get_int(line, "frame", -1);
        if (f < 0) { emu_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"missing frame\"}", id); return; }
        uint64_t oldest = (emu_history_count > EMU_FRAME_HISTORY_CAP)
                        ? emu_history_count - EMU_FRAME_HISTORY_CAP : 0;
        if ((uint64_t)f < oldest || (uint64_t)f >= emu_history_count) {
            emu_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"frame not in buffer\"}", id);
            return;
        }
        uint32_t idx = (uint32_t)f % EMU_FRAME_HISTORY_CAP;
        const EmuFrameRecord *r = &emu_frame_history[idx];
        emu_send_fmt("{\"id\":%d,\"ok\":true,"
                     "\"frame\":%u,"
                     "\"cpu\":{\"A\":\"0x%02X\",\"F\":\"0x%02X\","
                     "\"B\":\"0x%02X\",\"C\":\"0x%02X\","
                     "\"D\":\"0x%02X\",\"E\":\"0x%02X\","
                     "\"H\":\"0x%02X\",\"L\":\"0x%02X\","
                     "\"SP\":\"0x%04X\",\"PC\":\"0x%04X\"},"
                     "\"ppu\":{\"LCDC\":\"0x%02X\",\"STAT\":\"0x%02X\","
                     "\"SCY\":%d,\"SCX\":%d,\"LY\":%d},"
                     "\"rom_bank\":%d,"
                     "\"source\":\"peanut-gb\"}",
                     id, r->frame_number,
                     r->cpu_a, r->cpu_f, r->cpu_b, r->cpu_c,
                     r->cpu_d, r->cpu_e, r->cpu_h, r->cpu_l,
                     r->cpu_sp, r->cpu_pc,
                     r->lcdc, r->stat, r->scy, r->scx, r->ly,
                     r->rom_bank);
    }
    else if (strcmp(cmd, "set_input") == 0) {
        char val_str[32];
        if (!emu_json_get_str(line, "buttons", val_str, sizeof(val_str))) {
            emu_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"missing buttons\"}", id);
            return;
        }
        emu_input_override = (int)emu_hex_to_u32(val_str);
        emu_send_fmt("{\"id\":%d,\"ok\":true}", id);
    }
    else if (strcmp(cmd, "clear_input") == 0) {
        emu_input_override = -1;
        emu_send_fmt("{\"id\":%d,\"ok\":true}", id);
    }
    else if (strcmp(cmd, "frame_timeseries") == 0) {
        int start = emu_json_get_int(line, "start", -1);
        int end   = emu_json_get_int(line, "end", -1);
        if (start < 0 || end < 0) { emu_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"missing start/end\"}", id); return; }
        if (end - start + 1 > 200) { emu_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"max 200\"}", id); return; }
        uint64_t oldest = (emu_history_count > EMU_FRAME_HISTORY_CAP)
                        ? emu_history_count - EMU_FRAME_HISTORY_CAP : 0;
        char *buf = (char *)malloc(200 * 256 + 256);
        if (!buf) return;
        int pos = snprintf(buf, 64, "{\"id\":%d,\"ok\":true,\"ts\":[", id);
        int first = 1;
        for (int f = start; f <= end; f++) {
            if (!first) buf[pos++] = ',';
            first = 0;
            if ((uint64_t)f < oldest || (uint64_t)f >= emu_history_count) {
                pos += snprintf(buf + pos, 32, "null");
                continue;
            }
            uint32_t idx2 = (uint32_t)f % EMU_FRAME_HISTORY_CAP;
            const EmuFrameRecord *r = &emu_frame_history[idx2];
            pos += snprintf(buf + pos, 256,
                "{\"f\":%u,\"a\":%d,\"sp\":%d,\"pc\":%d,"
                "\"lcdc\":%d,\"ly\":%d,\"scx\":%d,\"scy\":%d,\"bk\":%d}",
                r->frame_number, r->cpu_a, r->cpu_sp, r->cpu_pc,
                r->lcdc, r->ly, r->scx, r->scy, r->rom_bank);
        }
        pos += snprintf(buf + pos, 8, "]}");
        emu_send_line(buf);
        free(buf);
    }
    else if (strcmp(cmd, "quit") == 0) {
        emu_send_fmt("{\"id\":%d,\"ok\":true}", id);
        exit(0);
    }
    else {
        emu_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"unknown command\"}", id);
    }
}

static void emu_server_init(int port)
{
    if (port > 0) emu_port = port;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    emu_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (emu_listen == SOCK_INVALID) return;
    int yes = 1;
    setsockopt(emu_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)emu_port);
    if (bind(emu_listen, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[emu-debug] Failed to bind port %d\n", emu_port);
        sock_close(emu_listen);
        emu_listen = SOCK_INVALID;
        return;
    }
    listen(emu_listen, 1);
    set_nonblocking(emu_listen);
    fprintf(stderr, "[emu-debug] TCP server listening on 127.0.0.1:%d\n", emu_port);
}

static void emu_server_poll(void)
{
    if (emu_listen == SOCK_INVALID) return;
    if (emu_client == SOCK_INVALID) {
        struct sockaddr_in caddr;
#ifdef _WIN32
        int clen = sizeof(caddr);
#else
        socklen_t clen = sizeof(caddr);
#endif
        sock_t c = accept(emu_listen, (struct sockaddr *)&caddr, &clen);
        if (c != SOCK_INVALID) {
            emu_client = c;
            set_nonblocking(emu_client);
            emu_recv_len = 0;
            fprintf(stderr, "[emu-debug] Client connected\n");
        }
        return;
    }
    int space = RECV_BUF_SIZE - emu_recv_len - 1;
    if (space > 0) {
        int n = recv(emu_client, emu_recv_buf + emu_recv_len, space, 0);
        if (n > 0) {
            emu_recv_len += n;
            emu_recv_buf[emu_recv_len] = '\0';
        } else if (n == 0) {
            sock_close(emu_client);
            emu_client = SOCK_INVALID;
            emu_paused = 0;
            return;
        }
    }
    char *nl;
    while ((nl = strchr(emu_recv_buf, '\n')) != NULL) {
        *nl = '\0';
        if (nl > emu_recv_buf && *(nl - 1) == '\r') *(nl - 1) = '\0';
        if (emu_recv_buf[0] != '\0') emu_process_command(emu_recv_buf);
        int consumed = (int)(nl - emu_recv_buf) + 1;
        emu_recv_len -= consumed;
        memmove(emu_recv_buf, nl + 1, emu_recv_len + 1);
    }
}

static void emu_record_frame(void)
{
    if (!g_gb) return;
    uint32_t idx = (uint32_t)(emu_frame_count % EMU_FRAME_HISTORY_CAP);
    EmuFrameRecord *r = &emu_frame_history[idx];
    r->frame_number = (uint32_t)emu_frame_count;
    r->cpu_a = g_gb->cpu_reg.a;
    r->cpu_f = g_gb->cpu_reg.f.reg;
    r->cpu_b = g_gb->cpu_reg.bc.bytes.b;
    r->cpu_c = g_gb->cpu_reg.bc.bytes.c;
    r->cpu_d = g_gb->cpu_reg.de.bytes.d;
    r->cpu_e = g_gb->cpu_reg.de.bytes.e;
    r->cpu_h = g_gb->cpu_reg.hl.bytes.h;
    r->cpu_l = g_gb->cpu_reg.hl.bytes.l;
    r->cpu_sp = g_gb->cpu_reg.sp.reg;
    r->cpu_pc = g_gb->cpu_reg.pc.reg;
    r->lcdc = g_gb->hram_io[IO_LCDC];
    r->stat = g_gb->hram_io[IO_STAT];
    r->scy  = g_gb->hram_io[IO_SCY];
    r->scx  = g_gb->hram_io[IO_SCX];
    r->ly   = g_gb->hram_io[IO_LY];
    r->rom_bank = (uint16_t)g_gb->selected_rom_bank;
    r->joypad = 0;
    emu_history_count = emu_frame_count + 1;
    emu_frame_count++;

    if (emu_step_count > 0) {
        emu_step_count--;
        if (emu_step_count == 0) {
            emu_paused = 1;
            emu_send_fmt("{\"event\":\"step_done\",\"frame\":%llu}",
                         (unsigned long long)emu_frame_count);
        }
    }
    if (emu_run_to > 0 && emu_frame_count >= emu_run_to) {
        emu_paused = 1;
        emu_run_to = 0;
    }
}

static void emu_wait_if_paused(void)
{
    while (emu_paused) {
        emu_server_poll();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) exit(0);
        }
        SDL_Delay(5);
    }
}

/* ---- Joypad state ---- */
static uint8_t g_joypad_bits = 0xFF;

/* ---- Main ---- */
int main(int argc, char *argv[])
{
    const char *rom_path = NULL;
    int port = 4371;
    int headless = 0;
    int max_frames = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--headless") == 0)
            headless = 1;
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            max_frames = atoi(argv[++i]);
        else
            rom_path = argv[i];
    }

    if (!rom_path) {
        fprintf(stderr, "Usage: gb-debug-emu [--port N] [--headless] [--frames N] rom.gb\n");
        return 1;
    }

    /* Load ROM */
    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open ROM: %s\n", rom_path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    g_rom_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_rom_data = (uint8_t *)malloc(g_rom_size);
    fread(g_rom_data, 1, g_rom_size, f);
    fclose(f);

    fprintf(stderr, "[emu] Loaded ROM: %s (%zu bytes)\n", rom_path, g_rom_size);

    /* Init Peanut-GB */
    struct gb_s gb;
    enum gb_init_error_e gb_ret = gb_init(&gb, &gb_rom_read, &gb_cart_ram_read,
                                           &gb_cart_ram_write, &gb_error, NULL);
    if (gb_ret != GB_INIT_NO_ERROR) {
        fprintf(stderr, "[emu] gb_init failed: %d\n", gb_ret);
        return 1;
    }
    g_gb = &gb;

    gb_init_lcd(&gb, &lcd_draw_line);

    /* Init SDL */
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "[emu] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    int scale = 3;

    if (!headless) {
        window = SDL_CreateWindow("GB Debug Emulator (Peanut-GB)",
                                  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  GB_LCD_WIDTH * scale, GB_LCD_HEIGHT * scale,
                                  SDL_WINDOW_SHOWN);
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING,
                                    GB_LCD_WIDTH, GB_LCD_HEIGHT);
    }

    /* Init TCP debug server */
    emu_server_init(port);

    /* Main loop */
    int running = 1;
    uint32_t frame_start;

    while (running) {
        frame_start = SDL_GetTicks();

        /* Poll TCP */
        emu_server_poll();
        emu_wait_if_paused();

        /* Handle SDL events */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = 0;
                break;
            }
            if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
                int pressed = (ev.type == SDL_KEYDOWN);
                switch (ev.key.keysym.sym) {
                case SDLK_RIGHT:  if (pressed) g_joypad_bits &= ~0x01; else g_joypad_bits |= 0x01; break;
                case SDLK_LEFT:   if (pressed) g_joypad_bits &= ~0x02; else g_joypad_bits |= 0x02; break;
                case SDLK_UP:     if (pressed) g_joypad_bits &= ~0x04; else g_joypad_bits |= 0x04; break;
                case SDLK_DOWN:   if (pressed) g_joypad_bits &= ~0x08; else g_joypad_bits |= 0x08; break;
                case SDLK_z:      if (pressed) g_joypad_bits &= ~0x10; else g_joypad_bits |= 0x10; break; /* A */
                case SDLK_x:      if (pressed) g_joypad_bits &= ~0x20; else g_joypad_bits |= 0x20; break; /* B */
                case SDLK_BACKSPACE: if (pressed) g_joypad_bits &= ~0x40; else g_joypad_bits |= 0x40; break; /* Select */
                case SDLK_RETURN: if (pressed) g_joypad_bits &= ~0x80; else g_joypad_bits |= 0x80; break; /* Start */
                case SDLK_ESCAPE: running = 0; break;
                default: break;
                }
            }
        }

        /* Apply input override from debug server */
        uint8_t joy = g_joypad_bits;
        if (emu_input_override >= 0)
            joy = (uint8_t)(~emu_input_override & 0xFF);

        /* Set joypad state via direct.joypad byte.
         * Peanut-GB: direct.joypad is active-low (0=pressed, 1=not pressed)
         *   Low nibble:  bit0=A, bit1=B, bit2=Select, bit3=Start
         *   High nibble: bit4=Right, bit5=Left, bit6=Up, bit7=Down
         * Our g_joypad_bits: also active-low
         *   bit0=Right, bit1=Left, bit2=Up, bit3=Down,
         *   bit4=A, bit5=B, bit6=Select, bit7=Start
         * We need to remap the bit layout: */
        {
            uint8_t pgb_joy = 0xFF;
            if (!(joy & 0x10)) pgb_joy &= ~0x01; /* A */
            if (!(joy & 0x20)) pgb_joy &= ~0x02; /* B */
            if (!(joy & 0x40)) pgb_joy &= ~0x04; /* Select */
            if (!(joy & 0x80)) pgb_joy &= ~0x08; /* Start */
            if (!(joy & 0x01)) pgb_joy &= ~0x10; /* Right */
            if (!(joy & 0x02)) pgb_joy &= ~0x20; /* Left */
            if (!(joy & 0x04)) pgb_joy &= ~0x40; /* Up */
            if (!(joy & 0x08)) pgb_joy &= ~0x80; /* Down */
            gb.direct.joypad = pgb_joy;
        }

        /* Run one frame */
        gb_run_frame(&gb);

        /* Record frame state */
        emu_record_frame();

        /* Render */
        if (!headless && renderer && texture) {
            SDL_UpdateTexture(texture, NULL, g_framebuffer, GB_LCD_WIDTH * sizeof(uint32_t));
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, NULL, NULL);
            SDL_RenderPresent(renderer);
        }

        /* Frame rate limiting (~59.7 FPS) */
        uint32_t elapsed = SDL_GetTicks() - frame_start;
        if (elapsed < 16)
            SDL_Delay(16 - elapsed);

        /* Max frames limit */
        if (max_frames > 0 && emu_frame_count >= (uint64_t)max_frames) {
            fprintf(stderr, "[emu] Reached frame limit %d\n", max_frames);
            running = 0;
        }
    }

    /* Cleanup */
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    free(g_rom_data);

    return 0;
}
