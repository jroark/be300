package com.jroark.be300;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;

final class Be300View extends View {
    interface InputSink {
        void onGuestTouch(boolean down, int x, int y);
        void onGuestButtons(int set1, int set2);
    }

    private static final int TOUCH_DWELL_MS = 120;
    private static final int BUTTON_DWELL_MS = 120;
    private static final float LCD_LEFT = 0.2109375f;
    private static final float LCD_TOP = 0.1636328f;
    private static final float LCD_WIDTH = 0.578125f;
    private static final float GUEST_LCD_ASPECT = 240f / 320f;

    private enum PointerMode {
        NONE,
        TOUCH,
        BUTTON
    }

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Paint bitmapPaint = new Paint(Paint.DITHER_FLAG);
    private final Paint blackPaint = new Paint();
    private final RectF frameDst = new RectF();
    private final RectF lcdDst = new RectF();
    private final RectF iconDst = new RectF();
    private final RectF fallbackScreenDst = new RectF();

    private final Bitmap deviceFrame;
    private final ButtonMask buttonMask;

    private Bitmap screenBitmap;
    private Object frameLock;
    private InputSink inputSink;
    private PointerMode pointerMode = PointerMode.NONE;
    private int activePointerId = -1;
    private int lastTouchX;
    private int lastTouchY;
    private long touchDownAtMs;
    private long buttonDownAtMs;
    private int pressedSet1;
    private int pressedSet2;
    private Runnable pendingTouchRelease;
    private Runnable pendingButtonRelease;

    Be300View(Context context) {
        this(context, null);
    }

    Be300View(Context context, AttributeSet attrs) {
        super(context, attrs);
        bitmapPaint.setFilterBitmap(false);
        bitmapPaint.setAntiAlias(false);
        deviceFrame = BitmapFactory.decodeResource(getResources(),
                R.drawable.be300_frame);
        Bitmap maskBitmap = BitmapFactory.decodeResource(getResources(),
                R.drawable.buttons_dpad_bw_mask);
        buttonMask = ButtonMask.fromBitmap(maskBitmap);
        blackPaint.setColor(Color.BLACK);
        blackPaint.setStyle(Paint.Style.FILL);
        setFocusable(true);
        setFocusableInTouchMode(true);
    }

    void setInputSink(InputSink inputSink) {
        this.inputSink = inputSink;
    }

    void setScreenBitmap(Bitmap bitmap, Object lock) {
        screenBitmap = bitmap;
        frameLock = lock;
        postInvalidateOnAnimation();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        canvas.drawColor(Color.BLACK);
        updateGeometry();

        if (deviceFrame != null) {
            drawScreen(canvas, lcdDst);
            canvas.drawBitmap(deviceFrame, null, frameDst, bitmapPaint);
        } else {
            drawScreen(canvas, fallbackScreenDst);
        }
    }

    private void drawScreen(Canvas canvas, RectF dst) {
        canvas.drawRect(dst, blackPaint);
        Bitmap bitmap = screenBitmap;
        Object lock = frameLock;
        if (bitmap == null) {
            return;
        }
        if (lock != null) {
            synchronized (lock) {
                canvas.drawBitmap(bitmap, null, dst, bitmapPaint);
            }
        } else {
            canvas.drawBitmap(bitmap, null, dst, bitmapPaint);
        }
    }

    private void updateGeometry() {
        int width = getWidth();
        int height = getHeight();
        if (width <= 0 || height <= 0) {
            frameDst.setEmpty();
            lcdDst.setEmpty();
            iconDst.setEmpty();
            fallbackScreenDst.setEmpty();
            return;
        }

        if (deviceFrame != null) {
            float frameAspect = (float)deviceFrame.getWidth() / deviceFrame.getHeight();
            float dstW = width;
            float dstH = dstW / frameAspect;
            if (dstH > height) {
                dstH = height;
                dstW = dstH * frameAspect;
            }
            float left = (width - dstW) * 0.5f;
            float top = (height - dstH) * 0.5f;
            frameDst.set(left, top, left + dstW, top + dstH);

            float lcdLeft = frameDst.left + frameDst.width() * LCD_LEFT;
            float lcdTop = frameDst.top + frameDst.height() * LCD_TOP;
            float lcdWidth = frameDst.width() * LCD_WIDTH;
            float lcdHeight = lcdWidth / GUEST_LCD_ASPECT;
            lcdDst.set(lcdLeft, lcdTop, lcdLeft + lcdWidth, lcdTop + lcdHeight);
            float iconHeight = lcdHeight / 8.0f;
            iconDst.set(lcdLeft, lcdTop + lcdHeight, lcdLeft + lcdWidth,
                    lcdTop + lcdHeight + iconHeight);
        } else {
            float aspect = GUEST_LCD_ASPECT;
            float dstW = width;
            float dstH = dstW / aspect;
            if (dstH > height) {
                dstH = height;
                dstW = dstH * aspect;
            }
            float left = (width - dstW) * 0.5f;
            float top = (height - dstH) * 0.5f;
            fallbackScreenDst.set(left, top, left + dstW, top + dstH);
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (inputSink == null) {
            return true;
        }

        updateGeometry();
        int action = event.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
                return handlePointerDown(event, event.getActionIndex());
            case MotionEvent.ACTION_POINTER_DOWN:
                return true;
            case MotionEvent.ACTION_MOVE:
                return handlePointerMove(event);
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                return handlePointerUp(event);
            default:
                return true;
        }
    }

    private boolean handlePointerDown(MotionEvent event, int index) {
        if (activePointerId != -1) {
            return true;
        }

        requestFocus();
        clearPendingTouchRelease();
        clearPendingButtonRelease();
        activePointerId = event.getPointerId(index);
        float x = event.getX(index);
        float y = event.getY(index);

        TouchPoint touch = mapToTouch(x, y);
        if (touch != null) {
            pointerMode = PointerMode.TOUCH;
            lastTouchX = touch.x;
            lastTouchY = touch.y;
            touchDownAtMs = event.getEventTime();
            inputSink.onGuestTouch(true, touch.x, touch.y);
            return true;
        }

        ButtonMask.Hit hit = mapToButton(x, y);
        if (hit != null) {
            pointerMode = PointerMode.BUTTON;
            pressedSet1 = hit.set1;
            pressedSet2 = hit.set2;
            buttonDownAtMs = event.getEventTime();
            inputSink.onGuestButtons(pressedSet1, pressedSet2);
            return true;
        }

        pointerMode = PointerMode.NONE;
        activePointerId = -1;
        return true;
    }

    private boolean handlePointerMove(MotionEvent event) {
        if (pointerMode != PointerMode.TOUCH || activePointerId == -1) {
            return true;
        }

        int index = event.findPointerIndex(activePointerId);
        if (index < 0) {
            return true;
        }

        TouchPoint touch = mapToTouch(event.getX(index), event.getY(index));
        if (touch == null) {
            return true;
        }

        lastTouchX = touch.x;
        lastTouchY = touch.y;
        inputSink.onGuestTouch(true, touch.x, touch.y);
        return true;
    }

    private boolean handlePointerUp(MotionEvent event) {
        if (activePointerId == -1) {
            return true;
        }

        if (pointerMode == PointerMode.TOUCH) {
            scheduleTouchRelease(event.getEventTime());
        } else if (pointerMode == PointerMode.BUTTON) {
            scheduleButtonRelease(event.getEventTime());
        }

        pointerMode = PointerMode.NONE;
        activePointerId = -1;
        return true;
    }

    private TouchPoint mapToTouch(float x, float y) {
        if (deviceFrame != null) {
            if (lcdDst.contains(x, y)) {
                return mapRectToTouch(x, y, lcdDst, 0, 320);
            }
            if (iconDst.contains(x, y)) {
                return mapRectToTouch(x, y, iconDst, 320, 40);
            }
            return null;
        }
        if (fallbackScreenDst.contains(x, y)) {
            return mapRectToTouch(x, y, fallbackScreenDst, 0, 320);
        }
        return null;
    }

    private TouchPoint mapRectToTouch(float x, float y, RectF rect, int yBase,
            int ySpan) {
        int tx = clamp((int)(((x - rect.left) * 240f) / rect.width()), 0, 239);
        int ty = clamp(yBase + (int)(((y - rect.top) * ySpan) / rect.height()),
                0, 359);
        return new TouchPoint(tx, ty);
    }

    private ButtonMask.Hit mapToButton(float x, float y) {
        if (buttonMask == null || deviceFrame == null || frameDst.isEmpty() ||
                !frameDst.contains(x, y)) {
            return null;
        }
        float fx = ((x - frameDst.left) / frameDst.width()) * buttonMask.width;
        float fy = ((y - frameDst.top) / frameDst.height()) * buttonMask.height;
        return buttonMask.hit(fx, fy);
    }

    private void scheduleTouchRelease(long eventTimeMs) {
        long delay = Math.max(0L, TOUCH_DWELL_MS - (eventTimeMs - touchDownAtMs));
        pendingTouchRelease = () -> {
            pendingTouchRelease = null;
            if (inputSink != null) {
                inputSink.onGuestTouch(false, lastTouchX, lastTouchY);
            }
        };
        handler.postDelayed(pendingTouchRelease, delay);
    }

    private void scheduleButtonRelease(long eventTimeMs) {
        long delay = Math.max(0L, BUTTON_DWELL_MS - (eventTimeMs - buttonDownAtMs));
        int releaseSet1 = pressedSet1;
        int releaseSet2 = pressedSet2;
        pendingButtonRelease = () -> {
            pendingButtonRelease = null;
            if (inputSink != null && releaseSet1 == pressedSet1 &&
                    releaseSet2 == pressedSet2) {
                pressedSet1 = 0;
                pressedSet2 = 0;
                inputSink.onGuestButtons(0, 0);
            }
        };
        handler.postDelayed(pendingButtonRelease, delay);
    }

    private void clearPendingTouchRelease() {
        if (pendingTouchRelease != null) {
            handler.removeCallbacks(pendingTouchRelease);
            pendingTouchRelease = null;
        }
    }

    private void clearPendingButtonRelease() {
        if (pendingButtonRelease != null) {
            handler.removeCallbacks(pendingButtonRelease);
            pendingButtonRelease = null;
        }
    }

    private static int clamp(int value, int min, int max) {
        return Math.max(min, Math.min(max, value));
    }

    private static final class TouchPoint {
        final int x;
        final int y;

        TouchPoint(int x, int y) {
            this.x = x;
            this.y = y;
        }
    }
}
