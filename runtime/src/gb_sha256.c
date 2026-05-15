/* Minimal SHA-256 (FIPS 180-4). Public-domain core
 * (Brad Conte / Aaron D. Gifford style); compact one-shot helpers
 * layered on top for the cart-launcher ROM-fingerprint use case. */
#include "gb_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_transform(GBSha256Ctx* c, const uint8_t* d) {
    uint32_t a, b, e, f, g, h, m[64], t1, t2, bb, dd;
    int i, j;
    for (i = 0, j = 0; i < 16; i++, j += 4) {
        m[i] = ((uint32_t)d[j] << 24) | ((uint32_t)d[j+1] << 16) |
               ((uint32_t)d[j+2] << 8) | (uint32_t)d[j+3];
    }
    for (; i < 64; i++) {
        uint32_t s0 = ROTR(m[i-15], 7) ^ ROTR(m[i-15], 18) ^ (m[i-15] >> 3);
        uint32_t s1 = ROTR(m[i-2], 17) ^ ROTR(m[i-2], 19) ^ (m[i-2] >> 10);
        m[i] = m[i-16] + s0 + m[i-7] + s1;
    }
    a  = c->state[0]; b  = c->state[1]; bb = c->state[2]; dd = c->state[3];
    e  = c->state[4]; f  = c->state[5]; g  = c->state[6]; h  = c->state[7];
    for (i = 0; i < 64; i++) {
        uint32_t s1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        t1 = h + s1 + ch + SHA256_K[i] + m[i];
        uint32_t s0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t mj = (a & b) ^ (a & bb) ^ (b & bb);
        t2 = s0 + mj;
        h = g; g = f; f = e; e = dd + t1; dd = bb; bb = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += bb; c->state[3] += dd;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

void gb_sha256_init(GBSha256Ctx* c) {
    c->datalen = 0;
    c->bitlen = 0;
    c->state[0] = 0x6a09e667; c->state[1] = 0xbb67ae85;
    c->state[2] = 0x3c6ef372; c->state[3] = 0xa54ff53a;
    c->state[4] = 0x510e527f; c->state[5] = 0x9b05688c;
    c->state[6] = 0x1f83d9ab; c->state[7] = 0x5be0cd19;
}

void gb_sha256_update(GBSha256Ctx* c, const uint8_t* d, size_t len) {
    for (size_t i = 0; i < len; i++) {
        c->data[c->datalen++] = d[i];
        if (c->datalen == 64) {
            sha256_transform(c, c->data);
            c->bitlen += 512;
            c->datalen = 0;
        }
    }
}

void gb_sha256_final(GBSha256Ctx* c, uint8_t out[32]) {
    uint32_t i = c->datalen;
    c->data[i++] = 0x80;
    if (i > 56) {
        while (i < 64) c->data[i++] = 0;
        sha256_transform(c, c->data);
        i = 0;
    }
    while (i < 56) c->data[i++] = 0;
    c->bitlen += (uint64_t)c->datalen * 8;
    for (int k = 7; k >= 0; k--) {
        c->data[56 + (7 - k)] = (uint8_t)(c->bitlen >> (k * 8));
    }
    sha256_transform(c, c->data);
    for (int j = 0; j < 4; j++) {
        for (int k = 0; k < 8; k++) {
            out[j + k * 4] = (uint8_t)(c->state[k] >> (24 - j * 8));
        }
    }
}

void gb_sha256_hex(const uint8_t* data, size_t len, char out_hex[65]) {
    GBSha256Ctx c;
    gb_sha256_init(&c);
    gb_sha256_update(&c, data, len);
    uint8_t dig[32];
    gb_sha256_final(&c, dig);
    static const char* hx = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i*2]     = hx[dig[i] >> 4];
        out_hex[i*2 + 1] = hx[dig[i] & 0xF];
    }
    out_hex[64] = '\0';
}

int gb_sha256_verify_file(const char* path,
                          const char* expected_hex,
                          const char* label) {
    if (!expected_hex || !*expected_hex) return 1;
    FILE* f = fopen(path, "rb");
    if (!f) return 1;
    GBSha256Ctx c;
    gb_sha256_init(&c);
    uint8_t buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        gb_sha256_update(&c, buf, n);
    }
    fclose(f);
    uint8_t dig[32];
    gb_sha256_final(&c, dig);
    char hex[65];
    static const char* hx = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[i*2]     = hx[dig[i] >> 4];
        hex[i*2 + 1] = hx[dig[i] & 0xF];
    }
    hex[64] = '\0';
    if (strcmp(hex, expected_hex) == 0) {
        return 0;
    }
    fprintf(stderr,
        "[LAUNCH] WARNING: %s sha256 %s does not match expected %s\n"
        "         The cart may not behave as expected; verify your ROM dump.\n",
        label ? label : path, hex, expected_hex);
    return 2;
}
