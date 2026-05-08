#ifndef GBRT_SERIAL_LINK_H
#define GBRT_SERIAL_LINK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GBContext;

/* Open a TCP listener on `port` and accept one peer using BGB link protocol.
 * Spawns a background recv thread. Returns true on success. */
bool gb_serial_link_start_listen(uint16_t port);

/* Connect outbound to `host:port` using BGB link protocol.
 * Spawns a background recv thread. Returns true on success. */
bool gb_serial_link_start_connect(const char* host, uint16_t port);

/* Close peer + listener, join the recv thread. Idempotent. */
void gb_serial_link_shutdown(void);

/* Read GB_LINK_LISTEN / GB_LINK_CONNECT environment variables and start the
 * link in the appropriate mode. No-op if neither is set. */
void gb_serial_link_init_from_env(void);

bool gb_serial_link_is_active(void);
bool gb_serial_link_is_ready(void);

/* Returns the remote peer's IP as a printable string while a link is up,
 * or an empty string when there's no active connection. The pointer is
 * valid until the next start_listen / start_connect / shutdown call. */
const char* gb_serial_link_peer_ip(void);

/* Set as the GBPlatformCallbacks.on_serial_byte handler. Called by the runtime
 * when a master-mode (internal clock) transfer's countdown reaches zero — we
 * forward the outgoing byte as a BGB sync1 packet and mark the transfer
 * deferred so the runtime waits for the peer's reply. */
void gb_serial_link_on_serial_byte(struct GBContext* ctx, uint8_t outgoing);

/* Drain inbound BGB packets and apply them to `ctx`. Call once per frame from
 * the platform main loop. Safe to call when the link is inactive. */
void gb_serial_link_tick(struct GBContext* ctx);

#ifdef __cplusplus
}
#endif

#endif
