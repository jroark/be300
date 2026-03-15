# BE-300 Android Port

This directory contains an Android Studio project that wraps the emulator core
through JNI and displays the guest framebuffer inside `be300.png`.

## What this includes

- Native bridge (`src/android_bridge.c`) that:
  - creates/runs/stops the emulator on a background pthread
  - captures emulator framebuffer updates through `ui_set_frame_callback`
  - converts RGB565 guest pixels to RGBA8888 for an Android `Bitmap`
- Android app (`android/app`) that:
  - renders the live framebuffer
  - overlays `be300_frame.png` (`be300.png` copied into drawable-nodpi)
  - positions the framebuffer in the bezel viewport via normalized constants

## Required Unicorn Android binaries

The Android CMake target expects prebuilt Unicorn artifacts at:

- `android/unicorn/include/unicorn/unicorn.h`
- `android/unicorn/arm64-v8a/libunicorn.so`

If these are missing, CMake fails with a clear error.

## Kernel file path

By default, the app looks for:

- `/sdcard/Download/vmlinux-pgui-demo`

You can override via activity extra:

- `kernel_path`

## Build (Android Studio)

1. Open `/Users/jroark/src/be300-framebuffer/android` in Android Studio.
2. Install Android SDK 35 + NDK (25+).
3. Ensure Unicorn Android headers/libs are staged at `android/unicorn/...`.
4. Build and run `app` on an `arm64-v8a` device.

## Viewport tuning

The screen cutout alignment is controlled in:

- `MainActivity.kt` constants:
  - `SCREEN_LEFT_FRAC`
  - `SCREEN_TOP_FRAC`
  - `SCREEN_WIDTH_FRAC`
  - `SCREEN_HEIGHT_FRAC`

Adjust these if your bezel image changes.
