#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <libusb.h>
#include <libuvc/libuvc.h>

#define LOG_TAG "UVC_Native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Single-camera singleton state. If you need multi-camera, move these into a
// per-instance struct stored in a Kotlin Long handle.
static libusb_context     *g_usb_ctx = nullptr;
static uvc_context_t      *g_uvc_ctx = nullptr;
static uvc_device_handle_t *g_devh   = nullptr;
static uvc_stream_ctrl_t   g_ctrl;
static ANativeWindow      *g_window  = nullptr;

static void frame_callback(uvc_frame_t *frame, void * /*user*/) {
    if (!g_window || !frame) return;

    uvc_frame_t *rgb = uvc_allocate_frame(frame->width * frame->height * 3);
    if (!rgb) return;

    if (uvc_any2rgb(frame, rgb) == UVC_SUCCESS) {
        ANativeWindow_Buffer buffer;
        if (ANativeWindow_lock(g_window, &buffer, nullptr) == 0) {
            auto *dest = static_cast<uint8_t *>(buffer.bits);
            auto *src  = static_cast<uint8_t *>(rgb->data);
            const int dest_stride = buffer.stride * 4; // ARGB_8888
            const int src_stride  = frame->width * 3;

            for (int y = 0; y < frame->height; y++) {
                for (int x = 0; x < frame->width; x++) {
                    dest[y * dest_stride + x * 4 + 0] = src[y * src_stride + x * 3 + 0];
                    dest[y * dest_stride + x * 4 + 1] = src[y * src_stride + x * 3 + 1];
                    dest[y * dest_stride + x * 4 + 2] = src[y * src_stride + x * 3 + 2];
                    dest[y * dest_stride + x * 4 + 3] = 255;
                }
            }
            ANativeWindow_unlockAndPost(g_window);
        }
    }
    uvc_free_frame(rgb);
}

static void cleanup_partial() {
    if (g_devh)    { uvc_close(g_devh);  g_devh = nullptr; }
    if (g_uvc_ctx) { uvc_exit(g_uvc_ctx); g_uvc_ctx = nullptr; }
    if (g_usb_ctx) { libusb_exit(g_usb_ctx); g_usb_ctx = nullptr; }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_xyz_xrtc_uvcstreamer_UVCCamera_nativeConnect(JNIEnv * /*env*/, jobject /*thiz*/, jint fd) {
    if (g_devh) {
        LOGE("nativeConnect: already connected");
        return JNI_FALSE;
    }

    // Android apps cannot scan /dev/bus/usb without root, so tell libusb to skip
    // device discovery entirely. We hand it an already-open fd from UsbManager.
    int rc = libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    if (rc != LIBUSB_SUCCESS) {
        LOGE("libusb_set_option(NO_DEVICE_DISCOVERY) failed: %d", rc);
        return JNI_FALSE;
    }

    rc = libusb_init(&g_usb_ctx);
    if (rc != LIBUSB_SUCCESS) {
        LOGE("libusb_init failed: %d", rc);
        return JNI_FALSE;
    }

    // Share the libusb context with libuvc so both see the same wrapped device.
    uvc_error_t ures = uvc_init(&g_uvc_ctx, g_usb_ctx);
    if (ures != UVC_SUCCESS) {
        LOGE("uvc_init failed: %d", ures);
        cleanup_partial();
        return JNI_FALSE;
    }

    // uvc_wrap takes the system fd, wraps it via libusb_wrap_sys_device internally,
    // and produces a ready-to-use uvc_device_handle_t.
    ures = uvc_wrap(fd, g_uvc_ctx, &g_devh);
    if (ures != UVC_SUCCESS) {
        LOGE("uvc_wrap failed: %d", ures);
        cleanup_partial();
        return JNI_FALSE;
    }

    // Negotiate stream format: 640x480 @ 30 fps, MJPEG preferred, YUYV fallback.
    ures = uvc_get_stream_ctrl_format_size(g_devh, &g_ctrl, UVC_FRAME_FORMAT_MJPEG, 640, 480, 30);
    if (ures != UVC_SUCCESS) {
        ures = uvc_get_stream_ctrl_format_size(g_devh, &g_ctrl, UVC_FRAME_FORMAT_YUYV, 640, 480, 30);
        if (ures != UVC_SUCCESS) {
            LOGE("no compatible format/size/fps: %d", ures);
            cleanup_partial();
            return JNI_FALSE;
        }
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_xyz_xrtc_uvcstreamer_UVCCamera_nativeStartStream(JNIEnv *env, jobject /*thiz*/, jobject surface) {
    if (!g_devh) return JNI_FALSE;

    if (g_window) {
        ANativeWindow_release(g_window);
        g_window = nullptr;
    }
    g_window = ANativeWindow_fromSurface(env, surface);
    if (!g_window) return JNI_FALSE;

    uvc_error_t res = uvc_start_streaming(g_devh, &g_ctrl, frame_callback, nullptr, 0);
    if (res != UVC_SUCCESS) {
        ANativeWindow_release(g_window);
        g_window = nullptr;
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_xyz_xrtc_uvcstreamer_UVCCamera_nativeStopStream(JNIEnv * /*env*/, jobject /*thiz*/) {
    if (g_devh) uvc_stop_streaming(g_devh);
    if (g_window) {
        ANativeWindow_release(g_window);
        g_window = nullptr;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_xyz_xrtc_uvcstreamer_UVCCamera_nativeRelease(JNIEnv * /*env*/, jobject /*thiz*/) {
    cleanup_partial();
}
