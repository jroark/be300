package com.jroark.be300android

import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Rect
import android.graphics.RectF
import android.graphics.Bitmap
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.Spinner
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.constraintlayout.widget.ConstraintLayout
import java.io.File
import java.io.IOException
import kotlin.math.roundToInt

class MainActivity : AppCompatActivity() {
    private val native = Be300Native()
    private val uiHandler = Handler(Looper.getMainLooper())

    private lateinit var frameContainer: FrameLayout
    private lateinit var framebufferView: ImageView
    private lateinit var frameOverlayView: ImageView
    private lateinit var controlOverlayView: Be300ControlOverlayView
    private lateinit var kernelSpinner: Spinner
    private lateinit var statusText: TextView

    private lateinit var framebufferBitmap: Bitmap
    private var emulatorHandle: Long = 0
    private var kernelChoices: List<KernelChoice> = emptyList()
    private var selectedKernelId: String? = null
    private var currentKernelLabel: String = ""
    private var isFullscreenFramebuffer = false
    private var screenMask: ScreenMask? = null
    private var frameAspectRatio: String = FRAME_ASPECT_RATIO_FALLBACK
    private lateinit var doubleTapDetector: GestureDetector
    private var btnSet1State = 0
    private var btnSet2State = 0
    private var lastTouchX = 0
    private var lastTouchY = 0
    private var activePointerId = MotionEvent.INVALID_POINTER_ID
    private var activeInputKind = ActiveInputKind.NONE

    private data class KernelBundle(
        val id: String,
        val label: String,
        val assetPath: String,
        val outputName: String
    )

    private data class KernelChoice(
        val id: String,
        val label: String,
        val path: String
    )

    private data class KernelBootOptions(
        val cmdline: String,
        val sfb5bitGreen: Boolean
    )

    private data class ScreenMask(
        val mask: BooleanArray,
        val width: Int,
        val height: Int,
        val isAlphaHole: Boolean,
        val leftFrac: Float,
        val topFrac: Float,
        val widthFrac: Float,
        val heightFrac: Float
    )

    private enum class ActiveInputKind {
        NONE,
        SCREEN,
        TOUCH_STRIP,
        BUTTONS
    }

    private val framePump = object : Runnable {
        override fun run() {
            if (emulatorHandle != 0L) {
                val hasFrame = native.nativeCopyFrameToBitmap(emulatorHandle, framebufferBitmap)
                if (hasFrame) {
                    framebufferView.invalidate()
                    statusText.text = if (currentKernelLabel.isBlank()) {
                        "Running"
                    } else {
                        "Running: $currentKernelLabel"
                    }
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
        controlOverlayView = findViewById(R.id.controlOverlayView)
        kernelSpinner = findViewById(R.id.kernelSpinner)
        statusText = findViewById(R.id.statusText)

        framebufferBitmap = Bitmap.createBitmap(FB_WIDTH, FB_HEIGHT, Bitmap.Config.ARGB_8888)
        framebufferView.setImageBitmap(framebufferBitmap)

        setupDoubleTapToggle()
        frameContainer.post {
            val frameBitmap = BitmapFactory.decodeResource(resources, R.drawable.be300_frame)
            if (frameBitmap.width > 0 && frameBitmap.height > 0) {
                frameAspectRatio = "${frameBitmap.width}:${frameBitmap.height}"
            }
            screenMask = detectScreenMask(frameBitmap)
            placeFramebufferViewport(frameContainer, framebufferView, screenMask)
            applyFrameOverlayCutout(frameContainer, frameOverlayView, frameBitmap, screenMask)
            controlOverlayView.post { updateControlOverlayLayout() }
            frameBitmap.recycle()
        }

        kernelChoices = prepareBundledKernels()

        val intentKernelPath = intent.getStringExtra("kernel_path")
        if (!intentKernelPath.isNullOrBlank()) {
            val intentFile = File(intentKernelPath)
            if (intentFile.exists() && intentFile.canRead()) {
                kernelChoices = listOf(
                    KernelChoice(
                        id = INTENT_KERNEL_ID,
                        label = "Intent override (${intentFile.name})",
                        path = intentFile.absolutePath
                    )
                ) + kernelChoices.filterNot { it.path == intentFile.absolutePath }
            }
        }

        if (kernelChoices.isEmpty()) {
            statusText.text = "Kernel bundle missing in app assets"
            return
        }

        setupKernelSelector()
    }

    override fun onDestroy() {
        stopEmulator()
        super.onDestroy()
    }

    override fun onPause() {
        releaseAllInputState()
        super.onPause()
    }

    private fun setupDoubleTapToggle() {
        doubleTapDetector = GestureDetector(this, object : GestureDetector.SimpleOnGestureListener() {
            override fun onDown(e: MotionEvent): Boolean = true

            override fun onDoubleTap(e: MotionEvent): Boolean {
                releaseAllInputState()
                isFullscreenFramebuffer = !isFullscreenFramebuffer
                applyDisplayMode()
                return true
            }
        })

        controlOverlayView.setOnTouchListener { _, event ->
            doubleTapDetector.onTouchEvent(event)
            handleOverlayTouch(event)
            true
        }
    }

    private fun handleOverlayTouch(event: MotionEvent) {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                activePointerId = event.getPointerId(0)
                handleActivePointer(event)
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                if (activePointerId == MotionEvent.INVALID_POINTER_ID) {
                    activePointerId = event.getPointerId(event.actionIndex)
                    handleActivePointer(event)
                }
            }
            MotionEvent.ACTION_MOVE -> handleActivePointer(event)
            MotionEvent.ACTION_POINTER_UP -> {
                if (event.getPointerId(event.actionIndex) == activePointerId) {
                    releaseAllInputState()
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> releaseAllInputState()
        }
    }

    private fun handleActivePointer(event: MotionEvent) {
        if (activePointerId == MotionEvent.INVALID_POINTER_ID) {
            return
        }

        val pointerIndex = event.findPointerIndex(activePointerId)
        if (pointerIndex < 0) {
            releaseAllInputState()
            return
        }

        if (activeInputKind == ActiveInputKind.SCREEN) {
            sendScreenTouchAtPoint(event.getX(pointerIndex), event.getY(pointerIndex), down = true)
            return
        }

        when (val hit = controlOverlayView.hitTest(event.getX(pointerIndex), event.getY(pointerIndex))) {
            is Be300ControlOverlayView.HitTarget.Screen -> {
                activeInputKind = ActiveInputKind.SCREEN
                controlOverlayView.setPressedControls(emptySet())
                setGuestButtons(0, 0)
                sendScreenTouch(hit.normalizedX, hit.normalizedY, down = true)
            }
            is Be300ControlOverlayView.HitTarget.TouchStrip -> {
                activeInputKind = ActiveInputKind.TOUCH_STRIP
                controlOverlayView.setPressedControls(setOf(hit.controlId))
                setGuestButtons(0, 0)
                sendTouchStripSlot(hit.slot, down = true)
            }
            is Be300ControlOverlayView.HitTarget.Buttons -> {
                activeInputKind = ActiveInputKind.BUTTONS
                releaseGuestTouch()
                controlOverlayView.setPressedControls(hit.highlights)
                setGuestButtons(hit.btnSet1Mask, hit.btnSet2Mask)
            }
            Be300ControlOverlayView.HitTarget.None -> {
                when (activeInputKind) {
                    ActiveInputKind.TOUCH_STRIP -> releaseGuestTouch()
                    ActiveInputKind.BUTTONS -> setGuestButtons(0, 0)
                    else -> Unit
                }
                activeInputKind = ActiveInputKind.NONE
                controlOverlayView.setPressedControls(emptySet())
            }
        }
    }

    private fun sendScreenTouchAtPoint(x: Float, y: Float, down: Boolean) {
        val rect = currentFramebufferRect()
        if (rect.width() <= 0f || rect.height() <= 0f) {
            return
        }

        val clampedX = x.coerceIn(rect.left, rect.right)
        val clampedY = y.coerceIn(rect.top, rect.bottom)
        val normalizedX = ((clampedX - rect.left) / rect.width()).coerceIn(0f, 1f)
        val normalizedY = ((clampedY - rect.top) / rect.height()).coerceIn(0f, 1f)
        sendScreenTouch(normalizedX, normalizedY, down)
    }

    private fun sendScreenTouch(normalizedX: Float, normalizedY: Float, down: Boolean) {
        val be300X = (normalizedX * (FB_WIDTH - 1).toFloat()).roundToInt().coerceIn(0, FB_WIDTH - 1)
        val be300Y = (normalizedY * (FB_HEIGHT - 1).toFloat()).roundToInt().coerceIn(0, FB_HEIGHT - 1)
        setGuestTouch(down, be300X, be300Y)
    }

    private fun sendTouchStripSlot(slot: Int, down: Boolean) {
        val slotCenter = ((slot + 0.5f) * FB_WIDTH.toFloat() / TOUCH_STRIP_SLOT_COUNT.toFloat())
        val be300X = slotCenter.roundToInt().coerceIn(0, FB_WIDTH - 1)
        setGuestTouch(down, be300X, TOUCH_STRIP_CENTER_Y)
    }

    private fun setGuestTouch(down: Boolean, x: Int, y: Int) {
        lastTouchX = x
        lastTouchY = y
        if (emulatorHandle != 0L) {
            native.nativeSendTouch(emulatorHandle, down, x, y)
        }
    }

    private fun releaseGuestTouch() {
        if (emulatorHandle != 0L) {
            native.nativeSendTouch(emulatorHandle, false, lastTouchX, lastTouchY)
        }
    }

    private fun setGuestButtons(btn1: Int, btn2: Int) {
        if (btnSet1State == btn1 && btnSet2State == btn2) {
            return
        }
        btnSet1State = btn1
        btnSet2State = btn2
        if (emulatorHandle != 0L) {
            native.nativeSendButton(emulatorHandle, btnSet1State, btnSet2State)
        }
    }

    private fun releaseAllInputState() {
        releaseGuestTouch()
        setGuestButtons(0, 0)
        activePointerId = MotionEvent.INVALID_POINTER_ID
        activeInputKind = ActiveInputKind.NONE
        controlOverlayView.setPressedControls(emptySet())
    }

    private fun updateControlOverlayLayout() {
        controlOverlayView.updateLayout(currentFramebufferRect(), isFullscreenFramebuffer)
    }

    private fun currentFramebufferRect(): RectF {
        return RectF(
            framebufferView.left.toFloat(),
            framebufferView.top.toFloat(),
            framebufferView.right.toFloat(),
            framebufferView.bottom.toFloat()
        )
    }

    private fun applyDisplayMode() {
        releaseAllInputState()
        val lp = frameContainer.layoutParams as ConstraintLayout.LayoutParams
        if (isFullscreenFramebuffer) {
            lp.width = 0
            lp.height = 0
            lp.setMargins(0, 0, 0, 0)
            lp.startToStart = ConstraintLayout.LayoutParams.PARENT_ID
            lp.endToEnd = ConstraintLayout.LayoutParams.PARENT_ID
            lp.topToTop = ConstraintLayout.LayoutParams.PARENT_ID
            lp.bottomToBottom = ConstraintLayout.LayoutParams.PARENT_ID
            lp.bottomToTop = ConstraintLayout.LayoutParams.UNSET
            lp.dimensionRatio = null

            frameOverlayView.visibility = View.GONE
            kernelSpinner.visibility = View.GONE
            statusText.visibility = View.GONE
        } else {
            lp.width = 0
            lp.height = 0
            lp.setMargins(0, 0, 0, 0)
            lp.startToStart = ConstraintLayout.LayoutParams.PARENT_ID
            lp.endToEnd = ConstraintLayout.LayoutParams.PARENT_ID
            lp.topToTop = ConstraintLayout.LayoutParams.PARENT_ID
            lp.bottomToTop = R.id.kernelSpinner
            lp.bottomToBottom = ConstraintLayout.LayoutParams.UNSET
            lp.dimensionRatio = frameAspectRatio

            frameOverlayView.visibility = View.VISIBLE
            kernelSpinner.visibility = View.VISIBLE
            statusText.visibility = View.VISIBLE
        }
        frameContainer.layoutParams = lp

        frameContainer.post {
            if (isFullscreenFramebuffer) {
                placeFullscreenFramebuffer(frameContainer, framebufferView)
            } else {
                placeFramebufferViewport(frameContainer, framebufferView, screenMask)
                val frameBitmap = BitmapFactory.decodeResource(resources, R.drawable.be300_frame)
                applyFrameOverlayCutout(frameContainer, frameOverlayView, frameBitmap, screenMask)
                frameBitmap.recycle()
            }
            controlOverlayView.post { updateControlOverlayLayout() }
        }
    }

    private fun placeFullscreenFramebuffer(container: FrameLayout, target: ImageView) {
        val containerW = container.width
        val containerH = container.height
        if (containerW <= 0 || containerH <= 0) return

        val targetAspect = FB_WIDTH.toFloat() / FB_HEIGHT.toFloat()
        val containerAspect = containerW.toFloat() / containerH.toFloat()

        val width: Int
        val height: Int
        if (containerAspect > targetAspect) {
            height = containerH
            width = (containerH * targetAspect).roundToInt()
        } else {
            width = containerW
            height = (containerW / targetAspect).roundToInt()
        }

        val lp = target.layoutParams as FrameLayout.LayoutParams
        lp.width = width
        lp.height = height
        lp.leftMargin = ((containerW - width) / 2f).roundToInt()
        lp.topMargin = ((containerH - height) / 2f).roundToInt()
        target.layoutParams = lp
    }

    private fun setupKernelSelector() {
        val labels = kernelChoices.map { it.label }
        val adapter = ArrayAdapter(
            this,
            R.layout.item_kernel_selected,
            android.R.id.text1,
            labels
        )
        adapter.setDropDownViewResource(R.layout.item_kernel_dropdown)
        kernelSpinner.adapter = adapter
        kernelSpinner.prompt = getString(R.string.kernel_select_prompt)

        val prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        val preferredKernelId = if (kernelChoices.firstOrNull()?.id == INTENT_KERNEL_ID) {
            INTENT_KERNEL_ID
        } else {
            prefs.getString(PREF_LAST_KERNEL_ID, DEFAULT_KERNEL_ID)
        }
        val initialIndex = kernelChoices.indexOfFirst { it.id == preferredKernelId }
            .takeIf { it >= 0 }
            ?: kernelChoices.indexOfFirst { it.id == DEFAULT_KERNEL_ID }.takeIf { it >= 0 }
            ?: 0

        kernelSpinner.setSelection(initialIndex, false)

        // Android Spinner fires onItemSelected spuriously when the adapter is
        // set and when setSelection is called (both queued asynchronously).
        // Use a local flag, cleared only after the message queue drains via
        // post{}, to absorb all of those before trusting user selections.
        var spinnerReady = false

        kernelSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: android.view.View?, position: Int, id: Long) {
                if (!spinnerReady) return
                startKernel(position)
            }

            override fun onNothingSelected(parent: AdapterView<*>?) {
                // no-op
            }
        }

        startKernel(initialIndex)

        // After all pending messages drain, trust onItemSelected callbacks.
        kernelSpinner.post { spinnerReady = true }
    }

    private fun startKernel(index: Int) {
        if (index !in kernelChoices.indices) return
        val choice = kernelChoices[index]
        // Don't restart if this kernel is already running.
        // Handles the Spinner's spurious onItemSelected callback on first
        // adapter attachment, which would otherwise kill the just-started emulator.
        if (emulatorHandle != 0L && selectedKernelId == choice.id) return

        stopEmulator()

        val bootOptions = bootOptionsFor(choice)
        emulatorHandle = native.nativeCreate(
            kernelPath = choice.path,
            cmdline = bootOptions.cmdline,
            sfb5bitGreen = bootOptions.sfb5bitGreen,
            sdramMb = 16
        )
        if (emulatorHandle == 0L) {
            statusText.text = "Failed to create emulator for ${choice.label}"
            return
        }

        if (!native.nativeStart(emulatorHandle)) {
            statusText.text = "Failed to start emulator for ${choice.label}"
            native.nativeDestroy(emulatorHandle)
            emulatorHandle = 0
            return
        }

        selectedKernelId = choice.id
        currentKernelLabel = choice.label
        statusText.text = "Booting kernel: ${choice.label}"
        uiHandler.post(framePump)

        if (choice.id != INTENT_KERNEL_ID) {
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                .edit()
                .putString(PREF_LAST_KERNEL_ID, choice.id)
                .apply()
        }
    }

    private fun bootOptionsFor(choice: KernelChoice): KernelBootOptions {
        return when (choice.id) {
            "linux4be20040908-vmlinux" -> KernelBootOptions(
                cmdline = "",
                sfb5bitGreen = true
            )
            "vmlinux-mw" -> KernelBootOptions(
                cmdline = "console=tty0 root=/dev/ram init=/linuxrc",
                sfb5bitGreen = false
            )
            "vmlinux-4.2.9" -> KernelBootOptions(
                cmdline = "console=tty0 console=ttyS0,9600 earlyprintk keep_bootcon root=/dev/ram",
                sfb5bitGreen = false
            )
            else -> KernelBootOptions(
                cmdline = DEFAULT_CMDLINE,
                sfb5bitGreen = false
            )
        }
    }

    private fun stopEmulator() {
        uiHandler.removeCallbacks(framePump)
        releaseAllInputState()
        if (emulatorHandle != 0L) {
            native.nativeDestroy(emulatorHandle)
            emulatorHandle = 0
        }
        currentKernelLabel = ""
    }

    private fun prepareBundledKernels(): List<KernelChoice> {
        val outputDir = File(filesDir, KERNEL_DIR_NAME)
        if (!outputDir.exists() && !outputDir.mkdirs()) {
            return emptyList()
        }

        val choices = mutableListOf<KernelChoice>()
        for (bundle in BUNDLED_KERNELS) {
            val outFile = File(outputDir, bundle.outputName)
            if (!copyAssetIfMissing(bundle.assetPath, outFile)) {
                continue
            }
            if (outFile.exists() && outFile.length() > 0L && outFile.canRead()) {
                choices.add(
                    KernelChoice(
                        id = bundle.id,
                        label = bundle.label,
                        path = outFile.absolutePath
                    )
                )
            }
        }
        return choices
    }

    private fun copyAssetIfMissing(assetPath: String, outFile: File): Boolean {
        if (outFile.exists() && outFile.length() > 0L && outFile.canRead()) {
            return true
        }
        return try {
            assets.open(assetPath).use { input ->
                outFile.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
            true
        } catch (e: IOException) {
            false
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
        if (screenMask?.isAlphaHole == true) {
            overlay.setImageBitmap(composited)
            return
        }

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
        detectScreenMask(frameBitmap, alphaHoleMask = true)?.let { return it }
        return detectScreenMask(frameBitmap, alphaHoleMask = false)
    }

    private fun detectScreenMask(frameBitmap: Bitmap, alphaHoleMask: Boolean): ScreenMask? {
        val w = frameBitmap.width
        val h = frameBitmap.height
        if (w <= 0 || h <= 0) return null

        val pixels = IntArray(w * h)
        frameBitmap.getPixels(pixels, 0, w, 0, 0, w, h)
        val visited = BooleanArray(pixels.size)
        val queue = IntArray(pixels.size)
        val component = IntArray(pixels.size)
        val bestIndices = IntArray(pixels.size)

        fun isScreenCandidate(color: Int): Boolean {
            val a = (color ushr 24) and 0xFF
            if (alphaHoleMask) {
                return a < 16
            }
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
                    if (!visited[n] && isScreenCandidate(pixels[n])) {
                        visited[n] = true
                        queue[tail++] = n
                    }
                }
                if (x + 1 < w) {
                    val n = idx + 1
                    if (!visited[n] && isScreenCandidate(pixels[n])) {
                        visited[n] = true
                        queue[tail++] = n
                    }
                }
                if (y > 0) {
                    val n = idx - w
                    if (!visited[n] && isScreenCandidate(pixels[n])) {
                        visited[n] = true
                        queue[tail++] = n
                    }
                }
                if (y + 1 < h) {
                    val n = idx + w
                    if (!visited[n] && isScreenCandidate(pixels[n])) {
                        visited[n] = true
                        queue[tail++] = n
                    }
                }
            }
            return count
        }

        for (x in 0 until w) {
            val top = x
            if (!visited[top] && isScreenCandidate(pixels[top])) {
                floodFrom(top, true)
            }
            val bottom = (h - 1) * w + x
            if (!visited[bottom] && isScreenCandidate(pixels[bottom])) {
                floodFrom(bottom, true)
            }
        }
        for (y in 0 until h) {
            val left = y * w
            if (!visited[left] && isScreenCandidate(pixels[left])) {
                floodFrom(left, true)
            }
            val right = y * w + (w - 1)
            if (!visited[right] && isScreenCandidate(pixels[right])) {
                floodFrom(right, true)
            }
        }

        var bestCount = 0
        var bestMinX = 0
        var bestMinY = 0
        var bestMaxX = 0
        var bestMaxY = 0

        for (idx in pixels.indices) {
            if (visited[idx] || !isScreenCandidate(pixels[idx])) continue
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
            isAlphaHole = alphaHoleMask,
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
        private const val PREFS_NAME = "be300_prefs"
        private const val PREF_LAST_KERNEL_ID = "last_kernel_id"
        private const val DEFAULT_KERNEL_ID = "vmlinux-pgui-demo"
        private const val DEFAULT_CMDLINE = "console=tty0 console=ttyS0,9600 root=/dev/ram init=/linuxrc"
        private const val INTENT_KERNEL_ID = "__intent_kernel_path__"
        private const val KERNEL_DIR_NAME = "kernels"

        private val BUNDLED_KERNELS = listOf(
            KernelBundle(
                id = "linux4be20040908-vmlinux",
                label = "kernels/vmlinux-2.6",
                assetPath = "kernels/linux4be20040908-vmlinux",
                outputName = "linux4be20040908-vmlinux"
            ),
            KernelBundle(
                id = "vmlinux",
                label = "kernels/vmlinux",
                assetPath = "kernels/vmlinux",
                outputName = "vmlinux"
            ),
            KernelBundle(
                id = "vmlinux_sdlregtest",
                label = "kernels/vmlinux_sdlregtest",
                assetPath = "kernels/vmlinux_sdlregtest",
                outputName = "vmlinux_sdlregtest"
            ),
            KernelBundle(
                id = "vmlinux-mw",
                label = "kernels/vmlinux-mw",
                assetPath = "kernels/vmlinux-mw",
                outputName = "vmlinux-mw"
            ),
            KernelBundle(
                id = "vmlinux-pgui-demo",
                label = "kernels/vmlinux-pgui-demo",
                assetPath = "kernels/vmlinux-pgui-demo",
                outputName = "vmlinux-pgui-demo"
            ),
            KernelBundle(
                id = "vmlinux-pgui-test1",
                label = "kernels/vmlinux-pgui-test1",
                assetPath = "kernels/vmlinux-pgui-test1",
                outputName = "vmlinux-pgui-test1"
            ),
            KernelBundle(
                id = "vmlinux-4.2.9",
                label = "kernels/vmlinux-4.2.9",
                assetPath = "kernels/vmlinux-4.2.9",
                outputName = "vmlinux-4.2.9"
            )
        )

        // Screen viewport inside be300_frame.png (normalized to frame image size).
        private const val SCREEN_LEFT_FRAC = 0.1100f
        private const val SCREEN_TOP_FRAC = 0.1000f
        private const val SCREEN_WIDTH_FRAC = 0.7450f
        private const val SCREEN_HEIGHT_FRAC = 0.6320f
        private const val TOUCH_STRIP_SLOT_COUNT = 7
        private const val TOUCH_STRIP_CENTER_Y = 340
        private const val FRAME_ASPECT_RATIO_FALLBACK = "1024:1536"
    }
}
