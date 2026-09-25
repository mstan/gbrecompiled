/*
 * gb_host_paths.h — where a packaged build keeps its files.
 *
 * Every packaged form of a recompiled game has to answer two different
 * questions, and conflating them is what breaks AppImages and .app bundles:
 *
 *   STATE  — writable, user-visible, must survive an update of the program:
 *            rom.cfg, keybinds.ini, runtime_prefs.ini, *.sav, *.rtc, *.stateN.
 *   ASSETS — read-only payload shipped WITH the program: launcher assets/,
 *            BPS enhancement patches, any other bundled data file.
 *
 * On Windows (a zip extracted to a folder) the two are the same directory and
 * nothing changes. They diverge exactly where the program lives inside a
 * read-only container:
 *
 *   Linux AppImage : assets are inside the squashfs mount, under its usr/bin,
 *                    state must land next to the .AppImage file itself.
 *   macOS .app     : assets are inside Foo.app/Contents/..., state belongs in
 *                    the folder the user actually sees the .app sitting in.
 *
 * Both accessors return a directory WITH a trailing separator, or an empty
 * string meaning "use the process working directory" (the historical
 * behaviour, and the last-resort fallback if the platform query fails).
 *
 * Resolution order for the state directory:
 *   1. $GBRECOMP_STATE_DIR   — explicit override (packaging tests, CI, kiosk
 *                              installs that want state elsewhere).
 *   2. $APPIMAGE             — set by the AppImage runtime to the path of the
 *                              .AppImage file; its directory is the anchor.
 *   3. the .app's container  — when the executable sits in *.app/Contents/MacOS.
 *   4. the executable's own directory.
 *   5. "" (process working directory).
 *
 * Resolution order for the asset directory:
 *   1. $GBRECOMP_ASSET_DIR   — explicit override.
 *   2. the executable's own directory.
 *   3. "" (process working directory).
 */
#ifndef GB_HOST_PATHS_H
#define GB_HOST_PATHS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Directory for writable, user-visible state (trailing separator, or ""). */
const char *gb_host_state_dir(void);

/* Directory for read-only payload shipped with the program (trailing
 * separator, or ""). Equals gb_host_state_dir() outside a container. */
const char *gb_host_asset_dir(void);

/* Join `leaf` onto the state / asset directory into `out`. Returns `out`. */
char *gb_host_state_path(const char *leaf, char *out, size_t out_size);
char *gb_host_asset_path(const char *leaf, char *out, size_t out_size);

/* Re-read the environment and re-query the platform. Only needed by tests;
 * ordinary callers get a value cached on first use. */
void gb_host_paths_reset(void);

/* Start this program again (the .AppImage for an AppImage) with the same
 * arguments and this process's environment, with `set_name`=`set_value` added
 * and `unset_name` removed (either may be NULL). The new process runs on its
 * own; call it as this one exits. Returns 1 if it started. Windows and Linux
 * only: gb_host_can_relaunch() says whether it can work here. */
int gb_host_relaunch(const char *set_name, const char *set_value, const char *unset_name);
int gb_host_can_relaunch(void);

#ifdef __cplusplus
}
#endif

#endif /* GB_HOST_PATHS_H */
