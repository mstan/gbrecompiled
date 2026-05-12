/**
 * @file mock_inject_file.c
 * @brief File-based Pokemon injectors. See header for format.
 */

#include "pokemon/mock_inject_file.h"
#include "pokemon/mock_gen1.h"
#include "pokemon/mock_gen2.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

static int file_has_ext(const char* name, const char* ext) {
    size_t nl = strlen(name), el = strlen(ext);
    if (nl < el) return 0;
    /* Case-insensitive extension compare. */
    for (size_t i = 0; i < el; i++) {
        int a = (unsigned char)name[nl - el + i];
        int b = (unsigned char)ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/* Strip a known trailing extension and return its length. Returns 0
 * if no matching extension found. */
static size_t ext_length(const char* name) {
    if (file_has_ext(name, ".gbmon")) return 6;
    if (file_has_ext(name, ".pk1")  ) return 4;
    if (file_has_ext(name, ".pk2")  ) return 4;
    return 0;
}

/* Decode 11 bytes of cart-charmap into ASCII (NUL-terminated, up to
 * out_size). Stops at the cart's 0x50 terminator. */
static void decode_charmap_buf(const uint8_t* in, size_t in_len,
                               char* out, size_t out_size) {
    size_t o = 0;
    for (size_t i = 0; i < in_len && o + 1 < out_size; i++) {
        uint8_t c = in[i];
        if (c == 0x50) break;
        char ch = '?';
        if (c >= 0x80 && c <= 0x99)      ch = (char)('A' + (c - 0x80));
        else if (c >= 0xA0 && c <= 0xB9) ch = (char)('a' + (c - 0xA0));
        else if (c == 0x7F)              ch = ' ';
        out[o++] = ch;
    }
    out[o] = '\0';
}

/* Build the friendly display label for an entry. Peeks at the file
 * contents — for binary formats decodes the species byte via the
 * cart's name table (PK1 uses internal-ID-indexed MonsterNames, PK2
 * uses dex-indexed PokemonNames), then appends level + OT info. For
 * .gbmon, lightly parses the text for species/level/shiny.
 *
 * Falls back to the bare filename-without-extension when we can't
 * peek (no cart context, unsupported file size, etc.). */
static void build_display(const GBContext* ctx,
                          const char* filename, const char* full_path,
                          char* out, size_t out_size) {
    size_t ext_len = ext_length(filename);
    size_t nlen = strlen(filename);
    size_t base_len = (nlen > ext_len) ? nlen - ext_len : nlen;
    /* Fallback default: bare filename. */
    snprintf(out, out_size, "%.*s", (int)base_len, filename);
    if (!ctx) return;

    FILE* f = fopen(full_path, "rb");
    if (!f) return;
    uint8_t buf[128];
    size_t got = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    bool is_pk1 = file_has_ext(filename, ".pk1");
    bool is_pk2 = file_has_ext(filename, ".pk2");

    if (is_pk1 || is_pk2) {
        bool is_pk2_file = is_pk2;
        bool jp = (got == 59 || got == 63);
        bool intl = (got == 69 || got == 73);
        if (!jp && !intl) return;

        size_t body_off = 3;
        size_t name_len = jp ? 6 : 11;
        size_t body_size = is_pk2_file ? 48 : 44;
        if (got < body_off + body_size + 2 * name_len) return;

        uint8_t species_byte = buf[body_off + 0];
        char species_name[12] = "?";
        if (is_pk2_file) {
            gb_mock_gen2_species_name(ctx, species_byte,
                                       species_name, sizeof(species_name));
        } else {
            gb_mock_gen1_species_name_by_internal(ctx, species_byte,
                                                  species_name, sizeof(species_name));
        }
        /* Level: pk1 body[33] (party Level), pk2 body[31] (Level). */
        int level = is_pk2_file ? buf[body_off + 31] : buf[body_off + 33];
        /* OT ID: pk1 body[12..13] BE, pk2 body[6..7] BE. */
        size_t otid_off = body_off + (is_pk2_file ? 6 : 12);
        unsigned otid = ((unsigned)buf[otid_off] << 8) | buf[otid_off + 1];
        /* OT name lives at body_off + body_size. */
        char ot_name[16];
        decode_charmap_buf(buf + body_off + body_size, name_len,
                           ot_name, sizeof(ot_name));

        if (ot_name[0]) {
            snprintf(out, out_size, "%s lvl%d - %s/%05u",
                     species_name, level, ot_name, otid);
        } else {
            snprintf(out, out_size, "%s lvl%d", species_name, level);
        }
        return;
    }

    if (file_has_ext(filename, ".gbmon")) {
        /* Light parse — find species and level lines. */
        char species[32] = "";
        int level = -1;
        bool shiny = false;
        char* p = (char*)buf;
        char* end = (char*)buf + got;
        char line[96];
        while (p < end) {
            char* nl = (char*)memchr(p, '\n', (size_t)(end - p));
            size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
            if (n >= sizeof(line)) n = sizeof(line) - 1;
            memcpy(line, p, n);
            line[n] = '\0';
            p = nl ? nl + 1 : end;
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            char* key = line;
            char* val = eq + 1;
            while (*key == ' ' || *key == '\t') key++;
            char* ke = key + strlen(key);
            while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
            *ke = '\0';
            while (*val == ' ' || *val == '\t') val++;
            char* ve = val + strlen(val);
            while (ve > val && (ve[-1] == ' ' || ve[-1] == '\t' ||
                                ve[-1] == '\r')) ve--;
            *ve = '\0';
            char* hash = strchr(val, '#');
            if (hash) { *hash = '\0';
                        while (hash > val && hash[-1] == ' ') *--hash = '\0'; }
            if (strcasecmp(key, "species") == 0) {
                snprintf(species, sizeof(species), "%s", val);
            } else if (strcasecmp(key, "level") == 0) {
                level = atoi(val);
            } else if (strcasecmp(key, "shiny") == 0) {
                shiny = (strcasecmp(val, "true") == 0 ||
                         strcasecmp(val, "yes")  == 0 ||
                         strcasecmp(val, "1")    == 0);
            }
        }
        if (species[0] && level > 0) {
            snprintf(out, out_size, "%s lvl%d%s",
                     species, level, shiny ? " shiny" : "");
        } else if (species[0]) {
            snprintf(out, out_size, "%s", species);
        }
        /* else: keep the filename fallback. */
    }
}

int gb_inject_file_scan(const GBContext* ctx, const char* game_id,
                        GBInjectFileEntry* out, int max) {
    if (!game_id || !out || max <= 0) return 0;

    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "injects/%s", game_id);

    DIR* d = opendir(dir_path);
    if (!d) return 0;

    int count = 0;
    struct dirent* ent;
    while (count < max && (ent = readdir(d)) != NULL) {
        if (ext_length(ent->d_name) == 0) continue;

        GBInjectFileEntry* e = &out[count];
        snprintf(e->filename, sizeof(e->filename), "%s", ent->d_name);
        snprintf(e->full_path, sizeof(e->full_path),
                 "%s/%s", dir_path, ent->d_name);
        build_display(ctx, ent->d_name, e->full_path,
                      e->display, sizeof(e->display));
        count++;
    }
    closedir(d);
    return count;
}

/* ---- .gbmon parser ---- */

typedef struct {
    char  species_name[32];   /* either "MEWTWO" or stringified dex# */
    int   species_dex;        /* -1 if unset */
    int   level;              /* -1 if unset */
    int   shiny;              /* 0/1, -1 if unset */
    char  nickname[16];       /* '\0' if unset */
} ParsedMon;

static void parsed_init(ParsedMon* p) {
    memset(p, 0, sizeof(*p));
    p->species_dex = -1;
    p->level = -1;
    p->shiny = -1;
}

static void strip_inplace(char* s) {
    /* trim leading whitespace */
    char* p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    /* trim trailing whitespace, comments */
    char* end = s + strlen(s);
    char* hash = strchr(s, '#');
    if (hash) end = hash;
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
}

static bool parse_bool(const char* v) {
    return (strcasecmp(v, "true") == 0 ||
            strcasecmp(v, "yes")  == 0 ||
            strcasecmp(v, "1")    == 0);
}

static bool parse_file(const char* path, ParsedMon* out) {
    parsed_init(out);
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        strip_inplace(line);
        if (!line[0]) continue;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = line;
        char* val = eq + 1;
        strip_inplace(key);
        strip_inplace(val);

        if (strcasecmp(key, "species") == 0) {
            /* Numeric dex# or ASCII name. */
            if (val[0] >= '0' && val[0] <= '9') {
                out->species_dex = atoi(val);
            } else {
                snprintf(out->species_name, sizeof(out->species_name),
                         "%s", val);
            }
        } else if (strcasecmp(key, "level") == 0) {
            out->level = atoi(val);
        } else if (strcasecmp(key, "shiny") == 0) {
            out->shiny = parse_bool(val) ? 1 : 0;
        } else if (strcasecmp(key, "nickname") == 0) {
            snprintf(out->nickname, sizeof(out->nickname), "%s", val);
        }
        /* Unknown keys silently skipped — forward-compat. */
    }
    fclose(f);
    return true;
}

/* Encode an ASCII nickname into Gen 1/2 charmap bytes (uppercase
 * letters A=$80..Z=$99, lowercase a=$A0..z=$B9, space=$7F,
 * terminator=$50). Writes up to 10 chars + terminator. */
static void encode_charmap(const char* ascii, uint8_t* out, size_t out_len) {
    size_t i;
    memset(out, 0x50, out_len);
    for (i = 0; i < 10 && i < out_len && ascii[i]; i++) {
        char c = ascii[i];
        if (c >= 'A' && c <= 'Z')      out[i] = 0x80 + (c - 'A');
        else if (c >= 'a' && c <= 'z') out[i] = 0xA0 + (c - 'a');
        else if (c == ' ')             out[i] = 0x7F;
        else                            out[i] = 0x80; /* fallback 'A' */
    }
}

/* ---- Direct-write helpers — for binary .pk1 / .pk2 the body already
 *      matches the cart's party_struct, so injection is just memcpy.
 *      Wraps the per-gen WRAM offsets via the existing accessors. */

extern uint8_t* gb_mock_gen2_party_mons_slot(GBContext*, int);   /* declared below */
extern uint8_t* gb_mock_gen2_party_ots_slot(GBContext*, int);
extern uint8_t* gb_mock_gen2_party_species_slot(GBContext*, int);
extern uint8_t  gb_mock_gen2_party_count_inc(GBContext*);
extern uint8_t* gb_mock_gen1_party_mons_slot(GBContext*, int);
extern uint8_t* gb_mock_gen1_party_ots_slot(GBContext*, int);
extern uint8_t* gb_mock_gen1_party_species_slot(GBContext*, int);
extern uint8_t  gb_mock_gen1_party_count_inc(GBContext*);

typedef enum {
    BIN_FMT_INVALID = 0,
    BIN_FMT_PK1_JP,    /* 59 bytes,  6-byte names */
    BIN_FMT_PK1_INTL,  /* 69 bytes, 11-byte names */
    BIN_FMT_PK2_JP,    /* 63 bytes */
    BIN_FMT_PK2_INTL,  /* 73 bytes */
} BinFormat;

static BinFormat detect_bin_format(const char* path, size_t file_size) {
    bool is_pk2 = file_has_ext(path, ".pk2");
    bool is_pk1 = file_has_ext(path, ".pk1");
    if (is_pk1 && file_size == 59) return BIN_FMT_PK1_JP;
    if (is_pk1 && file_size == 69) return BIN_FMT_PK1_INTL;
    if (is_pk2 && file_size == 63) return BIN_FMT_PK2_JP;
    if (is_pk2 && file_size == 73) return BIN_FMT_PK2_INTL;
    return BIN_FMT_INVALID;
}

static bool apply_binary_file(GBContext* ctx, const GBInjectFileEntry* entry,
                              char* out_err, size_t err_size) {
    FILE* f = fopen(entry->full_path, "rb");
    if (!f) {
        if (out_err) snprintf(out_err, err_size, "open failed");
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 1024) {
        fclose(f);
        if (out_err) snprintf(out_err, err_size, "bad file size");
        return false;
    }
    uint8_t buf[128];
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        if (out_err) snprintf(out_err, err_size, "short read");
        return false;
    }

    BinFormat fmt = detect_bin_format(entry->filename, got);
    if (fmt == BIN_FMT_INVALID) {
        if (out_err) snprintf(out_err, err_size,
                              "unrecognized .pk1/.pk2 size %zu", got);
        return false;
    }

    /* List wrapper:
     *   [0] count (=1 for single-entry files)
     *   [1] species byte (matches body[0])
     *   [2] 0xFF terminator
     *   [3..body_end] body (44 PK1 / 48 PK2)
     *   [body_end..]  OT name (6 JP / 11 INTL)
     *   [..]          Nickname (6 JP / 11 INTL)
     */
    bool is_gen2 = (fmt == BIN_FMT_PK2_JP || fmt == BIN_FMT_PK2_INTL);
    bool jp      = (fmt == BIN_FMT_PK1_JP || fmt == BIN_FMT_PK2_JP);
    size_t body_size = is_gen2 ? 48 : 44;
    size_t name_size = jp ? 6 : 11;

    GBGen2Game g2 = gb_mock_gen2_detect(ctx);
    GBGen1Game g1 = (g2 == GB_MOCK_GEN2_NONE) ? gb_mock_gen1_detect(ctx)
                                              : GB_MOCK_GEN1_NONE;
    bool cart_is_gen2 = (g2 != GB_MOCK_GEN2_NONE);
    bool cart_is_gen1 = (g1 != GB_MOCK_GEN1_NONE);

    /* Generation mismatch is fatal — a Gen 2 mon's struct is laid
     * out completely differently than Gen 1's. */
    if (is_gen2 && !cart_is_gen2) {
        if (out_err) snprintf(out_err, err_size,
                              "pk2 file but cart isn't Gen 2");
        return false;
    }
    if (!is_gen2 && !cart_is_gen1) {
        if (out_err) snprintf(out_err, err_size,
                              "pk1 file but cart isn't Gen 1");
        return false;
    }

    uint8_t pc = is_gen2 ? gb_mock_gen2_party_count(ctx)
                         : gb_mock_gen1_party_count(ctx);
    if (pc >= 6) {
        if (out_err) snprintf(out_err, err_size, "party full");
        return false;
    }

    const uint8_t* body     = buf + 3;
    const uint8_t* ot_name  = buf + 3 + body_size;
    const uint8_t* nickname = buf + 3 + body_size + name_size;

    /* Direct write: body matches party_struct byte-for-byte. */
    uint8_t* mon_dst  = is_gen2 ? gb_mock_gen2_party_mons_slot(ctx, pc)
                                : gb_mock_gen1_party_mons_slot(ctx, pc);
    uint8_t* ot_dst   = is_gen2 ? gb_mock_gen2_party_ots_slot(ctx, pc)
                                : gb_mock_gen1_party_ots_slot(ctx, pc);
    uint8_t* nick_dst = is_gen2 ? gb_mock_gen2_nick_slot(ctx, pc)
                                : gb_mock_gen1_nick_slot(ctx, pc);
    uint8_t* spec_dst = is_gen2 ? gb_mock_gen2_party_species_slot(ctx, pc)
                                : gb_mock_gen1_party_species_slot(ctx, pc);
    if (!mon_dst || !ot_dst || !nick_dst || !spec_dst) {
        if (out_err) snprintf(out_err, err_size, "wram lookup failed");
        return false;
    }
    memcpy(mon_dst, body, body_size);

    /* Names: JP files only have 6 bytes; pad with terminator. The
     * cart always reads 11 bytes regardless, so left-aligning into
     * an 11-byte buffer with 0x50 fill is correct. */
    memset(ot_dst,   0x50, 11);
    memset(nick_dst, 0x50, 11);
    memcpy(ot_dst,   ot_name,  name_size);
    memcpy(nick_dst, nickname, name_size);

    /* Species list — write the species byte from the body and the
     * terminator at the next slot. */
    spec_dst[0] = body[0];
    /* The slot AFTER party_count needs the terminator. Since
     * party_species_slot returns &species[pc], the next byte is
     * &species[pc+1], same array, +1. */
    spec_dst[1] = 0xFF;

    /* Bump party_count last. */
    if (is_gen2) (void)gb_mock_gen2_party_count_inc(ctx);
    else         (void)gb_mock_gen1_party_count_inc(ctx);

    fprintf(stderr, "[inject_file] applied %s (%s) → slot %d\n",
            entry->filename,
            fmt == BIN_FMT_PK1_JP   ? "pk1 jp"  :
            fmt == BIN_FMT_PK1_INTL ? "pk1 intl":
            fmt == BIN_FMT_PK2_JP   ? "pk2 jp"  : "pk2 intl",
            pc);
    return true;
}

bool gb_inject_file_apply(GBContext* ctx, const GBInjectFileEntry* entry,
                          char* out_err, size_t err_size) {
    if (out_err && err_size) out_err[0] = '\0';
    if (!ctx || !entry) {
        if (out_err) snprintf(out_err, err_size, "no context");
        return false;
    }

    /* Binary formats route through the direct-copy path; the body
     * layout in pk1/pk2 already matches the cart's party_struct
     * exactly, so no stat math needed. */
    if (file_has_ext(entry->filename, ".pk1") ||
        file_has_ext(entry->filename, ".pk2")) {
        return apply_binary_file(ctx, entry, out_err, err_size);
    }

    /* .gbmon — text format, routes through the builder for
     * cart-driven stat / move fill-in. */
    ParsedMon p;
    if (!parse_file(entry->full_path, &p)) {
        if (out_err) snprintf(out_err, err_size,
                              "failed to open %s", entry->filename);
        return false;
    }
    if (p.level < 0)  p.level = 50;
    if (p.shiny < 0)  p.shiny = 0;

    GBGen2Game g2 = gb_mock_gen2_detect(ctx);
    GBGen1Game g1 = (g2 == GB_MOCK_GEN2_NONE) ? gb_mock_gen1_detect(ctx)
                                              : GB_MOCK_GEN1_NONE;
    if (g2 == GB_MOCK_GEN2_NONE && g1 == GB_MOCK_GEN1_NONE) {
        if (out_err) snprintf(out_err, err_size,
                              "not a Pokemon cart");
        return false;
    }

    /* Resolve species name → dex if numeric not given. */
    int dex = p.species_dex;
    if (dex < 0 && p.species_name[0]) {
        dex = (g2 != GB_MOCK_GEN2_NONE)
            ? gb_mock_gen2_dex_for_name(ctx, p.species_name)
            : gb_mock_gen1_dex_for_name(ctx, p.species_name);
    }
    if (dex < 1) {
        if (out_err) snprintf(out_err, err_size,
                              "unknown species '%s'", p.species_name);
        return false;
    }

    /* Capture pre-inject party_count so we know which slot to
     * post-patch the nickname into. */
    uint8_t party_count_before = (g2 != GB_MOCK_GEN2_NONE)
        ? gb_mock_gen2_party_count(ctx)
        : gb_mock_gen1_party_count(ctx);
    if (party_count_before >= 6) {
        if (out_err) snprintf(out_err, err_size, "party full");
        return false;
    }

    bool ok = (g2 != GB_MOCK_GEN2_NONE)
        ? gb_mock_gen2_inject_builder(ctx, dex, p.level, p.shiny != 0)
        : gb_mock_gen1_inject_builder(ctx, dex, p.level, p.shiny != 0);
    if (!ok) {
        if (out_err) snprintf(out_err, err_size, "inject failed");
        return false;
    }

    /* Optional nickname override. The builder writes the species
     * name into the nickname slot; if the file specified a custom
     * nickname, overwrite it. Nickname slots are at the same WRAM
     * offset for both gens (wPartyMonNicks + slot*11). Generation
     * decides which info table to consult. */
    if (p.nickname[0]) {
        uint8_t* dst = (g2 != GB_MOCK_GEN2_NONE)
            ? gb_mock_gen2_nick_slot(ctx, party_count_before)
            : gb_mock_gen1_nick_slot(ctx, party_count_before);
        if (dst) encode_charmap(p.nickname, dst, 11);
    }

    return true;
}

#ifdef __cplusplus
}
#endif
