package com.jroark.be300android

import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.PorterDuffXfermode
import android.graphics.Rect
import android.graphics.Bitmap
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import java.util.LinkedHashSet
import kotlin.math.roundToInt

class MainActivity : AppCompatActivity() {
    private val native = Be300Native()
    private val uiHandler = Handler(Looper.getMainLooper())

    private lateinit var frameContainer: FrameLayout
    private lateinit var framebufferView: ImageView
    private lateinit var frameOverlayView: ImageView
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
        frameOverlayView = findViewById(R.id.frameOverlayView)
        statusText = findViewById(R.id.statusText)

        framebufferBitmap = Bitmap.createBitmap(FB_WIDTH, FB_HEIGHT, Bitmap.Config.ARGB_8888)
        framebufferView.setImageBitmap(framebufferBitmap)

        frameContainer.post {
            placeFramebufferViewport(frameContainer, framebufferView)
            applyFrameOverlayCutout(frameContainer, frameOverlayView)
        }

        val kernelCandidates = resolveKernelCandidates()
        if (kernelCandidates.isEmpty()) {
            statusText.text = "Kernel not found. Push $DEFAULT_KERNEL_NAME to ${File(filesDir, DEFAULT_KERNEL_NAME).absolutePath}"
            return
        }

        val cmdline = "console=tty0 console=ttyS0,9600 root=/dev/ram init=/linuxrc"
        var selectedKernel: String? = null
        for (candidate in kernelCandidates) {
            emulatorHandle = native.nativeCreate(
                kernelPath = candidate,
                cmdline = cmdline,
                sfb5bitGreen = false,
                sdramMb = 16
            )
            if (emulatorHandle != 0L) {
                selectedKernel = candidate
                break
            }
        }

        if (emulatorHandle == 0L) {
            statusText.text = "Failed to create emulator (tried ${kernelCandidates.joinToString()})"
            return
        }

        if (!native.nativeStart(emulatorHandle)) {
            statusText.text = "Failed to start emulator"
            native.nativeDestroy(emulatorHandle)
            emulatorHandle = 0
            return
        }

        statusText.text = "Booting kernel from $selectedKernel"
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

    private fun resolveKernelCandidates(): List<String> {
        val candidates = LinkedHashSet<String>()
        val fromIntent = intent.getStringExtra("kernel_path")
        if (!fromIntent.isNullOrBlank()) {
            candidates.add(fromIntent)
        }
        candidates.add(File(filesDir, DEFAULT_KERNEL_NAME).absolutePath)
        candidates.add("/sdcard/Download/$DEFAULT_KERNEL_NAME")
        return candidates.filter { path ->
            val file = File(path)
            file.exists() && file.canRead()
        }
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

    private fun applyFrameOverlayCutout(container: FrameLayout, overlay: ImageView) {
        val width = container.width
        val height = container.height
        if (width <= 0 || height <= 0) return

        val frameBitmap = BitmapFactory.decodeResource(resources, R.drawable.be300_frame)
        val composited = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(composited)
        canvas.drawBitmap(frameBitmap, null, Rect(0, 0, width, height), null)
        frameBitmap.recycle()

        val left = (width * SCREEN_LEFT_FRAC).roundToInt().toFloat()
        val top = (height * SCREEN_TOP_FRAC).roundToInt().toFloat()
        val right = left + (width * SCREEN_WIDTH_FRAC).roundToInt()
        val bottom = top + (height * SCREEN_HEIGHT_FRAC).roundToInt()

        val clearPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            xfermode = PorterDuffXfermode(PorterDuff.Mode.CLEAR)
        }
        canvas.drawRect(left, top, right, bottom, clearPaint)

        overlay.setImageBitmap(composited)
    }

    companion object {
        private const val FB_WIDTH = 240
        private const val FB_HEIGHT = 320
        private const val FRAME_INTERVAL_MS = 16L
        private const val DEFAULT_KERNEL_NAME = "vmlinux-pgui-demo"

        // Screen viewport inside be300_frame.png (normalized to frame image size).
        private const val SCREEN_LEFT_FRAC = 0.1100f
        private const val SCREEN_TOP_FRAC = 0.1000f
        private const val SCREEN_WIDTH_FRAC = 0.7450f
        private const val SCREEN_HEIGHT_FRAC = 0.6320f
    }
}
