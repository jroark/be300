package com.jroark.be300android

import android.graphics.BitmapFactory
import android.graphics.Canvas
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
import java.io.IOException
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
    private data class ScreenMask(
        val mask: BooleanArray,
        val width: Int,
        val height: Int,
        val leftFrac: Float,
        val topFrac: Float,
        val widthFrac: Float,
        val heightFrac: Float
    )

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
            val frameBitmap = BitmapFactory.decodeResource(resources, R.drawable.be300_frame)
            val screenMask = detectScreenMask(frameBitmap)
            placeFramebufferViewport(frameContainer, framebufferView, screenMask)
            applyFrameOverlayCutout(frameContainer, frameOverlayView, frameBitmap, screenMask)
            frameBitmap.recycle()
        }

        val bundledKernelPath = ensureBundledKernelInAppStorage()
        val kernelCandidates = resolveKernelCandidates()
        if (kernelCandidates.isEmpty()) {
            if (bundledKernelPath == null) {
                statusText.text = "Kernel not found; bundled kernel missing. Push $DEFAULT_KERNEL_NAME to ${File(filesDir, DEFAULT_KERNEL_NAME).absolutePath}"
            } else {
                statusText.text = "Kernel not found at expected locations"
            }
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

    private fun ensureBundledKernelInAppStorage(): String? {
        val outFile = File(filesDir, DEFAULT_KERNEL_NAME)
        if (outFile.exists() && outFile.length() > 0L && outFile.canRead()) {
            return outFile.absolutePath
        }

        return try {
            assets.open(DEFAULT_KERNEL_NAME).use { input ->
                outFile.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
            outFile.absolutePath
        } catch (e: IOException) {
            null
        }
    }

    private fun placeFramebufferViewport(container: FrameLayout, target: ImageView, screenMask: ScreenMask?) {
        val frameW = container.width.toFloat()
        val frameH = container.height.toFloat()
        val leftFrac = screenMask?.leftFrac ?: SCREEN_LEFT_FRAC
        val topFrac = screenMask?.topFrac ?: SCREEN_TOP_FRAC
        val widthFrac = screenMask?.widthFrac ?: SCREEN_WIDTH_FRAC
        val heightFrac = screenMask?.heightFrac ?: SCREEN_HEIGHT_FRAC

        val left = (frameW * leftFrac).roundToInt()
        val top = (frameH * topFrac).roundToInt()
        val width = (frameW * widthFrac).roundToInt()
        val height = (frameH * heightFrac).roundToInt()

        val lp = target.layoutParams as FrameLayout.LayoutParams
        lp.width = width
        lp.height = height
        lp.leftMargin = left
        lp.topMargin = top
        target.layoutParams = lp
    }

    private fun applyFrameOverlayCutout(
        container: FrameLayout,
        overlay: ImageView,
        frameBitmap: Bitmap,
        screenMask: ScreenMask?
    ) {
        val width = container.width
        val height = container.height
        if (width <= 0 || height <= 0) return

        val composited = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(composited)
        canvas.drawBitmap(frameBitmap, null, Rect(0, 0, width, height), null)
        clearNearWhiteEdgeRegion(composited)

        if (screenMask != null) {
            clearMaskRegion(composited, screenMask)
        } else {
            val left = (width * SCREEN_LEFT_FRAC).roundToInt()
            val top = (height * SCREEN_TOP_FRAC).roundToInt()
            val right = left + (width * SCREEN_WIDTH_FRAC).roundToInt()
            val bottom = top + (height * SCREEN_HEIGHT_FRAC).roundToInt()
            clearRectRegion(composited, left, top, right, bottom)
        }

        overlay.setImageBitmap(composited)
    }

    private fun clearMaskRegion(bitmap: Bitmap, screenMask: ScreenMask) {
        val dstW = bitmap.width
        val dstH = bitmap.height
        if (dstW <= 0 || dstH <= 0) return

        val srcW = screenMask.width
        val srcH = screenMask.height
        if (srcW <= 0 || srcH <= 0) return

        val dstPixels = IntArray(dstW * dstH)
        bitmap.getPixels(dstPixels, 0, dstW, 0, 0, dstW, dstH)
        val mask = screenMask.mask

        for (y in 0 until dstH) {
            val srcY = (y * srcH) / dstH
            val dstRow = y * dstW
            val srcRow = srcY * srcW
            for (x in 0 until dstW) {
                val srcX = (x * srcW) / dstW
                if (mask[srcRow + srcX]) {
                    dstPixels[dstRow + x] = 0
                }
            }
        }

        bitmap.setPixels(dstPixels, 0, dstW, 0, 0, dstW, dstH)
    }

    private fun clearRectRegion(bitmap: Bitmap, left: Int, top: Int, right: Int, bottom: Int) {
        val w = bitmap.width
        val h = bitmap.height
        if (w <= 0 || h <= 0) return
        val clampedLeft = left.coerceIn(0, w)
        val clampedTop = top.coerceIn(0, h)
        val clampedRight = right.coerceIn(0, w)
        val clampedBottom = bottom.coerceIn(0, h)
        if (clampedRight <= clampedLeft || clampedBottom <= clampedTop) return

        val pixels = IntArray(w * h)
        bitmap.getPixels(pixels, 0, w, 0, 0, w, h)
        for (y in clampedTop until clampedBottom) {
            val row = y * w
            for (x in clampedLeft until clampedRight) {
                pixels[row + x] = 0
            }
        }
        bitmap.setPixels(pixels, 0, w, 0, 0, w, h)
    }

    private fun detectScreenMask(frameBitmap: Bitmap): ScreenMask? {
        val w = frameBitmap.width
        val h = frameBitmap.height
        if (w <= 0 || h <= 0) return null

        val pixels = IntArray(w * h)
        frameBitmap.getPixels(pixels, 0, w, 0, 0, w, h)
        val visited = BooleanArray(pixels.size)
        val queue = IntArray(pixels.size)
        val component = IntArray(pixels.size)
        val bestIndices = IntArray(pixels.size)

        fun isNearWhite(color: Int): Boolean {
            val a = (color ushr 24) and 0xFF
            if (a < 16) return false
            val r = (color ushr 16) and 0xFF
            val g = (color ushr 8) and 0xFF
            val b = color and 0xFF
            return r >= 245 && g >= 245 && b >= 245
        }

        fun floodFrom(seed: Int, markOnly: Boolean): Int {
            var head = 0
            var tail = 0
            var count = 0
            queue[tail++] = seed
            visited[seed] = true
            while (head < tail) {
                val idx = queue[head++]
                if (!markOnly) {
                    component[count++] = idx
                }
                val x = idx % w
                val y = idx / w
                if (x > 0) {
                    val n = idx - 1
                    if (!visited[n] && isNearWhite(pixels[n])) {
                        visited[n] = true
                        queue[tail++] = n
                    }
                }
                if (x + 1 < w) {
                    val n = idx + 1
                    if (!visited[n] && isNearWhite(pixels[n])) {
                        visited[n] = true
                        queue[tail++] = n
                    }
                }
                if (y > 0) {
                    val n = idx - w
                    if (!visited[n] && isNearWhite(pixels[n])) {
                        visited[n] = true
                        queue[tail++] = n
                    }
                }
                if (y + 1 < h) {
                    val n = idx + w
                    if (!visited[n] && isNearWhite(pixels[n])) {
                        visited[n] = true
                        queue[tail++] = n
                    }
                }
            }
            return count
        }

        for (x in 0 until w) {
            val top = x
            if (!visited[top] && isNearWhite(pixels[top])) {
                floodFrom(top, true)
            }
            val bottom = (h - 1) * w + x
            if (!visited[bottom] && isNearWhite(pixels[bottom])) {
                floodFrom(bottom, true)
            }
        }
        for (y in 0 until h) {
            val left = y * w
            if (!visited[left] && isNearWhite(pixels[left])) {
                floodFrom(left, true)
            }
            val right = y * w + (w - 1)
            if (!visited[right] && isNearWhite(pixels[right])) {
                floodFrom(right, true)
            }
        }

        var bestCount = 0
        var bestMinX = 0
        var bestMinY = 0
        var bestMaxX = 0
        var bestMaxY = 0

        for (idx in pixels.indices) {
            if (visited[idx] || !isNearWhite(pixels[idx])) continue
            val count = floodFrom(idx, false)
            if (count <= 0) continue

            var minX = w
            var minY = h
            var maxX = 0
            var maxY = 0
            for (i in 0 until count) {
                val p = component[i]
                val x = p % w
                val y = p / w
                if (x < minX) minX = x
                if (x > maxX) maxX = x
                if (y < minY) minY = y
                if (y > maxY) maxY = y
            }

            if (count > bestCount) {
                bestCount = count
                bestMinX = minX
                bestMinY = minY
                bestMaxX = maxX
                bestMaxY = maxY
                System.arraycopy(component, 0, bestIndices, 0, count)
            }
        }

        if (bestCount == 0) return null

        val mask = BooleanArray(w * h)
        for (i in 0 until bestCount) {
            mask[bestIndices[i]] = true
        }

        return ScreenMask(
            mask = mask,
            width = w,
            height = h,
            leftFrac = bestMinX.toFloat() / w.toFloat(),
            topFrac = bestMinY.toFloat() / h.toFloat(),
            widthFrac = (bestMaxX - bestMinX + 1).toFloat() / w.toFloat(),
            heightFrac = (bestMaxY - bestMinY + 1).toFloat() / h.toFloat()
        )
    }

    private fun clearNearWhiteEdgeRegion(bitmap: Bitmap) {
        val w = bitmap.width
        val h = bitmap.height
        if (w <= 0 || h <= 0) return

        val pixels = IntArray(w * h)
        bitmap.getPixels(pixels, 0, w, 0, 0, w, h)
        val visited = BooleanArray(pixels.size)
        val queue = IntArray(pixels.size)
        var head = 0
        var tail = 0

        fun isNearWhite(color: Int): Boolean {
            val a = (color ushr 24) and 0xFF
            if (a < 16) return true
            val r = (color ushr 16) and 0xFF
            val g = (color ushr 8) and 0xFF
            val b = color and 0xFF
            return r >= 245 && g >= 245 && b >= 245
        }

        fun enqueue(index: Int) {
            if (!visited[index] && isNearWhite(pixels[index])) {
                visited[index] = true
                queue[tail++] = index
            }
        }

        for (x in 0 until w) {
            enqueue(x)
            enqueue((h - 1) * w + x)
        }
        for (y in 0 until h) {
            enqueue(y * w)
            enqueue(y * w + (w - 1))
        }

        while (head < tail) {
            val index = queue[head++]
            pixels[index] = 0
            val x = index % w
            val y = index / w
            if (x > 0) enqueue(index - 1)
            if (x + 1 < w) enqueue(index + 1)
            if (y > 0) enqueue(index - w)
            if (y + 1 < h) enqueue(index + w)
        }

        bitmap.setPixels(pixels, 0, w, 0, 0, w, h)
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
