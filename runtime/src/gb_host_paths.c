/*
 * gb_host_paths.c — see runtime/include/gb_host_paths.h for the contract.
 *
 * Deliberately free of any SDL dependency: SDL2 is optional in
 * runtime/CMakeLists.txt, and rom.cfg / keybinds.ini resolution must not
 * silently change shape between an SDL build and a headless one.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE   /* realpath, localtime_r under a strict -std */
#endif
#include "gb_host_paths.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shellapi.h>
#  define GB_PATH_SEP '\\'
#else
#  define GB_PATH_SEP '/'
#endif

#if defined(__linux__)
#  include <errno.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <time.h>
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

int gb_host_can_relaunch(void) {
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    return 1;
#else
    return 0;
#endif
}

#if defined(_WIN32)
static wchar_t *widen(const char *s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof(wchar_t)) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
#endif

int gb_host_relaunch(const char *set_name, const char *set_value, const char *unset_name) {
#if defined(_WIN32)
    wchar_t exe[MAX_PATH];
    const DWORD exe_len = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (exe_len == 0 || exe_len >= MAX_PATH) return 0;
    /* This process is on its way out, so its own environment is the one to
     * edit; the child inherits it. */
    if (unset_name) {
        wchar_t *name = widen(unset_name);
        if (name) SetEnvironmentVariableW(name, NULL);
        free(name);
    }
    if (set_name && set_value) {
        wchar_t *name = widen(set_name), *value = widen(set_value);
        if (name && value) SetEnvironmentVariableW(name, value);
        free(name);
        free(value);
    }
    const wchar_t *line = GetCommandLineW();
    const size_t len = wcslen(line);
    wchar_t *cmdline = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));   /* CreateProcessW writes to it */
    if (!cmdline) return 0;
    memcpy(cmdline, line, (len + 1) * sizeof(wchar_t));
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    const BOOL ok = CreateProcessW(exe, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    free(cmdline);
    if (!ok) return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
#elif defined(__linux__) && !defined(__ANDROID__)
    /* The arguments as given, NUL-separated. */
    static char args[65536];
    char *argv[256];
    int argc = 0;
    FILE *f = fopen("/proc/self/cmdline", "rb");
    if (!f) return 0;
    const size_t n = fread(args, 1, sizeof(args) - 1, f);
    fclose(f);
    args[n] = '\0';
    for (size_t i = 0; i < n && argc < 255; i += strlen(args + i) + 1) argv[argc++] = args + i;
    argv[argc] = NULL;
    /* Not the mounted copy inside an AppImage: that goes away with this
     * process. */
    char exe[GB_HOST_PATH_MAX];
    const char *appimage = getenv("APPIMAGE");
    if (appimage && appimage[0]) {
        snprintf(exe, sizeof(exe), "%s", appimage);
    } else {
        executable_path(exe, sizeof(exe));
    }
    if (!exe[0] || argc == 0) return 0;
    const pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        if (unset_name) unsetenv(unset_name);
        if (set_name && set_value) setenv(set_name, set_value, 1);
        execv(exe, argv);
        _exit(127);
    }
    return 1;
#else
    (void)set_name;
    (void)set_value;
    (void)unset_name;
    return 0;
#endif
}

#if defined(__linux__) && !defined(__ANDROID__)
/* The freedesktop.org Trash specification: a file goes to the home trash
 * ($XDG_DATA_HOME/Trash) when it is on the same file system, otherwise to the
 * trash at the top of its own file system ($topdir/.Trash/$uid, or
 * $topdir/.Trash-$uid). Moved with rename(), never copied. */

static int home_trash_dir(char *out, size_t out_size) {
    const char *data = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    int n;
    if (data && data[0] == '/') n = snprintf(out, out_size, "%s/Trash", data);
    else if (home && home[0] == '/') n = snprintf(out, out_size, "%s/.local/share/Trash", home);
    else return 0;
    return n > 0 && (size_t)n < out_size;
}

/* mkdir -p with mode 0700 for the parts that do not exist yet. */
static int make_dirs(const char *dir) {
    char work[GB_HOST_PATH_MAX];
    struct stat st;
    size_t i;
    if (strlen(dir) >= sizeof(work)) return 0;
    strcpy(work, dir);
    for (i = 1; work[i]; i++) {
        if (work[i] != '/') continue;
        work[i] = '\0';
        if (mkdir(work, 0700) != 0 && errno != EEXIST) return 0;
        work[i] = '/';
    }
    if (mkdir(work, 0700) != 0 && errno != EEXIST) return 0;
    return stat(work, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The mount point `path` (a directory) lies on: the highest ancestor still on
 * device `dev`. */
static int mount_top(const char *path, dev_t dev, char *out, size_t out_size) {
    char work[GB_HOST_PATH_MAX];
    struct stat st;
    if (strlen(path) >= sizeof(work) || strlen(path) >= out_size) return 0;
    strcpy(work, path);
    strcpy(out, path);
    for (;;) {
        char *slash = strrchr(work, '/');
        if (!slash) return 0;
        if (slash == work) {
            if (stat("/", &st) == 0 && st.st_dev == dev) strcpy(out, "/");
            return 1;
        }
        *slash = '\0';
        if (stat(work, &st) != 0 || st.st_dev != dev) return 1;
        strcpy(out, work);
    }
}

/* The trash for a file on device `dev` whose folder is `dir`: sets `trash`,
 * and `top` to the prefix Path= is written relative to ("" = absolute). */
static int pick_trash(const char *dir, dev_t dev, char *trash, size_t trash_size, char *top, size_t top_size) {
    struct stat st;
    char home[GB_HOST_PATH_MAX];
    top[0] = '\0';
    if (home_trash_dir(home, sizeof(home)) && make_dirs(home) && stat(home, &st) == 0 && st.st_dev == dev) {
        return snprintf(trash, trash_size, "%s", home) < (int)trash_size;
    }
    if (!mount_top(dir, dev, top, top_size)) return 0;
    {
        const char *root = strcmp(top, "/") == 0 ? "" : top;
        char shared[GB_HOST_PATH_MAX];
        snprintf(shared, sizeof(shared), "%s/.Trash", root);
        /* The shared .Trash only when it is a real, sticky directory. */
        if (lstat(shared, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & S_ISVTX)) {
            snprintf(trash, trash_size, "%s/%lu", shared, (unsigned long)getuid());
            if (make_dirs(trash) && lstat(trash, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid()) {
                return 1;
            }
        }
        snprintf(trash, trash_size, "%s/.Trash-%lu", root, (unsigned long)getuid());
        return make_dirs(trash) && lstat(trash, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid();
    }
}

/* Path= value: percent-encoded, '/' kept. */
static void url_escape(const char *in, char *out, size_t out_size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < out_size; in++) {
        const unsigned char c = (unsigned char)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
}

static int trash_linux(const char *path) {
    char dir[GB_HOST_PATH_MAX], full[GB_HOST_PATH_MAX], trash[GB_HOST_PATH_MAX], top[GB_HOST_PATH_MAX];
    char files[GB_HOST_PATH_MAX], info[GB_HOST_PATH_MAX], name[512], target[GB_HOST_PATH_MAX];
    char info_file[GB_HOST_PATH_MAX], escaped[GB_HOST_PATH_MAX * 3], stamp[32];
    const char *base = path;
    const char *p;
    struct stat st;
    int attempt;

    if (!path || !path[0] || lstat(path, &st) != 0) return 0;
    for (p = path; *p; p++) {
        if (*p == '/' && p[1]) base = p + 1;
    }
    /* The folder resolved, the name kept: a link is trashed, not its target. */
    {
        char parent[GB_HOST_PATH_MAX];
        const size_t n = (size_t)(base - path);
        char *resolved;
        if (n >= sizeof(parent)) return 0;
        memcpy(parent, path, n);
        parent[n] = '\0';
        resolved = realpath(n ? parent : ".", NULL);
        if (!resolved || strlen(resolved) >= sizeof(dir)) {
            free(resolved);
            return 0;
        }
        strcpy(dir, resolved);
        free(resolved);
    }
    if (snprintf(full, sizeof(full), "%s/%s", strcmp(dir, "/") ? dir : "", base) >= (int)sizeof(full)) return 0;
    if (!pick_trash(dir, st.st_dev, trash, sizeof(trash), top, sizeof(top))) return 0;
    snprintf(files, sizeof(files), "%s/files", trash);
    snprintf(info, sizeof(info), "%s/info", trash);
    if (!make_dirs(files) || !make_dirs(info)) return 0;
    {
        /* Relative to the top of the file system for its own trash. */
        const char *rel = full;
        if (top[0] && strcmp(top, "/") != 0 && strncmp(full, top, strlen(top)) == 0 && full[strlen(top)] == '/') {
            rel = full + strlen(top) + 1;
        } else if (top[0]) {
            rel = full + 1;
        }
        url_escape(rel, escaped, sizeof(escaped));
    }
    {
        const time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm);
    }
    /* The .trashinfo is created first, exclusively: it claims the name. */
    for (attempt = 1; attempt < 1000; attempt++) {
        int fd;
        FILE *f;
        if (attempt == 1) snprintf(name, sizeof(name), "%s", base);
        else snprintf(name, sizeof(name), "%s.%d", base, attempt);
        snprintf(info_file, sizeof(info_file), "%s/%s.trashinfo", info, name);
        snprintf(target, sizeof(target), "%s/%s", files, name);
        if (lstat(target, &st) == 0) continue;
        fd = open(info_file, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return 0;
        }
        f = fdopen(fd, "w");
        if (!f) {
            close(fd);
            unlink(info_file);
            return 0;
        }
        fprintf(f, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", escaped, stamp);
        if (fclose(f) != 0 || rename(full, target) != 0) {
            unlink(info_file);
            return 0;
        }
        return 1;
    }
    return 0;
}
#endif

int gb_host_has_trash(void) {
#if defined(_WIN32)
    return 1;
#elif defined(__linux__) && !defined(__ANDROID__)
    char home[GB_HOST_PATH_MAX];
    return home_trash_dir(home, sizeof(home));
#else
    return 0;
#endif
}

const char *gb_host_trash_name(void) {
#if defined(_WIN32)
    return "Recycle Bin";
#else
    return "Trash";
#endif
}

int gb_host_trash(const char *path) {
#if defined(__linux__) && !defined(__ANDROID__)
    return trash_linux(path);
#elif defined(_WIN32)
    /* The shell deletes outright a path that is not absolute, and takes
     * backslashes only; pFrom is a double-NUL-terminated list. */
    wchar_t *wide = path ? widen(path) : NULL;
    if (!wide) return 0;
    for (wchar_t *p = wide; *p; p++) {
        if (*p == L'/') *p = L'\\';
    }
    wchar_t full[MAX_PATH + 2];
    const DWORD n = GetFullPathNameW(wide, MAX_PATH, full, NULL);
    free(wide);
    if (n == 0 || n >= MAX_PATH || GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) return 0;
    full[n + 1] = L'\0';
    SHFILEOPSTRUCTW op;
    memset(&op, 0, sizeof(op));
    op.wFunc = FO_DELETE;
    op.pFrom = full;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted &&
           GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES;
#else
    (void)path;
    return 0;
#endif
}
