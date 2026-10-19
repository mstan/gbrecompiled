#include "gb_debug_json.h"
#include <string.h>

typedef struct { const char* p; unsigned depth; } JsonReader;
static void space(JsonReader* r) { while (*r->p == ' ' || *r->p == '\t' || *r->p == '\r' || *r->p == '\n') ++r->p; }
static int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool hex4(JsonReader* r, unsigned* value) {
    *value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        int n = nibble(*r->p);
        if (n < 0) return false;
        ++r->p;
        *value = (*value << 4) | (unsigned)n;
    }
    return true;
}
static bool append(char* out, size_t cap, size_t* used, unsigned byte) {
    if (out) {
        if (*used + 1 >= cap) return false;
        out[*used] = (char)byte;
    }
    ++*used;
    return true;
}
static bool string(JsonReader* r, char* out, size_t cap) {
    if (*r->p++ != '"') return false;
    size_t used = 0;
    while (*r->p && *r->p != '"') {
        unsigned c = (unsigned char)*r->p++;
        if (c < 0x20) return false;
        if (c == '\\') {
            c = (unsigned char)*r->p++;
            switch (c) {
                case '"': case '\\': case '/': break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    if (!hex4(r, &c) || !c) return false;
                    if (c >= 0xd800 && c <= 0xdbff) {
                        if (r->p[0] != '\\' || r->p[1] != 'u') return false;
                        r->p += 2;
                        unsigned low;
                        if (!hex4(r, &low) || low < 0xdc00 || low > 0xdfff) return false;
                        c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
                    } else if (c >= 0xdc00 && c <= 0xdfff) return false;
                    if (c >= 0x10000 && !append(out, cap, &used, 0xf0 | (c >> 18))) return false;
                    if (c >= 0x800 && !append(out, cap, &used, c >= 0x10000 ? 0x80 | ((c >> 12) & 63) : 0xe0 | (c >> 12))) return false;
                    if (c >= 0x80 && !append(out, cap, &used, c >= 0x800 ? 0x80 | ((c >> 6) & 63) : 0xc0 | (c >> 6))) return false;
                    c = c >= 0x80 ? 0x80 | (c & 63) : c;
                    break;
                }
                default: return false;
            }
        }
        if (!append(out, cap, &used, c)) return false;
    }
    if (*r->p != '"') return false;
    ++r->p;
    if (out) { if (used >= cap) return false; out[used] = 0; }
    return true;
}
static bool value(JsonReader* r, char* out, size_t cap) {
    space(r);
    if (*r->p == '"') return string(r, out, cap);
    if (*r->p == '{' || *r->p == '[') {
        if (out || ++r->depth > 32) return false;
        char close = *r->p++ == '{' ? '}' : ']';
        space(r);
        if (*r->p != close) for (;;) {
            if (close == '}') {
                if (*r->p != '"' || !string(r, NULL, 0)) return false;
                space(r);
                if (*r->p++ != ':') return false;
            }
            if (!value(r, NULL, 0)) return false;
            space(r);
            if (*r->p != ',') break;
            ++r->p;
            space(r);
        }
        if (*r->p++ != close) return false;
        --r->depth;
        return true;
    }
    const char* begin = r->p;
    if (!strncmp(r->p, "true", 4) || !strncmp(r->p, "null", 4)) r->p += 4;
    else if (!strncmp(r->p, "false", 5)) r->p += 5;
    else {
        if (*r->p == '-') ++r->p;
        if (*r->p == '0') ++r->p;
        else {
            if (*r->p < '1' || *r->p > '9') return false;
            while (*r->p >= '0' && *r->p <= '9') ++r->p;
        }
        if (*r->p == '.') {
            ++r->p;
            if (*r->p < '0' || *r->p > '9') return false;
            while (*r->p >= '0' && *r->p <= '9') ++r->p;
        }
        if (*r->p == 'e' || *r->p == 'E') {
            ++r->p;
            if (*r->p == '+' || *r->p == '-') ++r->p;
            if (*r->p < '0' || *r->p > '9') return false;
            while (*r->p >= '0' && *r->p <= '9') ++r->p;
        }
    }
    size_t len = (size_t)(r->p - begin);
    if (out) { if (len >= cap) return false; memcpy(out, begin, len); out[len] = 0; }
    return true;
}
static bool object(const char* json, const char* key, char* out, size_t cap) {
    if (!json || (key && (!out || !cap))) return false;
    JsonReader r = {json, 1};
    space(&r);
    if (*r.p++ != '{') return false;
    bool found = false;
    char seen[64][128];
    unsigned members = 0;
    space(&r);
    if (*r.p != '}') for (;;) {
        char member[128];
        if (*r.p != '"' || !string(&r, member, sizeof(member))) return false;
        if (members == 64) return false;
        for (unsigned i = 0; i < members; ++i) if (!strcmp(member, seen[i])) return false;
        strcpy(seen[members++], member);
        space(&r);
        if (*r.p++ != ':') return false;
        bool selected = key && !strcmp(member, key);
        if (selected && found) return false;
        if (!value(&r, selected ? out : NULL, selected ? cap : 0)) return false;
        found |= selected;
        space(&r);
        if (*r.p != ',') break;
        ++r.p;
        space(&r);
    }
    if (*r.p++ != '}') return false;
    space(&r);
    return !*r.p && (!key || found);
}
bool gb_debug_json_get(const char* json, const char* key, char* out, size_t capacity) {
    return key && object(json, key, out, capacity);
}
bool gb_debug_json_valid(const char* json) { return object(json, NULL, NULL, 0); }
