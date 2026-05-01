#include <android/bitmap.h>
#include <android/log.h>
#include <jni.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "be300.h"
#include "stowaway.h"

#define LOG_TAG "BE300Native"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define FRAME_WIDTH 240u
#define FRAME_HEIGHT 320u
#define FRAME_BYTES (FRAME_WIDTH * FRAME_HEIGHT * 4u)
#define SERIAL_BYTES 8192u

typedef struct android_be300_handle {
    machine_t *machine;
    char *nand_path;
    char *cf_paths[BE300_MAX_CF_SLOTS];
    uint8_t *frame_rgba;
} android_be300_handle_t;

static android_be300_handle_t *handle_from_jlong(jlong value)
{
    return (android_be300_handle_t *)(uintptr_t)value;
}

static void throw_runtime(JNIEnv *env, const char *message)
{
    jclass cls = (*env)->FindClass(env, "java/lang/RuntimeException");
    if (cls)
        (*env)->ThrowNew(env, cls, message);
}

static char *dup_jstring(JNIEnv *env, jstring value)
{
    const char *utf;
    char *copy;

    if (!value)
        return NULL;

    utf = (*env)->GetStringUTFChars(env, value, NULL);
    if (!utf)
        return NULL;

    copy = strdup(utf);
    (*env)->ReleaseStringUTFChars(env, value, utf);
    return copy;
}

JNIEXPORT jlong JNICALL
Java_com_jroark_be300_NativeBe300_nativeCreate(JNIEnv *env, jclass cls,
    jstring nand_path, jstring cf0_path, jstring cf1_path, jint sdram_mb,
    jint target_mhz, jboolean rtc_host_time, jboolean enable_stowaway)
{
    (void)cls;

    android_be300_handle_t *handle =
        (android_be300_handle_t *)calloc(1, sizeof(*handle));
    if (!handle) {
        throw_runtime(env, "Unable to allocate emulator handle");
        return 0;
    }

    handle->nand_path = dup_jstring(env, nand_path);
    handle->cf_paths[0] = dup_jstring(env, cf0_path);
    handle->cf_paths[1] = dup_jstring(env, cf1_path);
    handle->frame_rgba = (uint8_t *)malloc(FRAME_BYTES);
    if (!handle->nand_path || !handle->frame_rgba ||
        (*env)->ExceptionCheck(env)) {
        free(handle->nand_path);
        free(handle->cf_paths[0]);
        free(handle->cf_paths[1]);
        free(handle->frame_rgba);
        free(handle);
        if (!(*env)->ExceptionCheck(env))
            throw_runtime(env, "Unable to allocate emulator paths");
        return 0;
    }

    machine_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.nand_path = handle->nand_path;
    cfg.cf_paths[0] = handle->cf_paths[0];
    cfg.cf_paths[1] = handle->cf_paths[1];
    for (unsigned i = 0; i < BE300_MAX_CF_SLOTS; i++) {
        if (cfg.cf_paths[i])
            cfg.cf_count = i + 1u;
    }
    cfg.sdram_size = (sdram_mb > 0 ? (uint32_t)sdram_mb : 16u) *
        1024u * 1024u;
    cfg.target_mhz = target_mhz > 0 ? (uint32_t)target_mhz : 0u;
    cfg.enable_rtc_host_time = rtc_host_time == JNI_TRUE;
    cfg.enable_ne2000 = false;
    cfg.enable_stowaway_keyboard = enable_stowaway == JNI_TRUE;
    cfg.enable_pcconnect_time_sync = false;

    handle->machine = be300_create(&cfg);
    if (!handle->machine) {
        free(handle->nand_path);
        free(handle->cf_paths[0]);
        free(handle->cf_paths[1]);
        free(handle->frame_rgba);
        free(handle);
        throw_runtime(env, "be300_create failed");
        return 0;
    }

    handle->machine->web_mode = true;
    handle->machine->use_builtin_ui = false;
    handle->machine->save_exit_screenshot = false;
    handle->machine->mirror_serial_to_stdout = false;

    return (jlong)(uintptr_t)handle;
}

JNIEXPORT jint JNICALL
Java_com_jroark_be300_NativeBe300_nativeStep(JNIEnv *env, jclass cls,
    jlong ptr, jint batches)
{
    (void)env;
    (void)cls;

    android_be300_handle_t *handle = handle_from_jlong(ptr);
    if (!handle || !handle->machine)
        return -1;
    return (jint)be300_step(handle->machine,
        batches > 0 ? (uint32_t)batches : 1u);
}

JNIEXPORT jboolean JNICALL
Java_com_jroark_be300_NativeBe300_nativeCopyFrame(JNIEnv *env, jclass cls,
    jlong ptr, jobject bitmap)
{
    (void)cls;

    android_be300_handle_t *handle = handle_from_jlong(ptr);
    AndroidBitmapInfo info;
    void *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;

    if (!handle || !handle->machine || !handle->frame_rgba || !bitmap)
        return JNI_FALSE;

    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS)
        return JNI_FALSE;
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 ||
        info.width < FRAME_WIDTH || info.height < FRAME_HEIGHT)
        return JNI_FALSE;

    int copied = be300_copy_frame_rgba8888(handle->machine,
        handle->frame_rgba, FRAME_BYTES, &width, &height);
    if (copied <= 0 || width != FRAME_WIDTH || height != FRAME_HEIGHT)
        return JNI_FALSE;

    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) !=
        ANDROID_BITMAP_RESULT_SUCCESS)
        return JNI_FALSE;

    for (uint32_t y = 0; y < FRAME_HEIGHT; y++) {
        memcpy((uint8_t *)pixels + (size_t)y * info.stride,
            handle->frame_rgba + (size_t)y * FRAME_WIDTH * 4u,
            FRAME_WIDTH * 4u);
    }

    AndroidBitmap_unlockPixels(env, bitmap);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_jroark_be300_NativeBe300_nativeSetTouch(JNIEnv *env, jclass cls,
    jlong ptr, jboolean down, jint x, jint y)
{
    (void)env;
    (void)cls;

    android_be300_handle_t *handle = handle_from_jlong(ptr);
    if (!handle || !handle->machine)
        return;

    if (x < 0) x = 0;
    if (x > 239) x = 239;
    if (y < 0) y = 0;
    if (y > 359) y = 359;
    be300_set_touch(handle->machine, down == JNI_TRUE,
        (uint16_t)x, (uint16_t)y);
}

JNIEXPORT void JNICALL
Java_com_jroark_be300_NativeBe300_nativeSetButtons(JNIEnv *env, jclass cls,
    jlong ptr, jint set1, jint set2)
{
    (void)env;
    (void)cls;

    android_be300_handle_t *handle = handle_from_jlong(ptr);
    if (!handle || !handle->machine)
        return;

    be300_set_buttons(handle->machine, (uint8_t)set1, (uint8_t)set2);
}

JNIEXPORT jstring JNICALL
Java_com_jroark_be300_NativeBe300_nativeDrainSerial(JNIEnv *env, jclass cls,
    jlong ptr)
{
    (void)cls;

    android_be300_handle_t *handle = handle_from_jlong(ptr);
    char buf[SERIAL_BYTES + 1u];
    size_t len;

    if (!handle || !handle->machine)
        return (*env)->NewStringUTF(env, "");

    len = be300_drain_serial(handle->machine, buf, SERIAL_BYTES);
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\0')
            buf[i] = ' ';
    }
    buf[len] = '\0';
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jboolean JNICALL
Java_com_jroark_be300_NativeBe300_nativeStowawayKey(JNIEnv *env, jclass cls,
    jlong ptr, jint scancode, jboolean release)
{
    (void)env;
    (void)cls;

    android_be300_handle_t *handle = handle_from_jlong(ptr);
    if (!handle || !handle->machine ||
        !handle->machine->cfg.enable_stowaway_keyboard ||
        scancode < 0 || scancode > 127)
        return JNI_FALSE;

    return stowaway_queue_key((unsigned)scancode, release == JNI_TRUE)
        ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_jroark_be300_NativeBe300_nativeStop(JNIEnv *env, jclass cls,
    jlong ptr)
{
    (void)env;
    (void)cls;

    android_be300_handle_t *handle = handle_from_jlong(ptr);
    if (handle && handle->machine)
        be300_stop(handle->machine);
}

JNIEXPORT void JNICALL
Java_com_jroark_be300_NativeBe300_nativeDestroy(JNIEnv *env, jclass cls,
    jlong ptr)
{
    (void)env;
    (void)cls;

    android_be300_handle_t *handle = handle_from_jlong(ptr);
    if (!handle)
        return;

    if (handle->machine) {
        be300_destroy(handle->machine);
        handle->machine = NULL;
    }
    free(handle->nand_path);
    free(handle->cf_paths[0]);
    free(handle->cf_paths[1]);
    free(handle->frame_rgba);
    free(handle);
}
