/*
 * Game Boy Printer (DMG-PRR-01) emulation. See gb_printer.h.
 *
 * Protocol summary (each cart→printer transfer is one packet):
 *
 *     SYNC1=0x88, SYNC2=0x33, CMD, COMPRESS, LEN_LO, LEN_HI,
 *     DATA[LEN_LO|LEN_HI<<8], CHK_LO, CHK_HI, [byte], [byte]
 *
 * The two trailing bytes are clock pulses from the cart so the printer
 * can clock back its alive byte (0x81) and a status byte. We respond
 * 0x00 to all packet bytes, 0x81 on the alive slot, and a status code
 * on the status slot.
 *
 * Commands we care about:
 *   INIT  (0x01) — reset accumulator, no data
 *   DATA  (0x04) — append tile bytes to accumulator (compression NYI)
 *   PRINT (0x02) — flush accumulator to a PNG file
 *   STATUS(0x0F) — poll, no data, return idle
 */

#include "gb_printer.h"
#include "gbrt.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define PRN_CMD_INIT    0x01
#define PRN_CMD_PRINT   0x02
#define PRN_CMD_DATA    0x04
#define PRN_CMD_STATUS  0x0F

/* 20 tiles wide × 2 tile rows × 16 bytes-per-tile = 640 bytes per
 * 16-pixel strip. Real printers cap at 4 strips (2560 bytes) per
 * buffer, but the cart hits PRINT once it's full. Our accumulator
 * mirrors that limit and a bit of headroom. */
#define PRN_STRIP_W       160
#define PRN_STRIP_H       16
#define PRN_STRIP_TILES   40
#define PRN_STRIP_BYTES   (PRN_STRIP_TILES * 16)
#define PRN_BUFFER_BYTES  (PRN_STRIP_BYTES * 9)  /* roomy: 9 strips */

typedef enum {
    RX_SYNC1,
    RX_SYNC2,
    RX_CMD,
    RX_COMPRESS,
    RX_LEN_LO,
    RX_LEN_HI,
    RX_DATA,
    RX_CHKSUM_LO,
    RX_CHKSUM_HI,
    RX_ALIVE,
    RX_STATUS,
} GBPrinterRx;

struct GBPrinter {
    GBPrinterRx state;

    uint8_t  cmd;
    uint8_t  compress;
    uint16_t data_length;
    uint16_t data_received;
    uint8_t  print_data[4];
    uint8_t  print_data_pos;

    uint8_t  buffer[PRN_BUFFER_BYTES];
    size_t   buffer_size;

    /* Cross-PRINT accumulator. Pokemon Yellow's Pokédex print is too
     * tall for a single 4-strip printer buffer, so it sends multiple
     * DATA+PRINT cycles with feed_after=0 ("don't feed paper yet"),
     * then a final PRINT with feed_after>0 to flush the page. We
     * stack each cycle's strips into pending_rgba and save once we
     * see the finalizing PRINT — yields one PNG per logical page. */
    uint32_t* pending_rgba;
    int       pending_height;

    char output_dir[512];
    char output_prefix[64];
    uint32_t next_index;
    bool     index_resolved;

    uint32_t print_count;
};

GBPrinter* gb_printer_create(void) {
    GBPrinter* p = (GBPrinter*)calloc(1, sizeof(GBPrinter));
    if (!p) return NULL;
    p->state = RX_SYNC1;
    snprintf(p->output_dir, sizeof(p->output_dir), "prints");
    snprintf(p->output_prefix, sizeof(p->output_prefix), "print");
    return p;
}

void gb_printer_destroy(GBPrinter* p) {
    if (!p) return;
    free(p->pending_rgba);
    free(p);
}

void gb_printer_set_output(GBPrinter* p, const char* dir, const char* prefix) {
    if (!p) return;
    if (dir && *dir) {
        snprintf(p->output_dir, sizeof(p->output_dir), "%s", dir);
    }
    if (prefix && *prefix) {
        snprintf(p->output_prefix, sizeof(p->output_prefix), "%s", prefix);
    }
    /* Force re-scan of the output directory so a new prefix doesn't
     * trample previously saved prints. */
    p->index_resolved = false;
}

uint32_t gb_printer_print_count(const GBPrinter* p) {
    return p ? p->print_count : 0;
}

static void ensure_output_dir(GBPrinter* p) {
    if (!p->output_dir[0]) return;
    /* mkdir is racy but harmless — EEXIST is fine. */
    mkdir(p->output_dir, 0755);
}

/* Find the highest existing <prefix>_<NNNN>.png index in output_dir so
 * subsequent saves don't clobber prints from previous sessions. */
static void resolve_next_index(GBPrinter* p) {
    if (p->index_resolved) return;
    p->next_index = 1;
    p->index_resolved = true;

    DIR* dir = opendir(p->output_dir);
    if (!dir) return;

    size_t prefix_len = strlen(p->output_prefix);
    struct dirent* ent;
    uint32_t highest = 0;
    while ((ent = readdir(dir))) {
        const char* name = ent->d_name;
        if (strncmp(name, p->output_prefix, prefix_len) != 0) continue;
        if (name[prefix_len] != '_') continue;
        const char* num = name + prefix_len + 1;
        char* endp = NULL;
        unsigned long val = strtoul(num, &endp, 10);
        if (!endp || endp == num) continue;
        if (strcmp(endp, ".png") != 0) continue;
        if (val > highest) highest = (uint32_t)val;
    }
    closedir(dir);
    p->next_index = highest + 1;
}

/* Decode the accumulated 2bpp tile buffer to RGBA8888 and append the
 * resulting rows to pending_rgba. The buffer is a sequence of 40-tile
 * strips (20 tiles × 2 tile-rows = 16 pixel rows). print_data[2] is
 * the BGP-style palette mapping. We map shades to a neutral 4-level
 * grayscale — printer paper has no chrominance. */
static void decode_buffer_into_pending(GBPrinter* p) {
    if (p->buffer_size == 0) return;
    /* Round down to a whole number of strips so a partial trailing
     * strip doesn't leak garbage rows. */
    size_t strip_count = p->buffer_size / PRN_STRIP_BYTES;
    if (strip_count == 0) return;

    int width = PRN_STRIP_W;
    int new_rows = (int)strip_count * PRN_STRIP_H;
    int new_height = p->pending_height + new_rows;
    size_t new_pixels = (size_t)width * (size_t)new_height;

    uint32_t* grown = (uint32_t*)realloc(p->pending_rgba, new_pixels * 4);
    if (!grown) {
        fprintf(stderr, "[PRINTER] OOM growing pending image to %dx%d\n",
                width, new_height);
        return;
    }
    p->pending_rgba = grown;

    uint32_t* dst_base = &p->pending_rgba[(size_t)p->pending_height * width];

    static const uint32_t shade_to_rgba[4] = {
        0xFFFFFFFFu, 0xFFAAAAAAu, 0xFF555555u, 0xFF000000u,
    };
    uint8_t palette = p->print_data[2];

    int total_tiles = (int)strip_count * PRN_STRIP_TILES;
    for (int t = 0; t < total_tiles; t++) {
        int strip = t / PRN_STRIP_TILES;
        int idx_in_strip = t % PRN_STRIP_TILES;
        int tile_row_in_strip = idx_in_strip / 20;
        int tile_col = idx_in_strip % 20;
        int tile_y_global = strip * 2 + tile_row_in_strip;
        int origin_x = tile_col * 8;
        int origin_y = tile_y_global * 8;
        const uint8_t* tile_bytes = &p->buffer[(size_t)t * 16];

        for (int row = 0; row < 8; row++) {
            uint8_t lo = tile_bytes[row * 2 + 0];
            uint8_t hi = tile_bytes[row * 2 + 1];
            uint32_t* dst_row = &dst_base[(origin_y + row) * width + origin_x];
            for (int x = 0; x < 8; x++) {
                int bit = 7 - x;
                uint8_t raw = (uint8_t)(((hi >> bit) & 1) << 1 |
                                        ((lo >> bit) & 1));
                uint8_t shade = (uint8_t)((palette >> (raw * 2)) & 0x03);
                dst_row[x] = shade_to_rgba[shade];
            }
        }
    }

    p->pending_height = new_height;
}

static void flush_pending_image(GBPrinter* p) {
    if (p->pending_height == 0 || !p->pending_rgba) return;
    int width = PRN_STRIP_W;
    int height = p->pending_height;

    ensure_output_dir(p);
    resolve_next_index(p);

    char path[640];
    snprintf(path, sizeof(path), "%s/%s_%04u.png",
             p->output_dir, p->output_prefix, (unsigned)p->next_index);

    if (stbi_write_png(path, width, height, 4, p->pending_rgba, width * 4)) {
        fprintf(stderr, "[PRINTER] Saved %s (%dx%d)\n", path, width, height);
        p->next_index++;
        p->print_count++;
    } else {
        fprintf(stderr, "[PRINTER] Failed to save %s\n", path);
    }

    free(p->pending_rgba);
    p->pending_rgba = NULL;
    p->pending_height = 0;
}

static void dispatch_packet(GBPrinter* p) {
    switch (p->cmd) {
        case PRN_CMD_INIT:
            /* INIT clears the current strip buffer (Pokemon Yellow
             * sends one between each 4-strip batch of a paginated
             * page). It does NOT abandon the pending cross-PRINT
             * accumulator — that only flushes on a finalizing PRINT
             * with feed_after > 0. */
            p->buffer_size = 0;
            break;
        case PRN_CMD_DATA:
            /* Bytes were already appended during RX_DATA. */
            break;
        case PRN_CMD_PRINT: {
            /* print_data[1] = margins. Low nibble is "feed lines after
             * print". Pokemon Yellow paginates a Pokédex page by
             * sending feed_after=0 on each non-final PRINT, then
             * feed_after>0 on the last one. We append rows on every
             * PRINT and only flush to PNG on the finalizer — yields
             * one file per logical page. */
            decode_buffer_into_pending(p);
            p->buffer_size = 0;
            uint8_t feed_after = (uint8_t)(p->print_data[1] & 0x0F);
            if (feed_after > 0) {
                flush_pending_image(p);
            }
            break;
        }
        case PRN_CMD_STATUS:
            /* No-op; status reply already handed back. */
            break;
        default:
            break;
    }
}

/* The cart drives the SI/SO clock as master. For each byte it sends
 * we shift one byte back. Returns the byte the printer would put on
 * the wire in response to `incoming`. */
static uint8_t advance(GBPrinter* p, uint8_t incoming) {
    uint8_t reply = 0x00;
    switch (p->state) {
        case RX_SYNC1:
            if (incoming == 0x88) p->state = RX_SYNC2;
            break;
        case RX_SYNC2:
            if (incoming == 0x33) {
                p->state = RX_CMD;
                p->print_data_pos = 0;
            } else if (incoming != 0x88) {
                p->state = RX_SYNC1;
            }
            break;
        case RX_CMD:
            p->cmd = incoming;
            p->state = RX_COMPRESS;
            break;
        case RX_COMPRESS:
            p->compress = incoming;
            p->state = RX_LEN_LO;
            break;
        case RX_LEN_LO:
            p->data_length = incoming;
            p->state = RX_LEN_HI;
            break;
        case RX_LEN_HI:
            p->data_length |= (uint16_t)((uint16_t)incoming << 8);
            p->data_received = 0;
            p->state = (p->data_length == 0) ? RX_CHKSUM_LO : RX_DATA;
            break;
        case RX_DATA:
            if (p->cmd == PRN_CMD_DATA && !p->compress) {
                if (p->buffer_size < sizeof(p->buffer)) {
                    p->buffer[p->buffer_size++] = incoming;
                }
            } else if (p->cmd == PRN_CMD_PRINT && p->print_data_pos < 4) {
                p->print_data[p->print_data_pos++] = incoming;
            }
            /* Compressed payloads are swallowed-but-ignored for now — the
             * Pokemon series doesn't use compression. */
            p->data_received++;
            if (p->data_received >= p->data_length) {
                p->state = RX_CHKSUM_LO;
            }
            break;
        case RX_CHKSUM_LO:
            p->state = RX_CHKSUM_HI;
            break;
        case RX_CHKSUM_HI:
            p->state = RX_ALIVE;
            break;
        case RX_ALIVE:
            /* "I am a printer." Magic alive byte. */
            reply = 0x81;
            p->state = RX_STATUS;
            break;
        case RX_STATUS:
            /* Always idle/done — no error bits. The cart's
             * WaitForPrinter loop sees "ready" on the first poll and
             * moves on. */
            reply = 0x00;
            dispatch_packet(p);
            p->state = RX_SYNC1;
            break;
    }
    return reply;
}

bool gb_printer_on_serial_byte(GBPrinter* p, GBContext* ctx, uint8_t outgoing) {
    if (!p || !ctx) return false;

    /* Only claim the transfer once we've actually locked onto a packet
     * (i.e. seen SYNC1+SYNC2). Stray bytes outside a packet bubble up
     * to the runtime's "cable unplugged" 0xFF default — important when
     * the cart isn't trying to print at all. */
    bool was_active = (p->state != RX_SYNC1);
    uint8_t reply = advance(p, outgoing);
    bool now_active = (p->state != RX_SYNC1) || was_active;
    if (!now_active) return false;

    /* Order matters: gb_serial_complete_transfer clears `deferred` as
     * part of finishing the transfer, so we have to flag it AGAIN
     * after — otherwise the runtime's "no peer claimed it" path fires
     * and overwrites the printer's reply with 0xFF. */
    gb_serial_complete_transfer(ctx, reply);
    ctx->serial_transfer.deferred = 1;
    return true;
}
