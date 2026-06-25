/*
 * debug_server.h -- TCP debug server for GB recomp projects
 *
 * Non-blocking TCP server on localhost (default port 4370).
 * JSON-over-newline protocol, polled once per frame from the VBlank callback.
 * Includes a 36000-frame ring buffer for retroactive state queries.
 *
 * Modeled after nesrecomp/runner/debug_server and psxrecomp/runner/debug_server.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Ring buffer frame record ---- */

#define GB_FRAME_HISTORY_CAP 36000   /* ~10 min @ 60fps */

typedef struct {
    uint32_t frame_number;

    /* CPU state (SM83) */
    uint8_t  cpu_a, cpu_f;
    uint8_t  cpu_b, cpu_c;
    uint8_t  cpu_d, cpu_e;
    uint8_t  cpu_h, cpu_l;
    uint16_t cpu_sp, cpu_pc;
    uint8_t  cpu_ime;

    /* PPU state */
    uint8_t  lcdc;          /* FF40 */
    uint8_t  stat;          /* FF41 */
    uint8_t  scy, scx;     /* FF42-FF43 */
    uint8_t  ly;            /* FF44 */
    uint8_t  wy, wx;       /* FF4A-FF4B */

    /* Mapper + input */
    uint16_t rom_bank;
    uint8_t  ram_bank;
    uint8_t  joypad;        /* P1 register value */

    /* Timing */
    uint32_t cycles;

    /* Game-specific (filled by game_fill_frame_record hook) */
    uint8_t  game_data[16];

    /* Last recomp/interp function name */
    char     last_func[32];
} GBFrameRecord;

/* ---- Public API ---- */

/* Initialize the server. Call once at startup.
 * port=0 uses the default (4370). */
void gb_debug_server_init(int port);

/* Poll for incoming connections and commands. Non-blocking.
 * Call once per frame. */
void gb_debug_server_poll(void);

/* Record the current frame's state into the ring buffer.
 * Call after VBlank runs. */
void gb_debug_server_record_frame(void);

/* Block while paused, polling TCP + SDL events.
 * Call from frame callback before running game logic. */
void gb_debug_server_wait_if_paused(void);

/* Graceful shutdown. Call at exit. */
void gb_debug_server_shutdown(void);

/* Check if a TCP client is connected. */
int gb_debug_server_is_connected(void);

/* ---- Watchpoint notifications ---- */

/* Check all watchpoints against current RAM values.
 * Sends JSON notification for any changes. */
void gb_debug_server_check_watchpoints(void);

/* ---- Input override ---- */

/* Returns >= 0 if the debug server wants to override joypad input,
 * -1 if no override is active.
 * Bits: 0=Right,1=Left,2=Up,3=Down,4=A,5=B,6=Select,7=Start (active high) */
int gb_debug_server_get_input_override(void);

/* ---- Public send helpers (for game command handlers) ---- */

/* Send a complete JSON line to the connected client. */
void gb_debug_server_send_line(const char *json);

/* Send a formatted JSON line (printf-style). */
void gb_debug_server_send_fmt(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
