/**
 * @file relay_client.h
 * @brief Tiny HTTP client for the gb-link-rendezvous discovery server.
 *
 * Two clients agree on a short room code, both POST their reachable
 * IP+port, and the server returns each other's address. Once both sides
 * have peer info, gameplay traffic uses the existing BGB-over-TCP path
 * (serial_link.h) — this module is only involved during the handshake.
 *
 * Synchronous: each call blocks for one HTTP round-trip. The recv-side
 * is small enough (a few hundred bytes of JSON) that this is fine for
 * now; a worker thread can wrap the calls later if menu freezing
 * becomes annoying.
 */
#ifndef GB_RELAY_CLIENT_H
#define GB_RELAY_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GB_RELAY_IP_LEN        64
#define GB_RELAY_NICKNAME_LEN  64
#define GB_RELAY_ERROR_LEN    192

typedef struct {
    bool   has_peer;        /* false = no peer registered yet, not an error */
    char   ip[GB_RELAY_IP_LEN];
    int    port;
    char   nickname[GB_RELAY_NICKNAME_LEN];
} GBRelayPeer;

typedef struct {
    bool   ok;              /* false on transport / 4xx/5xx errors */
    int    http_status;     /* 0 if the request never completed */
    char   error[GB_RELAY_ERROR_LEN];
    GBRelayPeer peer;
} GBRelayResult;

/** Register self with the rendezvous server. The peer field of the
 *  result is populated if the other side already registered. `uuid` is
 *  the client's stable identity (gb_lan_self_uuid()); the server uses
 *  it to gate access via an optional whitelist file. Pass NULL or ""
 *  if the target server doesn't enforce a whitelist. */
GBRelayResult gb_relay_register(const char* server_url,
                                const char* room_code,
                                const char* role,        /* "host" | "join" */
                                int  port,
                                const char* nickname,
                                const char* uuid);

/** Poll for the other side's address. Idempotent — call repeatedly until
 *  result.peer.has_peer is true. */
GBRelayResult gb_relay_lookup(const char* server_url,
                              const char* room_code,
                              const char* role);

/** Best-effort delete of our slot. Errors are swallowed; the server
 *  will reap the entry via TTL anyway. */
void gb_relay_unregister(const char* server_url,
                         const char* room_code,
                         const char* role);

#define GB_RELAY_ROOM_CODE_LEN 33

typedef struct {
    char code[GB_RELAY_ROOM_CODE_LEN];
    char nickname[GB_RELAY_NICKNAME_LEN];
    int  age_seconds;
} GBRelayRoomInfo;

typedef struct {
    bool ok;
    int  http_status;
    char error[GB_RELAY_ERROR_LEN];
    int  count;                  /* rooms actually filled in below */
} GBRelayListResult;

/**
 * Fetch the list of rooms that have a host but no join — i.e. open
 * for someone to drop into. Writes up to `max` entries to `out_rooms`
 * (newest first); the actual count goes into result.count.
 */
GBRelayListResult gb_relay_list_rooms(const char* server_url,
                                      GBRelayRoomInfo* out_rooms,
                                      int max);

#ifdef __cplusplus
}
#endif

#endif /* GB_RELAY_CLIENT_H */
