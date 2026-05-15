/**
 * @file cheats.c
 * @brief See cheats.h.
 *
 * The libretro .cht format is plain text:
 *
 *   cheats = N
 *   cheatI_desc   = "Description"
 *   cheatI_code   = "01VVLLHH+01VVLLHH+..."
 *   cheatI_enable = false
 *
 * Codes inside a single `cheatI_code` string are `+`-joined. We
 * support up to GB_CHEAT_OPS_PER_CHEAT ops per cheat -- the largest
 * I've seen in the wild is the "Seen All Pokedex" cheat with ~40
 * writes, but bumping the cap is a one-line change.
 */

#include "cheats.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static GBCheat g_cheats[GB_CHEAT_MAX_ENTRIES];
static int     g_cheat_count = 0;


/* ---- HTML-entity decoding (some libretro descriptions use
 * `&eacute;`, `&amp;`, `&lt;`, `&gt;`, `&quot;` because the file
 * format is closer to XML than plain text). The ImGui default
 * font can't render most accented glyphs, so we substitute ASCII
 * equivalents and call it a day. */

static void decode_html_entities(char* s) {
    char* dst = s;
    char* src = s;
    while (*src) {
        if (*src != '&') { *dst++ = *src++; continue; }
        char* end = strchr(src, ';');
        if (!end || (end - src) > 12) { *dst++ = *src++; continue; }
        size_t len = (size_t)(end - src - 1);
        const char* name = src + 1;
        char repl = '\0';
        if      (len == 3 && memcmp(name, "amp",  3) == 0) repl = '&';
        else if (len == 2 && memcmp(name, "lt",   2) == 0) repl = '<';
        else if (len == 2 && memcmp(name, "gt",   2) == 0) repl = '>';
        else if (len == 4 && memcmp(name, "quot", 4) == 0) repl = '"';
        else if (len == 4 && memcmp(name, "apos", 4) == 0) repl = '\'';
        else if (len == 7 && memcmp(name, "eacute", 6) == 0) repl = 'e';
        else if (len == 7 && memcmp(name, "Eacute", 6) == 0) repl = 'E';
        if (repl) {
            *dst++ = repl;
            src = end + 1;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}


/* ---- Code-string parser. Each `cheatN_code` is a `+`-joined list
 * of individual codes; this returns ops[] for one such string. */

static int hexnib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool parse_hex_byte(const char* s, uint8_t* out) {
    int hi = hexnib(s[0]), lo = hexnib(s[1]);
    if (hi < 0 || lo < 0) return false;
    *out = (uint8_t)((hi << 4) | lo);
    return true;
}

/* GameShark GB: 8 hex chars TTVVLLHH where TT=type, VV=value, LLHH
 * = address (little-endian). Type 01 is "write byte each frame".
 * Other types (banked writes etc.) get GB_CHEAT_OP_NONE and we
 * skip them at apply time. */
static bool parse_gameshark(const char* code, GBCheatOp* out) {
    if (strlen(code) != 8) return false;
    uint8_t type, value, lo, hi;
    if (!parse_hex_byte(code + 0, &type))  return false;
    if (!parse_hex_byte(code + 2, &value)) return false;
    if (!parse_hex_byte(code + 4, &lo))    return false;
    if (!parse_hex_byte(code + 6, &hi))    return false;
    out->type    = (type == 0x01) ? GB_CHEAT_OP_GAMESHARK
                                  : GB_CHEAT_OP_NONE;
    out->value   = value;
    out->address = ((uint16_t)hi << 8) | lo;
    out->compare = -1;
    return true;
}

/* Game Genie GB: 6 or 9 hex chars. Dashes are visual separators.
 * Decode using the standard nibble layout (see Pan Docs / VBA's
 * decoder).
 *
 *   value   = digits[0..1]
 *   address = ((digits[5] ^ 0xF) << 12) | (digits[2] << 8)
 *                                       | (digits[3] << 4) | digits[4]
 *   compare = present only on 9-char codes; the trailing 3 digits
 *             encode an 8-bit compare byte with a known XOR/rotate
 *             scramble. We decode it for verification but don't
 *             require a match at apply time (most patches work
 *             regardless of bank). */
static bool parse_gamegenie(const char* code, GBCheatOp* out) {
    char buf[16];
    int n = 0;
    for (const char* p = code; *p && n < (int)sizeof(buf) - 1; p++) {
        if (*p == '-') continue;
        buf[n++] = *p;
    }
    buf[n] = '\0';
    if (n != 6 && n != 9) return false;
    int d[9];
    for (int i = 0; i < n; i++) {
        d[i] = hexnib(buf[i]);
        if (d[i] < 0) return false;
    }
    out->type    = GB_CHEAT_OP_GAMEGENIE;
    out->value   = (uint8_t)((d[0] << 4) | d[1]);
    out->address = (uint16_t)(((d[5] ^ 0xF) << 12) |
                              (d[2] << 8) |
                              (d[3] << 4) |
                               d[4]);
    if (n == 6) {
        out->compare = -1;
    } else {
        /* 9-char Game Boy Game Genie compare-byte decode. Only
         * digits G and I are used (digit H is a parity bit /
         * unused). Build (G<<4)|I, rotate right 2 bits in 8-bit
         * space, XOR with 0xBA. Verified against pokered cheat
         * 305 (RATTATA $A5, SPEAROW $05, EKANS $6C at Route 4
         * slots 0/1/5) which all decode correctly with this form.
         * The "rotate-left-2 over 12 bits then XOR upper byte" form
         * I had previously was wrong. */
        uint8_t v = (uint8_t)((d[6] << 4) | d[8]);
        v = (uint8_t)((v >> 2) | ((v << 6) & 0xFF));
        out->compare = (int)(v ^ 0xBA);
    }
    return true;
}

static int parse_code_string(const char* codes_in, GBCheatOp* ops, int max_ops) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", codes_in);
    int n = 0;
    char* save = NULL;
    for (char* tok = strtok_r(tmp, "+", &save);
         tok && n < max_ops;
         tok = strtok_r(NULL, "+", &save)) {
        /* Trim spaces. */
        while (*tok == ' ' || *tok == '\t') tok++;
        char* end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) *(--end) = '\0';
        if (!*tok) continue;

        GBCheatOp op = { GB_CHEAT_OP_NONE, 0, 0, -1 };
        bool ok = false;
        if (strchr(tok, '-')) {
            ok = parse_gamegenie(tok, &op);
        } else {
            ok = parse_gameshark(tok, &op);
        }
        if (ok) ops[n++] = op;
    }
    return n;
}


/* ---- .cht file parser. Walks lines of the form
 *     cheatN_desc   = "..."
 *     cheatN_code   = "..."
 *     cheatN_enable = false
 * and emits one GBCheat per index. */

static char* strip(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    char* end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n')) *(--end) = '\0';
    return s;
}

static char* strip_quotes(char* s) {
    s = strip(s);
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        s[len - 1] = '\0';
        return s + 1;
    }
    return s;
}

static void parse_cht_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof(line), f) && g_cheat_count < GB_CHEAT_MAX_ENTRIES) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = strip(line);
        char* val = strip(eq + 1);
        if (strncmp(key, "cheat", 5) != 0) continue;
        const char* num_start = key + 5;
        char* num_end = NULL;
        long idx = strtol(num_start, &num_end, 10);
        if (idx < 0 || idx >= GB_CHEAT_MAX_ENTRIES || !num_end || *num_end != '_')
            continue;
        const char* field = num_end + 1;
        /* Auto-grow the cheat array up to the highest seen index. */
        if (idx >= g_cheat_count) {
            for (int i = g_cheat_count; i <= idx; i++) {
                memset(&g_cheats[i], 0, sizeof(g_cheats[i]));
            }
            g_cheat_count = (int)(idx + 1);
        }
        GBCheat* c = &g_cheats[idx];
        if (strcmp(field, "desc") == 0) {
            const char* unquoted = strip_quotes(val);
            snprintf(c->description, sizeof(c->description), "%s", unquoted);
            decode_html_entities(c->description);
        } else if (strcmp(field, "code") == 0) {
            const char* unquoted = strip_quotes(val);
            c->op_count = parse_code_string(unquoted, c->ops,
                                            GB_CHEAT_OPS_PER_CHEAT);
        }
        /* `enable` field ignored -- enables are runtime-only,
         * persisted (if at all) via the platform's prefs. */
    }
    fclose(f);
}


int gb_cheats_load(const char* game_id) {
    g_cheat_count = 0;
    memset(g_cheats, 0, sizeof(g_cheats));
    if (!game_id) return 0;

    char dir[512];
    snprintf(dir, sizeof(dir), "cheats/%s", game_id);
    DIR* d = opendir(dir);
    if (!d) return -1;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nl = strlen(ent->d_name);
        if (nl < 4) continue;
        const char* ext = ent->d_name + nl - 4;
        bool is_cht = (ext[0]=='.' && (ext[1]=='c'||ext[1]=='C')
                                   && (ext[2]=='h'||ext[2]=='H')
                                   && (ext[3]=='t'||ext[3]=='T'));
        if (!is_cht) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        parse_cht_file(path);
    }
    closedir(d);

    /* Discard trailing empty entries that came from index-only
     * lines (e.g. a stray `cheat999_enable=false` past the last
     * real cheat). */
    while (g_cheat_count > 0 &&
           g_cheats[g_cheat_count - 1].description[0] == '\0' &&
           g_cheats[g_cheat_count - 1].op_count == 0) {
        g_cheat_count--;
    }

    fprintf(stderr, "[cheats] loaded %d cheats from %s/\n",
            g_cheat_count, dir);
    return g_cheat_count;
}

int            gb_cheats_count(void)      { return g_cheat_count; }
const GBCheat* gb_cheats_get(int idx) {
    if (idx < 0 || idx >= g_cheat_count) return NULL;
    return &g_cheats[idx];
}


/* ---- Apply / restore helpers. */

static uint8_t* wram_byte_for(GBContext* ctx, uint16_t addr) {
    /* Mapped GB regions we can write to:
     *   $A000-$BFFF cart external RAM (battery-backed)
     *   $C000-$DFFF Work RAM (DMG 8KB flat; CGB 4KB bank-0 + 4KB bank-N)
     *   $FF80-$FFFE High RAM (zero-page; some cheats target this)
     *
     * WRAM bank-1 lives at flat offset 0x1000 regardless of cart
     * type -- the runtime's wram allocation is one contiguous 32KB
     * buffer with bank 0 at 0..0xFFF and bank 1 at 0x1000..0x1FFF
     * for DMG, or the currently-selected CGB bank for Gen 2 carts.
     * Cheats that target $D000-$DFFF land in whichever bank happens
     * to be live when the tick fires; this matches how cart code
     * accesses those addresses too. */
    if (ctx->wram && addr >= 0xC000 && addr <= 0xDFFF) {
        size_t off = (size_t)(addr - 0xC000);
        return &ctx->wram[off];
    }
    if (ctx->eram && addr >= 0xA000 && addr <= 0xBFFF) {
        size_t off = (size_t)(addr - 0xA000);
        if (off < ctx->eram_size) return &ctx->eram[off];
    }
    if (ctx->hram && addr >= 0xFF80 && addr <= 0xFFFE) {
        size_t off = (size_t)(addr - 0xFF80);
        return &ctx->hram[off];
    }
    return NULL;
}

/* True iff `bank` is consistent with every GameGenie op in `c`:
 * for each banked op, the existing ROM byte at (bank, addr) must
 * match the op's compare byte. Bank-0 ops and ops without a compare
 * pass automatically. Used to find the bank that satisfies all of
 * a multi-code cheat's verifiers at once -- a much stronger signal
 * than each code's compare in isolation. */
static bool bank_satisfies_genie_cheat(const GBContext* ctx,
                                       const GBCheat* c,
                                       int bank) {
    for (int i = 0; i < c->op_count; i++) {
        const GBCheatOp* op = &c->ops[i];
        if (op->type != GB_CHEAT_OP_GAMEGENIE) continue;
        if (op->address < 0x4000) continue;
        if (op->compare < 0) continue;
        size_t off = (size_t)bank * 0x4000 +
                     (size_t)(op->address - 0x4000);
        if (off >= ctx->rom_size) return false;
        if (ctx->rom[off] != (uint8_t)op->compare) return false;
    }
    return true;
}

/* Pick the ROM bank for a multi-code GameGenie cheat. Banks are
 * scanned high-to-low so a real data bank (typically 2+) wins over
 * bank 1 if both happen to contain the same compare byte at the
 * target offset. Returns -1 if no bank satisfies the constraint
 * set (caller falls back to bank 1). */
static int choose_genie_bank(const GBContext* ctx, const GBCheat* c) {
    int n_banks = (int)(ctx->rom_size / 0x4000);
    if (n_banks <= 1) return -1;
    /* Need at least one banked op with a compare for this to be a
     * meaningful constraint. */
    bool any_constraint = false;
    for (int i = 0; i < c->op_count; i++) {
        const GBCheatOp* op = &c->ops[i];
        if (op->type == GB_CHEAT_OP_GAMEGENIE &&
            op->address >= 0x4000 && op->compare >= 0) {
            any_constraint = true;
            break;
        }
    }
    if (!any_constraint) return -1;
    for (int bank = n_banks - 1; bank >= 1; bank--) {
        if (bank_satisfies_genie_cheat(ctx, c, bank)) return bank;
    }
    return -1;
}

static void apply_genie(GBContext* ctx, GBCheat* c) {
    int chosen_bank = choose_genie_bank(ctx, c);
    bool fell_back = (chosen_bank < 0);
    if (fell_back) chosen_bank = 1;

    for (int i = 0; i < c->op_count; i++) {
        c->saved_valid[i] = false;
        if (c->ops[i].type != GB_CHEAT_OP_GAMEGENIE) continue;
        size_t off;
        if (c->ops[i].address < 0x4000) {
            off = (size_t)c->ops[i].address;
        } else {
            off = (size_t)chosen_bank * 0x4000 +
                  (size_t)(c->ops[i].address - 0x4000);
        }
        if (!ctx->rom || off >= ctx->rom_size) {
            fprintf(stderr, "[cheats] GG addr $%04X out of ROM range\n",
                    c->ops[i].address);
            continue;
        }
        c->applied_offset[i] = off;
        c->saved_byte[i]     = ctx->rom[off];
        c->saved_valid[i]    = true;
        ctx->rom[off]        = c->ops[i].value;
    }

    if (fell_back) {
        fprintf(stderr, "[cheats] GG \"%s\": no bank satisfies all compares; "
                "patching bank 1 as fallback (may corrupt code/data)\n",
                c->description);
    } else {
        fprintf(stderr, "[cheats] GG \"%s\": resolved to bank %d\n",
                c->description, chosen_bank);
    }
}

static void unapply_genie(GBContext* ctx, GBCheat* c) {
    for (int i = 0; i < c->op_count; i++) {
        if (c->ops[i].type != GB_CHEAT_OP_GAMEGENIE) continue;
        if (!c->saved_valid[i]) continue;
        size_t off = c->applied_offset[i];
        if (ctx->rom && off < ctx->rom_size) {
            ctx->rom[off] = c->saved_byte[i];
        }
        c->saved_valid[i] = false;
    }
}

void gb_cheats_set_enabled(GBContext* ctx, int idx, bool on) {
    if (idx < 0 || idx >= g_cheat_count) return;
    GBCheat* c = &g_cheats[idx];
    if (c->enabled == on) return;
    c->enabled = on;
    if (!ctx) return;
    if (on) apply_genie(ctx, c);
    else    unapply_genie(ctx, c);
}

void gb_cheats_set_op_value(GBContext* ctx, int cheat_idx,
                            int op_idx, uint8_t value) {
    if (cheat_idx < 0 || cheat_idx >= g_cheat_count) return;
    GBCheat* c = &g_cheats[cheat_idx];
    if (op_idx < 0 || op_idx >= c->op_count) return;
    c->ops[op_idx].value = value;
    /* GameShark: nothing to do, next tick re-writes with the new
     * value. GameGenie + currently-enabled: poke the new value
     * into ROM directly; the saved_byte for restore stays as the
     * pre-cheat original. */
    if (c->enabled && ctx &&
        c->ops[op_idx].type == GB_CHEAT_OP_GAMEGENIE &&
        c->saved_valid[op_idx]) {
        /* Reuse the bank already chosen by apply_genie; we don't
         * re-scan because the saved byte refers to that location. */
        size_t off = c->applied_offset[op_idx];
        if (ctx->rom && off < ctx->rom_size) {
            ctx->rom[off] = value;
        }
    }
}

void gb_cheats_disable_all(GBContext* ctx) {
    for (int i = 0; i < g_cheat_count; i++) {
        if (!g_cheats[i].enabled) continue;
        g_cheats[i].enabled = false;
        if (ctx) unapply_genie(ctx, &g_cheats[i]);
    }
}

void gb_cheats_tick(GBContext* ctx) {
    if (!ctx) return;
    for (int i = 0; i < g_cheat_count; i++) {
        if (!g_cheats[i].enabled) continue;
        for (int j = 0; j < g_cheats[i].op_count; j++) {
            if (g_cheats[i].ops[j].type != GB_CHEAT_OP_GAMESHARK) continue;
            uint8_t* p = wram_byte_for(ctx, g_cheats[i].ops[j].address);
            if (p) *p = g_cheats[i].ops[j].value;
        }
    }
}
