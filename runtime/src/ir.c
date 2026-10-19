/*
 * See ir.h. State machine for the GBC IR port at $FF56.
 *
 * RP register layout:
 *   bit 0 (R/W) — LED on/off (the cart drives this to send signal)
 *   bit 1 (R)   — receiving signal (0 = signal present, 1 = idle).
 *                 Only valid when bits 6-7 are both set.
 *   bits 2-5    — read as 1, ignored on write.
 *   bits 6-7    — read enable (write 11 to enable bit 1, 00 to gate).
 */
#include "ir.h"

#include <stdlib.h>
#include <string.h>

struct GBIRState {
    bool local_led;
    bool peer_led;
    uint8_t read_enable_bits;  /* bits 6-7 of last write */

    GBIRSendCallback send_cb;
    void* send_cb_user;
};

GBIRState* gb_ir_create(void) {
    GBIRState* ir = (GBIRState*)calloc(1, sizeof(GBIRState));
    if (!ir) return NULL;
    return ir;
}

void gb_ir_destroy(GBIRState* ir) {
    free(ir);
}

size_t gb_ir_state_size(void) { return 3; }

bool gb_ir_save_state(const GBIRState* ir, void* data, size_t size) {
    if (!ir || !data || size != gb_ir_state_size()) return false;
    uint8_t* bytes = (uint8_t*)data;
    bytes[0] = ir->local_led;
    bytes[1] = ir->peer_led;
    bytes[2] = ir->read_enable_bits;
    return true;
}

bool gb_ir_load_state(GBIRState* ir, const void* data, size_t size) {
    if (!ir || !data || size != gb_ir_state_size()) return false;
    const uint8_t* bytes = (const uint8_t*)data;
    if (bytes[0] > 1 || bytes[1] > 1 || (bytes[2] & 0x3f)) return false;
    ir->local_led = bytes[0] != 0;
    ir->peer_led = bytes[1] != 0;
    ir->read_enable_bits = bytes[2];
    return true;
}

void gb_ir_reset(GBIRState* ir) {
    if (!ir) return;
    GBIRSendCallback cb = ir->send_cb;
    void* user = ir->send_cb_user;
    memset(ir, 0, sizeof(*ir));
    ir->send_cb = cb;
    ir->send_cb_user = user;
}

void gb_ir_set_send_callback(GBIRState* ir, GBIRSendCallback cb, void* user) {
    if (!ir) return;
    ir->send_cb = cb;
    ir->send_cb_user = user;
}

void gb_ir_write_rp(GBIRState* ir, uint8_t value) {
    if (!ir) return;
    bool new_led = (value & 0x01) != 0;
    ir->read_enable_bits = (uint8_t)(value & 0xC0);
    if (new_led != ir->local_led) {
        ir->local_led = new_led;
        if (ir->send_cb) {
            ir->send_cb(ir->send_cb_user, new_led);
        }
    }
}

uint8_t gb_ir_read_rp(const GBIRState* ir) {
    if (!ir) return 0xFF;
    uint8_t r = (uint8_t)(0x3C | ir->read_enable_bits);
    if (ir->local_led) r |= 0x01;
    bool receive_enabled = (ir->read_enable_bits == 0xC0);
    if (receive_enabled && ir->peer_led) {
        /* bit 1 = 0 means "signal present" */
    } else {
        r |= 0x02;
    }
    return r;
}

void gb_ir_receive_peer_led(GBIRState* ir, bool peer_led) {
    if (!ir) return;
    ir->peer_led = peer_led;
}

bool gb_ir_local_led(const GBIRState* ir) {
    return ir && ir->local_led;
}

bool gb_ir_peer_led(const GBIRState* ir) {
    return ir && ir->peer_led;
}
