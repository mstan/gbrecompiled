/*
 * BGB-compatible Game Boy link cable over TCP.
 *
 * Protocol reference: https://bgb.bircd.org/bgblink.html
 * Each packet is exactly 8 bytes, little-endian:
 *   b1 cmd, b2 b3 b4 data, i1 (uint32) timestamp.
 *
 * Threading: a single background thread does blocking recv() and pushes parsed
 * packets onto a mutex-guarded ring. The platform main thread is the only
 * sender (drains the inbound ring in gb_serial_link_tick and writes responses
 * inline) — keeps the wire-write path single-threaded so 8-byte packets never
 * interleave.
 */

#include "serial_link.h"
#include "gbrt.h"
#include "gb_platform_compat.h"

#ifndef _WIN32
#include <netinet/tcp.h>  /* IPPROTO_TCP / TCP_NODELAY (in winsock2 on Win32) */
#endif

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>  /* EINTR */

/* Windows spells the shutdown(2) "how" constant differently. */
#ifdef _WIN32
#  ifndef SHUT_RDWR
#    define SHUT_RDWR SD_BOTH
#  endif
#endif

#define BGB_CMD_VERSION    1
#define BGB_CMD_JOYPAD     101
#define BGB_CMD_SYNC1      104   /* master sent a byte */
#define BGB_CMD_SYNC2      105   /* slave's reply byte */
#define BGB_CMD_SYNC3      106   /* sync without data (timestamp/keepalive) */
#define BGB_CMD_STATUS     108
#define BGB_CMD_DISCONNECT 109

#define BGB_PROTO_MAJOR 1
#define BGB_PROTO_MINOR 4

#define INBOX_CAP 64

typedef struct {
    uint8_t  cmd;
    uint8_t  b2;
    uint8_t  b3;
    uint8_t  b4;
    uint32_t timestamp;
} BGBPacket;

typedef enum {
    LINK_OFF = 0,
    LINK_LISTEN,
    LINK_CONNECT,
} LinkMode;

static struct {
    LinkMode mode;
    sock_t   listen_fd;
    sock_t   peer_fd;
    char     peer_ip[64];   /* remote IP set on connect/accept; cleared on shutdown */

    /* Net thread state. Booleans are read by the main thread without
     * synchronization; on x86/ARM aligned byte loads/stores are atomic in
     * practice, and a missed transition just delays a packet by one tick. */
    volatile bool connected;
    volatile bool version_ok;
    volatile bool peer_ready;
    volatile bool shutdown_requested;

    pthread_t  thread;
    bool       thread_running;
    pthread_mutex_t inbox_mutex;
    BGBPacket  inbox[INBOX_CAP];
    int        inbox_head;
    int        inbox_tail;

    /* Touched only by main thread. */
    bool master_pending;        /* awaiting peer sync2 for our last sync1 */
    bool initial_handshake_done;/* sent version + initial sync3 yet */
} g = {
    .mode = LINK_OFF,
    .listen_fd = SOCK_INVALID,
    .peer_fd = SOCK_INVALID,
};

static void link_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[LINK] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void inbox_push_locked(BGBPacket pkt) {
    int next = (g.inbox_tail + 1) % INBOX_CAP;
    if (next == g.inbox_head) {
        /* Drop oldest to make room — better than blocking the net thread. */
        g.inbox_head = (g.inbox_head + 1) % INBOX_CAP;
    }
    g.inbox[g.inbox_tail] = pkt;
    g.inbox_tail = next;
}

static bool inbox_pop(BGBPacket* out) {
    pthread_mutex_lock(&g.inbox_mutex);
    bool ok = false;
    if (g.inbox_head != g.inbox_tail) {
        *out = g.inbox[g.inbox_head];
        g.inbox_head = (g.inbox_head + 1) % INBOX_CAP;
        ok = true;
    }
    pthread_mutex_unlock(&g.inbox_mutex);
    return ok;
}

static bool send_all(sock_t fd, const void* buf, size_t len) {
    const char* p = (const char*)buf;
    size_t sent = 0;
    while (sent < len) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, p + sent, (int)(len - sent), MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, p + sent, (int)(len - sent), 0);
#endif
        if (n <= 0) {
            if (n < 0 && sock_error() == EINTR) continue;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static bool recv_all(sock_t fd, void* buf, size_t len) {
    char* p = (char*)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, (int)(len - got), 0);
        if (n == 0) return false; /* peer closed */
        if (n < 0) {
            if (sock_error() == EINTR) continue;
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

static bool send_packet(BGBPacket pkt) {
    if (g.peer_fd == SOCK_INVALID) return false;
    uint8_t buf[8];
    buf[0] = pkt.cmd;
    buf[1] = pkt.b2;
    buf[2] = pkt.b3;
    buf[3] = pkt.b4;
    buf[4] = (uint8_t)(pkt.timestamp & 0xFF);
    buf[5] = (uint8_t)((pkt.timestamp >> 8) & 0xFF);
    buf[6] = (uint8_t)((pkt.timestamp >> 16) & 0xFF);
    buf[7] = (uint8_t)((pkt.timestamp >> 24) & 0xFF);
    return send_all(g.peer_fd, buf, 8);
}

static bool recv_packet(sock_t fd, BGBPacket* out) {
    uint8_t buf[8];
    if (!recv_all(fd, buf, 8)) return false;
    out->cmd = buf[0];
    out->b2  = buf[1];
    out->b3  = buf[2];
    out->b4  = buf[3];
    out->timestamp =
        (uint32_t)buf[4] |
        ((uint32_t)buf[5] << 8) |
        ((uint32_t)buf[6] << 16) |
        ((uint32_t)buf[7] << 24);
    return true;
}

static void* net_thread_main(void* arg) {
    (void)arg;

    if (g.mode == LINK_LISTEN) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        sock_t peer = accept(g.listen_fd, (struct sockaddr*)&client, &clen);
        if (peer == SOCK_INVALID) {
            if (!g.shutdown_requested) {
                link_log("accept() failed: %d", sock_error());
            }
            return NULL;
        }
        g.peer_fd = peer;
        snprintf(g.peer_ip, sizeof(g.peer_ip), "%s", inet_ntoa(client.sin_addr));
        link_log("Peer connected from %s:%d",
                 g.peer_ip, (int)ntohs(client.sin_port));
    }
    /* For LINK_CONNECT, peer_fd was set by start_connect(). */

    int yes = 1;
    setsockopt(g.peer_fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));

    g.connected = true;

    /* Receive loop. The main thread sends — we only consume. */
    while (!g.shutdown_requested) {
        BGBPacket pkt;
        if (!recv_packet(g.peer_fd, &pkt)) {
            if (!g.shutdown_requested) {
                link_log("Peer disconnected");
            }
            break;
        }

        if (pkt.cmd == BGB_CMD_VERSION) {
            if (pkt.b2 == BGB_PROTO_MAJOR && pkt.b3 == BGB_PROTO_MINOR) {
                g.version_ok = true;
                link_log("BGB protocol version %d.%d confirmed",
                         (int)pkt.b2, (int)pkt.b3);
            } else {
                link_log("Unsupported peer version %d.%d (need %d.%d), disconnecting",
                         (int)pkt.b2, (int)pkt.b3,
                         BGB_PROTO_MAJOR, BGB_PROTO_MINOR);
                break;
            }
            continue;
        }
        if (pkt.cmd == BGB_CMD_DISCONNECT) {
            link_log("Peer requested disconnect");
            break;
        }
        if (pkt.cmd == BGB_CMD_STATUS) {
            /* bit 0 = running. Treat anything with bit 0 set as ready. */
            g.peer_ready = (pkt.b2 & 0x01) != 0;
            /* Hand to main thread so it can echo a status back. */
            pthread_mutex_lock(&g.inbox_mutex);
            inbox_push_locked(pkt);
            pthread_mutex_unlock(&g.inbox_mutex);
            continue;
        }
        if (pkt.cmd == BGB_CMD_SYNC1 ||
            pkt.cmd == BGB_CMD_SYNC2 ||
            pkt.cmd == BGB_CMD_SYNC3) {
            pthread_mutex_lock(&g.inbox_mutex);
            inbox_push_locked(pkt);
            pthread_mutex_unlock(&g.inbox_mutex);
            continue;
        }
        /* Ignore JOYPAD (101) and any unknown commands. */
    }

    g.connected = false;
    return NULL;
}

bool gb_serial_link_start_listen(uint16_t port) {
    if (g.mode != LINK_OFF) return false;

    gb_net_global_init();
    pthread_mutex_init(&g.inbox_mutex, NULL);

    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == SOCK_INVALID) {
        link_log("socket() failed: %d", sock_error());
        return false;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        link_log("bind(:%d) failed: %d", (int)port, sock_error());
        sock_close(fd);
        return false;
    }
    if (listen(fd, 1) < 0) {
        link_log("listen() failed: %d", sock_error());
        sock_close(fd);
        return false;
    }

    g.listen_fd = fd;
    g.mode = LINK_LISTEN;
    link_log("Listening on TCP :%d (BGB protocol)", (int)port);

    if (pthread_create(&g.thread, NULL, net_thread_main, NULL) != 0) {
        link_log("pthread_create failed: %s", strerror(errno));
        sock_close(fd);
        g.listen_fd = SOCK_INVALID;
        g.mode = LINK_OFF;
        return false;
    }
    g.thread_running = true;
    return true;
}

bool gb_serial_link_start_connect(const char* host, uint16_t port) {
    if (g.mode != LINK_OFF) return false;

    gb_net_global_init();
    pthread_mutex_init(&g.inbox_mutex, NULL);

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = NULL;
    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0 || !res) {
        link_log("getaddrinfo(%s:%d) failed: %s", host, (int)port, gai_strerror(err));
        return false;
    }

    sock_t fd = SOCK_INVALID;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == SOCK_INVALID) continue;
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        sock_close(fd);
        fd = SOCK_INVALID;
    }
    freeaddrinfo(res);

    if (fd == SOCK_INVALID) {
        link_log("connect(%s:%d) failed: %d", host, (int)port, sock_error());
        return false;
    }

    g.peer_fd = fd;
    g.mode = LINK_CONNECT;
    snprintf(g.peer_ip, sizeof(g.peer_ip), "%s", host);
    link_log("Connected to %s:%d (BGB protocol)", host, (int)port);

    if (pthread_create(&g.thread, NULL, net_thread_main, NULL) != 0) {
        link_log("pthread_create failed: %s", strerror(errno));
        sock_close(fd);
        g.peer_fd = SOCK_INVALID;
        g.mode = LINK_OFF;
        return false;
    }
    g.thread_running = true;
    return true;
}

void gb_serial_link_shutdown(void) {
    if (g.mode == LINK_OFF) return;
    g.shutdown_requested = true;

    if (g.peer_fd != SOCK_INVALID && g.connected) {
        BGBPacket bye = { .cmd = BGB_CMD_DISCONNECT };
        send_packet(bye);
    }
    if (g.peer_fd != SOCK_INVALID) {
        shutdown(g.peer_fd, SHUT_RDWR);
        sock_close(g.peer_fd);
        g.peer_fd = SOCK_INVALID;
    }
    if (g.listen_fd != SOCK_INVALID) {
        shutdown(g.listen_fd, SHUT_RDWR);
        sock_close(g.listen_fd);
        g.listen_fd = SOCK_INVALID;
    }
    if (g.thread_running) {
        pthread_join(g.thread, NULL);
        g.thread_running = false;
    }
    pthread_mutex_destroy(&g.inbox_mutex);
    g.mode = LINK_OFF;
    g.connected = false;
    g.version_ok = false;
    g.peer_ready = false;
    g.master_pending = false;
    g.initial_handshake_done = false;
    g.peer_ip[0] = '\0';
}

const char* gb_serial_link_peer_ip(void) {
    return g.peer_ip;
}

void gb_serial_link_init_from_env(void) {
    const char* listen_env = getenv("GB_LINK_LISTEN");
    const char* connect_env = getenv("GB_LINK_CONNECT");

    if (listen_env && listen_env[0]) {
        unsigned port = (unsigned)strtoul(listen_env, NULL, 10);
        if (port == 0 || port > 65535) {
            link_log("GB_LINK_LISTEN: invalid port '%s'", listen_env);
            return;
        }
        gb_serial_link_start_listen((uint16_t)port);
        return;
    }

    if (connect_env && connect_env[0]) {
        /* Accept "host:port" or "host" + GB_LINK_PORT. */
        char buf[256];
        strncpy(buf, connect_env, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char* colon = strrchr(buf, ':');
        const char* host = buf;
        unsigned port = 8765; /* BGB default */
        if (colon) {
            *colon = '\0';
            port = (unsigned)strtoul(colon + 1, NULL, 10);
        } else {
            const char* port_env = getenv("GB_LINK_PORT");
            if (port_env && port_env[0]) {
                port = (unsigned)strtoul(port_env, NULL, 10);
            }
        }
        if (port == 0 || port > 65535) {
            link_log("GB_LINK_CONNECT: invalid port (%u)", port);
            return;
        }
        gb_serial_link_start_connect(host, (uint16_t)port);
    }
}

bool gb_serial_link_is_active(void) {
    return g.mode != LINK_OFF;
}

bool gb_serial_link_is_ready(void) {
    return g.mode != LINK_OFF && g.connected && g.version_ok;
}

static uint32_t bgb_timestamp_for(struct GBContext* ctx) {
    /* BGB timestamps are in 2 MiHz cycles; the CPU runs at ~4 MiHz. */
    return ctx ? (ctx->cycles >> 1) : 0;
}

static void send_initial_handshake(struct GBContext* ctx) {
    BGBPacket version = {
        .cmd = BGB_CMD_VERSION,
        .b2 = BGB_PROTO_MAJOR,
        .b3 = BGB_PROTO_MINOR,
        .b4 = 0,
        .timestamp = 0,
    };
    BGBPacket status = {
        .cmd = BGB_CMD_STATUS,
        .b2 = 0x01, /* running, not paused, no reconnect */
        .b3 = 0,
        .b4 = 0,
        .timestamp = bgb_timestamp_for(ctx),
    };
    send_packet(version);
    send_packet(status);
    g.initial_handshake_done = true;
}

void gb_serial_link_on_serial_byte(struct GBContext* ctx, uint8_t outgoing) {
    if (!gb_serial_link_is_ready()) {
        return;
    }
    BGBPacket sync1 = {
        .cmd = BGB_CMD_SYNC1,
        .b2 = outgoing,
        .b3 = 0x81, /* control bits: high, no double-speed */
        .b4 = 0,
        .timestamp = bgb_timestamp_for(ctx),
    };
    if (send_packet(sync1)) {
        ctx->serial_transfer.deferred = 1;
        g.master_pending = true;
    }
}

void gb_serial_link_tick(struct GBContext* ctx) {
    if (g.mode == LINK_OFF) return;

    /* Once the recv thread has the socket up, fire our version + status from
     * the main thread (only sender). */
    if (g.connected && !g.initial_handshake_done) {
        send_initial_handshake(ctx);
    }

    BGBPacket pkt;
    while (inbox_pop(&pkt)) {
        switch (pkt.cmd) {
        case BGB_CMD_SYNC1: {
            /* Peer was master; we should be slave. Reply with SB if armed. */
            uint8_t our_byte = 0xFF;
            bool armed = gb_serial_take_slave_byte(ctx, &our_byte);
            BGBPacket sync2 = {
                .cmd = BGB_CMD_SYNC2,
                .b2 = our_byte,
                .b3 = 0,
                .b4 = 0,
                .timestamp = bgb_timestamp_for(ctx),
            };
            send_packet(sync2);
            if (armed) {
                gb_serial_complete_transfer(ctx, pkt.b2);
            }
            break;
        }
        case BGB_CMD_SYNC2: {
            /* Reply to our master sync1. */
            if (g.master_pending) {
                gb_serial_complete_transfer(ctx, pkt.b2);
                g.master_pending = false;
            }
            break;
        }
        case BGB_CMD_SYNC3: {
            /* Keepalive — echo back. */
            BGBPacket reply = {
                .cmd = BGB_CMD_SYNC3,
                .b2 = 1,
                .b3 = 0,
                .b4 = 0,
                .timestamp = bgb_timestamp_for(ctx),
            };
            send_packet(reply);
            break;
        }
        case BGB_CMD_STATUS: {
            BGBPacket reply = {
                .cmd = BGB_CMD_STATUS,
                .b2 = 0x01,
                .b3 = 0,
                .b4 = 0,
                .timestamp = bgb_timestamp_for(ctx),
            };
            send_packet(reply);
            break;
        }
        default:
            break;
        }
    }
}
