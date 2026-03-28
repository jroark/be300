package com.jroark.be300android

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.View
import kotlin.math.min

class Be300ControlOverlayView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {
    enum class ControlId {
        TOUCH_STRIP_0,
        TOUCH_STRIP_1,
        TOUCH_STRIP_2,
        TOUCH_STRIP_3,
        TOUCH_STRIP_4,
        TOUCH_STRIP_5,
        TOUCH_STRIP_6,
        DPAD_UP,
        DPAD_DOWN,
        DPAD_LEFT,
        DPAD_RIGHT,
        OK,
        ESC,
        ROCKET
    }

    sealed class HitTarget {
        data class Screen(val normalizedX: Float, val normalizedY: Float) : HitTarget()
        data class TouchStrip(val slot: Int, val controlId: ControlId) : HitTarget()
        data class Buttons(
            val btnSet1Mask: Int,
            val btnSet2Mask: Int,
            val highlights: Set<ControlId>
        ) : HitTarget()
        data object None : HitTarget()
    }

    private val screenRect = RectF()
    private var fullscreenMode = false
    private var pressedControls: Set<ControlId> = emptySet()

    private val framedPressedPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(112, 0x11, 0x47, 0xA8)
        style = Paint.Style.FILL
    }
    private val fullscreenFillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(88, 0x11, 0x47, 0xA8)
        style = Paint.Style.FILL
    }
    private val fullscreenPressedPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(156, 0x11, 0x47, 0xA8)
        style = Paint.Style.FILL
    }
    private val fullscreenStrokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(190, 0xE6, 0xE6, 0xE6)
        style = Paint.Style.STROKE
        strokeWidth = dp(2f)
    }
    private val fullscreenTextPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(230, 0xE6, 0xE6, 0xE6)
        textAlign = Paint.Align.CENTER
        textSize = dp(14f)
    }

    fun updateLayout(screenBounds: RectF, isFullscreen: Boolean) {
        screenRect.set(screenBounds)
        fullscreenMode = isFullscreen
        invalidate()
    }

    fun setPressedControls(controls: Set<ControlId>) {
        if (pressedControls == controls) {
            return
        }
        pressedControls = controls.toSet()
        invalidate()
    }

    fun hitTest(x: Float, y: Float): HitTarget {
        if (fullscreenMode) {
            return hitTestFullscreen(x, y)
        }
        return hitTestFramed(x, y)
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        if (fullscreenMode) {
            drawFullscreenControls(canvas)
        } else {
            drawFramedHighlights(canvas)
        }
    }

    private fun hitTestFramed(x: Float, y: Float): HitTarget {
        if (screenRect.contains(x, y)) {
            return HitTarget.Screen(
                normalizedX = ((x - screenRect.left) / screenRect.width()).coerceIn(0f, 1f),
                normalizedY = ((y - screenRect.top) / screenRect.height()).coerceIn(0f, 1f)
            )
        }

        framedTouchStripSlotIndex(x, y)?.let { slot ->
            return HitTarget.TouchStrip(slot, touchStripControlId(slot))
        }

        framedButtonHit(x, y)?.let { return it }
        return HitTarget.None
    }

    private fun hitTestFullscreen(x: Float, y: Float): HitTarget {
        fullscreenTouchStripSlotIndex(x, y)?.let { slot ->
            return HitTarget.TouchStrip(slot, touchStripControlId(slot))
        }

        fullscreenButtonHit(x, y)?.let { return it }

        if (screenRect.contains(x, y)) {
            return HitTarget.Screen(
                normalizedX = ((x - screenRect.left) / screenRect.width()).coerceIn(0f, 1f),
                normalizedY = ((y - screenRect.top) / screenRect.height()).coerceIn(0f, 1f)
            )
        }

        return HitTarget.None
    }

    private fun drawFramedHighlights(canvas: Canvas) {
        if (pressedControls.isEmpty()) {
            return
        }

        for (slot in 0 until TOUCH_STRIP_SLOT_COUNT) {
            val id = touchStripControlId(slot)
            if (pressedControls.contains(id)) {
                canvas.drawRoundRect(framedTouchStripSlotRect(slot), dp(10f), dp(10f), framedPressedPaint)
            }
        }

        drawHighlightedButton(canvas, framedDpadUpRect(), ControlId.DPAD_UP, dp(18f))
        drawHighlightedButton(canvas, framedDpadDownRect(), ControlId.DPAD_DOWN, dp(18f))
        drawHighlightedButton(canvas, framedDpadLeftRect(), ControlId.DPAD_LEFT, dp(18f))
        drawHighlightedButton(canvas, framedDpadRightRect(), ControlId.DPAD_RIGHT, dp(18f))
        drawHighlightedButton(canvas, framedOkRect(), ControlId.OK, dp(28f))
        drawHighlightedButton(canvas, framedEscRect(), ControlId.ESC, dp(28f))
        drawHighlightedButton(canvas, framedRocketRect(), ControlId.ROCKET, dp(28f))
    }

    private fun drawFullscreenControls(canvas: Canvas) {
        for (slot in 0 until TOUCH_STRIP_SLOT_COUNT) {
            val id = touchStripControlId(slot)
            drawFullscreenRoundRect(
                canvas,
                fullscreenTouchStripSlotRect(slot),
                pressedControls.contains(id)
            )
        }

        drawFullscreenRoundRect(canvas, fullscreenDpadUpRect(), pressedControls.contains(ControlId.DPAD_UP))
        drawFullscreenRoundRect(canvas, fullscreenDpadDownRect(), pressedControls.contains(ControlId.DPAD_DOWN))
        drawFullscreenRoundRect(canvas, fullscreenDpadLeftRect(), pressedControls.contains(ControlId.DPAD_LEFT))
        drawFullscreenRoundRect(canvas, fullscreenDpadRightRect(), pressedControls.contains(ControlId.DPAD_RIGHT))
        drawFullscreenRoundRect(canvas, fullscreenOkRect(), pressedControls.contains(ControlId.OK))
        drawFullscreenRoundRect(canvas, fullscreenEscRect(), pressedControls.contains(ControlId.ESC))
        drawFullscreenRoundRect(canvas, fullscreenRocketRect(), pressedControls.contains(ControlId.ROCKET))

        drawCenteredLabel(canvas, fullscreenEscRect(), "ESC")
        drawCenteredLabel(canvas, fullscreenRocketRect(), "ROCKET")
        drawCenteredLabel(canvas, fullscreenOkRect(), "OK")
    }

    private fun drawHighlightedButton(canvas: Canvas, rect: RectF, controlId: ControlId, radius: Float) {
        if (pressedControls.contains(controlId)) {
            canvas.drawRoundRect(rect, radius, radius, framedPressedPaint)
        }
    }

    private fun drawFullscreenRoundRect(canvas: Canvas, rect: RectF, pressed: Boolean) {
        val radius = min(rect.width(), rect.height()) * 0.28f
        canvas.drawRoundRect(rect, radius, radius, if (pressed) fullscreenPressedPaint else fullscreenFillPaint)
        canvas.drawRoundRect(rect, radius, radius, fullscreenStrokePaint)
    }

    private fun drawCenteredLabel(canvas: Canvas, rect: RectF, label: String) {
        val baseline = rect.centerY() - ((fullscreenTextPaint.descent() + fullscreenTextPaint.ascent()) / 2f)
        canvas.drawText(label, rect.centerX(), baseline, fullscreenTextPaint)
    }

    private fun framedButtonHit(x: Float, y: Float): HitTarget? {
        val point = Point(x, y)
        return when {
            framedRocketRect().contains(point.x, point.y) ->
                HitTarget.Buttons(0, BTN2_ROCKET, setOf(ControlId.ROCKET))
            framedEscRect().contains(point.x, point.y) ->
                HitTarget.Buttons(BTN1_ESC, 0, setOf(ControlId.ESC))
            framedOkRect().contains(point.x, point.y) ->
                HitTarget.Buttons(BTN1_OK, 0, setOf(ControlId.OK))
            framedDpadUpRect().contains(point.x, point.y) ->
                HitTarget.Buttons(BTN1_UP, 0, setOf(ControlId.DPAD_UP))
            framedDpadDownRect().contains(point.x, point.y) ->
                HitTarget.Buttons(BTN1_DOWN, 0, setOf(ControlId.DPAD_DOWN))
            framedDpadLeftRect().contains(point.x, point.y) ->
                HitTarget.Buttons(BTN1_LEFT, 0, setOf(ControlId.DPAD_LEFT))
            framedDpadRightRect().contains(point.x, point.y) ->
                HitTarget.Buttons(BTN1_RIGHT, 0, setOf(ControlId.DPAD_RIGHT))
            else -> null
        }
    }

    private fun fullscreenButtonHit(x: Float, y: Float): HitTarget? {
        val point = Point(x, y)
        return when {
            fullscreenRocketRect().contains(point.x, point.y) ->
                HitTarget.Buttons(0, BTN2_ROCKET, setOf(ControlId.ROCKET))
            fullscreenEscRect().contains(point.x, point.y) ->
                HitTarget.Buttons(BTN1_ESC, 0, setOf(ControlId.ESC))
            fullscreenOkRect().contains(point.x, point.y) ->
                HitTarget.Buttons(BTN1_OK, 0, setOf(ControlId.OK))
            fullscreenDpadUpRect().contains(point.x, point.y) ->
                HitTarget.Buttons(BTN1_UP, 0, setOf(ControlId.DPAD_UP))
            fullscreenDpadDownRect().contains(point.x, point.y) ->
                HitTarget.Buttons(BTN1_DOWN, 0, setOf(ControlId.DPAD_DOWN))
            fullscreenDpadLeftRect().contains(point.x, point.y) ->
                HitTarget.Buttons(BTN1_LEFT, 0, setOf(ControlId.DPAD_LEFT))
            fullscreenDpadRightRect().contains(point.x, point.y) ->
                HitTarget.Buttons(BTN1_RIGHT, 0, setOf(ControlId.DPAD_RIGHT))
            else -> null
        }
    }

    private fun framedTouchStripSlotIndex(x: Float, y: Float): Int? {
        for (slot in 0 until TOUCH_STRIP_SLOT_COUNT) {
            if (framedTouchStripSlotRect(slot).contains(x, y)) {
                return slot
            }
        }
        return null
    }

    private fun fullscreenTouchStripSlotIndex(x: Float, y: Float): Int? {
        for (slot in 0 until TOUCH_STRIP_SLOT_COUNT) {
            if (fullscreenTouchStripSlotRect(slot).contains(x, y)) {
                return slot
            }
        }
        return null
    }

    private fun framedTouchStripSlotRect(slot: Int): RectF {
        val strip = RectF(width * 0.175f, height * 0.664f, width * 0.829f, height * 0.735f)
        return splitHorizontalSlot(strip, slot, TOUCH_STRIP_SLOT_COUNT, gap = width * 0.004f)
    }

    private fun framedDpadBounds(): RectF {
        return RectF(width * 0.358f, height * 0.780f, width * 0.654f, height * 0.952f)
    }

    private fun framedDpadUpRect(): RectF {
        val dpad = framedDpadBounds()
        return RectF(dpad.left + dpad.width() * 0.28f, dpad.top, dpad.right - dpad.width() * 0.28f, dpad.top + dpad.height() * 0.32f)
    }

    private fun framedDpadDownRect(): RectF {
        val dpad = framedDpadBounds()
        return RectF(dpad.left + dpad.width() * 0.28f, dpad.bottom - dpad.height() * 0.30f, dpad.right - dpad.width() * 0.28f, dpad.bottom)
    }

    private fun framedDpadLeftRect(): RectF {
        val dpad = framedDpadBounds()
        return RectF(dpad.left, dpad.top + dpad.height() * 0.26f, dpad.left + dpad.width() * 0.34f, dpad.bottom - dpad.height() * 0.24f)
    }

    private fun framedDpadRightRect(): RectF {
        val dpad = framedDpadBounds()
        return RectF(dpad.right - dpad.width() * 0.34f, dpad.top + dpad.height() * 0.26f, dpad.right, dpad.bottom - dpad.height() * 0.24f)
    }

    private fun framedOkRect(): RectF {
        val dpad = framedDpadBounds()
        return RectF(dpad.left + dpad.width() * 0.30f, dpad.top + dpad.height() * 0.29f, dpad.right - dpad.width() * 0.30f, dpad.bottom - dpad.height() * 0.27f)
    }

    private fun framedEscRect(): RectF {
        return RectF(width * 0.579f, height * 0.884f, width * 0.782f, height * 0.985f)
    }

    private fun framedRocketRect(): RectF {
        return RectF(width * 0.022f, height * 0.837f, width * 0.190f, height * 0.938f)
    }

    private fun fullscreenTouchStripRect(): RectF {
        val horizontalInset = width * 0.20f
        val slotHeight = dp(44f)
        val bottom = height - dp(16f)
        return RectF(horizontalInset, bottom - slotHeight, width - horizontalInset, bottom)
    }

    private fun fullscreenTouchStripSlotRect(slot: Int): RectF {
        return splitHorizontalSlot(fullscreenTouchStripRect(), slot, TOUCH_STRIP_SLOT_COUNT, gap = dp(4f))
    }

    private fun fullscreenDpadBounds(): RectF {
        val size = dp(156f).coerceAtMost(width * 0.34f)
        val left = dp(16f)
        val bottom = height - dp(72f)
        return RectF(left, bottom - size, left + size, bottom)
    }

    private fun fullscreenDpadUpRect(): RectF {
        val dpad = fullscreenDpadBounds()
        return RectF(dpad.left + dpad.width() * 0.28f, dpad.top, dpad.right - dpad.width() * 0.28f, dpad.top + dpad.height() * 0.32f)
    }

    private fun fullscreenDpadDownRect(): RectF {
        val dpad = fullscreenDpadBounds()
        return RectF(dpad.left + dpad.width() * 0.28f, dpad.bottom - dpad.height() * 0.30f, dpad.right - dpad.width() * 0.28f, dpad.bottom)
    }

    private fun fullscreenDpadLeftRect(): RectF {
        val dpad = fullscreenDpadBounds()
        return RectF(dpad.left, dpad.top + dpad.height() * 0.26f, dpad.left + dpad.width() * 0.34f, dpad.bottom - dpad.height() * 0.24f)
    }

    private fun fullscreenDpadRightRect(): RectF {
        val dpad = fullscreenDpadBounds()
        return RectF(dpad.right - dpad.width() * 0.34f, dpad.top + dpad.height() * 0.26f, dpad.right, dpad.bottom - dpad.height() * 0.24f)
    }

    private fun fullscreenOkRect(): RectF {
        val dpad = fullscreenDpadBounds()
        return RectF(dpad.left + dpad.width() * 0.30f, dpad.top + dpad.height() * 0.29f, dpad.right - dpad.width() * 0.30f, dpad.bottom - dpad.height() * 0.27f)
    }

    private fun fullscreenEscRect(): RectF {
        val buttonWidth = dp(96f)
        val buttonHeight = dp(48f)
        val right = width - dp(16f)
        val bottom = height - dp(16f)
        return RectF(right - buttonWidth, bottom - buttonHeight, right, bottom)
    }

    private fun fullscreenRocketRect(): RectF {
        val buttonWidth = dp(96f)
        val buttonHeight = dp(48f)
        val right = width - dp(16f)
        val bottom = height - dp(72f)
        return RectF(right - buttonWidth, bottom - buttonHeight, right, bottom)
    }

    private fun splitHorizontalSlot(base: RectF, slot: Int, count: Int, gap: Float): RectF {
        val totalGap = gap * (count - 1)
        val slotWidth = (base.width() - totalGap) / count.toFloat()
        val left = base.left + slot * (slotWidth + gap)
        return RectF(left, base.top, left + slotWidth, base.bottom)
    }

    private fun touchStripControlId(slot: Int): ControlId {
        return when (slot) {
            0 -> ControlId.TOUCH_STRIP_0
            1 -> ControlId.TOUCH_STRIP_1
            2 -> ControlId.TOUCH_STRIP_2
            3 -> ControlId.TOUCH_STRIP_3
            4 -> ControlId.TOUCH_STRIP_4
            5 -> ControlId.TOUCH_STRIP_5
            else -> ControlId.TOUCH_STRIP_6
        }
    }

    private fun dp(value: Float): Float {
        return value * resources.displayMetrics.density
    }

    private data class Point(val x: Float, val y: Float)

    companion object {
        private const val TOUCH_STRIP_SLOT_COUNT = 7

        private const val BTN1_OK = 0x04
        private const val BTN1_ESC = 0x08
        private const val BTN1_UP = 0x10
        private const val BTN1_DOWN = 0x20
        private const val BTN1_RIGHT = 0x40
        private const val BTN1_LEFT = 0x80
        private const val BTN2_ROCKET = 0x10
    }
}
