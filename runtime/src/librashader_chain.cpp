/*
 * See librashader_chain.h. librashader's OpenGL runtime driven over the
 * runtime's GL context: the game texture in, a texture the size of the
 * on-screen picture out.
 */
#include "librashader_chain.h"

#include <SDL.h>
#include <SDL_opengles2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
#define GB_LRS_SUPPORTED 1
#define LIBRA_RUNTIME_OPENGL
#include "librashader/librashader_ld.h"
#endif

#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif

namespace fs = std::filesystem;

namespace {

struct Param {
    std::string name;
    std::string description;
    GBLrsParam pub;
};

bool g_tried = false;
bool g_ok = false;
std::string g_status = "librashader has not been loaded yet";
std::string g_error;
std::string g_path;
std::vector<Param> g_params;
bool g_clear_history = false;
bool g_disk_cache = true;

/* Probe child exit code for "loaded, but GL rejected a pass at draw time". */
constexpr int kProbeDrewNothing = 4;

/* Compile mode of the running chain, and what ANGLE's D3DCompile calls get
 * (read on ANGLE's compile threads). They differ only while a load runs. */
bool g_chain_unoptimized = false;
std::atomic<bool> g_compile_unoptimized{false};

#ifdef _WIN32
/* d3dcompiler.h, which the MinGW headers do not always carry. */
typedef HRESULT(WINAPI* PFN_gbD3DCompile)(LPCVOID data, SIZE_T size, LPCSTR name, const void* defines,
                                          void* include, LPCSTR entry, LPCSTR target, UINT flags1,
                                          UINT flags2, void** code, void** errors);
constexpr UINT kD3DCompileSkipOptimization = 1u << 2;
constexpr UINT kD3DCompileOptimizationLevelMask = (1u << 14) | (1u << 15);
PFN_gbD3DCompile g_d3d_compile = NULL;
bool g_compiler_hooked = false;

HRESULT WINAPI hooked_d3d_compile(LPCVOID data, SIZE_T size, LPCSTR name, const void* defines, void* include,
                                  LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2, void** code,
                                  void** errors) {
    if (g_compile_unoptimized.load(std::memory_order_relaxed)) {
        flags1 = (flags1 & ~kD3DCompileOptimizationLevelMask) | kD3DCompileSkipOptimization;
    }
    return g_d3d_compile(data, size, name, defines, include, entry, target, flags1, flags2, code, errors);
}

/* Stands in for GetProcAddress in ANGLE's import table. */
FARPROC WINAPI hooked_get_proc_address(HMODULE module, LPCSTR name) {
    FARPROC proc = GetProcAddress(module, name);
    if (proc && HIWORD((ULONG_PTR)name) && strcmp(name, "D3DCompile") == 0) {
        g_d3d_compile = (PFN_gbD3DCompile)proc;
        return (FARPROC)hooked_d3d_compile;
    }
    return proc;
}

/* Point `module`'s import of dll!func at `replacement`. */
bool patch_import(HMODULE module, const char* dll, const char* func, void* replacement) {
    BYTE* base = (BYTE*)module;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (const IMAGE_IMPORT_DESCRIPTOR* imp = (const IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
         imp->Name; imp++) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        const IMAGE_THUNK_DATA* names =
            (const IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA* slots = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const IMAGE_IMPORT_BY_NAME* by_name = (const IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)by_name->Name, func) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old)) {
                return false;
            }
            slots->u1.Function = (ULONG_PTR)replacement;
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
            return true;
        }
    }
    return false;
}
#endif

GLuint g_out_tex = 0;
int g_out_w = 0;
int g_out_h = 0;

/* glBindSampler is ES 3.0, so it is not in the ES 2.0 headers the runtime
 * compiles against. librashader binds a sampler object to every unit it
 * samples from, and a sampler object overrides the texture's own filter --
 * left on unit 0 it would decide the filtering of every later draw. */
typedef void (GL_APIENTRY* PFN_gbBindSampler)(GLuint unit, GLuint sampler);
PFN_gbBindSampler g_bind_sampler = NULL;
GLint g_sampler_units = 0;

#ifdef GB_LRS_SUPPORTED
libra_instance_t g_lr;
libra_gl_filter_chain_t g_chain = NULL;
size_t g_frame = 0;
Uint64 g_last_counter = 0;
unsigned g_frame_failures = 0;
unsigned g_gl_error_frames = 0;

/* librashader calls the loader as extern "system", which is __stdcall on
 * 32-bit x86; its header's libra_gl_loader_t names no convention (cdecl). */
#ifdef _WIN32
#define GB_LRS_LOADER_CALL __stdcall
#else
#define GB_LRS_LOADER_CALL
#endif
const void* GB_LRS_LOADER_CALL gl_loader(const char* name) {
    return SDL_GL_GetProcAddress(name);
}

void drop(libra_error_t e) {
    if (e) g_lr.error_free(&e);
}

/* librashader's message for `e`, then free it. */
std::string take_error(libra_error_t e, const char* what) {
    std::string out = what;
    char* msg = NULL;
    if (e && g_lr.error_write(e, &msg) == 0 && msg) {
        out += ": ";
        out += msg;
        g_lr.error_free_string(&msg);
    } else {
        out += ": librashader error ";
        out += std::to_string(e ? (int)g_lr.error_errno(e) : -1);
    }
    drop(e);
    return out;
}

void chain_free(void) {
    if (g_chain) drop(g_lr.gl_filter_chain_free(&g_chain));
    g_chain = NULL;
}
#endif

/* librashader needs ES 3.0 or desktop 3.3. */
bool gl_version_ok(const char* version) {
    if (!version) return false;
    const char* p = strstr(version, "OpenGL ES");
    const bool es = p != NULL;
    if (es) {
        p += 9;
        while (*p && (*p < '0' || *p > '9')) p++;
    } else {
        p = version;
    }
    int major = 0, minor = 0;
    if (sscanf(p, "%d.%d", &major, &minor) < 1) return false;
    return es ? major >= 3 : (major > 3 || (major == 3 && minor >= 3));
}

void restore_gl_state(void) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (g_bind_sampler) {
        for (GLint unit = 0; unit < g_sampler_units; unit++) g_bind_sampler((GLuint)unit, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

bool ensure_output(int w, int h) {
    if (g_out_tex && g_out_w == w && g_out_h == h) return true;
    while (glGetError() != GL_NO_ERROR) {}
    if (!g_out_tex) glGenTextures(1, &g_out_tex);
    glBindTexture(GL_TEXTURE_2D, g_out_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (glGetError() != GL_NO_ERROR) {
        g_out_w = g_out_h = 0;
        return false;
    }
    g_out_w = w;
    g_out_h = h;
    return true;
}

/* ---- declaration order ---------------------------------------------------
 * librashader returns a preset's parameters out of a hash map. RetroArch lists
 * them in the order the shaders declare them, and presets rely on that: a
 * zero-range "parameter" is a section heading for the ones after it. So the
 * order is recovered from the sources: the preset's passes (following
 * #reference), and each pass's `#pragma parameter` lines (following #include).
 */

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string unquote(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == '"') {
        size_t close = s.find('"', 1);
        return close == std::string::npos ? s.substr(1) : s.substr(1, close - 1);
    }
    /* Unquoted values end at a trailing comment. */
    size_t cut = s.find_first_of("#;");
    if (cut != std::string::npos) s = trim(s.substr(0, cut));
    return s;
}

void scan_source(const fs::path& file, std::vector<std::string>& order,
                 std::set<std::string>& seen_names, std::set<fs::path>& seen_files, int depth) {
    if (depth > 16 || !seen_files.insert(file.lexically_normal()).second) return;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.rfind("#include", 0) == 0) {
            const std::string inc = unquote(t.substr(8));
            if (!inc.empty()) scan_source(file.parent_path() / fs::u8path(inc), order, seen_names, seen_files, depth + 1);
        } else if (t.rfind("#pragma parameter", 0) == 0) {
            const std::string rest = trim(t.substr(17));
            const std::string name = rest.substr(0, rest.find_first_of(" \t"));
            if (!name.empty() && seen_names.insert(name).second) order.push_back(name);
        }
    }
}

void scan_preset(const fs::path& file, std::vector<std::string>& order,
                 std::set<std::string>& seen_names, std::set<fs::path>& seen_files, int depth) {
    if (depth > 16 || !seen_files.insert(file.lexically_normal()).second) return;
    std::ifstream in(file);
    std::string line;
    std::vector<std::pair<int, std::string>> passes;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.rfind("#reference", 0) == 0) {
            const std::string ref = unquote(t.substr(10));
            if (!ref.empty()) scan_preset(file.parent_path() / fs::u8path(ref), order, seen_names, seen_files, depth + 1);
            continue;
        }
        if (t.rfind("shader", 0) != 0 || t.size() < 7 || t[6] < '0' || t[6] > '9') continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        char* end = NULL;
        const long index = strtol(t.c_str() + 6, &end, 10);
        const size_t digits_end = (size_t)(end - t.c_str());
        if (!trim(t.substr(digits_end, eq - digits_end)).empty()) continue;   /* shader0_foo = ... */
        passes.emplace_back((int)index, unquote(t.substr(eq + 1)));
    }
    std::stable_sort(passes.begin(), passes.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& pass : passes) {
        if (!pass.second.empty()) {
            scan_source(file.parent_path() / fs::u8path(pass.second), order, seen_names, seen_files, depth + 1);
        }
    }
}

void sort_params(const char* preset_path) {
    std::vector<std::string> order;
    std::set<std::string> seen_names;
    std::set<fs::path> seen_files;
    try {
        scan_preset(fs::u8path(preset_path), order, seen_names, seen_files, 0);
    } catch (const std::exception&) {
        /* A path the scan cannot follow only costs the ordering. */
    }
    std::unordered_map<std::string, size_t> rank;
    for (size_t i = 0; i < order.size(); i++) rank.emplace(order[i], i);
    std::stable_sort(g_params.begin(), g_params.end(), [&](const Param& a, const Param& b) {
        const auto ra = rank.find(a.name), rb = rank.find(b.name);
        const size_t ia = ra == rank.end() ? SIZE_MAX : ra->second;
        const size_t ib = rb == rank.end() ? SIZE_MAX : rb->second;
        return ia != ib ? ia < ib : a.name < b.name;
    });
}

void republish_params(void) {
    for (Param& p : g_params) {
        p.pub.name = p.name.c_str();
        p.pub.description = p.description.c_str();
    }
}

}  /* anonymous namespace */

extern "C" {

bool gb_lrs_init(void) {
    if (g_tried) return g_ok;
    g_tried = true;
#ifdef GB_LRS_SUPPORTED
    const char* version = (const char*)glGetString(GL_VERSION);
    if (!gl_version_ok(version)) {
        g_status = std::string("librashader needs OpenGL ES 3.0 or OpenGL 3.3; this context is ") +
                   (version ? version : "unknown");
        return false;
    }
    g_lr = librashader_load_instance();
    if (!g_lr.instance_loaded) {
        if (g_lr.instance_abi_version != __librashader__noop_instance_abi_version) {
            g_status = "librashader was found but its ABI is " +
                       std::to_string((unsigned long long)g_lr.instance_abi_version()) +
                       ", this build expects " + std::to_string(LIBRASHADER_CURRENT_ABI);
        } else {
            g_status = "librashader is not installed (no librashader library beside the executable)";
        }
        return false;
    }
    if (g_lr.gl_filter_chain_create == __librashader__noop_gl_filter_chain_create) {
        g_status = "librashader was built without its OpenGL runtime (feature runtime-opengl)";
        return false;
    }
    g_bind_sampler = (PFN_gbBindSampler)SDL_GL_GetProcAddress("glBindSampler");
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &g_sampler_units);
    g_sampler_units = std::min<GLint>(std::max<GLint>(g_sampler_units, 0), 64);
    g_status = "librashader API " + std::to_string((unsigned long long)g_lr.instance_api_version()) +
               " on " + version;
    g_ok = true;
    fprintf(stderr, "[SHADER] %s\n", g_status.c_str());
    return true;
#else
    g_status = "librashader is not supported on this platform";
    return false;
#endif
}

bool gb_lrs_available(void) { return g_ok; }
const char* gb_lrs_status(void) { return g_status.c_str(); }
const char* gb_lrs_current(void) { return g_path.c_str(); }
const char* gb_lrs_error(void) { return g_error.c_str(); }

bool gb_lrs_active(void) {
#ifdef GB_LRS_SUPPORTED
    return g_chain != NULL;
#else
    return false;
#endif
}

bool gb_lrs_unoptimized(void) {
    return gb_lrs_active() && g_chain_unoptimized;
}

bool gb_lrs_load(const char* path, bool unoptimized) {
#ifdef GB_LRS_SUPPORTED
    if (!path || !*path) {
        chain_free();
        g_params.clear();
        g_path.clear();
        g_error.clear();
        g_chain_unoptimized = false;
        g_compile_unoptimized = false;
        return true;
    }
    if (!gb_lrs_init()) {
        g_error = g_status;
        fprintf(stderr, "[SHADER] %s: %s\n", path, g_error.c_str());
        return false;
    }

    const Uint64 load_started = SDL_GetPerformanceCounter();
    libra_preset_ctx_t ctx = NULL;
    drop(g_lr.preset_ctx_create(&ctx));
    if (ctx) drop(g_lr.preset_ctx_set_runtime(&ctx, LIBRA_PRESET_CTX_RUNTIME_GL_CORE));

    libra_preset_opt_t popt;
    memset(&popt, 0, sizeof(popt));
    popt.version = LIBRASHADER_CURRENT_VERSION;
    popt.original_aspect_uniforms = true;   /* aspect_ratio 0 = the source's own */
    popt.frametime_uniforms = true;         /* frames_per_second + frametime_delta are set */

    libra_shader_preset_t preset = NULL;
    libra_error_t e = g_lr.preset_create_with_options(path, ctx ? &ctx : NULL, &popt, &preset);
    if (ctx) drop(g_lr.preset_ctx_free(&ctx));
    if (e || !preset) {
        g_error = e ? take_error(e, "preset") : std::string("preset: could not read ") + path;
        if (preset) drop(g_lr.preset_free(&preset));
        fprintf(stderr, "[SHADER] %s: %s\n", path, g_error.c_str());
        return false;
    }

    /* Parameters must be read before the chain is created, which consumes the
     * preset. A preset without a readable list still runs. */
    std::vector<Param> params;
    libra_preset_param_list_t list;
    memset(&list, 0, sizeof(list));
    e = g_lr.preset_get_runtime_params(&preset, &list);
    if (e) {
        drop(e);
    } else {
        for (uint64_t i = 0; i < list.length; i++) {
            const libra_preset_param_t& src = list.parameters[i];
            Param p;
            p.name = src.name ? src.name : "";
            p.description = src.description ? src.description : p.name;
            p.pub.minimum = src.minimum;
            p.pub.maximum = src.maximum;
            p.pub.step = src.step;
            p.pub.initial = p.pub.value = src.initial;
            params.push_back(std::move(p));
        }
        drop(g_lr.preset_free_runtime_params(list));
    }

    filter_chain_gl_opt_t copt;
    memset(&copt, 0, sizeof(copt));
    copt.version = LIBRASHADER_CURRENT_VERSION;
    copt.glsl_version = 0;      /* from the context: GLSL ES 3.10 on ANGLE */
    copt.use_dsa = false;       /* DSA is desktop GL 4.5 only */
    copt.disable_cache = !g_disk_cache;
    libra_gl_filter_chain_t chain = NULL;
    g_compile_unoptimized = unoptimized;
    if (unoptimized) fprintf(stderr, "[SHADER] %s: compiling without the HLSL optimizer\n", path);
    e = g_lr.gl_filter_chain_create(&preset, (libra_gl_loader_t)gl_loader, &copt, &chain);
    /* The create takes the preset whether or not it succeeds; free whatever is
     * still ours rather than relying on which. */
    if (preset) drop(g_lr.preset_free(&preset));
    restore_gl_state();
    if (e || !chain) {
        g_compile_unoptimized = g_chain_unoptimized;
        g_error = e ? take_error(e, "compile") : std::string("compile: librashader returned no chain");
        fprintf(stderr, "[SHADER] %s: %s\n", path, g_error.c_str());
        return false;
    }

    /* Only now drop the old chain: a preset that fails to compile should not
     * also cost the one already running. */
    chain_free();
    g_chain = chain;
    g_chain_unoptimized = unoptimized;
    g_params = std::move(params);
    for (Param& p : g_params) {
        float v = 0.0f;
        e = g_lr.gl_filter_chain_get_param(&g_chain, p.name.c_str(), &v);
        if (e) drop(e);
        else p.pub.initial = p.pub.value = v;
    }
    sort_params(path);
    republish_params();
    g_path = path;
    g_error.clear();
    g_frame = 0;
    g_frame_failures = 0;
    g_gl_error_frames = 0;
    g_last_counter = 0;
    g_clear_history = false;
    fprintf(stderr, "[SHADER] loaded %s (%zu parameters) in %.0f ms\n", path, g_params.size(),
            (double)(SDL_GetPerformanceCounter() - load_started) * 1000.0 / (double)SDL_GetPerformanceFrequency());
    return true;
#else
    (void)unoptimized;
    if (!path || !*path) return true;
    g_error = g_status;
    return false;
#endif
}

void gb_lrs_hook_shader_compiler(void) {
#ifdef _WIN32
    if (g_compiler_hooked) return;
    HMODULE angle = GetModuleHandleW(L"libGLESv2.dll");
    g_compiler_hooked =
        angle && patch_import(angle, "KERNEL32.dll", "GetProcAddress", (void*)hooked_get_proc_address);
#endif
}

unsigned gb_lrs_render(unsigned src_tex, int src_w, int src_h, int out_w, int out_h) {
#ifdef GB_LRS_SUPPORTED
    if (!g_chain || !src_tex || src_w <= 0 || src_h <= 0 || out_w <= 0 || out_h <= 0) return 0;
    if (!ensure_output(out_w, out_h)) return 0;

    /* librashader samples through sampler objects whose min filter is always a
     * mipmap one, and a texture with only level 0 is incomplete under those:
     * it reads as black. Capping the chain at level 0 makes it complete. Set
     * every frame because the runtime recreates its texture on size changes. */
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    const Uint64 now = SDL_GetPerformanceCounter();
    const Uint64 delta = g_last_counter ? now - g_last_counter : 0;
    g_last_counter = now;

    libra_image_gl_t in = { src_tex, GL_RGBA8, (uint32_t)src_w, (uint32_t)src_h };
    libra_image_gl_t out = { g_out_tex, GL_RGBA8, (uint32_t)out_w, (uint32_t)out_h };
    frame_gl_opt_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.version = LIBRASHADER_CURRENT_VERSION;
    opt.clear_history = g_clear_history;
    opt.frame_direction = 1;
    opt.total_subframes = 1;
    opt.current_subframe = 1;
    opt.aspect_ratio = 0.0f;                   /* the source's own: GB pixels are square */
    opt.frames_per_second = 4194304.0f / 70224.0f;
    opt.frametime_delta = (uint32_t)(delta * 1000 / SDL_GetPerformanceFrequency());
    opt.brightness_nits = 200.0f;

    /* NULL viewport and MVP: the whole output texture, librashader's default
     * projection. */
    while (glGetError() != GL_NO_ERROR) {}
    libra_error_t e = g_lr.gl_filter_chain_frame(&g_chain, g_frame, in, out, NULL, NULL, &opt);
    /* librashader does not check GL errors itself; a call the driver rejects
     * just leaves a pass (or the history) black. Report the first one. */
    const GLenum gl_error = glGetError();
    if (gl_error != GL_NO_ERROR && g_gl_error_frames++ == 0) {
        fprintf(stderr, "[SHADER] GL error 0x%04X during frame %zu of %s\n",
                gl_error, g_frame, g_path.c_str());
    }
    restore_gl_state();
    if (e) {
        /* Keep the first message; a chain failing every frame would otherwise
         * rewrite it 60 times a second. */
        if (g_frame_failures++ == 0) {
            g_error = take_error(e, "frame");
            fprintf(stderr, "[SHADER] %s\n", g_error.c_str());
        } else {
            drop(e);
        }
        return 0;
    }
    g_frame++;
    g_clear_history = false;
    return g_out_tex;
#else
    (void)src_tex; (void)src_w; (void)src_h; (void)out_w; (void)out_h;
    return 0;
#endif
}

void gb_lrs_clear_history(void) {
    g_clear_history = true;
}

int gb_lrs_param_count(void) { return (int)g_params.size(); }

const GBLrsParam* gb_lrs_param(int index) {
    if (index < 0 || index >= (int)g_params.size()) return NULL;
    return &g_params[(size_t)index].pub;
}

int gb_lrs_find_param(const char* name) {
    if (!name) return -1;
    for (size_t i = 0; i < g_params.size(); i++) {
        if (g_params[i].name == name) return (int)i;
    }
    return -1;
}

bool gb_lrs_set_param(int index, float value) {
#ifdef GB_LRS_SUPPORTED
    if (!g_chain || index < 0 || index >= (int)g_params.size()) return false;
    GBLrsParam& p = g_params[(size_t)index].pub;
    if (p.maximum > p.minimum) value = std::min(std::max(value, p.minimum), p.maximum);
    libra_error_t e = g_lr.gl_filter_chain_set_param(&g_chain, g_params[(size_t)index].name.c_str(), value);
    if (e) {
        drop(e);
        return false;
    }
    p.value = value;
    return true;
#else
    (void)index; (void)value;
    return false;
#endif
}

void gb_lrs_set_error(const char* message) {
    g_error = message ? message : "";
}

#ifdef _WIN32
namespace {
HANDLE g_probe_process = NULL;
std::string g_probe_path;
Uint64 g_probe_started = 0;   /* the first attempt: what the menu shows */
Uint64 g_child_started = 0;   /* the running attempt: what the time limit is on */
bool g_child_unoptimized = false;

/* Run this executable as a probe child for `path`. */
bool start_child(const std::string& path, bool unoptimized) {
    auto widen = [](const std::string& s) {
        std::wstring w(s.size() + 1, L'\0');
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], (int)w.size());
        w.resize(n > 0 ? (size_t)n - 1 : 0);
        return w;
    };
    wchar_t exe[MAX_PATH];
    const DWORD exe_len = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (exe_len == 0 || exe_len >= MAX_PATH) return false;

    /* The inherited environment, minus anything the probe sets itself. */
    const std::wstring unoptimized_var = L"GBRECOMP_SHADER_PROBE_UNOPTIMIZED";
    std::vector<std::pair<std::wstring, std::wstring>> vars = {
        { L"GBRECOMP_SHADER_PROBE", widen(path) },
        { L"GBRECOMP_NO_LAUNCHER", L"1" },
        { L"GBRECOMP_HIDDEN_WINDOW", L"1" },
        { L"SDL_AUDIODRIVER", L"dummy" },
    };
    auto is_var = [](const wchar_t* e, const std::wstring& name) {
        return _wcsnicmp(e, name.c_str(), name.size()) == 0 && e[name.size()] == L'=';
    };
    std::wstring env;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* e = block; *e; e += wcslen(e) + 1) {
            bool ours = is_var(e, unoptimized_var);
            for (const auto& v : vars) ours = ours || is_var(e, v.first);
            if (!ours) env.append(e).push_back(L'\0');
        }
        FreeEnvironmentStringsW(block);
    }
    if (unoptimized) vars.emplace_back(unoptimized_var, L"1");
    for (const auto& v : vars) env.append(v.first).append(L"=").append(v.second).push_back(L'\0');
    env.push_back(L'\0');

    std::wstring cmdline = L"\"" + std::wstring(exe) + L"\"";
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    /* Below normal priority: the game this probe serves keeps running, and the
     * child is only there to compile. */
    if (!CreateProcessW(exe, &cmdline[0], NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | BELOW_NORMAL_PRIORITY_CLASS,
                        &env[0], NULL, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    g_probe_process = pi.hProcess;
    g_probe_path = path;
    g_child_started = SDL_GetPerformanceCounter();
    g_child_unoptimized = unoptimized;
    return true;
}
}  /* anonymous namespace */
#elif defined(__linux__)
namespace {
pid_t g_probe_pid = 0;
std::string g_probe_path;
Uint64 g_probe_started = 0;

/* Run this executable as a probe child for `path`. Everything the child needs
 * is built before fork(): between fork and exec it only makes system calls. */
bool start_child(const std::string& path) {
    static const char* const kOurs[] = {
        "GBRECOMP_SHADER_PROBE", "GBRECOMP_NO_LAUNCHER", "GBRECOMP_HIDDEN_WINDOW",
        "SDL_AUDIODRIVER", "GBRECOMP_SHADER_PROBE_UNOPTIMIZED",
    };
    std::vector<std::string> env;
    for (char** e = environ; e && *e; e++) {
        bool ours = false;
        for (const char* name : kOurs) {
            const size_t n = strlen(name);
            ours = ours || (strncmp(*e, name, n) == 0 && (*e)[n] == '=');
        }
        if (!ours) env.emplace_back(*e);
    }
    env.push_back("GBRECOMP_SHADER_PROBE=" + path);
    env.push_back("GBRECOMP_NO_LAUNCHER=1");
    env.push_back("GBRECOMP_HIDDEN_WINDOW=1");
    env.push_back("SDL_AUDIODRIVER=dummy");
    std::vector<char*> envp;
    for (std::string& s : env) envp.push_back(&s[0]);
    envp.push_back(NULL);
    char exe[] = "/proc/self/exe";
    char* argv[] = { exe, NULL };
    struct rlimit files;
    const int max_fd = getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur != RLIM_INFINITY
                           ? (int)std::min<rlim_t>(files.rlim_cur, 65536) : 65536;

    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        /* Like the Windows child: no inherited handles (the debug server's
         * socket among them), no crash report for a preset that kills it,
         * and a lower priority than the game it serves. stderr stays shared,
         * so its log lines land in the game's. */
        if (syscall(436 /* close_range */, 3u, ~0u, 0u) != 0) {
            for (int fd = 3; fd < max_fd; fd++) close(fd);
        }
        struct rlimit no_core = { 0, 0 };
        setrlimit(RLIMIT_CORE, &no_core);
        setpriority(PRIO_PROCESS, 0, 10);
        execve(exe, argv, envp.data());
        _exit(127);
    }
    g_probe_pid = pid;
    g_probe_path = path;
    g_probe_started = SDL_GetPerformanceCounter();
    return true;
}
}  /* anonymous namespace */
#endif

void gb_lrs_probe_cancel(void) {
#ifdef _WIN32
    if (!g_probe_process) return;
    TerminateProcess(g_probe_process, 0);
    WaitForSingleObject(g_probe_process, 5000);
    CloseHandle(g_probe_process);
    g_probe_process = NULL;
    g_probe_path.clear();
#elif defined(__linux__)
    if (!g_probe_pid) return;
    kill(g_probe_pid, SIGKILL);
    waitpid(g_probe_pid, NULL, 0);
    g_probe_pid = 0;
    g_probe_path.clear();
#endif
}

double gb_lrs_probe_seconds(void) {
#ifdef _WIN32
    if (g_probe_process) {
        return (double)(SDL_GetPerformanceCounter() - g_probe_started) / (double)SDL_GetPerformanceFrequency();
    }
#elif defined(__linux__)
    if (g_probe_pid) {
        return (double)(SDL_GetPerformanceCounter() - g_probe_started) / (double)SDL_GetPerformanceFrequency();
    }
#endif
    return 0.0;
}

GBLrsProbeResult gb_lrs_probe_start(const char* path) {
#ifdef _WIN32
    gb_lrs_probe_cancel();
    if (!path || !*path || !start_child(path, false)) return GB_LRS_PROBE_SKIPPED;
    g_probe_started = g_child_started;
    return GB_LRS_PROBE_RUNNING;
#elif defined(__linux__)
    gb_lrs_probe_cancel();
    if (!path || !*path || !start_child(path)) return GB_LRS_PROBE_SKIPPED;
    return GB_LRS_PROBE_RUNNING;
#else
    (void)path;
    return GB_LRS_PROBE_SKIPPED;
#endif
}

GBLrsProbeResult gb_lrs_probe_poll(void) {
#ifdef _WIN32
    if (!g_probe_process) return GB_LRS_PROBE_SKIPPED;
    DWORD code = 0;
    if (WaitForSingleObject(g_probe_process, 0) != WAIT_OBJECT_0) {
        /* Mega Bezel's biggest presets take about a minute cold; five is a hang. */
        const double seconds =
            (double)(SDL_GetPerformanceCounter() - g_child_started) / (double)SDL_GetPerformanceFrequency();
        if (seconds < 300.0) return GB_LRS_PROBE_RUNNING;
        TerminateProcess(g_probe_process, 0xDEAD);
        WaitForSingleObject(g_probe_process, 5000);
        code = 0xDEAD;
    } else {
        GetExitCodeProcess(g_probe_process, &code);
    }
    CloseHandle(g_probe_process);
    g_probe_process = NULL;
    const std::string path = g_probe_path;
    const bool unoptimized = g_child_unoptimized;
    g_probe_path.clear();
    fprintf(stderr, "[SHADER] probe of %s%s: exit 0x%08lX\n", path.c_str(),
            unoptimized ? " (HLSL optimizer off)" : "", (unsigned long)code);
    /* The HLSL optimizer ran out of stack: try once more without it, when
     * ANGLE's compiler is known to go through the hook. */
    if (code == 0xC00000FDu && !unoptimized && g_d3d_compile && start_child(path, true)) {
        return GB_LRS_PROBE_RUNNING;
    }
    /* 0 = the child survived; kProbeDrewNothing is its own verdict. Other
     * small codes mean it never reached the probe (no window, no librashader),
     * which proves nothing either way. NTSTATUS errors (0xC...) are crashes. */
    if (code == 0) return unoptimized ? GB_LRS_PROBE_OK_UNOPTIMIZED : GB_LRS_PROBE_OK;
    if (code == kProbeDrewNothing) {
        g_error = path + " cannot be drawn on this renderer (GL rejected one of its passes in a test run), "
                         "so it was not loaded";
        return GB_LRS_PROBE_FAILED;
    }
    if (code == 0xDEAD) {
        g_error = path + " did not finish compiling within five minutes in a test run, so it was not loaded";
        return GB_LRS_PROBE_FAILED;
    }
    if ((code & 0xC0000000u) == 0xC0000000u) {
        char buf[96];
        snprintf(buf, sizeof(buf), "0x%08lX%s", (unsigned long)code,
                 code != 0xC00000FDu ? ""
                 : unoptimized       ? ", a stack overflow in the shader compiler even with its optimizer off"
                                     : ", a stack overflow in the shader compiler");
        g_error = path + " crashed a test run (" + buf + "), so it was not loaded";
        return GB_LRS_PROBE_FAILED;
    }
    return GB_LRS_PROBE_SKIPPED;
#elif defined(__linux__)
    if (!g_probe_pid) return GB_LRS_PROBE_SKIPPED;
    int status = 0;
    bool timed_out = false;
    const pid_t done = waitpid(g_probe_pid, &status, WNOHANG);
    if (done == 0) {
        /* Mega Bezel's biggest presets take about a minute cold; five is a hang. */
        if (gb_lrs_probe_seconds() < 300.0) return GB_LRS_PROBE_RUNNING;
        kill(g_probe_pid, SIGKILL);
        waitpid(g_probe_pid, &status, 0);
        timed_out = true;
    }
    g_probe_pid = 0;
    const std::string path = g_probe_path;
    g_probe_path.clear();
    if (done < 0) return GB_LRS_PROBE_SKIPPED;   /* reaped elsewhere: no verdict */
    if (timed_out) {
        fprintf(stderr, "[SHADER] probe of %s: timed out\n", path.c_str());
        g_error = path + " did not finish compiling within five minutes in a test run, so it was not loaded";
        return GB_LRS_PROBE_FAILED;
    }
    if (WIFSIGNALED(status)) {
        const int sig = WTERMSIG(status);
        fprintf(stderr, "[SHADER] probe of %s: killed by signal %d\n", path.c_str(), sig);
        const char* name = strsignal(sig);
        g_error = path + " crashed a test run (signal " + std::to_string(sig) + (name ? std::string(", ") + name : "") +
                  "), so it was not loaded";
        return GB_LRS_PROBE_FAILED;
    }
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    fprintf(stderr, "[SHADER] probe of %s: exit %d\n", path.c_str(), code);
    /* 0 = the child survived; kProbeDrewNothing is its own verdict. Other
     * codes mean it never reached the probe (no window, no librashader),
     * which proves nothing either way. */
    if (code == 0) return GB_LRS_PROBE_OK;
    if (code == kProbeDrewNothing) {
        g_error = path + " cannot be drawn on this renderer (GL rejected one of its passes in a test run), "
                         "so it was not loaded";
        return GB_LRS_PROBE_FAILED;
    }
    return GB_LRS_PROBE_SKIPPED;
#else
    return GB_LRS_PROBE_SKIPPED;
#endif
}

int gb_lrs_probe_child(const char* preset) {
#ifdef _WIN32
    /* A crash is the expected way for a bad preset to fail here: no dialog. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    /* A copy: `preset` may be SDL_getenv's buffer, which the next SDL_getenv
     * overwrites on Windows. */
    const std::string path_copy = preset ? preset : "";
    const char* const path = path_copy.c_str();
    const bool unoptimized = SDL_getenv("GBRECOMP_SHADER_PROBE_UNOPTIMIZED") != NULL;
    /* librashader's own cache is one database file that a process holds open,
     * and one that cannot open it deletes it -- the game's, while the game is
     * running. The program cache is what carries the compile back anyway. */
    g_disk_cache = false;
    if (!gb_lrs_init()) return 3;
    const bool loaded = gb_lrs_load(path, unoptimized);
    bool drew = true;
    if (loaded) {
        /* ANGLE compiles pixel shaders for the actual output formats only at
         * the first draw, and that is where it rejects the ones it cannot
         * translate. */
        std::vector<unsigned char> gray((size_t)160 * 144 * 4, 128);
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 160, 144, 0, GL_RGBA, GL_UNSIGNED_BYTE, gray.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        for (int i = 0; i < 3; i++) drew = gb_lrs_render(tex, 160, 144, 640, 576) != 0 && drew;
        glFinish();
        glDeleteTextures(1, &tex);
#ifdef GB_LRS_SUPPORTED
        drew = drew && g_gl_error_frames == 0;
#endif
    }
    fprintf(stderr, "[SHADER] probe %s: %s\n", !loaded ? "load error" : drew ? "ok" : "drew nothing",
            loaded ? path : gb_lrs_error());
    fflush(stderr);
    return drew ? 0 : kProbeDrewNothing;
}

void gb_lrs_shutdown(void) {
    gb_lrs_probe_cancel();
#ifdef GB_LRS_SUPPORTED
    chain_free();
#endif
    g_params.clear();
    g_path.clear();
    if (g_out_tex) {
        glDeleteTextures(1, &g_out_tex);
        g_out_tex = 0;
    }
    g_out_w = g_out_h = 0;
}

}  /* extern "C" */
