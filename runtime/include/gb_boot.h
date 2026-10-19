#ifndef GB_BOOT_H
#define GB_BOOT_H
#include "gbrt.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Developer build choice. LLE requires a matching real firmware image and
 * executes its native recompilation. */
bool gb_boot_start(GBContext* ctx, const char* path);
bool gb_boot_dispatch(GBContext* ctx, uint16_t addr);
const char* gb_boot_implementation(void);
/* Launcher-facing firmware validation and per-model path cache. No raw
 * firmware is copied or embedded. Empty resolution uses the state directory. */
bool gb_boot_verify_file(const char* path, bool cgb);
bool gb_boot_resolve_path(bool cgb, char* path, size_t capacity);
bool gb_boot_remember_path(const char* path);
#ifdef __cplusplus
}
#endif
#endif
