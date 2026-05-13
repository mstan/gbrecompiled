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
/* `.pkm` is our own generation-agnostic text format — the same file
 * applies on either Gen 1 or Gen 2 carts. Keys that don't fit the
 * active generation are silently dropped (e.g. `held_item` on a
 * Gen 1 cart). `.pk1` / `.pk2` are the binary EventsGallery/PKHeX
 * formats, which are gen-specific by definition. */
static bool file_is_text(const char* name) { return file_has_ext(name, ".pkm"); }

static size_t ext_length(const char* name) {
    if (file_is_text(name))           return 4;  /* ".pkm" */
    if (file_has_ext(name, ".pk1"))   return 4;
    if (file_has_ext(name, ".pk2"))   return 4;
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

/* Build the friendly display label for an entry. Format is
 * "<basename> - <parsed info>", e.g.
 *   "europe_mew - Mew lvl5 EUROPE/01693"
 *   "sample_shiny_mewtwo - Mewtwo lvl70 shiny"
 * The filename comes first so two files producing the same mon
 * are still distinguishable, and so sorting by filename gives a
 * predictable order. Falls back to bare filename when the parsed
 * info can't be extracted. */
static void build_display(const GBContext* ctx,
                          const char* filename, const char* full_path,
                          char* out, size_t out_size) {
    size_t ext_len = ext_length(filename);
    size_t nlen = strlen(filename);
    size_t base_len = (nlen > ext_len) ? nlen - ext_len : nlen;
    char basename[64];
    if (base_len >= sizeof(basename)) base_len = sizeof(basename) - 1;
    memcpy(basename, filename, base_len);
    basename[base_len] = '\0';

    /* Fallback: just the bare filename. */
    snprintf(out, out_size, "%s", basename);
    if (!ctx) return;

    FILE* f = fopen(full_path, "rb");
    if (!f) return;
    uint8_t buf[128];
    size_t got = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    bool is_pk1 = file_has_ext(filename, ".pk1");
    bool is_pk2 = file_has_ext(filename, ".pk2");
    char info[64] = "";

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
            snprintf(info, sizeof(info), "%s lvl%d %s/%05u",
                     species_name, level, ot_name, otid);
        } else {
            snprintf(info, sizeof(info), "%s lvl%d", species_name, level);
        }
        goto finalize;
    }

    if (file_is_text(filename)) {
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
            snprintf(info, sizeof(info), "%s lvl%d%s",
                     species, level, shiny ? " shiny" : "");
        } else if (species[0]) {
            snprintf(info, sizeof(info), "%s", species);
        }
        /* else: keep info empty → display is just filename. */
    }

finalize:
    if (info[0]) {
        snprintf(out, out_size, "%s - %s", basename, info);
    } /* else `out` already holds the filename fallback. */
}

int gb_inject_file_scan(const GBContext* ctx, const char* game_id,
                        GBInjectFileEntry* out, int max) {
    if (!game_id || !out || max <= 0) return 0;

    /* Filter to the active cart's generation. A .pk1 / .gen1 on a
     * Gen 2 cart (or vice versa) would fail at apply, so hide
     * incompatible files from the dropdown to keep the menu honest. */
    bool cart_is_gen1 = ctx && (gb_mock_gen1_detect(ctx) != GB_MOCK_GEN1_NONE);
    bool cart_is_gen2 = ctx && (gb_mock_gen2_detect(ctx) != GB_MOCK_GEN2_NONE);

    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "injects/%s", game_id);

    DIR* d = opendir(dir_path);
    if (!d) return 0;

    int count = 0;
    struct dirent* ent;
    while (count < max && (ent = readdir(d)) != NULL) {
        if (ext_length(ent->d_name) == 0) continue;

        /* .pkm is gen-agnostic so it shows on both. Binaries are
         * gen-specific by their nature, so filter them. */
        if (file_has_ext(ent->d_name, ".pk1") && !cart_is_gen1) continue;
        if (file_has_ext(ent->d_name, ".pk2") && !cart_is_gen2) continue;

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

/* ---- .pkm parser ---- */

typedef struct {
    /* Required fields. */
    char  species_name[32];   /* either "MEWTWO" or stringified dex# */
    int   species_dex;        /* -1 if unset */
    int   level;              /* -1 if unset */
    int   shiny;              /* 0/1, -1 if unset */

    /* Optional gen-agnostic overrides. -1 / empty = "use cart default". */
    char  nickname[16];
    char  ot_name[16];
    int   ot_id;              /* -1 if unset, else 0..65535 */
    /* Moves: stored as either a numeric ID (>=0) or as a name to
     * resolve at apply time. -1 + empty string = unset. `moves_set`
     * tracks whether the file had a `moves =` key at all so apply
     * can decide between "use file's moves authoritatively" and
     * "fall back to cart learnset". */
    int   moves[4];
    char  move_names[4][20];
    bool  moves_set;
    int   dvs[4];             /* -1 if unset; order: atk, def, spd, spc (0..15) */

    /* Optional Gen 2 only — ignored on Gen 1. */
    int   held_item;          /* -1 if unset, else 0..255 */
    int   happiness;          /* -1 if unset, else 0..255 */
    int   pokerus;            /* -1 if unset, else 0..255 */

    /* Optional Gen 1 only — ignored on Gen 2. */
    int   catch_rate;         /* -1 if unset, else 1..255 */
} ParsedMon;

static void parsed_init(ParsedMon* p) {
    memset(p, 0, sizeof(*p));
    p->species_dex = -1;
    p->level = -1;
    p->shiny = -1;
    p->ot_id = -1;
    for (int i = 0; i < 4; i++) p->moves[i] = -1;
    for (int i = 0; i < 4; i++) p->dvs[i]   = -1;
    p->held_item  = -1;
    p->happiness  = -1;
    p->pokerus    = -1;
    p->catch_rate = -1;
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
        } else if (strcasecmp(key, "ot_name") == 0) {
            snprintf(out->ot_name, sizeof(out->ot_name), "%s", val);
        } else if (strcasecmp(key, "ot_id") == 0) {
            int v = atoi(val);
            if (v >= 0 && v <= 0xFFFF) out->ot_id = v;
        } else if (strcasecmp(key, "moves") == 0) {
            /* Comma-separated up to 4 entries; each entry is either
             * a numeric move ID or a move name (e.g. "PSYCHIC").
             * Names are resolved at apply time against the active
             * cart's MoveNames table. Seeing the key at all flips
             * moves_set, which makes apply use the file's slots
             * authoritatively (empties become no-move). */
            out->moves_set = true;
            int idx = 0;
            const char* p = val;
            while (*p && idx < 4) {
                while (*p == ' ' || *p == '\t' || *p == ',') p++;
                if (!*p) break;
                const char* token_start = p;
                while (*p && *p != ',') p++;
                const char* token_end = p;
                /* Trim trailing whitespace. */
                while (token_end > token_start &&
                       isspace((unsigned char)token_end[-1])) token_end--;
                size_t tlen = (size_t)(token_end - token_start);
                if (tlen > 0) {
                    if (*token_start >= '0' && *token_start <= '9') {
                        int v = atoi(token_start);
                        if (v >= 0 && v <= 255) out->moves[idx] = v;
                    } else {
                        if (tlen >= sizeof(out->move_names[idx]))
                            tlen = sizeof(out->move_names[idx]) - 1;
                        memcpy(out->move_names[idx], token_start, tlen);
                        out->move_names[idx][tlen] = '\0';
                    }
                }
                idx++;
            }
        } else if (strcasecmp(key, "dvs") == 0) {
            /* Positional: atk,def,spd,spc each 0..15. An empty slot
             * (`dvs = 5,,,10`) means "leave the builder's roll for
             * this slot" -- the apply path then only overrides the
             * slots the file explicitly set. Commas separate; the
             * parser walks position-by-position so it tracks empty
             * slots correctly. */
            int idx = 0;
            const char* p = val;
            while (idx < 4) {
                while (*p == ' ' || *p == '\t') p++;
                /* Empty slot? Either we hit the comma or the end of
                 * the line without seeing a digit. */
                if (*p == '\0' || *p == ',') {
                    if (*p == ',') p++;
                    idx++;
                    continue;
                }
                int v = atoi(p);
                if (v >= 0 && v <= 15) out->dvs[idx] = v;
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
                idx++;
            }
        } else if (strcasecmp(key, "held_item") == 0) {
            int v = atoi(val);
            if (v >= 0 && v <= 255) out->held_item = v;
        } else if (strcasecmp(key, "happiness") == 0) {
            int v = atoi(val);
            if (v >= 0 && v <= 255) out->happiness = v;
        } else if (strcasecmp(key, "pokerus") == 0) {
            int v = atoi(val);
            if (v >= 0 && v <= 255) out->pokerus = v;
        } else if (strcasecmp(key, "catch_rate") == 0) {
            int v = atoi(val);
            if (v >= 1 && v <= 255) out->catch_rate = v;
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

/* Standardized key labels used by both describe_binary and describe_text.
 * 12 chars each (longest is "Catch Rate: ") so values line up cleanly. */
#define KEY_SPECIES    "Species:    "
#define KEY_LEVEL      "Level:      "
#define KEY_SHINY      "Shiny:      "
#define KEY_NICKNAME   "Nickname:   "
#define KEY_OT_NAME    "OT Name:    "
#define KEY_OT_ID      "OT ID:      "
#define KEY_MOVES      "Moves:      "
#define KEY_DVS        "DVs:        "
#define KEY_HELD_ITEM  "Held Item:  "
#define KEY_HAPPINESS  "Happiness:  "
#define KEY_POKERUS    "Pokerus:    "
#define KEY_CATCH_RATE "Catch Rate: "

#define VAL_NOT_SET    "not set"

/* Describe a binary pk1/pk2 entry into a multi-line summary using
 * the standardized key/value layout. Binary files carry a value for
 * every field (it's all raw bytes), so "not set" only appears for
 * move slots that hold the no-move sentinel 0. */
static void describe_binary(const GBContext* ctx, const uint8_t* buf,
                            bool is_gen2, bool jp,
                            char* out, size_t out_size) {
    size_t body_off = 3;
    size_t name_len = jp ? 6 : 11;
    size_t body_size = is_gen2 ? 48 : 44;

    uint8_t species_byte = buf[body_off + 0];
    char species_name[12] = "";
    if (is_gen2) {
        gb_mock_gen2_species_name(ctx, species_byte,
                                   species_name, sizeof(species_name));
    } else {
        gb_mock_gen1_species_name_by_internal(ctx, species_byte,
                                              species_name, sizeof(species_name));
    }
    int level = is_gen2 ? buf[body_off + 31] : buf[body_off + 33];
    size_t otid_off = body_off + (is_gen2 ? 6 : 12);
    unsigned otid = ((unsigned)buf[otid_off] << 8) | buf[otid_off + 1];
    char ot_name[16];
    decode_charmap_buf(buf + body_off + body_size, name_len,
                       ot_name, sizeof(ot_name));
    char nick[16];
    decode_charmap_buf(buf + body_off + body_size + name_len, name_len,
                       nick, sizeof(nick));

    size_t moves_off = body_off + (is_gen2 ? 2 : 8);
    uint8_t m[4] = { buf[moves_off], buf[moves_off + 1],
                     buf[moves_off + 2], buf[moves_off + 3] };

    size_t dvs_off = body_off + (is_gen2 ? 21 : 27);
    uint8_t dv_hi = buf[dvs_off], dv_lo = buf[dvs_off + 1];
    int atk_dv = (dv_hi >> 4) & 0xF, def_dv = dv_hi & 0xF;
    int spd_dv = (dv_lo >> 4) & 0xF, spc_dv = dv_lo & 0xF;
    bool shiny = (def_dv == 10 && spd_dv == 10 && spc_dv == 10 &&
                  (atk_dv & 0x02));

    /* Resolve move IDs to names via the cart's MoveNames table.
     * Slot value 0 = empty (no move); we skip empty slots entirely
     * so the joined output never has stray commas. A non-zero ID
     * that fails to resolve falls back to "#N" so the user can still
     * see what's in the file. */
    char moves_joined[112] = "";
    size_t mj = 0;
    for (int i = 0; i < 4; i++) {
        if (m[i] == 0) continue;
        char nm[24];
        bool ok = is_gen2
            ? gb_mock_gen2_move_name(ctx, m[i], nm, sizeof(nm))
            : gb_mock_gen1_move_name(ctx, m[i], nm, sizeof(nm));
        if (!ok || nm[0] == '\0') snprintf(nm, sizeof(nm), "#%u", m[i]);
        const char* sep = (mj == 0) ? "" : ", ";
        mj += snprintf(moves_joined + mj, sizeof(moves_joined) - mj,
                       "%s%s", sep, nm);
        if (mj >= sizeof(moves_joined)) break;
    }
    if (mj == 0) snprintf(moves_joined, sizeof(moves_joined), VAL_NOT_SET);

    /* Gen-specific extras: pk1 carries catch_rate; pk2 carries
     * held_item / happiness / pokerus. */
    char extras[160] = "";
    if (is_gen2) {
        uint8_t held = buf[body_off + 1];
        uint8_t hap  = buf[body_off + 27];
        uint8_t pkrs = buf[body_off + 28];
        snprintf(extras, sizeof(extras),
                 "\n" KEY_HELD_ITEM "%u"
                 "\n" KEY_HAPPINESS "%u"
                 "\n" KEY_POKERUS "0x%02X",
                 held, hap, pkrs);
    } else {
        uint8_t catch_rate = buf[body_off + 7];
        snprintf(extras, sizeof(extras),
                 "\n" KEY_CATCH_RATE "%u", catch_rate);
    }

    snprintf(out, out_size,
        KEY_SPECIES   "%s\n"
        KEY_LEVEL     "%d\n"
        KEY_SHINY     "%s\n"
        KEY_NICKNAME  "%s\n"
        KEY_OT_NAME   "%s\n"
        KEY_OT_ID     "%u\n"
        KEY_MOVES     "%s\n"
        KEY_DVS       "atk=%d def=%d spd=%d spc=%d"
        "%s",
        species_name[0] ? species_name : VAL_NOT_SET,
        level,
        shiny ? "yes" : "no",
        nick[0]    ? nick    : VAL_NOT_SET,
        ot_name[0] ? ot_name : VAL_NOT_SET,
        otid,
        moves_joined,
        atk_dv, def_dv, spd_dv, spc_dv,
        extras);
}

/* Describe a .pkm entry — every standardized key is rendered, with
 * "not set" for fields the file omits. Gen-specific extras are only
 * shown for the active cart's generation (held_item/happiness/pokerus
 * on Gen 2; catch_rate on Gen 1) so the summary stays relevant. */
static void describe_text(const GBContext* ctx, const char* path,
                          char* out, size_t out_size) {
    ParsedMon p;
    if (!parse_file(path, &p)) {
        snprintf(out, out_size, "(failed to read)");
        return;
    }

    char species_val[40];
    if (p.species_dex > 0) {
        snprintf(species_val, sizeof(species_val), "%d", p.species_dex);
    } else if (p.species_name[0]) {
        snprintf(species_val, sizeof(species_val), "%s", p.species_name);
    } else {
        snprintf(species_val, sizeof(species_val), VAL_NOT_SET);
    }

    char level_val[16];
    if (p.level > 0) snprintf(level_val, sizeof(level_val), "%d", p.level);
    else             snprintf(level_val, sizeof(level_val), VAL_NOT_SET);

    char ot_id_val[16];
    if (p.ot_id >= 0) snprintf(ot_id_val, sizeof(ot_id_val), "%d", p.ot_id);
    else              snprintf(ot_id_val, sizeof(ot_id_val), VAL_NOT_SET);

    const char* shiny_val =
        p.shiny > 0 ? "yes" :
        p.shiny == 0 ? "no"  : VAL_NOT_SET;

    const char* nickname_val = p.nickname[0] ? p.nickname : VAL_NOT_SET;
    const char* ot_name_val  = p.ot_name[0]  ? p.ot_name  : VAL_NOT_SET;

    /* Moves: skip empty slots so "Pound, , , " never happens. If
     * the file specifies no moves at all, fall back to "not set" for
     * the whole field. Resolve numeric IDs against the active cart's
     * MoveNames; bare numeric fallback "#N" if lookup fails. */
    bool cart_is_gen2 = (gb_mock_gen2_detect(ctx) != GB_MOCK_GEN2_NONE);
    bool cart_is_gen1 = (gb_mock_gen1_detect(ctx) != GB_MOCK_GEN1_NONE);
    char moves_buf[112] = "";
    size_t mb = 0;
    for (int i = 0; i < 4; i++) {
        char nm[24] = "";
        if (p.move_names[i][0]) {
            snprintf(nm, sizeof(nm), "%s", p.move_names[i]);
        } else if (p.moves[i] > 0) {
            bool ok = false;
            if (cart_is_gen2)
                ok = gb_mock_gen2_move_name(ctx, p.moves[i], nm, sizeof(nm));
            else if (cart_is_gen1)
                ok = gb_mock_gen1_move_name(ctx, p.moves[i], nm, sizeof(nm));
            if (!ok || nm[0] == '\0')
                snprintf(nm, sizeof(nm), "#%d", p.moves[i]);
        } else {
            continue;
        }
        const char* sep = (mb == 0) ? "" : ", ";
        mb += snprintf(moves_buf + mb, sizeof(moves_buf) - mb,
                       "%s%s", sep, nm);
        if (mb >= sizeof(moves_buf)) break;
    }
    if (mb == 0) snprintf(moves_buf, sizeof(moves_buf), VAL_NOT_SET);

    /* DVs: each slot likewise renders independently. */
    char dvs_buf[64];
    bool any_dv = (p.dvs[0] >= 0 || p.dvs[1] >= 0 ||
                   p.dvs[2] >= 0 || p.dvs[3] >= 0);
    if (any_dv) {
        char a[8], d[8], s[8], c[8];
        if (p.dvs[0] >= 0) snprintf(a, sizeof(a), "%d", p.dvs[0]); else snprintf(a, sizeof(a), "?");
        if (p.dvs[1] >= 0) snprintf(d, sizeof(d), "%d", p.dvs[1]); else snprintf(d, sizeof(d), "?");
        if (p.dvs[2] >= 0) snprintf(s, sizeof(s), "%d", p.dvs[2]); else snprintf(s, sizeof(s), "?");
        if (p.dvs[3] >= 0) snprintf(c, sizeof(c), "%d", p.dvs[3]); else snprintf(c, sizeof(c), "?");
        snprintf(dvs_buf, sizeof(dvs_buf),
                 "atk=%s def=%s spd=%s spc=%s", a, d, s, c);
    } else {
        snprintf(dvs_buf, sizeof(dvs_buf), VAL_NOT_SET);
    }

    /* Gen-conditional extras: show only the ones that apply to the
     * active cart's generation. Each is always rendered when shown
     * (value or "not set"). */
    char extras[200] = "";
    size_t eb = 0;
    if (cart_is_gen2) {
        if (p.held_item >= 0)
            eb += snprintf(extras + eb, sizeof(extras) - eb,
                           "\n" KEY_HELD_ITEM "%d", p.held_item);
        else
            eb += snprintf(extras + eb, sizeof(extras) - eb,
                           "\n" KEY_HELD_ITEM VAL_NOT_SET);
        if (p.happiness >= 0)
            eb += snprintf(extras + eb, sizeof(extras) - eb,
                           "\n" KEY_HAPPINESS "%d", p.happiness);
        else
            eb += snprintf(extras + eb, sizeof(extras) - eb,
                           "\n" KEY_HAPPINESS VAL_NOT_SET);
        if (p.pokerus >= 0)
            eb += snprintf(extras + eb, sizeof(extras) - eb,
                           "\n" KEY_POKERUS "0x%02X", p.pokerus);
        else
            eb += snprintf(extras + eb, sizeof(extras) - eb,
                           "\n" KEY_POKERUS VAL_NOT_SET);
    } else if (cart_is_gen1) {
        if (p.catch_rate >= 0)
            eb += snprintf(extras + eb, sizeof(extras) - eb,
                           "\n" KEY_CATCH_RATE "%d", p.catch_rate);
        else
            eb += snprintf(extras + eb, sizeof(extras) - eb,
                           "\n" KEY_CATCH_RATE VAL_NOT_SET);
    }

    snprintf(out, out_size,
        KEY_SPECIES   "%s\n"
        KEY_LEVEL     "%s\n"
        KEY_SHINY     "%s\n"
        KEY_NICKNAME  "%s\n"
        KEY_OT_NAME   "%s\n"
        KEY_OT_ID     "%s\n"
        KEY_MOVES     "%s\n"
        KEY_DVS       "%s"
        "%s"
        "\n\nAny value not set will use the cart's default at inject time.",
        species_val, level_val, shiny_val,
        nickname_val, ot_name_val, ot_id_val,
        moves_buf, dvs_buf, extras);
}

bool gb_inject_file_describe(const GBContext* ctx,
                             const GBInjectFileEntry* entry,
                             char* out, size_t out_size) {
    if (!entry || !out || out_size < 32) return false;
    out[0] = '\0';

    if (file_has_ext(entry->filename, ".pk1") ||
        file_has_ext(entry->filename, ".pk2")) {
        FILE* f = fopen(entry->full_path, "rb");
        if (!f) { snprintf(out, out_size, "(open failed)"); return false; }
        uint8_t buf[128];
        size_t got = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        bool is_pk2 = file_has_ext(entry->filename, ".pk2");
        bool jp = (got == 59 || got == 63);
        bool intl = (got == 69 || got == 73);
        if (!jp && !intl) {
            snprintf(out, out_size, "(unrecognized size %zu)", got);
            return false;
        }
        describe_binary(ctx, buf, is_pk2, jp, out, out_size);
        return true;
    }
    if (file_is_text(entry->filename)) {
        describe_text(ctx, entry->full_path, out, out_size);
        return true;
    }
    snprintf(out, out_size, "(unknown format)");
    return false;
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

    /* .gen1 / .gen2 — text format, routes through the builder for
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

    /* Optional post-patches. The builder has already populated the
     * party slot with cart-default stats + moves + OT. Anything the
     * file explicitly sets overwrites those defaults. Keys that
     * don't apply to the active generation are silently skipped:
     * `held_item` / `happiness` / `pokerus` are gen-2-only;
     * `catch_rate` is gen-1-only.
     *
     * Party-struct field offsets:
     *   Gen 1 (44 bytes)               Gen 2 (48 bytes)
     *   ----                            ----
     *   0  MON_SPECIES                  0  MON_SPECIES
     *   1-2 HP (cur)                    1  MON_ITEM (held)
     *   3  BOX_LEVEL                    2-5 MOVES
     *   4  STATUS                       6-7 OT_ID
     *   5  TYPE1                        8-10 EXP
     *   6  TYPE2                        11-20 STAT_EXP
     *   7  CATCH_RATE                   21-22 DVS
     *   8-11 MOVES                      23-26 PP
     *   12-13 OT_ID                     27 HAPPINESS
     *   14-16 EXP                       28 POKERUS
     *   17-26 STAT_EXP                  29-30 CAUGHT_DATA
     *   27-28 DVS                       31 LEVEL
     *   29-32 PP                        32 STATUS
     *   33 LEVEL                        ... stats tail
     *   34-43 stats tail
     */
    bool is_gen2 = (g2 != GB_MOCK_GEN2_NONE);
    uint8_t* mon = is_gen2
        ? gb_mock_gen2_party_mons_slot(ctx, party_count_before)
        : gb_mock_gen1_party_mons_slot(ctx, party_count_before);
    uint8_t* ot_dst   = is_gen2
        ? gb_mock_gen2_party_ots_slot(ctx, party_count_before)
        : gb_mock_gen1_party_ots_slot(ctx, party_count_before);
    uint8_t* nick_dst = is_gen2
        ? gb_mock_gen2_nick_slot(ctx, party_count_before)
        : gb_mock_gen1_nick_slot(ctx, party_count_before);

    if (p.nickname[0] && nick_dst) {
        encode_charmap(p.nickname, nick_dst, 11);
    }
    if (p.ot_name[0] && ot_dst) {
        encode_charmap(p.ot_name, ot_dst, 11);
    }
    if (p.ot_id >= 0 && mon) {
        size_t off = is_gen2 ? 6 : 12;
        mon[off]     = (uint8_t)((p.ot_id >> 8) & 0xFF);
        mon[off + 1] = (uint8_t)(p.ot_id & 0xFF);
    }
    /* Moves: if the file has a `moves =` key, it's authoritative —
     * unset slots clear to 0 instead of inheriting the builder's
     * learnset fill. If the key is omitted entirely, leave the
     * builder's choices alone. */
    if (mon && p.moves_set) {
        size_t moves_off = is_gen2 ? 2 : 8;
        for (int i = 0; i < 4; i++) {
            int mv = p.moves[i];
            if (mv < 0 && p.move_names[i][0]) {
                mv = is_gen2
                    ? gb_mock_gen2_move_id_for_name(ctx, p.move_names[i])
                    : gb_mock_gen1_move_id_for_name(ctx, p.move_names[i]);
                if (mv < 0) {
                    /* Move name doesn't exist on this cart's gen (e.g.
                     * "RAIN_DANCE" on Red). Surface the failure so the
                     * user knows why a slot ended up empty. */
                    if (out_err) snprintf(out_err, err_size,
                        "unknown move '%s' (slot %d)",
                        p.move_names[i], i + 1);
                    return false;
                }
            }
            mon[moves_off + i] = (mv > 0) ? (uint8_t)mv : 0;
        }
    }
    if (mon && (p.dvs[0] >= 0 || p.dvs[1] >= 0 || p.dvs[2] >= 0 || p.dvs[3] >= 0)) {
        size_t dvs_off = is_gen2 ? 21 : 27;
        /* Build from current packed value so partially-specified DVs
         * preserve the rest. */
        int atk = (mon[dvs_off]     >> 4) & 0xF;
        int def =  mon[dvs_off]            & 0xF;
        int spd = (mon[dvs_off + 1] >> 4) & 0xF;
        int spc =  mon[dvs_off + 1]        & 0xF;
        if (p.dvs[0] >= 0) atk = p.dvs[0];
        if (p.dvs[1] >= 0) def = p.dvs[1];
        if (p.dvs[2] >= 0) spd = p.dvs[2];
        if (p.dvs[3] >= 0) spc = p.dvs[3];
        mon[dvs_off]     = (uint8_t)((atk << 4) | (def & 0xF));
        mon[dvs_off + 1] = (uint8_t)((spd << 4) | (spc & 0xF));
        /* Stats stay at whatever the builder computed from
         * its DVs — a future enhancement could recompute here. */
    }

    /* Gen 2-only fields. */
    if (is_gen2 && mon) {
        if (p.held_item >= 0) mon[1]  = (uint8_t)p.held_item;
        if (p.happiness >= 0) mon[27] = (uint8_t)p.happiness;
        if (p.pokerus   >= 0) mon[28] = (uint8_t)p.pokerus;
    }
    /* Gen 1-only fields. */
    if (!is_gen2 && mon) {
        if (p.catch_rate >= 0) mon[7] = (uint8_t)p.catch_rate;
    }

    return true;
}

#ifdef __cplusplus
}
#endif
