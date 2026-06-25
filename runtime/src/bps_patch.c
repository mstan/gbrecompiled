/* See bps_patch.h. Self-contained BPS (Beat) patch applier. */
#include "bps_patch.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static uint32_t bps_crc32(const uint8_t *data, size_t len) {
    static uint32_t table[256];
    static int init = 0;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++)
                c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0);
            table[i] = c;
        }
        init = 1;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF];
    return crc ^ 0xFFFFFFFFu;
}

/* BPS number decoding (variable-length). Advances *pos. */
static uint64_t bps_decode(const uint8_t *buf, size_t len, size_t *pos, int *ok) {
    uint64_t data = 0, shift = 1;
    while (*pos < len) {
        uint8_t x = buf[(*pos)++];
        data += (uint64_t)(x & 0x7f) * shift;
        if (x & 0x80) { *ok = 1; return data; }
        shift <<= 7;
        data += shift;
    }
    *ok = 0;
    return 0;
}

#define BPS_FAIL(...) do { snprintf(err, err_size, __VA_ARGS__); return 1; } while (0)

int gb_bps_apply(const uint8_t *patch, size_t patch_len,
                 const uint8_t *src, size_t src_len,
                 uint8_t **out, size_t *out_len,
                 char *err, size_t err_size) {
    if (!out || !out_len) return 1;
    *out = NULL; *out_len = 0;
    if (patch_len < 4 + 12 || memcmp(patch, "BPS1", 4) != 0)
        BPS_FAIL("not a BPS patch");

    size_t pos = 4;
    int ok = 1;
    uint64_t in_size  = bps_decode(patch, patch_len, &pos, &ok); if (!ok) BPS_FAIL("bad header");
    uint64_t tgt_size = bps_decode(patch, patch_len, &pos, &ok); if (!ok) BPS_FAIL("bad header");
    uint64_t meta     = bps_decode(patch, patch_len, &pos, &ok); if (!ok) BPS_FAIL("bad header");
    pos += (size_t)meta;
    if (pos > patch_len - 12) BPS_FAIL("truncated patch");

    if (in_size != src_len)
        BPS_FAIL("source ROM size mismatch (expected %llu, got %llu)",
                 (unsigned long long)in_size, (unsigned long long)src_len);

    /* footer: source CRC, target CRC, patch CRC (little-endian) */
    const uint8_t *foot = patch + patch_len - 12;
    uint32_t src_crc = (uint32_t)foot[0] | (foot[1]<<8) | (foot[2]<<16) | ((uint32_t)foot[3]<<24);
    uint32_t tgt_crc = (uint32_t)foot[4] | (foot[5]<<8) | (foot[6]<<16) | ((uint32_t)foot[7]<<24);
    if (bps_crc32(src, src_len) != src_crc)
        BPS_FAIL("source ROM CRC mismatch (wrong ROM for this patch)");

    uint8_t *o = (uint8_t *)malloc((size_t)tgt_size);
    if (!o) BPS_FAIL("out of memory");
    size_t op = 0;
    size_t src_rel = 0, tgt_rel = 0;
    const size_t end = patch_len - 12;

    while (pos < end) {
        uint64_t v = bps_decode(patch, patch_len, &pos, &ok);
        if (!ok) { free(o); BPS_FAIL("bad action"); }
        uint64_t cmd = v & 3, length = (v >> 2) + 1;
        if (op + length > (size_t)tgt_size) { free(o); BPS_FAIL("output overflow"); }
        switch (cmd) {
        case 0: /* SourceRead */
            if (op + length > src_len) { free(o); BPS_FAIL("source read OOB"); }
            memcpy(o + op, src + op, (size_t)length); op += (size_t)length;
            break;
        case 1: /* TargetRead */
            if (pos + length > end) { free(o); BPS_FAIL("target read OOB"); }
            memcpy(o + op, patch + pos, (size_t)length); pos += (size_t)length; op += (size_t)length;
            break;
        case 2: { /* SourceCopy */
            uint64_t d = bps_decode(patch, patch_len, &pos, &ok);
            if (!ok) { free(o); BPS_FAIL("bad SourceCopy"); }
            int neg = d & 1; src_rel += (size_t)((neg ? -1 : 1) * (long long)(d >> 1));
            for (uint64_t k = 0; k < length; k++) {
                if (src_rel >= src_len) { free(o); BPS_FAIL("SourceCopy OOB"); }
                o[op++] = src[src_rel++];
            }
            break;
        }
        case 3: { /* TargetCopy */
            uint64_t d = bps_decode(patch, patch_len, &pos, &ok);
            if (!ok) { free(o); BPS_FAIL("bad TargetCopy"); }
            int neg = d & 1; tgt_rel += (size_t)((neg ? -1 : 1) * (long long)(d >> 1));
            for (uint64_t k = 0; k < length; k++) {
                if (tgt_rel >= op) { free(o); BPS_FAIL("TargetCopy OOB"); }
                o[op++] = o[tgt_rel++];
            }
            break;
        }
        }
    }

    if (op != (size_t)tgt_size) { free(o); BPS_FAIL("incomplete patch output"); }
    if (bps_crc32(o, op) != tgt_crc) { free(o); BPS_FAIL("target CRC mismatch after apply"); }

    *out = o; *out_len = op;
    return 0;
}
