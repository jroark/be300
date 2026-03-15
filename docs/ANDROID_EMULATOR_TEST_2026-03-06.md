# Android Emulator Test Report (2026-03-06)

## Goal
Validate that the Android BE-300 app can be built, installed, launched, and run on an Android emulator.

## Environment
- Host: macOS (Apple Silicon)
- Emulator AVD: `be300_api34` (`arm64`)
- Device observed via adb: `emulator-5554`
- SDK path used: `/Users/jroark/Library/Android/sdk`
- JDK used for Gradle: `/Applications/Android Studio.app/Contents/jbr/Contents/Home`

## What was attempted
1. Started emulator and verified adb connectivity.
2. Built APK with Gradle (`:app:assembleDebug`).
3. Installed APK and launched `com.jroark.be300android/.MainActivity`.
4. Pushed kernel image (`kernels/vmlinux-pgui-demo`) for runtime testing.
5. Collected `logcat`, activity state, and process health metrics.

## Initial blockers and resolutions
- Blocker: no Android Gradle wrapper in this repo's `android/` project.
  - Resolution: used wrapper from sibling project (`../be300/android/gradlew`) to execute the build.
- Blocker: Gradle TLS fetch issue (`peer not authenticated`) and missing SDK vars.
  - Resolution: set `JAVA_HOME` to Android Studio JBR and exported `ANDROID_HOME` / `ANDROID_SDK_ROOT`.
- Blocker: native CMake failed because `android/unicorn/...` did not exist.
  - Resolution: built Unicorn for Android arm64 from source and staged:
    - `android/unicorn/arm64-v8a/libunicorn.so`
    - `android/unicorn/include/unicorn/*.h`

## Build result
- `:app:assembleDebug` succeeded after staging Unicorn Android artifacts.
- Output APK used for install:
  - `android/app/build/outputs/apk/debug/app-debug.apk`

## Runtime result
- Install succeeded (`adb install -r`).
- App process launched and remained alive.
- Verified foreground/top-resumed activity is `com.jroark.be300android/.MainActivity`.
- Verified native startup message in logcat:
  - `BE300Native: Emulator thread started`

## Important runtime nuance
- Default kernel path in app code (`/sdcard/Download/vmlinux-pgui-demo`) caused native creation failure (`machine_create failed`) in this emulator setup.
- Successful startup was achieved by placing kernel in app-private storage and launching with explicit extra:
  - `--es kernel_path /data/user/0/com.jroark.be300android/files/vmlinux-pgui-demo`

## Soak check (10s)
- App process remained alive (`pidof com.jroark.be300android` returned PID).
- No crash/ANR signatures observed in filtered logcat during soak window.

## Next step recommendation
- Update app startup path handling so first-run launch does not depend on external storage permissions/path assumptions.
  - Options: copy kernel into app-private storage at startup, or request/manage storage access explicitly.
