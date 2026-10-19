#ifndef GB_DEBUG_JSON_H
#define GB_DEBUG_JSON_H
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Flat command-member lookup with full JSON validation and string unescaping.
 * Nested values are validated/skipped; only a top-level scalar can be returned.
 * Duplicate requested members, truncation and embedded NUL are rejected. */
bool gb_debug_json_get(const char* json, const char* key, char* out, size_t capacity);
bool gb_debug_json_valid(const char* json);
#ifdef __cplusplus
}
#endif
#endif
