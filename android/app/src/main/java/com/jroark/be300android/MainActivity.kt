package com.jroark.be300android

import android.graphics.Bitmap
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import kotlin.math.roundToInt

class MainActivity : AppCompatActivity() {
    private val native = Be300Native()
    private val uiHandler = Handler(Looper.getMainLooper())

    private lateinit var frameContainer: FrameLayout
    private lateinit var framebufferView: ImageView
    private lateinit var statusText: TextView

    private lateinit var framebufferBitmap: Bitmap
    private var emulatorHandle: Long = 0

    private val framePump = object : Runnable {
        override fun run() {
            if (emulatorHandle != 0L) {
                val hasFrame = native.nativeCopyFrameToBitmap(emulatorHandle, framebufferBitmap)
                if (hasFrame) {
                    framebufferView.invalidate()
                    statusText.text = "Running"
                }
            }
            uiHandler.postDelayed(this, FRAME_INTERVAL_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        frameContainer = findViewById(R.id.deviceFrameContainer)
        framebufferView = findViewById(R.id.framebufferView)
        statusText = findViewById(R.id.statusText)

        framebufferBitmap = Bitmap.createBitmap(FB_WIDTH, FB_HEIGHT, Bitmap.Config.ARGB_8888)
        framebufferView.setImageBitmap(framebufferBitmap)

        frameContainer.post {
            placeFramebufferViewport(frameContainer, framebufferView)
        }

        val kernelPath = resolveKernelPath()
        val kernelFile = File(kernelPath)
        if (!kernelFile.exists()) {
            statusText.text = "Kernel not found at $kernelPath"
            return
        }

        emulatorHandle = native.nativeCreate(
            kernelPath = kernelPath,
            cmdline = "console=ttyS0,9600 root=/dev/ram init=/linuxrc",
            sfb5bitGreen = false,
            sdramMb = 16
        )

        if (emulatorHandle == 0L) {
            statusText.text = "Failed to create emulator"
            return
        }

        if (!native.nativeStart(emulatorHandle)) {
            statusText.text = "Failed to start emulator"
            native.nativeDestroy(emulatorHandle)
            emulatorHandle = 0
            return
        }

        statusText.text = "Booting kernel from $kernelPath"
        uiHandler.post(framePump)
    }

    override fun onDestroy() {
        uiHandler.removeCallbacks(framePump)
        if (emulatorHandle != 0L) {
            native.nativeDestroy(emulatorHandle)
            emulatorHandle = 0
        }
        super.onDestroy()
    }

    private fun resolveKernelPath(): String {
        val fromIntent = intent.getStringExtra("kernel_path")
        if (!fromIntent.isNullOrBlank()) {
            return fromIntent
        }
        return "/sdcard/Download/vmlinux-pgui-demo"
    }

    private fun placeFramebufferViewport(container: FrameLayout, target: ImageView) {
        val frameW = container.width.toFloat()
        val frameH = container.height.toFloat()

        val left = (frameW * SCREEN_LEFT_FRAC).roundToInt()
        val top = (frameH * SCREEN_TOP_FRAC).roundToInt()
        val width = (frameW * SCREEN_WIDTH_FRAC).roundToInt()
        val height = (frameH * SCREEN_HEIGHT_FRAC).roundToInt()

        val lp = target.layoutParams as FrameLayout.LayoutParams
        lp.width = width
        lp.height = height
        lp.leftMargin = left
        lp.topMargin = top
        target.layoutParams = lp
    }

    companion object {
        private const val FB_WIDTH = 240
        private const val FB_HEIGHT = 320
        private const val FRAME_INTERVAL_MS = 16L

        // Screen viewport inside be300_frame.png (normalized to frame image size).
        private const val SCREEN_LEFT_FRAC = 0.2148f
        private const val SCREEN_TOP_FRAC = 0.1990f
        private const val SCREEN_WIDTH_FRAC = 0.5703f
        private const val SCREEN_HEIGHT_FRAC = 0.7422f
    }
}
