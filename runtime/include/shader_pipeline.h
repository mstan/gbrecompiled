/**
 * @file shader_pipeline.h
 * @brief Tiny GLSL ES 2.0 post-process pipeline for the GB framebuffer.
 *
 * The runtime renders the 160x144 GB framebuffer (and the optional
 * 256x224 SGB border) into GL textures, then this module draws them
 * onto the window with a user-selectable fragment shader. Targets
 * GLES 2.0 so the same code runs on desktop Mesa, Mali GPUs (Orange
 * Pi), and any other embedded GL stack — the GLSL is `#version 100`
 * and the pipeline avoids VAOs / immutable storage / texture3D.
 *
 * Built-in shaders are bundled as embedded source strings. If a file
 * `shaders/<name>.frag.glsl` exists next to the executable (or in a
 * directory the user chooses), it overrides the embedded version.
 */
#ifndef GB_SHADER_PIPELINE_H
#define GB_SHADER_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GBShaderPipeline GBShaderPipeline;

GBShaderPipeline* gb_shader_pipeline_create(void);
void              gb_shader_pipeline_destroy(GBShaderPipeline* p);

/** How many shaders the UI can pick from (built-in + on-disk overrides). */
int gb_shader_pipeline_count(const GBShaderPipeline* p);

/** Display name for shader index. Stable strings, no copy needed. */
const char* gb_shader_pipeline_name(const GBShaderPipeline* p, int index);

/** Index of the active shader, -1 if none. */
int gb_shader_pipeline_active(const GBShaderPipeline* p);

/** Select a shader by index. Returns false if compile fails (active stays). */
bool gb_shader_pipeline_set_active(GBShaderPipeline* p, int index);

/** Convenience: select by name. Used at startup to restore prefs. */
bool gb_shader_pipeline_set_active_by_name(GBShaderPipeline* p, const char* name);

/** Force a recompile of all shaders, picking up any on-disk overrides. */
void gb_shader_pipeline_reload(GBShaderPipeline* p);

/**
 * Draw a textured quad into the current framebuffer using the active
 * shader. Coords are in pixels in the current viewport, top-left
 * origin (matching SDL conventions). The shader gets `u_tex`,
 * `u_viewport`, `u_src_size`, `u_dst_size`, `u_time` uniforms.
 *
 * Caller is responsible for calling glViewport() to match
 * viewport_w/viewport_h before calling this; we don't change it.
 */
void gb_shader_pipeline_draw(GBShaderPipeline* p,
                             unsigned int src_texture,
                             int src_w, int src_h,
                             int dst_x, int dst_y,
                             int dst_w, int dst_h,
                             int viewport_w, int viewport_h);

/** Where to look for `<name>.frag.glsl` overrides. NULL/empty disables. */
void gb_shader_pipeline_set_override_dir(GBShaderPipeline* p, const char* dir);

#ifdef __cplusplus
}
#endif

#endif /* GB_SHADER_PIPELINE_H */
