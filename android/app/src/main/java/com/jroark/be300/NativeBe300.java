package com.jroark.be300;

import android.graphics.Bitmap;

final class NativeBe300 {
    static {
        System.loadLibrary("be300_android");
    }

    private NativeBe300() {
    }

    static long create(String nandPath, String cf0Path, String cf1Path) {
        return nativeCreate(nandPath, emptyToNull(cf0Path), emptyToNull(cf1Path),
                16, 0, true, true);
    }

    static int step(long handle, int batches) {
        return nativeStep(handle, batches);
    }

    static boolean copyFrame(long handle, Bitmap bitmap) {
        return nativeCopyFrame(handle, bitmap);
    }

    static void setTouch(long handle, boolean down, int x, int y) {
        nativeSetTouch(handle, down, x, y);
    }

    static void setButtons(long handle, int set1, int set2) {
        nativeSetButtons(handle, set1, set2);
    }

    static boolean stowawayKey(long handle, int scancode, boolean release) {
        return nativeStowawayKey(handle, scancode, release);
    }

    static String drainSerial(long handle) {
        return nativeDrainSerial(handle);
    }

    static void stop(long handle) {
        nativeStop(handle);
    }

    static void destroy(long handle) {
        nativeDestroy(handle);
    }

    private static String emptyToNull(String value) {
        return value == null || value.isEmpty() ? null : value;
    }

    private static native long nativeCreate(String nandPath, String cf0Path,
            String cf1Path, int sdramMb, int targetMhz, boolean rtcHostTime,
            boolean enableStowaway);
    private static native int nativeStep(long handle, int batches);
    private static native boolean nativeCopyFrame(long handle, Bitmap bitmap);
    private static native void nativeSetTouch(long handle, boolean down, int x, int y);
    private static native void nativeSetButtons(long handle, int set1, int set2);
    private static native boolean nativeStowawayKey(long handle, int scancode, boolean release);
    private static native String nativeDrainSerial(long handle);
    private static native void nativeStop(long handle);
    private static native void nativeDestroy(long handle);
}
