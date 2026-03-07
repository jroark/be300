package com.jroark.be300android

class Be300Native {
    external fun nativeCreate(
        kernelPath: String,
        cmdline: String,
        sfb5bitGreen: Boolean,
        sdramMb: Int
    ): Long

    external fun nativeStart(handle: Long): Boolean
    external fun nativeCopyFrameToBitmap(handle: Long, bitmap: android.graphics.Bitmap): Boolean
    external fun nativeStop(handle: Long)
    external fun nativeDestroy(handle: Long)

    companion object {
        init {
            System.loadLibrary("be300_jni")
        }
    }
}
