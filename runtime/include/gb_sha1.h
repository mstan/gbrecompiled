/* Minimal SHA-1 (public-domain, Steve Reid 1998). */
#ifndef GB_SHA1_H
#define GB_SHA1_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t buffer[64];
} SHA1_CTX;

void SHA1Init(SHA1_CTX* ctx);
void SHA1Update(SHA1_CTX* ctx, const uint8_t* data, size_t len);
void SHA1Final(uint8_t digest[20], SHA1_CTX* ctx);

#ifdef __cplusplus
}
#endif

#endif
