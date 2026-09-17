/*
 * gb_host_paths.c — see runtime/include/gb_host_paths.h for the contract.
 *
 * Deliberately free of any SDL dependency: SDL2 is optional in
 * runtime/CMakeLists.txt, and rom.cfg / keybinds.ini resolution must not
 * silently change shape between an SDL build and a headless one.
 */
#include "gb_host_paths.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define GB_PATH_SEP '\\'
#else
#  define GB_PATH_SEP '/'
#endif

#if defined(__linux__)
#  include <unistd.h>
#endif
#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif

#define GB_HOST_PATH_MAX 1024

static char s_state_dir[GB_HOST_PATH_MAX];
static char s_asset_dir[GB_HOST_PATH_MAX];
static int  s_resolved = 0;

static int is_sep(char c) { return c == '/' || c == '\\'; }

/* Copy `path` into `out` truncated after its final separator (so the result
 * carries a trailing separator). Empty result when `path` has no separator. */
static void dir_of(const char *path, char *out, size_t out_size) {
    size_t last = 0;
    size_t i;
    out[0] = '\0';
    if (!path || !path[0]) return;
    for (i = 0; path[i]; i++) {
        if (is_sep(path[i])) last = i + 1;
    }
    if (last == 0 || last >= out_size) return;
    memcpy(out, path, last);
    out[last] = '\0';
}

/* Normalise a user-supplied directory into "dir<sep>" form. */
static void dir_with_sep(const char *dir, char *out, size_t out_size) {
    size_t n;
    out[0] = '\0';
    if (!dir || !dir[0]) return;
    n = strlen(dir);
    if (n + 2 >= out_size) return;
    memcpy(out, dir, n);
    if (!is_sep(out[n - 1])) out[n++] = GB_PATH_SEP;
    out[n] = '\0';
}

/* Absolute path of the running executable; empty string when unavailable. */
static void executable_path(char *out, size_t out_size) {
    out[0] = '\0';
#if defined(_WIN32)
    {
        DWORD n = GetModuleFileNameA(NULL, out, (DWORD)out_size);
        if (n == 0 || n >= out_size) out[0] = '\0';
    }
#elif defined(__linux__)
    {
        ssize_t n = readlink("/proc/self/exe", out, out_size - 1);
        if (n <= 0) out[0] = '\0';
        else out[n] = '\0';
    }
#elif defined(__APPLE__)
    {
        uint32_t cap = (uint32_t)out_size;
        if (_NSGetExecutablePath(out, &cap) != 0) out[0] = '\0';
    }
#else
    (void)out_size;
#endif
}

/* Remove the last path component of `path` (which must NOT end in a
 * separator), leaving the parent without a trailing separator. Returns a
 * pointer to the component that was removed, or NULL when there is none. */
static const char *chop_last(char *path) {
    char *slash = NULL;
    char *p;
    for (p = path; *p; p++) {
        if (is_sep(*p)) slash = p;
    }
    if (!slash) return NULL;
    *slash = '\0';
    return slash + 1;
}

/* Does `s` end with `suffix`? */
static int ends_with(const char *s, const char *suffix) {
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

/* When `exe_dir` is "<something>/Foo.app/Contents/MacOS/", yield "<something>"
 * — the folder the user sees the bundle sitting in. Returns 0 when the path is
 * not a bundle layout. */
static int bundle_container(const char *exe_dir, char *out, size_t out_size) {
    char work[GB_HOST_PATH_MAX];
    const char *leaf;
    size_t n;

    out[0] = '\0';
    if (!exe_dir || !exe_dir[0]) return 0;
    n = strlen(exe_dir);
    if (n == 0 || n >= sizeof(work)) return 0;
    memcpy(work, exe_dir, n + 1);
    if (is_sep(work[n - 1])) work[n - 1] = '\0';   /* ".../Contents/MacOS" */

    leaf = chop_last(work);                         /* ".../Contents" */
    if (!leaf || strcmp(leaf, "MacOS") != 0) return 0;
    leaf = chop_last(work);                         /* ".../Foo.app" */
    if (!leaf || strcmp(leaf, "Contents") != 0) return 0;
    leaf = chop_last(work);                         /* "<something>" */
    if (!leaf || !ends_with(leaf, ".app")) return 0;

    if (!work[0] || strlen(work) + 1 >= out_size) return 0;
    strcpy(out, work);
    return 1;
}

static void resolve(void) {
    char exe[GB_HOST_PATH_MAX];
    char exe_dir[GB_HOST_PATH_MAX];
    const char *env;

    s_state_dir[0] = '\0';
    s_asset_dir[0] = '\0';

    executable_path(exe, sizeof(exe));
    dir_of(exe, exe_dir, sizeof(exe_dir));

    /* ── assets: the payload shipped with the program ── */
    env = getenv("GBRECOMP_ASSET_DIR");
    if (env && env[0]) dir_with_sep(env, s_asset_dir, sizeof(s_asset_dir));
    if (!s_asset_dir[0] && exe_dir[0])
        dir_with_sep(exe_dir, s_asset_dir, sizeof(s_asset_dir));

    /* ── state: writable, survives an update of the program ── */
    env = getenv("GBRECOMP_STATE_DIR");
    if (env && env[0]) {
        dir_with_sep(env, s_state_dir, sizeof(s_state_dir));
    }
    if (!s_state_dir[0]) {
        /* The AppImage runtime exports $APPIMAGE as the path of the .AppImage
         * file itself; everything the user should still see after replacing
         * that file belongs beside it, never inside the read-only mount. */
        env = getenv("APPIMAGE");
        if (env && env[0]) dir_of(env, s_state_dir, sizeof(s_state_dir));
    }
    if (!s_state_dir[0]) {
        char container[GB_HOST_PATH_MAX];
        if (bundle_container(exe_dir, container, sizeof(container)))
            dir_with_sep(container, s_state_dir, sizeof(s_state_dir));
    }
    if (!s_state_dir[0] && exe_dir[0])
        dir_with_sep(exe_dir, s_state_dir, sizeof(s_state_dir));

    s_resolved = 1;
}

static void ensure_resolved(void) {
    if (!s_resolved) resolve();
}

void gb_host_paths_reset(void) {
    s_resolved = 0;
    s_state_dir[0] = '\0';
    s_asset_dir[0] = '\0';
}

const char *gb_host_state_dir(void) {
    ensure_resolved();
    return s_state_dir;
}

const char *gb_host_asset_dir(void) {
    ensure_resolved();
    return s_asset_dir;
}

static char *join(const char *dir, const char *leaf, char *out, size_t out_size) {
    if (!out || out_size == 0) return out;
    if (!leaf) leaf = "";
    snprintf(out, out_size, "%s%s", dir ? dir : "", leaf);
    return out;
}

char *gb_host_state_path(const char *leaf, char *out, size_t out_size) {
    return join(gb_host_state_dir(), leaf, out, out_size);
}

char *gb_host_asset_path(const char *leaf, char *out, size_t out_size) {
    return join(gb_host_asset_dir(), leaf, out, out_size);
}
