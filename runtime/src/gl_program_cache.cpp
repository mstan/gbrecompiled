/*
 * See gl_program_cache.h. One file per cache entry, named by a hash of the
 * key: [u64 key size][key][value]. The driver may call back from its own
 * worker threads, and a preset probe (another process) may write the same
 * folder, so entries are written to a temporary file and renamed into place.
 */
#include "gl_program_cache.h"

#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define GB_EGLAPIENTRY __stdcall
#endif

namespace fs = std::filesystem;

namespace {

/* EGL_ANDROID_blob_cache, declared here rather than pulling in EGL headers. */
typedef intptr_t BlobSize;   /* EGLsizeiANDROID */
typedef void (*SetBlobFn)(const void* key, BlobSize key_size, const void* value, BlobSize value_size);
typedef BlobSize (*GetBlobFn)(const void* key, BlobSize key_size, void* value, BlobSize value_size);
#ifdef _WIN32
typedef void* (GB_EGLAPIENTRY* GetCurrentDisplayFn)(void);
typedef void (GB_EGLAPIENTRY* SetBlobCacheFuncsFn)(void* display, SetBlobFn set, GetBlobFn get);
#endif

/* Big presets are a few MB of programs; this holds hundreds of them. */
constexpr uintmax_t kCapBytes = 256ull << 20;

std::mutex g_mutex;
fs::path g_dir;
uintmax_t g_total = 0;
std::atomic<unsigned> g_temp_serial{0};

fs::path file_for(const void* key, BlobSize key_size) {
    uint64_t h = 1469598103934665603ull;   /* FNV-1a; collisions are caught by the stored key */
    for (BlobSize i = 0; i < key_size; i++) {
        h ^= ((const unsigned char*)key)[i];
        h *= 1099511628211ull;
    }
    char name[32];
    snprintf(name, sizeof(name), "%016llx.blob", (unsigned long long)h);
    return g_dir / name;
}

/* Oldest first (reads refresh the time) until three quarters of the cap. */
void evict_locked(void) {
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    std::error_code ec;
    for (fs::directory_iterator it(g_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".blob") files.emplace_back(it->last_write_time(ec), it->path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        if (g_total <= kCapBytes / 4 * 3) break;
        const uintmax_t size = fs::file_size(file.second, ec);
        if (!ec && fs::remove(file.second, ec)) g_total -= std::min(g_total, size);
    }
}

void set_blob(const void* key, BlobSize key_size, const void* value, BlobSize value_size) {
    if (key_size <= 0 || value_size < 0) return;
    const fs::path path = file_for(key, key_size);
    fs::path temp = path;
#ifdef _WIN32
    temp += ".tmp" + std::to_string(_getpid()) + "-" + std::to_string(g_temp_serial++);
#else
    temp += ".tmp" + std::to_string(g_temp_serial++);
#endif
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        const uint64_t n = (uint64_t)key_size;
        out.write((const char*)&n, sizeof(n));
        out.write((const char*)key, key_size);
        out.write((const char*)value, value_size);
        if (!out) {
            out.close();
            std::error_code ec;
            fs::remove(temp, ec);
            return;
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    std::error_code ec;
    const uintmax_t old = fs::exists(path, ec) ? fs::file_size(path, ec) : 0;
    fs::remove(path, ec);
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        return;
    }
    g_total = g_total - std::min(g_total, old) + sizeof(uint64_t) + (uintmax_t)key_size + (uintmax_t)value_size;
    if (g_total > kCapBytes) evict_locked();
}

/* Called once with no buffer to learn the size, then again to copy. */
BlobSize get_blob(const void* key, BlobSize key_size, void* value, BlobSize value_size) {
    if (key_size <= 0) return 0;
    const fs::path path = file_for(key, key_size);
    std::ifstream in(path, std::ios::binary);
    uint64_t n = 0;
    if (!in || !in.read((char*)&n, sizeof(n)) || n != (uint64_t)key_size) return 0;
    std::vector<char> stored((size_t)n);
    if (!in.read(stored.data(), (std::streamsize)n) || memcmp(stored.data(), key, (size_t)n) != 0) return 0;
    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    const std::streamoff start = (std::streamoff)(sizeof(n) + n);
    if (end < start) return 0;
    const BlobSize size = (BlobSize)(end - start);
    if (value && value_size >= size) {
        in.seekg(start);
        if (!in.read((char*)value, size)) return 0;
        std::error_code ec;
        fs::last_write_time(path, fs::file_time_type::clock::now(), ec);
    }
    return size;
}

}  /* anonymous namespace */

extern "C" bool gb_gl_program_cache_install(const char* dir) {
#ifdef _WIN32
    /* Windows only: that is ANGLE, where every compile goes through the slow
     * HLSL compiler. Elsewhere the GL context may be GLX, whose proc-address
     * lookup returns a non-null stub even for EGL names, and Mesa keeps its
     * own shader cache anyway. */
    static bool installed = false;
    if (installed || !dir || !*dir) return installed;
    const auto get_display = (GetCurrentDisplayFn)SDL_GL_GetProcAddress("eglGetCurrentDisplay");
    const auto set_funcs = (SetBlobCacheFuncsFn)SDL_GL_GetProcAddress("eglSetBlobCacheFuncsANDROID");
    void* display = get_display ? get_display() : NULL;
    if (!display || !set_funcs) return false;

    std::error_code ec;
    g_dir = fs::u8path(dir);
    fs::create_directories(g_dir, ec);
    if (!fs::is_directory(g_dir, ec)) return false;
    const auto stale = fs::file_time_type::clock::now() - std::chrono::hours(1);
    for (fs::directory_iterator it(g_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".blob") {
            g_total += it->file_size(ec);
        } else if (it->path().filename().string().find(".blob.tmp") != std::string::npos &&
                   it->last_write_time(ec) < stale) {
            std::error_code ignored;
            fs::remove(it->path(), ignored);   /* left by a process that died mid-write */
        }
    }
    set_funcs(display, set_blob, get_blob);
    installed = true;
    fprintf(stderr, "[GL] program cache: %s (%.1f MB)\n", dir, g_total / 1048576.0);
    return true;
#else
    (void)dir;
    return false;
#endif
}
