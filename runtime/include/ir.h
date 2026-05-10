/**
 * @file ir.h
 * @brief Game Boy Color infrared port (RP register $FF56) emulation.
 *
 * The CGB has an IR LED and photodiode used by carts like Pokemon
 * Gold/Silver/Crystal for Mystery Gift, Battle Tower trainer-card
 * sharing, etc. The cart drives the LED by writing bit 0 of $FF56,
 * and reads the photodiode state in bit 1 (with bits 6-7 set to
 * enable read).
 *
 * Spec:
 *   Bit 0     — LED on (write/read; 0 = off, 1 = on)
 *   Bit 1     — Read data: 0 = receiving signal, 1 = not receiving.
 *               Only valid when bits 6-7 are both set.
 *   Bits 2-5  — Unused, read as 1.
 *   Bits 6-7  — Read enable: write 11 to enable bit 1, 00 to disable.
 *
 * The state machine here just tracks local LED state, peer LED state,
 * and the read-enable bits. A platform-side callback (registered via
 * gb_ir_set_send_callback) gets fired whenever the local LED changes
 * — the BGB-link side of the platform turns those into IR_TOGGLE
 * messages to the peer. Conversely, gb_ir_receive_peer_led is called
 * by the link layer when the peer's LED state changes.
 *
 * Pokemon Crystal's IR routines poll RP at frame-rate during peer
 * discovery, so TCP-latency-level sync is enough — we don't try to
 * emulate the microsecond-level pulse timing the cart uses for
 * actual byte transmission. (May need a higher-level protocol if
 * Mystery Gift's data-exchange phase turns out to be timing-tight.)
 */
#ifndef GB_IR_H
#define GB_IR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBIRState GBIRState;

GBIRState* gb_ir_create(void);
void       gb_ir_destroy(GBIRState* ir);
void       gb_ir_reset(GBIRState* ir);

/* Cart-side: hooks for IO read/write at $FF56. */
void    gb_ir_write_rp(GBIRState* ir, uint8_t value);
uint8_t gb_ir_read_rp(const GBIRState* ir);

/* Network-side: peer signalling. The link layer sets the callback so
 * it gets a notification any time the cart toggles its LED, and calls
 * gb_ir_receive_peer_led when an IR_TOGGLE message comes in from the
 * peer. */
typedef void (*GBIRSendCallback)(void* user, bool led_on);
void gb_ir_set_send_callback(GBIRState* ir, GBIRSendCallback cb, void* user);
void gb_ir_receive_peer_led(GBIRState* ir, bool peer_led);

/* Diagnostics. */
bool gb_ir_local_led(const GBIRState* ir);
bool gb_ir_peer_led(const GBIRState* ir);

#ifdef __cplusplus
}
#endif

#endif /* GB_IR_H */
