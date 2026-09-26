/**
 * @file gl_program_cache.h
 * @brief Keep the GL driver's compiled programs on disk between runs.
 *
 * Through EGL_ANDROID_blob_cache (ANGLE, Mesa's EGL): the driver hands us its
 * compiled program binaries and asks for them back the next time the same
 * sources are linked. Under ANGLE that skips Microsoft's HLSL compiler, which
 * for large .slangp presets is tens of seconds per load (measured: nnedi3
 * nns256 32.6 s cold, 0.7 s from the cache). It also lets a preset probe
 * (librashader_chain.h) warm the cache for the process that started it.
 */
#ifndef GB_GL_PROGRAM_CACHE_H
#define GB_GL_PROGRAM_CACHE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install the cache for the current EGL display, storing files in `dir`
 * (created if missing). Call once with the context current, before programs
 * are linked. Returns false where EGL_ANDROID_blob_cache is unavailable, which
 * only means nothing is cached. */
bool gb_gl_program_cache_install(const char* dir);

#ifdef __cplusplus
}
#endif

#endif /* GB_GL_PROGRAM_CACHE_H */
