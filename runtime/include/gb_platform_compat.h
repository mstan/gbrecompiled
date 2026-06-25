/*
 * gb_platform_compat.h -- shared Windows/POSIX portability layer.
 *
 * One vocabulary for sockets and directory creation across the runtime.
 * Modeled on the local compat block that previously lived in debug_server.c;
 * all networking modules now share this single header.
 *
 *   sock_t            socket descriptor type (SOCKET on Win32, int on POSIX)
 *   SOCK_INVALID      invalid-descriptor sentinel
 *   sock_close(fd)    close a socket (closesocket on Win32)
 *   SOCK_WOULDBLOCK   "would block" error code
 *   sock_error()      last socket error (WSAGetLastError / errno)
 *   gb_net_global_init()  idempotent WSAStartup on Win32, no-op elsewhere
 *   gb_mkdir(path)    create a directory (mode 0755 on POSIX)
 */
#ifndef GB_PLATFORM_COMPAT_H
#define GB_PLATFORM_COMPAT_H

/* ---- Platform sockets ---- */
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define sock_close closesocket
#  define SOCK_WOULDBLOCK WSAEWOULDBLOCK
   static inline int sock_error(void) { return WSAGetLastError(); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
   typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define sock_close close
#  define SOCK_WOULDBLOCK EWOULDBLOCK
   static inline int sock_error(void) { return errno; }
#endif

/* ---- Winsock global init ---- *
 * Winsock requires WSAStartup before any socket call. Idempotent and safe
 * to call from every networking module's init path. No-op on POSIX. */
static inline void gb_net_global_init(void)
{
#ifdef _WIN32
    static int s_initialized = 0;
    if (!s_initialized) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
            s_initialized = 1;
        }
    }
#endif
}

/* ---- Portable directory create ---- *
 * Returns 0 on success, -1 on failure (matching mkdir/_mkdir). Sets errno. */
#ifdef _WIN32
#  include <direct.h>
static inline int gb_mkdir(const char *path) { return _mkdir(path); }
#else
#  include <sys/stat.h>
#  include <sys/types.h>
static inline int gb_mkdir(const char *path) { return mkdir(path, 0755); }
#endif

#endif /* GB_PLATFORM_COMPAT_H */
