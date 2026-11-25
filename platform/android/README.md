# Android game releases

Games provide an `android/build.gradle` with `ext.gbcGame` and apply `app.gradle`.
The shared CMake project builds the ordinary generated game target as
`libmain.so`, including its `extras.c`, `game_build.cmake`, additional bodies,
shared boot provider, common launcher and rb-engine. Desktop targets remain executables.

Use JDK 17, Gradle 8.11.1, Android SDK 35, NDK 27.1.12297006 and CMake 3.22.1.
Generate the game first using a native host `gbrecomp`.

```
gradle -p android assembleRelease --no-daemon --max-workers=4 \
  -PSDL2_SOURCE_DIR=/path/to/SDL2 \
  -PgbcHostCompiler=/path/to/native/gbrecomp
```

`-PgbcEngineRoot` and `-PgbcUiRoot` override local dependencies. Android defaults
to HLE for boot and uncovered RAM. An explicit reference build can set
`-PgbcBootImplementation=LLE` or `-PgbcRamFallbackImplementation=LLE`. When boot
uses LLE, set `-PgbcBiosDir` to the private firmware directory.
Set `GBRECOMP_SIGNING_PROPERTIES` to a private Java properties file containing
`storeFile`, `storePassword`, `keyAlias` and optionally `keyPassword`. Keep that
file and its keystore outside source control; the same key must sign updates.

The Android setup imports the original game ROM through the system document
picker into private app storage. Later launches prefill the shared launcher.
In an explicit LLE boot build, Setup also imports and verifies the appropriate
original firmware. The Mods page selects adaptive widescreen and any
additional body. Gamepads and keyboards use the native bindings. The
Touch controls toggle reveals a multitouch pad and a held Rewind button;
hide it while using the launcher. Android Back opens the runtime menu.
Firmware requirements, old-state incompatibility and validation limits belong
in each game's release notes. APK build/signature checks do not constitute
device gameplay validation.
