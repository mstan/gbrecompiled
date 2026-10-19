# Android game releases

Games provide an `android/build.gradle` with `ext.gbcGame` and apply `app.gradle`.
The shared CMake project builds the ordinary generated game target as
`libmain.so`, including its `extras.c`, `game_build.cmake`, additional bodies,
native BIOS, common launcher and rb-engine. Desktop targets remain executables.

Use JDK 17, Gradle 8.11.1, Android SDK 35, NDK 27.1.12297006 and CMake 3.22.1.
Generate the game first using a native host `gbrecomp`.

```
gradle -p android assembleRelease --no-daemon --max-workers=4 \
  -PSDL2_SOURCE_DIR=/path/to/SDL2 \
  -PgbcHostCompiler=/path/to/native/gbrecomp
```

`-PgbcEngineRoot`, `-PgbcUiRoot` and `-PgbcBiosDir` override local dependencies.
Set `GBRECOMP_SIGNING_PROPERTIES` to a private Java properties file containing
`storeFile`, `storePassword`, `keyAlias` and optionally `keyPassword`. Keep that
file and its keystore outside source control; the same key must sign updates.

No cart or firmware is packaged. The Android setup imports each verified ROM
and BIOS once through the system document picker into private app storage.
Later launches prefill the shared launcher. Its Mods page selects adaptive
widescreen and any additional body. Original SML2 requires DMG firmware; DX
requires CGB firmware. Gamepads and keyboards use the native bindings. The
Touch controls toggle reveals a multitouch pad and a held Rewind button;
hide it while using the launcher. Android Back opens the runtime menu.
Firmware requirements, old-state incompatibility and validation limits belong
in each game's release notes. APK build/signature checks do not constitute
device gameplay validation.
