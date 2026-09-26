/**
 * @file librashader_chain.h
 * @brief RetroArch .slangp shader presets through librashader's OpenGL runtime.
 *
 * librashader is loaded at run time by name (librashader.dll / .so / .dylib
 * beside the executable) through the vendored librashader_ld.h; nothing links
 * against it, so a missing library only means "no presets". It needs an
 * OpenGL ES 3.0 or OpenGL 3.3 context; on Windows the runtime asks ANGLE for
 * ES 3.1, which presets using arrays of arrays need.
 *
 * Display only: the chain samples the uploaded game texture and renders into
 * a texture of its own, so screenshots, frame dumps and the verify paths never
 * see a preset.
 */
#ifndef GB_LIBRASHADER_CHAIN_H
#define GB_LIBRASHADER_CHAIN_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBLrsParam {
    const char* name;
    const char* description;
    float minimum;
    float maximum;
    float step;
    float initial;   /* value the preset loaded with: its override, else the shader default */
    float value;
} GBLrsParam;

/* Load librashader and check the current GL context. Runs once; call with the
 * context current. Returns gb_lrs_available(). */
bool gb_lrs_init(void);
bool gb_lrs_available(void);
/* What was loaded, or why librashader is unavailable. Never NULL. */
const char* gb_lrs_status(void);

/* Load a .slangp. NULL or "" unloads the running preset and succeeds. On
 * failure the previous preset keeps running and gb_lrs_error() says why.
 * `unoptimized` compiles it without the HLSL optimizer, for presets that
 * crash it (GB_LRS_PROBE_OK_UNOPTIMIZED); it holds while the preset runs, as
 * ANGLE compiles some shaders only at their first draw. */
bool gb_lrs_load(const char* path, bool unoptimized);
bool gb_lrs_active(void);
bool gb_lrs_unoptimized(void);      /* the running preset was loaded unoptimized */
const char* gb_lrs_current(void);   /* path of the running preset, or "" */
const char* gb_lrs_error(void);     /* last load or frame error, or "" */

/* Run the chain over src_tex (src_w x src_h, RGBA, row 0 = top of the image)
 * into an out_w x out_h texture owned by this module and return it, or 0 if
 * nothing ran and the caller should draw unshaded. out_w x out_h must be the
 * size the result is drawn at: presets take OutputSize from the target, so a
 * larger target with a sub-rect drawn from it would quantise to the wrong
 * pixel grid. Leaves framebuffer 0, texture unit 0 and no sampler objects
 * bound; the viewport is the caller's to reset. */
unsigned gb_lrs_render(unsigned src_tex, int src_w, int src_h, int out_w, int out_h);

/* Drop history and feedback frames on the next render (after a state load,
 * so a CRT preset's persistence does not bleed the old scene through). */
void gb_lrs_clear_history(void);

/* Parameters of the running preset, in the order the shaders declare them. */
int gb_lrs_param_count(void);
const GBLrsParam* gb_lrs_param(int index);
int gb_lrs_find_param(const char* name);
bool gb_lrs_set_param(int index, float value);

/* Frees the chain and its GL objects; call with the context current. */
void gb_lrs_shutdown(void);

/* Replace the error text, for failures the caller detects itself. */
void gb_lrs_set_error(const char* message);

/* Windows/ANGLE: ANGLE turns each GLSL shader into HLSL and compiles that with
 * Microsoft's D3DCompile, looked up from d3dcompiler_47.dll on first use. This
 * routes that lookup through the chain, so gb_lrs_load(path, true) can turn
 * the HLSL optimizer off. Call once, right after the GL context is created and
 * before any program is linked. Does nothing elsewhere. */
void gb_lrs_hook_shader_compiler(void);

/* ---- probing a preset in a child process ----
 * The HLSL optimizer recurses without bound on a few presets (the stock
 * vectorscale ones: a stack overflow even with a 1 GB stack) and kills the
 * process from one of ANGLE's threads -- not something that can be caught. So
 * a preset is first compiled by this executable run again, hidden, with
 * GBRECOMP_SHADER_PROBE=<path>: the child's gb_platform_init calls
 * gb_lrs_probe_child and exits before any ROM or save is touched. The game
 * keeps running meanwhile, and the child's compile lands in the program cache
 * (gl_program_cache.h), so loading the preset afterwards is quick. A child
 * that dies of a stack overflow is run once more with the optimizer off
 * (D3DCOMPILE_SKIP_OPTIMIZATION, ANGLE's own last resort for shaders that fail
 * to compile), and a preset that works that way is loaded that way. The child
 * also reports a preset whose passes GL rejects at draw time, which would
 * otherwise just draw black. A preset that fails to compile is not a probe
 * failure: it fails cleanly in-process too, with its message.
 * Linux runs the same child (fork + exec of /proc/self/exe), for a driver
 * compiler that crashes or hangs on a preset; there is no optimizer retry, and
 * the driver's own shader cache is what makes the load afterwards quick. */
typedef enum GBLrsProbeResult {
    GB_LRS_PROBE_RUNNING,          /* the child is still compiling */
    GB_LRS_PROBE_OK,               /* the child compiled it and rendered frames */
    GB_LRS_PROBE_OK_UNOPTIMIZED,   /* ...only with the HLSL optimizer off */
    GB_LRS_PROBE_FAILED,           /* crashed, hung or drew nothing; gb_lrs_error() says which */
    GB_LRS_PROBE_SKIPPED,          /* nothing to report: no probe, or no child could run */
} GBLrsProbeResult;

/* Start probing `path`, cancelling any probe already running. RUNNING, or
 * SKIPPED where no child can be started (then load directly). */
GBLrsProbeResult gb_lrs_probe_start(const char* path);
/* RUNNING while the child runs; then its verdict, once. */
GBLrsProbeResult gb_lrs_probe_poll(void);
void gb_lrs_probe_cancel(void);
double gb_lrs_probe_seconds(void);   /* how long the running probe has taken */

/* Child side: load `path`, render a few frames; returns the exit code. The
 * parent sets GBRECOMP_SHADER_PROBE_UNOPTIMIZED for the second attempt. */
int gb_lrs_probe_child(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* GB_LIBRASHADER_CHAIN_H */
