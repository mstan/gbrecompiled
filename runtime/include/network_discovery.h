/*
 * LAN peer discovery for the BGB-protocol link cable.
 *
 * Each running instance generates a stable UUID + nickname (persisted
 * via the platform's runtime prefs) and, when discovery is enabled,
 * broadcasts a small UDP hello on the local subnet every few seconds.
 * Other instances on the same network see those hellos and maintain a
 * peer list (with TTL) the menu can render. Selecting a peer and
 * pressing connect calls into serial_link's existing TCP-connect path.
 *
 * Discovery runs on a single background thread. The peer list is
 * mutex-guarded; everything else is owned by the main thread.
 */
#ifndef GBRT_NETWORK_DISCOVERY_H
#define GBRT_NETWORK_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GB_LAN_UUID_LEN     37   /* including null terminator */
#define GB_LAN_NICKNAME_LEN 33   /* including null terminator */
#define GB_LAN_IP_LEN       46   /* INET6_ADDRSTRLEN */

typedef struct {
    char uuid[GB_LAN_UUID_LEN];
    char nickname[GB_LAN_NICKNAME_LEN];
    char ip[GB_LAN_IP_LEN];      /* peer's address as seen on the wire */
    uint16_t bgb_port;           /* peer's TCP port for BGB protocol */
    uint64_t last_seen_ms;       /* monotonic ms; main thread can age this */
} GBLanPeer;

/* Lifecycle. Call gb_lan_init() once at startup (loads/generates the
 * UUID via the prefs callbacks below). gb_lan_set_enabled() spins up or
 * tears down the broadcast/listen thread. */
void gb_lan_init(void);
void gb_lan_shutdown(void);

void gb_lan_set_enabled(bool enabled);
bool gb_lan_is_enabled(void);

/* Identity. The UUID is stable across runs; the nickname is editable.
 * Setting a nickname triggers a re-broadcast so peers see the new name. */
const char* gb_lan_self_uuid(void);
const char* gb_lan_self_nickname(void);
void gb_lan_set_self_nickname(const char* name);

/* The TCP port advertised in our hellos. Updates take effect on the next
 * broadcast. Default 8765 (BGB convention). */
void gb_lan_set_advertised_port(uint16_t port);
uint16_t gb_lan_advertised_port(void);

/* Snapshot the current peer list. Caller passes a buffer; we fill up to
 * `max` entries and return how many we wrote. Peers older than ~10 seconds
 * are not returned. Thread-safe. */
size_t gb_lan_get_peers(GBLanPeer* out, size_t max);

/* Hooks the platform layer must provide so the discovery module can
 * persist identity through the same prefs file the rest of the runtime
 * uses, without dragging in platform_sdl directly. Both can be NULL on
 * platforms with no persistent storage; we'll just regenerate the UUID
 * each launch. */
typedef struct {
    /* Returns true and fills `out` with the persisted value if present.
     * out_size is the buffer size (incl. null terminator). */
    bool (*load_string)(const char* key, char* out, size_t out_size);
    /* Persist value for `key`. Called whenever identity changes. */
    void (*save_string)(const char* key, const char* value);
} GBLanPrefsHooks;

void gb_lan_set_prefs_hooks(const GBLanPrefsHooks* hooks);

#ifdef __cplusplus
}
#endif

#endif
