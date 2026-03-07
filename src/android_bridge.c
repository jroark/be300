#include <jni.h>
#include <android/bitmap.h>
#include <android/log.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "ui.h"

#define LOG_TAG "BE300Native"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

typedef struct {
    machine_t *machine;
    pthread_t thread;
    pthread_mutex_t frame_lock;
    uint16_t *latest_frame;
    uint32_t frame_width;
    uint32_t frame_height;
    bool frame_ready;
    bool thread_started;
} android_emulator_t;

static android_emulator_t *g_active_emulator = NULL;

static void android_frame_cb(const uint16_t *pixels,
                             uint32_t width,
                             uint32_t height,
                             uint32_t stride,
                             void *user_data)
{
    android_emulator_t *emu = (android_emulator_t *)user_data;
    if (!emu || !pixels)
        return;

    if (stride < width)
        return;

    pthread_mutex_lock(&emu->frame_lock);

    size_t pixel_count = (size_t)width * (size_t)height;
    if (!emu->latest_frame || emu->frame_width != width || emu->frame_height != height) {
        uint16_t *new_frame = realloc(emu->latest_frame, pixel_count * sizeof(uint16_t));
        if (!new_frame) {
            pthread_mutex_unlock(&emu->frame_lock);
            return;
        }
        emu->latest_frame = new_frame;
        emu->frame_width = width;
        emu->frame_height = height;
    }

    for (uint32_t y = 0; y < height; y++) {
        memcpy(&emu->latest_frame[y * width],
               &pixels[y * stride],
               (size_t)width * sizeof(uint16_t));
    }
    emu->frame_ready = true;

    pthread_mutex_unlock(&emu->frame_lock);
}

static void *emulator_thread_main(void *arg)
{
    android_emulator_t *emu = (android_emulator_t *)arg;
    if (!emu || !emu->machine)
        return NULL;

    machine_run(emu->machine);
    return NULL;
}

static android_emulator_t *emu_from_handle(jlong handle)
{
    if (handle == 0)
        return NULL;
    return (android_emulator_t *)(uintptr_t)handle;
}

JNIEXPORT jlong JNICALL
Java_com_jroark_be300android_Be300Native_nativeCreate(
        JNIEnv *env,
        jobject thiz,
        jstring kernel_path,
        jstring cmdline,
        jboolean sfb_5bit_green,
        jint sdram_mb)
{
    (void)thiz;

    if (!kernel_path)
        return 0;

    const char *kernel_path_c = (*env)->GetStringUTFChars(env, kernel_path, NULL);
    const char *cmdline_c = NULL;
    if (cmdline)
        cmdline_c = (*env)->GetStringUTFChars(env, cmdline, NULL);

    machine_config_t cfg = {
        .trace = false,
        .log_mmio = false,
        .sfb_5bit_green = (sfb_5bit_green == JNI_TRUE),
        .rom_path = NULL,
        .kernel_path = kernel_path_c,
        .cmdline = cmdline_c,
        .ram_path = NULL,
        .sdram_size = 16u * 1024u * 1024u,
    };

    if (sdram_mb >= 1 && sdram_mb <= 64)
        cfg.sdram_size = (uint32_t)sdram_mb * 1024u * 1024u;

    machine_t *machine = machine_create(&cfg);

    if (cmdline_c)
        (*env)->ReleaseStringUTFChars(env, cmdline, cmdline_c);
    (*env)->ReleaseStringUTFChars(env, kernel_path, kernel_path_c);

    if (!machine) {
        ALOGE("machine_create failed");
        return 0;
    }

    android_emulator_t *emu = calloc(1, sizeof(*emu));
    if (!emu) {
        machine_destroy(machine);
        return 0;
    }

    emu->machine = machine;
    pthread_mutex_init(&emu->frame_lock, NULL);

    return (jlong)(uintptr_t)emu;
}

JNIEXPORT jboolean JNICALL
Java_com_jroark_be300android_Be300Native_nativeStart(JNIEnv *env, jobject thiz, jlong handle)
{
    (void)env;
    (void)thiz;

    android_emulator_t *emu = emu_from_handle(handle);
    if (!emu || !emu->machine)
        return JNI_FALSE;

    if (emu->thread_started)
        return JNI_TRUE;

    g_active_emulator = emu;
    ui_set_frame_callback(android_frame_cb, emu);

    int rc = pthread_create(&emu->thread, NULL, emulator_thread_main, emu);
    if (rc != 0) {
        ALOGE("pthread_create failed: %d", rc);
        ui_set_frame_callback(NULL, NULL);
        g_active_emulator = NULL;
        return JNI_FALSE;
    }

    emu->thread_started = true;
    ALOGI("Emulator thread started");
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_jroark_be300android_Be300Native_nativeCopyFrameToBitmap(
        JNIEnv *env,
        jobject thiz,
        jlong handle,
        jobject bitmap)
{
    (void)env;
    (void)thiz;

    android_emulator_t *emu = emu_from_handle(handle);
    if (!emu || !bitmap)
        return JNI_FALSE;

    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS)
        return JNI_FALSE;

    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888)
        return JNI_FALSE;

    pthread_mutex_lock(&emu->frame_lock);
    bool ready = emu->frame_ready && emu->latest_frame && emu->frame_width && emu->frame_height;
    uint32_t src_w = emu->frame_width;
    uint32_t src_h = emu->frame_height;

    if (!ready) {
        pthread_mutex_unlock(&emu->frame_lock);
        return JNI_FALSE;
    }

    void *pixels = NULL;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS) {
        pthread_mutex_unlock(&emu->frame_lock);
        return JNI_FALSE;
    }

    uint8_t *dst = (uint8_t *)pixels;
    uint32_t out_w = info.width;
    uint32_t out_h = info.height;

    for (uint32_t y = 0; y < out_h; y++) {
        uint32_t src_y = (uint32_t)(((uint64_t)y * src_h) / out_h);
        uint8_t *row = dst + (size_t)y * info.stride;
        for (uint32_t x = 0; x < out_w; x++) {
            uint32_t src_x = (uint32_t)(((uint64_t)x * src_w) / out_w);
            uint16_t p = emu->latest_frame[src_y * src_w + src_x];
            uint32_t r5 = (p >> 11) & 0x1Fu;
            uint32_t g6 = (p >> 5) & 0x3Fu;
            uint32_t b5 = p & 0x1Fu;
            uint8_t r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
            uint8_t g8 = (uint8_t)((g6 << 2) | (g6 >> 4));
            uint8_t b8 = (uint8_t)((b5 << 3) | (b5 >> 2));

            size_t o = (size_t)x * 4u;
            /* ANDROID_BITMAP_FORMAT_RGBA_8888 is little-endian in memory (BGRA byte order). */
            row[o + 0] = b8;
            row[o + 1] = g8;
            row[o + 2] = r8;
            row[o + 3] = 0xFFu;
        }
    }

    AndroidBitmap_unlockPixels(env, bitmap);
    pthread_mutex_unlock(&emu->frame_lock);

    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_jroark_be300android_Be300Native_nativeStop(JNIEnv *env, jobject thiz, jlong handle)
{
    (void)env;
    (void)thiz;

    android_emulator_t *emu = emu_from_handle(handle);
    if (!emu)
        return;

    if (emu->machine)
        machine_stop(emu->machine);

    if (emu->thread_started) {
        pthread_join(emu->thread, NULL);
        emu->thread_started = false;
    }

    if (g_active_emulator == emu) {
        ui_set_frame_callback(NULL, NULL);
        g_active_emulator = NULL;
    }
}

JNIEXPORT void JNICALL
Java_com_jroark_be300android_Be300Native_nativeDestroy(JNIEnv *env, jobject thiz, jlong handle)
{
    (void)env;
    (void)thiz;

    android_emulator_t *emu = emu_from_handle(handle);
    if (!emu)
        return;

    Java_com_jroark_be300android_Be300Native_nativeStop(env, thiz, handle);

    if (emu->machine)
        machine_destroy(emu->machine);

    pthread_mutex_destroy(&emu->frame_lock);
    free(emu->latest_frame);
    free(emu);
}
