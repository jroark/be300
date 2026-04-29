# BE-300 Android Port

This directory contains a native Android app that embeds the existing BE-300
emulator core through JNI. The app renders the 240x320 framebuffer inside the
BE-300 frame asset and forwards touch-panel and frame-button input to the guest.

## Build

```bash
cd android
./gradlew :app:assembleDebug
```

The debug APK is written to `app/build/outputs/apk/debug/app-debug.apk`.

## NAND Images

The app does not require a NAND image to be bundled into the APK. On first run,
tap `NAND` or `Boot` and choose a NAND image from Android storage.

For a one-tap local debug build, place `All_nand_300.bin` in
`app/src/main/assets/` before building. The app copies that asset into
app-private storage on launch and boots from the private copy.
