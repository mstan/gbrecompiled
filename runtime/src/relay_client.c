/*
 * See relay_client.h. libcurl-backed HTTP client + a tiny extractor for
 * the flat JSON shape the rendezvous server returns. We stay
 * synchronous and small on purpose — the responses are short, the
 * traffic is one-shot during pairing, and we only need to read four
 * fields out of "peer" plus an "ok" / "error".
 */
#include "relay_client.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

/* --- response buffer -------------------------------------------------- */

typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} RecvBuf;

static size_t recv_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    RecvBuf* buf = (RecvBuf*)userdata;
    size_t add = size * nmemb;
    if (buf->len + add + 1 > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap * 2 : 256;
        while (new_cap < buf->len + add + 1) new_cap *= 2;
        char* p = (char*)realloc(buf->data, new_cap);
        if (!p) return 0;
        buf->data = p;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, add);
    buf->len += add;
    buf->data[buf->len] = '\0';
    return add;
}

static void recvbuf_free(RecvBuf* buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = buf->cap = 0;
}

/* --- url helpers ------------------------------------------------------ */

static void rstrip_slash(char* s) {
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/') s[--n] = '\0';
}

static bool build_url(char* out, size_t out_size,
                      const char* base, const char* path) {
    if (!base || !*base) return false;
    char trimmed[512];
    snprintf(trimmed, sizeof(trimmed), "%s", base);
    rstrip_slash(trimmed);
    snprintf(out, out_size, "%s%s", trimmed, path);
    return true;
}

/* --- minimal JSON field extraction ------------------------------------
 * Our responses are deliberately flat strings, ints, bools, and one
 * nested "peer" object that's either null or {ip,port,nickname,role}.
 * A full JSON parser is overkill; we just locate the named key and read
 * the literal that follows. Strings here never contain escapes, so we
 * don't bother with unescaping. */

static const char* find_key(const char* json, const char* key) {
    if (!json) return NULL;
    char pat[64];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(pat)) return NULL;
    return strstr(json, pat);
}

/* Skip past a key match, the colon, and any whitespace. */
static const char* skip_to_value(const char* p, const char* key) {
    if (!p) return NULL;
    p += strlen(key) + 2;  /* the quoted key including its quotes */
    while (*p && *p != ':') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static bool extract_string(const char* json, const char* key,
                           char* out, size_t out_size) {
    const char* p = find_key(json, key);
    if (!p) return false;
    p = skip_to_value(p, key);
    if (!p || *p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        if (i + 1 < out_size) out[i++] = *p;
        p++;
    }
    if (i < out_size) out[i] = '\0';
    else if (out_size) out[out_size - 1] = '\0';
    return *p == '"';
}

static bool extract_int(const char* json, const char* key, int* out) {
    const char* p = find_key(json, key);
    if (!p) return false;
    p = skip_to_value(p, key);
    if (!p) return false;
    char* end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = (int)v;
    return true;
}

static bool peer_is_null(const char* json) {
    const char* p = find_key(json, "peer");
    if (!p) return true;
    p = skip_to_value(p, "peer");
    if (!p) return true;
    return strncmp(p, "null", 4) == 0;
}

static const char* peer_object(const char* json) {
    const char* p = find_key(json, "peer");
    if (!p) return NULL;
    p = skip_to_value(p, "peer");
    if (!p || *p != '{') return NULL;
    return p;
}

static void fill_peer(GBRelayPeer* peer, const char* json) {
    memset(peer, 0, sizeof(*peer));
    if (!json) return;
    if (peer_is_null(json)) return;
    const char* obj = peer_object(json);
    if (!obj) return;
    /* Find a closing brace so we don't accidentally read fields from
     * outside the peer object. */
    int depth = 0;
    const char* end = obj;
    while (*end) {
        if (*end == '{') depth++;
        else if (*end == '}') { depth--; if (depth == 0) { end++; break; } }
        end++;
    }
    /* Use a temporary nul-terminated copy of the peer object. */
    size_t obj_len = (size_t)(end - obj);
    char tmp[512];
    if (obj_len >= sizeof(tmp)) obj_len = sizeof(tmp) - 1;
    memcpy(tmp, obj, obj_len);
    tmp[obj_len] = '\0';

    if (extract_string(tmp, "ip", peer->ip, sizeof(peer->ip)) &&
        extract_int(tmp, "port", &peer->port)) {
        peer->has_peer = true;
    }
    extract_string(tmp, "nickname", peer->nickname, sizeof(peer->nickname));
}

static void fill_error(GBRelayResult* res, const char* json) {
    if (!json) return;
    extract_string(json, "error", res->error, sizeof(res->error));
}

/* --- request helpers -------------------------------------------------- */

static void result_init(GBRelayResult* res) {
    memset(res, 0, sizeof(*res));
}

static void result_transport_error(GBRelayResult* res, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(res->error, sizeof(res->error), fmt, ap);
    va_end(ap);
}

typedef enum { HTTP_GET, HTTP_POST, HTTP_DELETE } HttpMethod;

static GBRelayResult http_call(HttpMethod method,
                               const char* url,
                               const char* json_body) {
    GBRelayResult res;
    result_init(&res);

    CURL* curl = curl_easy_init();
    if (!curl) {
        result_transport_error(&res, "curl_easy_init failed");
        return res;
    }

    RecvBuf buf = {0};
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    if (json_body) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, recv_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    /* Keep the synchronous freeze short — we run on the UI thread and
     * 8 s with no network feels like a lockup. The server does a small
     * SQLite write + return, so successful calls finish in well under
     * a second; a 4 s overall budget is plenty. */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "gb-recompiled/1.0 (relay)");

    switch (method) {
        case HTTP_POST:
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body ? json_body : "");
            break;
        case HTTP_DELETE:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
            break;
        case HTTP_GET:
        default:
            break;
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        result_transport_error(&res, "%s", curl_easy_strerror(rc));
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        res.http_status = (int)status;
        if (status >= 200 && status < 300) {
            res.ok = true;
            fill_peer(&res.peer, buf.data);
        } else {
            fill_error(&res, buf.data);
            if (!res.error[0]) {
                snprintf(res.error, sizeof(res.error),
                         "HTTP %ld", status);
            }
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    recvbuf_free(&buf);
    return res;
}

/* --- public API ------------------------------------------------------- */

static bool sanitize_room_code(const char* code) {
    if (!code || !*code) return false;
    size_t n = strlen(code);
    if (n < 2 || n > 32) return false;
    for (size_t i = 0; i < n; i++) {
        char c = code[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return false;
    }
    return true;
}

static void escape_json_string(const char* in, char* out, size_t out_size) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (j + 3 >= out_size) break;
            out[j++] = '\\';
            out[j++] = (char)c;
        } else if (c < 0x20) {
            /* skip control chars rather than emit \uXXXX */
            continue;
        } else {
            out[j++] = (char)c;
        }
    }
    if (j < out_size) out[j] = '\0';
    else if (out_size) out[out_size - 1] = '\0';
}

GBRelayResult gb_relay_register(const char* server_url,
                                const char* room_code,
                                const char* role,
                                int port,
                                const char* nickname,
                                const char* uuid) {
    GBRelayResult res;
    result_init(&res);
    if (!sanitize_room_code(room_code)) {
        result_transport_error(&res, "invalid room code");
        return res;
    }
    if (!role || (strcmp(role, "host") != 0 && strcmp(role, "join") != 0)) {
        result_transport_error(&res, "invalid role");
        return res;
    }
    if (port < 1 || port > 65535) {
        result_transport_error(&res, "port out of range");
        return res;
    }

    char url[768];
    char path[128];
    snprintf(path, sizeof(path), "/v1/room/%s", room_code);
    if (!build_url(url, sizeof(url), server_url, path)) {
        result_transport_error(&res, "invalid server URL");
        return res;
    }

    char esc_nick[GB_RELAY_NICKNAME_LEN * 2 + 8];
    escape_json_string(nickname ? nickname : "", esc_nick, sizeof(esc_nick));
    /* Whitelist-gated servers want our UUID; open servers ignore it.
     * Send "" when the caller didn't supply one — the server treats a
     * blank uuid as "not provided" and only refuses if it has a
     * whitelist active. */
    char esc_uuid[80];
    escape_json_string(uuid ? uuid : "", esc_uuid, sizeof(esc_uuid));
    char body[640];
    snprintf(body, sizeof(body),
             "{\"role\":\"%s\",\"port\":%d,\"nickname\":\"%s\",\"uuid\":\"%s\"}",
             role, port, esc_nick, esc_uuid);

    return http_call(HTTP_POST, url, body);
}

GBRelayResult gb_relay_lookup(const char* server_url,
                              const char* room_code,
                              const char* role) {
    GBRelayResult res;
    result_init(&res);
    if (!sanitize_room_code(room_code)) {
        result_transport_error(&res, "invalid room code");
        return res;
    }
    if (!role || (strcmp(role, "host") != 0 && strcmp(role, "join") != 0)) {
        result_transport_error(&res, "invalid role");
        return res;
    }

    char url[768];
    char path[160];
    snprintf(path, sizeof(path), "/v1/room/%s?role=%s", room_code, role);
    if (!build_url(url, sizeof(url), server_url, path)) {
        result_transport_error(&res, "invalid server URL");
        return res;
    }

    return http_call(HTTP_GET, url, NULL);
}

void gb_relay_unregister(const char* server_url,
                         const char* room_code,
                         const char* role) {
    if (!sanitize_room_code(room_code)) return;
    if (!role) return;
    if (strcmp(role, "host") != 0 && strcmp(role, "join") != 0) return;

    char url[768];
    char path[160];
    snprintf(path, sizeof(path), "/v1/room/%s?role=%s", room_code, role);
    if (!build_url(url, sizeof(url), server_url, path)) return;

    GBRelayResult res = http_call(HTTP_DELETE, url, NULL);
    (void)res;  /* best-effort */
}

/* --- list rooms ------------------------------------------------------ */

/* Walks "rooms": [...] looking for the {code,nickname,age_seconds}
 * objects. Stops after `max` entries or end of array. */
static int parse_room_list(const char* json,
                           GBRelayRoomInfo* out, int max) {
    if (!json) return 0;
    const char* p = strstr(json, "\"rooms\"");
    if (!p) return 0;
    p = skip_to_value(p, "rooms");
    if (!p || *p != '[') return 0;
    p++;
    int count = 0;
    while (*p && count < max) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ']') break;
        if (*p != '{') break;
        /* find matching close brace */
        int depth = 0;
        const char* obj_start = p;
        const char* obj_end = p;
        while (*obj_end) {
            if (*obj_end == '{') depth++;
            else if (*obj_end == '}') { depth--; if (depth == 0) { obj_end++; break; } }
            obj_end++;
        }
        size_t obj_len = (size_t)(obj_end - obj_start);
        char tmp[512];
        if (obj_len >= sizeof(tmp)) obj_len = sizeof(tmp) - 1;
        memcpy(tmp, obj_start, obj_len);
        tmp[obj_len] = '\0';

        GBRelayRoomInfo* room = &out[count];
        memset(room, 0, sizeof(*room));
        bool ok = extract_string(tmp, "code", room->code, sizeof(room->code));
        extract_string(tmp, "nickname", room->nickname, sizeof(room->nickname));
        extract_int(tmp, "age_seconds", &room->age_seconds);
        if (ok) count++;

        p = obj_end;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == ',') p++;
    }
    return count;
}

GBRelayListResult gb_relay_list_rooms(const char* server_url,
                                      GBRelayRoomInfo* out_rooms,
                                      int max) {
    GBRelayListResult res;
    memset(&res, 0, sizeof(res));

    char url[768];
    if (!build_url(url, sizeof(url), server_url, "/v1/rooms")) {
        snprintf(res.error, sizeof(res.error), "invalid server URL");
        return res;
    }

    /* Reuse the http_call infrastructure but we need the raw body to
     * walk the rooms array — http_call already throws away the body,
     * so do a small inline curl handle here. */
    CURL* curl = curl_easy_init();
    if (!curl) {
        snprintf(res.error, sizeof(res.error), "curl_easy_init failed");
        return res;
    }

    RecvBuf buf = {0};
    struct curl_slist* headers = curl_slist_append(NULL, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, recv_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    /* Keep the synchronous freeze short — we run on the UI thread and
     * 8 s with no network feels like a lockup. The server does a small
     * SQLite write + return, so successful calls finish in well under
     * a second; a 4 s overall budget is plenty. */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "gb-recompiled/1.0 (relay)");

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        snprintf(res.error, sizeof(res.error), "%s", curl_easy_strerror(rc));
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        res.http_status = (int)status;
        if (status >= 200 && status < 300) {
            res.ok = true;
            res.count = parse_room_list(buf.data, out_rooms, max);
        } else {
            extract_string(buf.data, "error", res.error, sizeof(res.error));
            if (!res.error[0]) snprintf(res.error, sizeof(res.error), "HTTP %ld", status);
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    recvbuf_free(&buf);
    return res;
}
