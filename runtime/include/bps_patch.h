/* Minimal BPS patch applier. Used by the launcher to derive an "extended"
 * ROM from a user-supplied stock ROM at launch time, so distributions can ship
 * a small .bps instead of (illegally) shipping the ROM. */
#ifndef GB_BPS_PATCH_H
#define GB_BPS_PATCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Apply a BPS patch to `src`. On success allocates *out (malloc, caller frees)
 * holding the target ROM and sets *out_len. Validates the BPS source CRC32
 * against `src` (so a wrong source ROM fails cleanly) and the target CRC32 of
 * the result. Returns 0 on success, non-zero on any error (with a message in
 * `err`). */
int gb_bps_apply(const uint8_t *patch, size_t patch_len,
                 const uint8_t *src, size_t src_len,
                 uint8_t **out, size_t *out_len,
                 char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* GB_BPS_PATCH_H */
