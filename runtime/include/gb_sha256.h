/* Minimal SHA-256 (FIPS 180-4) plus a one-shot ROM-fingerprint helper.
 * Used by per-cart launchers to verify that a user-supplied ROM file
 * matches the revision the static recompilation was built against. */
#ifndef GB_SHA256_H
#define GB_SHA256_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} GBSha256Ctx;

void gb_sha256_init(GBSha256Ctx* c);
void gb_sha256_update(GBSha256Ctx* c, const uint8_t* data, size_t len);
void gb_sha256_final(GBSha256Ctx* c, uint8_t out[32]);

/* Convenience: hash `data` and write the lowercase-hex digest
 * (64 chars + NUL) to `out_hex`. */
void gb_sha256_hex(const uint8_t* data, size_t len, char out_hex[65]);

/* Verify the contents of `path` match `expected_hex` (a lowercase
 * 64-char digest). Prints a single-line warning to stderr on
 * mismatch (using `label` to identify the cart, e.g. "pokered.gb"),
 * stays silent on match. Returns:
 *    0 = match
 *    1 = file unreadable, or expected_hex is NULL / empty
 *    2 = digest mismatch */
int gb_sha256_verify_file(const char* path,
                          const char* expected_hex,
                          const char* label);

#ifdef __cplusplus
}
#endif

#endif /* GB_SHA256_H */
