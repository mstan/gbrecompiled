# Android

This document walks through the current Android flow for `gb-recompiled`: taking a Game Boy ROM, generating an Android project, building a debug APK, and installing it on a device or emulator.

Android support is currently:

- single-ROM only
- landscape only
- `arm64-v8a` only
- controller-first, with no touch gameplay overlay yet
- based on an external SDL2 source checkout

## Prerequisites

You need:

- a built `gbrecomp` binary
- `gradle`
- the Android SDK and NDK
- `adb`
- an SDL2 source checkout

Build the recompiler first:

```bash
cmake -G Ninja -B build .
ninja -C build
```

## 1. Generate an Android Project

Generate the usual desktop output plus an Android scaffold:

```bash
./build/bin/gbrecomp path/to/game.gb -o output/game --android
```

Optional Android-specific flags:

- `--android-package <java.package>`
- `--android-app-name <label>`

Example:

```bash
./build/bin/gbrecomp roms/game.gb \
  -o output/game \
  --android \
  --android-package io.gbrecompiled.game \
  --android-app-name "My Game"
```

This creates:

- the normal desktop project in `output/game`
- the Android project in `output/game/android`

## 2. Provide SDL2

Android builds use SDL2 from an external source checkout. Point the generated project at that checkout with `SDL2_SOURCE_DIR`.

Example:

```bash
export SDL2_SOURCE_DIR=/path/to/SDL
```

If `SDL2_SOURCE_DIR` is missing, the generated Android project fails early with an explicit error.

## 3. Build the APK

From the repo root:

```bash
SDL2_SOURCE_DIR=/path/to/SDL \
gradle -p output/game/android :app:assembleDebug
```

The debug APK will be written to:

```bash
output/game/android/app/build/outputs/apk/debug/app-debug.apk
```

## 4. Install on a Device or Emulator

Install to the default connected device:

```bash
adb install -r output/game/android/app/build/outputs/apk/debug/app-debug.apk
```

If you have more than one device connected, target one explicitly:

```bash
adb devices -l
adb -s <device_id> install -r output/game/android/app/build/outputs/apk/debug/app-debug.apk
```

## 5. Launch the App

Launch the generated Android app with:

```bash
adb -s <device_id> shell am start -W io.gbrecompiled.game/.GameActivity
```

If you used a custom package name, replace `io.gbrecompiled.game` with that package.

You can also launch it from the Android launcher like a normal app after installation.

## Controller Mapping

The default Android mapping is based on physical button position so it feels natural on handhelds and Xbox-style controllers:

- D-pad or left stick: move
- Bottom face button (`Xbox A` / `Switch B` / `Cross`): Game Boy `B`
- Right face button (`Xbox B` / `Switch A` / `Circle`): Game Boy `A`
- Left shoulder: Game Boy `B`
- Right shoulder: Game Boy `A`
- Start / Menu: `Start`
- Back / View / Share: `Select`
- Guide / Home or Android Back: open the runtime settings menu

When SDL can identify the connected controller, the runtime settings menu shows labels that match that controller profile.

## Generated Android Defaults

The generated Android project currently uses:

- minimum SDK `24`
- target SDK `34`
- `arm64-v8a`
- landscape orientation
- fullscreen by default
- app-private storage for saves and generated runtime artifacts

## Notes

- Multi-ROM Android output is not supported yet.
- The desktop project is still generated alongside the Android project.
- The generated app is meant for play, not for a touch-first mobile UX yet.
- Relative runtime files such as saves, screenshots, and logs are redirected into app-private writable storage on Android.

## Troubleshooting

### `device unauthorized`

If `adb devices` shows:

```text
<device_id> unauthorized
```

unlock the device, accept the USB debugging prompt, and run:

```bash
adb devices -l
```

again until it shows `device`.

### `SDL2_SOURCE_DIR is required`

Set the SDL source checkout path explicitly:

```bash
export SDL2_SOURCE_DIR=/path/to/SDL
```

or inline:

```bash
SDL2_SOURCE_DIR=/path/to/SDL gradle -p output/game/android :app:assembleDebug
```

### More than one Android target connected

Use `adb -s <device_id> ...` for install and launch:

```bash
adb devices -l
adb -s <device_id> install -r output/game/android/app/build/outputs/apk/debug/app-debug.apk
adb -s <device_id> shell am start -W io.gbrecompiled.game/.GameActivity
```

### Check logs

Use logcat while launching:

```bash
adb -s <device_id> logcat -c
adb -s <device_id> shell am start -W io.gbrecompiled.game/.GameActivity
adb -s <device_id> logcat -d
```
