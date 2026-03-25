#include <jni.h>
#include <android/log.h>
#include <android/bitmap.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>

#include "be300.h"

// Include GXemul headers to resolve fb_data
#include "machine.h"
#include "devices.h"

#define LOG_TAG "Be300Native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/*
 * GXemul's console_charavail() spins forever if stdin is /dev/null:
 *   select() always returns "readable" on /dev/null, but read() returns 0
 *   (EOF), so the FIFO never fills and the break condition never triggers.
 *
 * Fix: replace stdin with the read end of a pipe whose write end is kept
 * open (but never written to). select() then returns 0 (not readable) on
 * an empty pipe, so console_stdin_avail() returns 0 and the loop exits.
 */
static int stdin_pipe_write_end = -1;

static void android_fix_stdin(void)
{
    if (stdin_pipe_write_end >= 0)
        return; // Already done

    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        LOGE("android_fix_stdin: pipe() failed");
        return;
    }

    if (dup2(pipe_fds[0], STDIN_FILENO) < 0) {
        LOGE("android_fix_stdin: dup2() failed");
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return;
    }

    close(pipe_fds[0]);
    stdin_pipe_write_end = pipe_fds[1]; // Keep write end open forever
    LOGI("android_fix_stdin: stdin replaced with blocking pipe (write_end=%d)",
         stdin_pipe_write_end);
}

typedef struct {
    machine_t *m;
    pthread_t thread;
    machine_config_t cfg;
} bridge_state_t;

JNIEXPORT jlong JNICALL
Java_com_jroark_be300android_Be300Native_nativeCreate(
    JNIEnv *env, jobject thiz,
    jstring kernelPath, jstring cmdline, jboolean sfb5bitGreen, jint sdramMb)
{
    android_fix_stdin();

    const char *c_kernelPath = (*env)->GetStringUTFChars(env, kernelPath, NULL);
    const char *c_cmdline = (*env)->GetStringUTFChars(env, cmdline, NULL);
    LOGI("nativeCreate: kernel=%s, cmdline=%s", c_kernelPath, c_cmdline);

    bridge_state_t *state = (bridge_state_t *)calloc(1, sizeof(bridge_state_t));
    if (!state) return 0;

    state->cfg.kernel_path = strdup(c_kernelPath);
    state->cfg.cmdline = strdup(c_cmdline);
    state->cfg.sfb_5bit_green = sfb5bitGreen;
    state->cfg.sdram_size = (uint32_t)sdramMb * 1024u * 1024u;
    state->cfg.trace = false;
    state->cfg.log_mmio = false;
    state->cfg.target_mhz = 10u;

    (*env)->ReleaseStringUTFChars(env, kernelPath, c_kernelPath);
    (*env)->ReleaseStringUTFChars(env, cmdline, c_cmdline);

    LOGI("Calling be300_create...");
    state->m = be300_create(&state->cfg);
    if (!state->m) {
        LOGE("Failed to create BE-300 machine");
        free((void*)state->cfg.kernel_path);
        free((void*)state->cfg.cmdline);
        free(state);
        return 0;
    }
    LOGI("be300_create succeeded: m=%p gxe_machine=%p",
         state->m, state->m ? state->m->gxe_machine : NULL);

    return (jlong)(uintptr_t)state;
}

static void *run_emulator(void *arg)
{
    bridge_state_t *state = (bridge_state_t *)arg;
    LOGI("Starting emulator thread (be300_run)");
    be300_run(state->m);
    LOGI("Emulator thread finished (be300_run returned)");
    return NULL;
}

JNIEXPORT jboolean JNICALL
Java_com_jroark_be300android_Be300Native_nativeStart(JNIEnv *env, jobject thiz, jlong handle)
{
    bridge_state_t *state = (bridge_state_t *)(uintptr_t)handle;
    if (!state || !state->m) return JNI_FALSE;

    if (pthread_create(&state->thread, NULL, run_emulator, state) != 0) {
        LOGE("Failed to create emulator thread");
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_jroark_be300android_Be300Native_nativeCopyFrameToBitmap(JNIEnv *env, jobject thiz, jlong handle, jobject bitmap)
{
    bridge_state_t *state = (bridge_state_t *)(uintptr_t)handle;
    if (!state || !state->m) return JNI_FALSE;

    machine_t *m = state->m;

    // Resolve fb_data if needed
    if (!m->fb_data) {
        if (!m->gxe_machine || !m->gxe_machine->fb || !m->gxe_machine->fb->framebuffer) {
            return JNI_FALSE; // Not initialized yet
        }
        m->fb_data = m->gxe_machine->fb->framebuffer;
        LOGI("Framebuffer resolved (%dx%d) fb_data=%p", m->fb_width, m->fb_height, m->fb_data);
    }

    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) < 0) {
        return JNI_FALSE;
    }
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
        return JNI_FALSE;
    }

    void *pixels;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) < 0) {
        return JNI_FALSE;
    }

    uint32_t *dst = (uint32_t *)pixels;
    const uint16_t *src = (const uint16_t *)m->fb_data;

    // Fixed BE-300 hardware values
    uint32_t fb_width = 240;
    uint32_t fb_height = 320;
    uint32_t fb_stride = 256;
    uint32_t dst_stride = info.stride / sizeof(uint32_t);

    for (uint32_t y = 0; y < fb_height; y++) {
        for (uint32_t x = 0; x < fb_width; x++) {
            uint16_t pixel = src[y * fb_stride + x];

            // RGB565 -> ARGB8888
            uint32_t r = (pixel >> 11) & 0x1F;
            uint32_t g = (pixel >> 5) & 0x3F;
            uint32_t b = pixel & 0x1F;

            // Scale up to 8-bit
            r = (r << 3) | (r >> 2);
            g = (g << 2) | (g >> 4);
            b = (b << 3) | (b >> 2);

            dst[y * dst_stride + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }

    AndroidBitmap_unlockPixels(env, bitmap);
    return JNI_TRUE;
}

extern volatile bool emul_shutdown;

JNIEXPORT void JNICALL
Java_com_jroark_be300android_Be300Native_nativeSendTouch(
    JNIEnv *env, jobject thiz, jlong handle,
    jboolean down, jint x, jint y)
{
    bridge_state_t *state = (bridge_state_t *)(uintptr_t)handle;
    if (!state || !state->m) return;
    machine_t *m = state->m;
    m->touch_down = (bool)down;
    m->touch_x    = (uint16_t)x;
    m->touch_y    = (uint16_t)y;
    __sync_synchronize();
}

JNIEXPORT void JNICALL
Java_com_jroark_be300android_Be300Native_nativeSendButton(
    JNIEnv *env, jobject thiz, jlong handle,
    jint btn1, jint btn2)
{
    bridge_state_t *state = (bridge_state_t *)(uintptr_t)handle;
    if (!state || !state->m) return;
    machine_t *m = state->m;
    m->btn_set1 = (uint8_t)btn1;
    m->btn_set2 = (uint8_t)btn2;
    __sync_synchronize();
}

JNIEXPORT void JNICALL
Java_com_jroark_be300android_Be300Native_nativeStop(JNIEnv *env, jobject thiz, jlong handle)
{
    bridge_state_t *state = (bridge_state_t *)(uintptr_t)handle;
    if (!state || !state->m) return;

    LOGI("nativeStop: setting emul_shutdown = true");
    emul_shutdown = true;
    __sync_synchronize();
}

JNIEXPORT void JNICALL
Java_com_jroark_be300android_Be300Native_nativeDestroy(JNIEnv *env, jobject thiz, jlong handle)
{
    bridge_state_t *state = (bridge_state_t *)(uintptr_t)handle;
    if (!state) return;

    LOGI("nativeDestroy: stopping thread...");
    emul_shutdown = true;
    __sync_synchronize();

    if (state->thread) {
        LOGI("nativeDestroy: joining thread %p...", (void*)state->thread);
        pthread_join(state->thread, NULL);
        LOGI("nativeDestroy: thread joined");
    }

    if (state->m) {
        LOGI("nativeDestroy: calling be300_destroy...");
        be300_destroy(state->m);
        LOGI("nativeDestroy: be300_destroy done");
    }

    free((void*)state->cfg.kernel_path);
    free((void*)state->cfg.cmdline);
    free(state);
    LOGI("nativeDestroy: bridge state freed");
}
