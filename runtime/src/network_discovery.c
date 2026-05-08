/*
 * See network_discovery.h.
 *
 * Wire format (little-endian on wire — bytes laid out as named):
 *
 *   offset  size  field
 *   0       4     magic = "PG1!"
 *   4       2     protocol version (uint16)
 *   6       2     advertised BGB TCP port (uint16)
 *   8       36    uuid (ASCII, no null inside; pad with 0 bytes)
 *   44      32    nickname (ASCII, padded with 0 bytes)
 *   total   76 bytes
 *
 * UDP port 8766 (BGB+1). Sent every ~3 seconds while enabled.
 *
 * Peer table is fixed-size; on overflow the oldest entry is evicted.
 */
#include "network_discovery.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define LAN_MAGIC          "PG1!"
#define LAN_PROTO_VERSION  1
#define LAN_PORT           8766
#define LAN_HELLO_INTERVAL_MS  3000
#define LAN_PEER_TTL_MS    10000
#define LAN_PEER_CAP       32
#define LAN_PACKET_SIZE    76

static void lan_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[LAN] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static struct {
    /* Identity (set on init, edited via setter). */
    char uuid[GB_LAN_UUID_LEN];
    char nickname[GB_LAN_NICKNAME_LEN];
    uint16_t advertised_port;

    GBLanPrefsHooks prefs;
    bool prefs_set;

    /* Lifecycle. */
    volatile bool enabled;
    volatile bool shutdown_requested;
    pthread_t thread;
    bool thread_running;
    int sock_fd;

    /* Peer table. */
    pthread_mutex_t peers_mu;
    GBLanPeer peers[LAN_PEER_CAP];
    size_t peer_count;
} g = {
    .advertised_port = 8765,
    .sock_fd = -1,
};

/* ============================================================================
 * UUIDv4 generation (random + version/variant bits)
 * ============================================================================ */

static void random_bytes(uint8_t* buf, size_t n) {
    /* /dev/urandom is universally available on Linux/macOS; if it's missing
     * (very unusual), fall back to clock+pid mixing — not crypto, but fine
     * for a stable-across-runs identifier. */
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t got = fread(buf, 1, n, f);
        fclose(f);
        if (got == n) return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t seed = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    seed ^= (uint64_t)(uintptr_t)getpid() << 32;
    for (size_t i = 0; i < n; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        buf[i] = (uint8_t)(seed >> 56);
    }
}

static void generate_uuid_v4(char out[GB_LAN_UUID_LEN]) {
    uint8_t b[16];
    random_bytes(b, 16);
    b[6] = (b[6] & 0x0F) | 0x40;  /* version 4 */
    b[8] = (b[8] & 0x3F) | 0x80;  /* RFC 4122 variant */
    snprintf(out, GB_LAN_UUID_LEN,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void default_nickname_from_uuid(const char* uuid, char out[GB_LAN_NICKNAME_LEN]) {
    /* Use the first 4 hex chars of the UUID for a friendly suffix. */
    snprintf(out, GB_LAN_NICKNAME_LEN, "Player-%c%c%c%c",
             uuid[0], uuid[1], uuid[2], uuid[3]);
}

/* ============================================================================
 * Peer table
 * ============================================================================ */

static void peer_table_upsert(const char* uuid, const char* nickname,
                               const char* ip, uint16_t bgb_port) {
    pthread_mutex_lock(&g.peers_mu);
    /* Skip ourselves. */
    if (strcmp(uuid, g.uuid) == 0) {
        pthread_mutex_unlock(&g.peers_mu);
        return;
    }
    uint64_t now = now_ms();
    /* Update existing entry if uuid matches. */
    for (size_t i = 0; i < g.peer_count; i++) {
        if (strcmp(g.peers[i].uuid, uuid) == 0) {
            strncpy(g.peers[i].nickname, nickname, GB_LAN_NICKNAME_LEN - 1);
            g.peers[i].nickname[GB_LAN_NICKNAME_LEN - 1] = '\0';
            strncpy(g.peers[i].ip, ip, GB_LAN_IP_LEN - 1);
            g.peers[i].ip[GB_LAN_IP_LEN - 1] = '\0';
            g.peers[i].bgb_port = bgb_port;
            g.peers[i].last_seen_ms = now;
            pthread_mutex_unlock(&g.peers_mu);
            return;
        }
    }
    /* Insert new entry; evict oldest on overflow. */
    if (g.peer_count == LAN_PEER_CAP) {
        size_t oldest = 0;
        for (size_t i = 1; i < g.peer_count; i++) {
            if (g.peers[i].last_seen_ms < g.peers[oldest].last_seen_ms) {
                oldest = i;
            }
        }
        g.peers[oldest] = g.peers[g.peer_count - 1];
        g.peer_count--;
    }
    GBLanPeer* p = &g.peers[g.peer_count++];
    strncpy(p->uuid, uuid, GB_LAN_UUID_LEN - 1);
    p->uuid[GB_LAN_UUID_LEN - 1] = '\0';
    strncpy(p->nickname, nickname, GB_LAN_NICKNAME_LEN - 1);
    p->nickname[GB_LAN_NICKNAME_LEN - 1] = '\0';
    strncpy(p->ip, ip, GB_LAN_IP_LEN - 1);
    p->ip[GB_LAN_IP_LEN - 1] = '\0';
    p->bgb_port = bgb_port;
    p->last_seen_ms = now;
    pthread_mutex_unlock(&g.peers_mu);
}

static void peer_table_age(void) {
    pthread_mutex_lock(&g.peers_mu);
    uint64_t now = now_ms();
    size_t kept = 0;
    for (size_t i = 0; i < g.peer_count; i++) {
        if (now - g.peers[i].last_seen_ms <= LAN_PEER_TTL_MS) {
            if (kept != i) g.peers[kept] = g.peers[i];
            kept++;
        }
    }
    g.peer_count = kept;
    pthread_mutex_unlock(&g.peers_mu);
}

/* ============================================================================
 * Wire format
 * ============================================================================ */

static void encode_hello(uint8_t buf[LAN_PACKET_SIZE]) {
    memcpy(buf, LAN_MAGIC, 4);
    buf[4] = (uint8_t)(LAN_PROTO_VERSION & 0xFF);
    buf[5] = (uint8_t)((LAN_PROTO_VERSION >> 8) & 0xFF);
    uint16_t port = g.advertised_port;
    buf[6] = (uint8_t)(port & 0xFF);
    buf[7] = (uint8_t)((port >> 8) & 0xFF);
    memset(&buf[8], 0, 36);
    strncpy((char*)&buf[8], g.uuid, 36);
    memset(&buf[44], 0, 32);
    strncpy((char*)&buf[44], g.nickname, 32);
}

static bool decode_hello(const uint8_t* buf, size_t len,
                         char uuid_out[GB_LAN_UUID_LEN],
                         char nick_out[GB_LAN_NICKNAME_LEN],
                         uint16_t* port_out) {
    if (len < LAN_PACKET_SIZE) return false;
    if (memcmp(buf, LAN_MAGIC, 4) != 0) return false;
    uint16_t proto = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
    if (proto != LAN_PROTO_VERSION) return false;
    *port_out = (uint16_t)buf[6] | ((uint16_t)buf[7] << 8);
    memcpy(uuid_out, &buf[8], 36);
    uuid_out[36] = '\0';
    memcpy(nick_out, &buf[44], 32);
    nick_out[32] = '\0';
    /* Sanity: uuid should be ASCII printable. */
    for (int i = 0; i < 36; i++) {
        if (uuid_out[i] != '\0' && (uuid_out[i] < 0x20 || uuid_out[i] > 0x7E)) {
            return false;
        }
    }
    return true;
}

/* ============================================================================
 * Discovery thread
 * ============================================================================ */

static int open_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        lan_log("socket() failed: %s", strerror(errno));
        return -1;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LAN_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        lan_log("bind(:%d) failed: %s", LAN_PORT, strerror(errno));
        close(fd);
        return -1;
    }

    /* Make recv non-blocking via timeout. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 250 * 1000 };  /* 250 ms */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static void send_hello(int fd) {
    uint8_t buf[LAN_PACKET_SIZE];
    encode_hello(buf);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LAN_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(fd, buf, LAN_PACKET_SIZE, 0, (struct sockaddr*)&addr, sizeof(addr));
}

static void* discovery_thread(void* arg) {
    (void)arg;
    int fd = g.sock_fd;
    uint64_t last_send_ms = 0;
    while (!g.shutdown_requested) {
        uint64_t now = now_ms();
        if (now - last_send_ms >= LAN_HELLO_INTERVAL_MS) {
            send_hello(fd);
            last_send_ms = now;
        }
        uint8_t buf[LAN_PACKET_SIZE];
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);
        ssize_t got = recvfrom(fd, buf, sizeof(buf), 0,
                               (struct sockaddr*)&src, &srclen);
        if (got > 0) {
            char uuid[GB_LAN_UUID_LEN];
            char nick[GB_LAN_NICKNAME_LEN];
            uint16_t port = 0;
            if (decode_hello(buf, (size_t)got, uuid, nick, &port)) {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
                peer_table_upsert(uuid, nick, ip, port);
            }
        }
        peer_table_age();
    }
    return NULL;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void gb_lan_set_prefs_hooks(const GBLanPrefsHooks* hooks) {
    if (hooks) {
        g.prefs = *hooks;
        g.prefs_set = true;
    } else {
        g.prefs_set = false;
    }
}

void gb_lan_init(void) {
    pthread_mutex_init(&g.peers_mu, NULL);

    /* UUID: load from prefs or generate. */
    bool got_uuid = false;
    if (g.prefs_set && g.prefs.load_string) {
        got_uuid = g.prefs.load_string("lan.uuid", g.uuid, sizeof(g.uuid));
        if (got_uuid && strlen(g.uuid) != 36) got_uuid = false;
    }
    if (!got_uuid) {
        generate_uuid_v4(g.uuid);
        if (g.prefs_set && g.prefs.save_string) {
            g.prefs.save_string("lan.uuid", g.uuid);
        }
    }

    /* Nickname: load or default from UUID. */
    bool got_nick = false;
    if (g.prefs_set && g.prefs.load_string) {
        got_nick = g.prefs.load_string("lan.nickname", g.nickname, sizeof(g.nickname));
        if (got_nick && g.nickname[0] == '\0') got_nick = false;
    }
    if (!got_nick) {
        default_nickname_from_uuid(g.uuid, g.nickname);
        if (g.prefs_set && g.prefs.save_string) {
            g.prefs.save_string("lan.nickname", g.nickname);
        }
    }

    lan_log("identity: %s [%s]", g.nickname, g.uuid);
}

void gb_lan_set_enabled(bool enabled) {
    if (enabled == g.enabled) return;
    if (enabled) {
        g.shutdown_requested = false;
        g.sock_fd = open_socket();
        if (g.sock_fd < 0) {
            return;
        }
        if (pthread_create(&g.thread, NULL, discovery_thread, NULL) != 0) {
            lan_log("pthread_create failed: %s", strerror(errno));
            close(g.sock_fd);
            g.sock_fd = -1;
            return;
        }
        g.thread_running = true;
        g.enabled = true;
        lan_log("discovery enabled on UDP :%d", LAN_PORT);
    } else {
        g.shutdown_requested = true;
        if (g.thread_running) {
            pthread_join(g.thread, NULL);
            g.thread_running = false;
        }
        if (g.sock_fd >= 0) {
            close(g.sock_fd);
            g.sock_fd = -1;
        }
        g.enabled = false;
        pthread_mutex_lock(&g.peers_mu);
        g.peer_count = 0;
        pthread_mutex_unlock(&g.peers_mu);
        lan_log("discovery disabled");
    }
}

bool gb_lan_is_enabled(void) {
    return g.enabled;
}

void gb_lan_shutdown(void) {
    gb_lan_set_enabled(false);
    pthread_mutex_destroy(&g.peers_mu);
}

const char* gb_lan_self_uuid(void) {
    return g.uuid;
}

const char* gb_lan_self_nickname(void) {
    return g.nickname;
}

void gb_lan_set_self_nickname(const char* name) {
    if (!name) return;
    strncpy(g.nickname, name, GB_LAN_NICKNAME_LEN - 1);
    g.nickname[GB_LAN_NICKNAME_LEN - 1] = '\0';
    if (g.prefs_set && g.prefs.save_string) {
        g.prefs.save_string("lan.nickname", g.nickname);
    }
    /* Force a re-broadcast on the next thread iteration by sending now. */
    if (g.enabled && g.sock_fd >= 0) {
        send_hello(g.sock_fd);
    }
}

void gb_lan_set_advertised_port(uint16_t port) {
    g.advertised_port = port;
}

uint16_t gb_lan_advertised_port(void) {
    return g.advertised_port;
}

size_t gb_lan_get_peers(GBLanPeer* out, size_t max) {
    pthread_mutex_lock(&g.peers_mu);
    size_t n = g.peer_count;
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) out[i] = g.peers[i];
    pthread_mutex_unlock(&g.peers_mu);
    return n;
}
