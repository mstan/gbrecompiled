/*
 * See shader_pipeline.h. Tiny GLSL ES 2.0 post-process pipeline.
 *
 * One full-screen quad per draw, one fragment shader at a time. The
 * vertex shader is fixed and just maps screen-pixel coords to clip
 * space and passes a UV through. Each fragment shader gets the source
 * texture plus source/destination size and elapsed time as uniforms.
 */
#include "shader_pipeline.h"

#include <SDL.h>
#include <SDL_opengles2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

/* Vertex shader, shared by every effect. */
constexpr const char* VS_SRC = R"GLSL(
#version 100
attribute vec2 a_pos;
attribute vec2 a_uv;
uniform vec2 u_viewport;
varying vec2 v_uv;
void main() {
    /* SDL-style top-left origin → GL clip space. */
    vec2 clip = (a_pos / u_viewport) * 2.0 - 1.0;
    clip.y = -clip.y;
    v_uv = a_uv;
    gl_Position = vec4(clip, 0.0, 1.0);
}
)GLSL";

/* Fragment shaders — GLSL ES 1.00 (`#version 100`, attribute/varying,
 * texture2D, mediump precision). Authored to look right at 3-6× upscale. */

constexpr const char* FS_SHARP = R"GLSL(
#version 100
precision mediump float;
uniform sampler2D u_tex;
varying vec2 v_uv;
void main() {
    gl_FragColor = texture2D(u_tex, v_uv);
}
)GLSL";

constexpr const char* FS_SMOOTH = R"GLSL(
#version 100
precision mediump float;
uniform sampler2D u_tex;
varying vec2 v_uv;
void main() {
    /* Identical fragment code to "sharp" — the difference is the
     * texture filter binding (sharp = NEAREST, smooth = LINEAR). */
    gl_FragColor = texture2D(u_tex, v_uv);
}
)GLSL";

/* Dot-matrix LCD: visible grid between cells + slight olive cast. */
constexpr const char* FS_LCD = R"GLSL(
#version 100
precision mediump float;
uniform sampler2D u_tex;
uniform vec2 u_src_size;
uniform vec2 u_dst_size;
varying vec2 v_uv;
void main() {
    vec3 color = texture2D(u_tex, v_uv).rgb;
    /* Distance from the nearest cell edge, normalized to upscale. */
    vec2 cell = fract(v_uv * u_src_size);
    vec2 scale = u_dst_size / u_src_size;
    vec2 edge = min(cell, vec2(1.0) - cell) * scale;
    float line = smoothstep(0.0, 1.0, min(edge.x, edge.y));
    color *= mix(0.78, 1.0, line);
    color = mix(color, color * vec3(0.96, 1.00, 0.92), 0.18);
    gl_FragColor = vec4(color, 1.0);
}
)GLSL";


struct ShaderEntry {
    std::string name;
    const char* embedded_fs;
    GLuint  program = 0;
    GLint   a_pos = -1;
    GLint   a_uv  = -1;
    GLint   u_tex = -1;
    GLint   u_viewport = -1;
    GLint   u_src_size = -1;
    GLint   u_dst_size = -1;
    GLint   u_time = -1;
    bool    nearest_filter = false;
    bool    compile_attempted = false;
};

bool log_compile(GLuint sh, const char* label) {
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok) return true;
    GLint len = 0;
    glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
    std::string log(len > 0 ? len : 1, '\0');
    if (len > 0) glGetShaderInfoLog(sh, len, NULL, &log[0]);
    fprintf(stderr, "[shader] compile failed (%s): %s\n", label, log.c_str());
    return false;
}

bool log_link(GLuint prog, const char* label) {
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok) return true;
    GLint len = 0;
    glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
    std::string log(len > 0 ? len : 1, '\0');
    if (len > 0) glGetProgramInfoLog(prog, len, NULL, &log[0]);
    fprintf(stderr, "[shader] link failed (%s): %s\n", label, log.c_str());
    return false;
}

GLuint compile_program(const char* vs_src, const char* fs_src, const char* label) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    if (!log_compile(vs, label)) { glDeleteShader(vs); return 0; }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    if (!log_compile(fs, label)) { glDeleteShader(vs); glDeleteShader(fs); return 0; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!log_link(prog, label)) { glDeleteProgram(prog); return 0; }
    return prog;
}

void cache_uniforms(ShaderEntry& e) {
    e.a_pos      = glGetAttribLocation(e.program,  "a_pos");
    e.a_uv       = glGetAttribLocation(e.program,  "a_uv");
    e.u_tex      = glGetUniformLocation(e.program, "u_tex");
    e.u_viewport = glGetUniformLocation(e.program, "u_viewport");
    e.u_src_size = glGetUniformLocation(e.program, "u_src_size");
    e.u_dst_size = glGetUniformLocation(e.program, "u_dst_size");
    e.u_time     = glGetUniformLocation(e.program, "u_time");
}

bool ensure_compiled(ShaderEntry& e, const std::string& override_dir) {
    if (e.compile_attempted) return e.program != 0;
    e.compile_attempted = true;

    std::string fs_src;
    if (!override_dir.empty()) {
        std::string path = override_dir + "/" + e.name + ".frag.glsl";
        std::ifstream f(path);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            fs_src = ss.str();
            fprintf(stderr, "[shader] loaded %s from %s\n",
                    e.name.c_str(), path.c_str());
        }
    }
    const char* fs = fs_src.empty() ? e.embedded_fs : fs_src.c_str();
    e.program = compile_program(VS_SRC, fs, e.name.c_str());
    if (!e.program) return false;
    cache_uniforms(e);
    return true;
}

}  /* anonymous namespace */

struct GBShaderPipeline {
    std::vector<ShaderEntry> shaders;
    int    active = -1;
    GLuint quad_vbo = 0;
    std::string override_dir;
};

GBShaderPipeline* gb_shader_pipeline_create(void) {
    GBShaderPipeline* p = new GBShaderPipeline();
    p->shaders.push_back({"sharp",  FS_SHARP,  0, -1, -1, -1, -1, -1, -1, -1, true,  false});
    p->shaders.push_back({"smooth", FS_SMOOTH, 0, -1, -1, -1, -1, -1, -1, -1, false, false});
    p->shaders.push_back({"lcd",    FS_LCD,    0, -1, -1, -1, -1, -1, -1, -1, false, false});
    glGenBuffers(1, &p->quad_vbo);
    p->active = 0;
    return p;
}

void gb_shader_pipeline_destroy(GBShaderPipeline* p) {
    if (!p) return;
    for (auto& e : p->shaders) {
        if (e.program) glDeleteProgram(e.program);
    }
    if (p->quad_vbo) glDeleteBuffers(1, &p->quad_vbo);
    delete p;
}

int gb_shader_pipeline_count(const GBShaderPipeline* p) {
    return p ? (int)p->shaders.size() : 0;
}

const char* gb_shader_pipeline_name(const GBShaderPipeline* p, int index) {
    if (!p || index < 0 || index >= (int)p->shaders.size()) return "";
    return p->shaders[index].name.c_str();
}

int gb_shader_pipeline_active(const GBShaderPipeline* p) {
    return p ? p->active : -1;
}

bool gb_shader_pipeline_set_active(GBShaderPipeline* p, int index) {
    if (!p || index < 0 || index >= (int)p->shaders.size()) return false;
    if (!ensure_compiled(p->shaders[index], p->override_dir)) return false;
    p->active = index;
    return true;
}

bool gb_shader_pipeline_set_active_by_name(GBShaderPipeline* p, const char* name) {
    if (!p || !name) return false;
    for (int i = 0; i < (int)p->shaders.size(); i++) {
        if (p->shaders[i].name == name) {
            return gb_shader_pipeline_set_active(p, i);
        }
    }
    return false;
}

void gb_shader_pipeline_reload(GBShaderPipeline* p) {
    if (!p) return;
    int prev = p->active;
    for (auto& e : p->shaders) {
        if (e.program) glDeleteProgram(e.program);
        e.program = 0;
        e.compile_attempted = false;
    }
    if (prev >= 0) gb_shader_pipeline_set_active(p, prev);
}

void gb_shader_pipeline_set_override_dir(GBShaderPipeline* p, const char* dir) {
    if (!p) return;
    p->override_dir = (dir && *dir) ? dir : "";
}

void gb_shader_pipeline_draw(GBShaderPipeline* p,
                             unsigned int src_texture,
                             int src_w, int src_h,
                             int dst_x, int dst_y,
                             int dst_w, int dst_h,
                             int viewport_w, int viewport_h) {
    if (!p || p->active < 0) return;
    ShaderEntry& e = p->shaders[p->active];
    if (!ensure_compiled(e, p->override_dir)) return;

    glUseProgram(e.program);

    const float x0 = (float)dst_x;
    const float y0 = (float)dst_y;
    const float x1 = (float)(dst_x + dst_w);
    const float y1 = (float)(dst_y + dst_h);
    const float verts[] = {
        x0, y0, 0.0f, 0.0f,
        x1, y0, 1.0f, 0.0f,
        x0, y1, 0.0f, 1.0f,
        x1, y0, 1.0f, 0.0f,
        x1, y1, 1.0f, 1.0f,
        x0, y1, 0.0f, 1.0f,
    };

    glBindBuffer(GL_ARRAY_BUFFER, p->quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);

    if (e.a_pos >= 0) {
        glEnableVertexAttribArray(e.a_pos);
        glVertexAttribPointer(e.a_pos, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4,
                              (void*)0);
    }
    if (e.a_uv >= 0) {
        glEnableVertexAttribArray(e.a_uv);
        glVertexAttribPointer(e.a_uv, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4,
                              (void*)(sizeof(float) * 2));
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_texture);
    GLint filter = e.nearest_filter ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (e.u_tex >= 0)      glUniform1i(e.u_tex, 0);
    if (e.u_viewport >= 0) glUniform2f(e.u_viewport, (float)viewport_w, (float)viewport_h);
    if (e.u_src_size >= 0) glUniform2f(e.u_src_size, (float)src_w, (float)src_h);
    if (e.u_dst_size >= 0) glUniform2f(e.u_dst_size, (float)dst_w, (float)dst_h);
    if (e.u_time >= 0)     glUniform1f(e.u_time, (float)SDL_GetTicks() * 0.001f);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    if (e.a_pos >= 0) glDisableVertexAttribArray(e.a_pos);
    if (e.a_uv  >= 0) glDisableVertexAttribArray(e.a_uv);
}
