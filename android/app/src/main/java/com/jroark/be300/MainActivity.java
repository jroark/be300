package com.jroark.be300;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

@SuppressWarnings("deprecation")
public final class MainActivity extends Activity {
    private static final int REQ_NAND = 1;
    private static final int REQ_CF0 = 2;
    private static final int REQ_CF1 = 3;
    private static final int FRAME_INTERVAL_MS = 33;
    private static final int STEP_BATCHES = 64;
    private static final int SERIAL_LIMIT = 120000;

    private Be300View be300View;
    private TextView statusView;
    private TextView serialView;
    private ScrollView serialPanel;
    private Button bootButton;
    private Button stopButton;
    private Button cf0Button;
    private Button cf1Button;
    private Button logButton;
    private File nandFile;
    private File cf0File;
    private File cf1File;
    private EmulatorRunner runner;
    private boolean serialVisible;
    private boolean bootAfterNandPick;
    private boolean stoppingRunner;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        Window window = getWindow();
        window.setStatusBarColor(Color.BLACK);
        window.setNavigationBarColor(Color.BLACK);

        be300View = new Be300View(this);
        be300View.setInputSink(new Be300View.InputSink() {
            @Override
            public void onGuestTouch(boolean down, int x, int y) {
                EmulatorRunner current = runner;
                if (current != null) {
                    current.setTouch(down, x, y);
                }
            }

            @Override
            public void onGuestButtons(int set1, int set2) {
                EmulatorRunner current = runner;
                if (current != null) {
                    current.setButtons(set1, set2);
                }
            }
        });

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.BLACK);

        root.addView(be300View, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        statusView = new TextView(this);
        statusView.setTextColor(0xffd7dde8);
        statusView.setTextSize(13f);
        statusView.setSingleLine(false);
        statusView.setPadding(dp(12), dp(6), dp(12), dp(4));
        statusView.setText("Choose NAND");
        root.addView(statusView, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        root.addView(buildControlBar(), new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        serialView = new TextView(this);
        serialView.setTextColor(0xffd7dde8);
        serialView.setTextSize(11f);
        serialView.setTypeface(android.graphics.Typeface.MONOSPACE);
        serialView.setPadding(dp(10), dp(8), dp(10), dp(8));
        serialPanel = new ScrollView(this);
        serialPanel.setBackgroundColor(0xff101318);
        serialPanel.addView(serialView, new ScrollView.LayoutParams(
                ScrollView.LayoutParams.MATCH_PARENT,
                ScrollView.LayoutParams.WRAP_CONTENT));
        serialPanel.setVisibility(View.GONE);
        root.addView(serialPanel, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(180)));

        setContentView(root);
        syncButtons(false);
        loadBundledNandIfPresent();
    }

    private LinearLayout buildControlBar() {
        LinearLayout controls = new LinearLayout(this);
        controls.setGravity(Gravity.CENTER_VERTICAL);
        controls.setPadding(dp(8), dp(4), dp(8), dp(8));
        controls.setBackgroundColor(0xff11151d);

        Button nandButton = makeButton("NAND");
        nandButton.setOnClickListener(v -> chooseFile(REQ_NAND));
        controls.addView(nandButton);

        cf0Button = makeButton("CF0");
        cf0Button.setOnClickListener(v -> chooseFile(REQ_CF0));
        controls.addView(cf0Button);

        cf1Button = makeButton("CF1");
        cf1Button.setOnClickListener(v -> chooseFile(REQ_CF1));
        controls.addView(cf1Button);

        bootButton = makeButton("Boot");
        bootButton.setOnClickListener(v -> bootOrChooseNand());
        controls.addView(bootButton);

        stopButton = makeButton("Stop");
        stopButton.setOnClickListener(v -> stopCurrentRunner(false));
        controls.addView(stopButton);

        logButton = makeButton("Log");
        logButton.setOnClickListener(v -> toggleSerial());
        controls.addView(logButton);

        return controls;
    }

    private Button makeButton(String text) {
        Button button = new Button(this);
        button.setText(text);
        button.setTextSize(12f);
        button.setAllCaps(false);
        button.setMinHeight(dp(42));
        button.setMinimumHeight(dp(42));
        button.setPadding(dp(10), 0, dp(10), 0);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        lp.setMargins(dp(3), 0, dp(3), 0);
        button.setLayoutParams(lp);
        return button;
    }

    private void bootOrChooseNand() {
        if (stoppingRunner) {
            setStatus("Stopping emulator...");
            return;
        }
        if (nandFile == null || !nandFile.isFile()) {
            bootAfterNandPick = true;
            chooseFile(REQ_NAND);
            return;
        }
        startRunner();
    }

    private void startRunner() {
        if (runner != null && !stopCurrentRunner(true)) {
            setStatus("Stopping emulator...");
            return;
        }

        serialView.setText("");
        EmulatorRunner next = new EmulatorRunner(nandFile.getAbsolutePath(),
                cf0File != null ? cf0File.getAbsolutePath() : null,
                cf1File != null ? cf1File.getAbsolutePath() : null);
        runner = next;
        be300View.setScreenBitmap(next.screenBitmap, next.frameLock);
        syncButtons(true);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setStatus("Booting " + nandFile.getName() + "...");
        next.start();
    }

    private boolean stopCurrentRunner(boolean wait) {
        EmulatorRunner current = runner;
        if (current == null) {
            return true;
        }

        stoppingRunner = true;
        syncButtons(true);
        current.requestStop();
        if (wait) {
            boolean stopped = current.join(1500);
            if (!stopped) {
                return false;
            }
        }
        return true;
    }

    private void onRunnerFinished(EmulatorRunner finished, String message) {
        if (runner != finished) {
            return;
        }
        runner = null;
        stoppingRunner = false;
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setStatus(message);
        syncButtons(false);
    }

    private void syncButtons(boolean runningOrStopping) {
        if (bootButton == null) {
            return;
        }
        boolean running = runner != null || runningOrStopping;
        bootButton.setEnabled(!running);
        stopButton.setEnabled(runner != null);
        cf0Button.setEnabled(!running);
        cf1Button.setEnabled(!running);
    }

    private void chooseFile(int requestCode) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivityForResult(intent, requestCode);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            bootAfterNandPick = false;
            return;
        }

        Uri uri = data.getData();
        String displayName = displayNameFor(uri);
        if (requestCode == REQ_NAND) {
            copyUriAsync(uri, new File(getFilesDir(), "selected_nand.bin"),
                    "NAND " + displayName, file -> {
                        nandFile = file;
                        setStatus("Ready: " + displayName);
                        if (bootAfterNandPick) {
                            bootAfterNandPick = false;
                            startRunner();
                        }
                    });
        } else if (requestCode == REQ_CF0) {
            copyUriAsync(uri, new File(getFilesDir(), "cf0.img"),
                    "CF0 " + displayName, file -> {
                        cf0File = file;
                        setStatus("CF0: " + displayName);
                    });
        } else if (requestCode == REQ_CF1) {
            copyUriAsync(uri, new File(getFilesDir(), "cf1.img"),
                    "CF1 " + displayName, file -> {
                        cf1File = file;
                        setStatus("CF1: " + displayName);
                    });
        }
    }

    private interface FileReady {
        void onReady(File file);
    }

    private void copyUriAsync(Uri uri, File dest, String label, FileReady ready) {
        setStatus("Copying " + label + "...");
        new Thread(() -> {
            try {
                copyStream(getContentResolver().openInputStream(uri), dest);
                runOnUiThread(() -> ready.onReady(dest));
            } catch (IOException e) {
                runOnUiThread(() -> setStatus("Copy failed: " + e.getMessage()));
            }
        }, "be300-file-copy").start();
    }

    private void loadBundledNandIfPresent() {
        File dest = new File(getFilesDir(), "All_nand_300.bin");
        new Thread(() -> {
            try {
                if (!dest.isFile()) {
                    copyStream(getAssets().open("All_nand_300.bin"), dest);
                }
                runOnUiThread(() -> {
                    if (nandFile == null) {
                        nandFile = dest;
                        setStatus("Ready: All_nand_300.bin");
                    }
                });
            } catch (IOException ignored) {
                if (dest.isFile()) {
                    runOnUiThread(() -> {
                        if (nandFile == null) {
                            nandFile = dest;
                            setStatus("Ready: All_nand_300.bin");
                        }
                    });
                }
            }
        }, "be300-asset-copy").start();
    }

    private void copyStream(InputStream in, File dest) throws IOException {
        if (in == null) {
            throw new IOException("Unable to open input");
        }
        File tmp = new File(dest.getParentFile(), dest.getName() + ".tmp");
        try (InputStream input = in; FileOutputStream output = new FileOutputStream(tmp)) {
            byte[] buffer = new byte[1024 * 1024];
            int n;
            while ((n = input.read(buffer)) >= 0) {
                output.write(buffer, 0, n);
            }
        }
        if (!tmp.renameTo(dest)) {
            throw new IOException("Unable to replace " + dest.getName());
        }
    }

    private String displayNameFor(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (index >= 0) {
                    String name = cursor.getString(index);
                    if (name != null && !name.isEmpty()) {
                        return name;
                    }
                }
            }
        } catch (Exception ignored) {
        }
        return "image";
    }

    private void appendSerial(String text) {
        if (text == null || text.isEmpty()) {
            return;
        }
        String current = serialView.getText().toString() + text;
        if (current.length() > SERIAL_LIMIT) {
            current = current.substring(current.length() - (SERIAL_LIMIT * 2 / 3));
        }
        serialView.setText(current);
        serialPanel.post(() -> serialPanel.fullScroll(View.FOCUS_DOWN));
    }

    private void toggleSerial() {
        serialVisible = !serialVisible;
        serialPanel.setVisibility(serialVisible ? View.VISIBLE : View.GONE);
        logButton.setText(serialVisible ? "Hide" : "Log");
    }

    private void setStatus(String text) {
        statusView.setText(text);
    }

    @Override
    protected void onPause() {
        super.onPause();
        EmulatorRunner current = runner;
        if (current != null) {
            current.setPaused(true);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        EmulatorRunner current = runner;
        if (current != null) {
            current.setPaused(false);
        }
    }

    @Override
    protected void onDestroy() {
        stopCurrentRunner(true);
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        if (serialVisible) {
            toggleSerial();
            return;
        }
        super.onBackPressed();
    }

    private int dp(int value) {
        return (int)(value * getResources().getDisplayMetrics().density + 0.5f);
    }

    private final class EmulatorRunner implements Runnable {
        final Bitmap screenBitmap = Bitmap.createBitmap(240, 320,
                Bitmap.Config.ARGB_8888);
        final Object frameLock = new Object();

        private final Object nativeLock = new Object();
        private final Object pauseLock = new Object();
        private final String nandPath;
        private final String cf0Path;
        private final String cf1Path;
        private Thread thread;
        private long handle;
        private volatile boolean stopRequested;
        private volatile boolean paused;

        EmulatorRunner(String nandPath, String cf0Path, String cf1Path) {
            this.nandPath = nandPath;
            this.cf0Path = cf0Path;
            this.cf1Path = cf1Path;
        }

        void start() {
            thread = new Thread(this, "be300-emulator");
            thread.start();
        }

        @Override
        public void run() {
            String finishMessage = "Stopped";
            try {
                long created = NativeBe300.create(nandPath, cf0Path, cf1Path);
                synchronized (nativeLock) {
                    handle = created;
                }
                runOnUiThread(() -> setStatus("Running"));

                long lastFrameAt = 0;
                while (!stopRequested) {
                    waitIfPaused();
                    if (stopRequested) {
                        break;
                    }

                    int result;
                    String serial;
                    boolean frameCopied = false;
                    long now = System.currentTimeMillis();
                    synchronized (nativeLock) {
                        if (handle == 0) {
                            break;
                        }
                        result = NativeBe300.step(handle, STEP_BATCHES);
                        if (now - lastFrameAt >= FRAME_INTERVAL_MS) {
                            synchronized (frameLock) {
                                frameCopied = NativeBe300.copyFrame(handle,
                                        screenBitmap);
                            }
                            lastFrameAt = now;
                        }
                        serial = NativeBe300.drainSerial(handle);
                    }

                    if (frameCopied) {
                        be300View.postInvalidateOnAnimation();
                    }
                    if (serial != null && !serial.isEmpty()) {
                        runOnUiThread(() -> appendSerial(serial));
                    }
                    if (result <= 0) {
                        finishMessage = "Emulator stopped";
                        break;
                    }

                    try {
                        Thread.sleep(1L);
                    } catch (InterruptedException ignored) {
                    }
                }
            } catch (Throwable t) {
                finishMessage = "Error: " + t.getMessage();
            } finally {
                long destroyHandle;
                synchronized (nativeLock) {
                    destroyHandle = handle;
                    handle = 0;
                }
                if (destroyHandle != 0) {
                    NativeBe300.destroy(destroyHandle);
                }
                String message = finishMessage;
                runOnUiThread(() -> onRunnerFinished(this, message));
            }
        }

        void requestStop() {
            stopRequested = true;
            synchronized (pauseLock) {
                paused = false;
                pauseLock.notifyAll();
            }
            synchronized (nativeLock) {
                if (handle != 0) {
                    NativeBe300.stop(handle);
                }
            }
        }

        boolean join(long timeoutMs) {
            Thread t = thread;
            if (t == null) {
                return true;
            }
            try {
                t.join(timeoutMs);
                return !t.isAlive();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return false;
            }
        }

        void setPaused(boolean paused) {
            synchronized (pauseLock) {
                this.paused = paused;
                if (!paused) {
                    pauseLock.notifyAll();
                }
            }
        }

        void setTouch(boolean down, int x, int y) {
            synchronized (nativeLock) {
                if (handle != 0) {
                    NativeBe300.setTouch(handle, down, x, y);
                }
            }
        }

        void setButtons(int set1, int set2) {
            synchronized (nativeLock) {
                if (handle != 0) {
                    NativeBe300.setButtons(handle, set1, set2);
                }
            }
        }

        private void waitIfPaused() {
            synchronized (pauseLock) {
                while (paused && !stopRequested) {
                    try {
                        pauseLock.wait(250L);
                    } catch (InterruptedException ignored) {
                    }
                }
            }
        }
    }
}
