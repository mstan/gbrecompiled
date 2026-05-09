/**
 * @file gb_printer.h
 * @brief Game Boy Printer (DMG-PRR-01) emulation over the serial link.
 *
 * Drop-in replacement for a physical Printer plugged into the link cable.
 * State machine tracks the standard 9+ byte printer packet
 * (sync1 0x88, sync2 0x33, command, compress, len_lo, len_hi, data...,
 * chksum_lo, chksum_hi, alive-byte, status-byte) and replies inline.
 *
 * INIT clears the tile buffer, DATA appends decompressed strips, PRINT
 * decodes the accumulated 2bpp tile data into an RGBA bitmap and writes
 * it to disk as <output_dir>/<prefix>_<NNNN>.png. STATUS is a no-op
 * (returns idle); the cart polls it until the printer reports done,
 * which we do on the very first poll for a snappy "instant print".
 *
 * One printer instance per platform; the platform shovels every serial
 * byte (when no BGB peer is connected) into gb_printer_on_serial_byte.
 *
 * Spec reference: https://gbdev.gg8.se/wiki/articles/Gameboy_Printer
 */
#ifndef GB_PRINTER_H
#define GB_PRINTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBContext GBContext;
typedef struct GBPrinter GBPrinter;

GBPrinter* gb_printer_create(void);
void       gb_printer_destroy(GBPrinter* p);

/**
 * Set the directory + filename prefix used for saved prints. The first
 * file goes to <dir>/<prefix>_0001.png, the next 0002, and so on.
 * Counter is restored from any existing _NNNN.png files in the dir on
 * the first save, so prints from previous sessions don't get
 * overwritten.
 */
void gb_printer_set_output(GBPrinter* p, const char* dir, const char* prefix);

/**
 * Hook this from the platform's on_serial_byte callback. Returns true
 * if the printer claimed the transfer (set serial_transfer.deferred
 * and called gb_serial_complete_transfer with the printer's reply
 * byte). Returns false if the byte didn't fit the printer protocol —
 * caller should fall through to other handlers / leave the cable
 * "unplugged".
 */
bool gb_printer_on_serial_byte(GBPrinter* p, GBContext* ctx, uint8_t outgoing);

/** Diagnostics: number of completed prints since process start. */
uint32_t gb_printer_print_count(const GBPrinter* p);

#ifdef __cplusplus
}
#endif

#endif /* GB_PRINTER_H */
