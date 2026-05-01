# BE-300 Android Port

This directory contains a native Android app that embeds the existing BE-300
emulator core through JNI. The app renders the 240x320 framebuffer inside the
BE-300 frame asset and forwards touch-panel, frame-button, and (when a
hardware keyboard is connected) Targus / Stowaway dock keystrokes to the
guest.

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

## Hardware keyboard input

The app enables the Stowaway serial keyboard dock by default. When an
Android device with an attached or paired hardware keyboard has the
emulator view focused, key down/up events are mapped to Stowaway scancodes
in `Be300View.java` (mirroring the SDL and web frontends) and forwarded
through `NativeBe300.stowawayKey` -> `stowaway_queue_key`. The dock probe
handshake completes during NK boot; subsequent keystrokes are dispatched
to WinCE's `serial.dll` via the same CommMode modem-event path the probe
uses, and arrive at the focused control once `WaitCommEvent` returns
EV_RXCHAR. NAND must be `nand_stowaway.bin` (built by
`tools/inject_stowaway.py`) so the WinCE Stowaway driver is registered;
plain `All_nand_300.bin` will boot but no keystrokes will be delivered.
