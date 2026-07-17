/*
 * launcher_ui_seam.h — the gb-recompiled ↔ recomp-ui pre-boot seam.
 *
 * gb_launcher_preboot() shows the shared recomp-ui Dear ImGui launcher BEFORE
 * the game window is created (called from the top of gb_platform_init()). It
 * seeds the launcher from runtime_prefs.ini + rom.cfg, pulls the game's
 * identity from the game_extras hooks (game_get_name / game_get_platform /
 * game_get_valid_crcs / game_get_expected_crc32), lets the user pick a ROM and
 * edit settings + keybinds, then — on LAUNCH — persists the chosen settings to
 * runtime_prefs.ini (which gb_platform_init's load_runtime_preferences() reads
 * moments later) and the chosen ROM to rom.cfg (which launcher_get_rom_path()
 * reads during game init, so its CRC/SHA gate + BPS auto-patch still run).
 *
 * Compiled + linked only when RECOMP_LAUNCHER is defined (the recomp-ui
 * integration is wired via recomp_ui.cmake). When absent, the runtime keeps
 * its legacy rom.cfg-or-file-picker flow untouched.
 */
#ifndef GBRT_LAUNCHER_UI_SEAM_H
#define GBRT_LAUNCHER_UI_SEAM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes from gb_launcher_preboot(). */
#define GB_LAUNCHER_LAUNCH      0  /* user hit PLAY: settings+rom persisted, boot */
#define GB_LAUNCHER_QUIT        1  /* user closed the launcher: the caller exits */
#define GB_LAUNCHER_UNAVAILABLE 2  /* launcher could not initialize: fall back */

/* Show the pre-boot launcher. See the file comment. Returns one of the codes
 * above. Safe to call once, before SDL is initialized (it self-manages SDL). */
int gb_launcher_preboot(void);

#ifdef __cplusplus
}
#endif

#endif /* GBRT_LAUNCHER_UI_SEAM_H */
