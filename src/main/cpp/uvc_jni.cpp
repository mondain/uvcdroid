#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <libusb.h>
#include <libuvc/libuvc.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

#define LOG_TAG "UVC_Native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Single-camera singleton state. If you need multi-camera, move these into a
// per-instance struct stored in a Kotlin Long handle.
static libusb_context     *g_usb_ctx = nullptr;
static uvc_context_t      *g_uvc_ctx = nullptr;
static uvc_device_handle_t *g_devh   = nullptr;
static uvc_stream_handle_t *g_strmh  = nullptr;
static uvc_stream_ctrl_t   g_ctrl;
static ANativeWindow      *g_window  = nullptr;
static ANativeWindow      *g_preview_window = nullptr;
static int                 g_stream_width = 640;
static int                 g_stream_height = 480;
static int                 g_stream_fps = 30;
static enum uvc_frame_format g_stream_format = UVC_FRAME_FORMAT_UNKNOWN;
static uint32_t            g_frame_count = 0;
static bool                g_logged_convert_error = false;
static bool                g_logged_lock_error = false;
static bool                g_logged_buffer_info = false;
static std::atomic_bool    g_usb_events_running{false};
static std::thread         g_usb_event_thread;
static std::atomic_bool    g_stream_running{false};
static std::thread         g_stream_thread;

static void LIBUSB_CALL libusb_log_callback(libusb_context * /*ctx*/,
                                            enum libusb_log_level level,
                                            const char *message) {
    if (!message) return;
    if (level <= LIBUSB_LOG_LEVEL_WARNING) LOGE("libusb[%d]: %s", level, message);
    else LOGI("libusb[%d]: %s", level, message);
}

static const char *format_name(enum uvc_frame_format format) {
    switch (format) {
        case UVC_FRAME_FORMAT_MJPEG: return "MJPEG";
        case UVC_FRAME_FORMAT_YUYV: return "YUYV";
        default: return "OTHER";
    }
}

static void log_formats() {
    const uvc_format_desc_t *format = uvc_get_format_descs(g_devh);
    for (; format; format = format->next) {
        char fourcc[5] = {
            static_cast<char>(format->fourccFormat[0]),
            static_cast<char>(format->fourccFormat[1]),
            static_cast<char>(format->fourccFormat[2]),
            static_cast<char>(format->fourccFormat[3]),
            0
        };
        LOGI("format desc index=%u subtype=%u fourcc=%s frames=%u",
             format->bFormatIndex,
             format->bDescriptorSubtype,
             fourcc,
             format->bNumFrameDescriptors);
        for (const uvc_frame_desc_t *frame = format->frame_descs; frame; frame = frame->next) {
            LOGI("  frame index=%u %ux%u defaultInterval=%u maxFrame=%u",
                 frame->bFrameIndex,
                 frame->wWidth,
                 frame->wHeight,
                 frame->dwDefaultFrameInterval,
                 frame->dwMaxVideoFrameBufferSize);
        }
    }
}

static bool try_stream_ctrl(enum uvc_frame_format format, int width, int height, int fps) {
    uvc_error_t res = uvc_get_stream_ctrl_format_size(g_devh, &g_ctrl, format, width, height, fps);
    if (res != UVC_SUCCESS) {
        LOGI("stream control rejected: %s %dx%d@%d res=%d",
             format_name(format), width, height, fps, res);
        return false;
    }
    g_stream_width = width;
    g_stream_height = height;
    g_stream_fps = fps;
    g_stream_format = format;
    LOGI("stream control selected: %s format=%u frame=%u %dx%d@%d interval=%u maxFrame=%u maxPayload=%u",
         format_name(format),
         g_ctrl.bFormatIndex,
         g_ctrl.bFrameIndex,
         g_stream_width,
         g_stream_height,
         fps,
         g_ctrl.dwFrameInterval,
         g_ctrl.dwMaxVideoFrameSize,
         g_ctrl.dwMaxPayloadTransferSize);
    return true;
}

static void apply_bandwidth_fix() {
    if (g_stream_format != UVC_FRAME_FORMAT_YUYV) return;

    const uint32_t interval = g_ctrl.dwFrameInterval > 100000
        ? g_ctrl.dwFrameInterval
        : static_cast<uint32_t>(10000000 / (g_stream_fps > 0 ? g_stream_fps : 15));
    uint32_t bandwidth = static_cast<uint32_t>(g_stream_width * g_stream_height * 2);
    bandwidth *= 10000000 / interval + 1;
    bandwidth /= 1000;
    bandwidth /= 8;
    bandwidth += 12;
    if (bandwidth < 1024) bandwidth = 1024;

    if (g_ctrl.dwMaxPayloadTransferSize != bandwidth) {
        LOGI("applying LifeCam-style bandwidth fix: maxPayload %u -> %u",
             g_ctrl.dwMaxPayloadTransferSize,
             bandwidth);
        g_ctrl.dwMaxPayloadTransferSize = bandwidth;
    }
}

static void draw_to_window(ANativeWindow *window, uvc_frame_t *frame, uvc_frame_t *rgb) {
    if (!window || !frame || !rgb) return;

    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window, &buffer, nullptr) == 0) {
        auto *dest = static_cast<uint8_t *>(buffer.bits);
        auto *src  = static_cast<uint8_t *>(rgb->data);
        const int dest_stride = buffer.stride * 4; // RGBA_8888
        const int src_stride  = frame->width * 3;
        const int dest_width = std::min(buffer.width, buffer.stride);
        const int dest_height = buffer.height;
        if (!g_logged_buffer_info) {
            LOGI("native window buffer: %dx%d stride=%d format=%d",
                 buffer.width,
                 buffer.height,
                 buffer.stride,
                 buffer.format);
            g_logged_buffer_info = true;
        }

        for (int y = 0; y < dest_height; y++) {
            uint8_t *row = dest + y * dest_stride;
            std::memset(row, 0, static_cast<size_t>(dest_width) * 4);
        }

        int draw_width = dest_width;
        int draw_height = frame->height * dest_width / frame->width;
        if (draw_height > dest_height) {
            draw_height = dest_height;
            draw_width = frame->width * dest_height / frame->height;
        }
        const int x_offset = (dest_width - draw_width) / 2;
        const int y_offset = (dest_height - draw_height) / 2;

        for (int y = 0; y < draw_height; y++) {
            int src_y = y * frame->height / draw_height;
            for (int x = 0; x < draw_width; x++) {
                int src_x = x * frame->width / draw_width;
                uint8_t *pixel = dest + (y + y_offset) * dest_stride + (x + x_offset) * 4;
                uint8_t *src_pixel = src + src_y * src_stride + src_x * 3;
                pixel[0] = src_pixel[0];
                pixel[1] = src_pixel[1];
                pixel[2] = src_pixel[2];
                pixel[3] = 255;
            }
        }
        ANativeWindow_unlockAndPost(window);
    } else if (!g_logged_lock_error) {
        LOGE("ANativeWindow_lock failed");
        g_logged_lock_error = true;
    }
}

static void draw_frame(uvc_frame_t *frame) {
    if ((!g_window && !g_preview_window) || !frame) return;

    const uint32_t frame_number = ++g_frame_count;
    if (frame_number <= 3 || frame_number % 90 == 0) {
        LOGI("frame %u: %ux%u format=%d bytes=%zu",
             frame_number,
             frame->width,
             frame->height,
             frame->frame_format,
             frame->data_bytes);
    }

    uvc_frame_t *rgb = uvc_allocate_frame(frame->width * frame->height * 3);
    if (!rgb) return;

    uvc_error_t convert_result = uvc_any2rgb(frame, rgb);
    if (convert_result == UVC_SUCCESS) {
        draw_to_window(g_window, frame, rgb);
        draw_to_window(g_preview_window, frame, rgb);
    } else if (!g_logged_convert_error) {
        LOGE("uvc_any2rgb failed: %d", convert_result);
        g_logged_convert_error = true;
    }
    uvc_free_frame(rgb);
}

static void frame_callback(uvc_frame_t *frame, void * /*user*/) {
    draw_frame(frame);
}

static void usb_event_loop() {
    LOGI("libusb event thread started");
    while (g_usb_events_running.load()) {
        timeval timeout {0, 100000};
        int res = libusb_handle_events_timeout_completed(g_usb_ctx, &timeout, nullptr);
        if (res != LIBUSB_SUCCESS && g_usb_events_running.load()) {
            LOGE("libusb_handle_events failed: %d", res);
        }
    }
    LOGI("libusb event thread stopped");
}

static void stream_loop() {
    LOGI("stream polling thread started");
    uint32_t timeout_count = 0;
    while (g_stream_running.load()) {
        uvc_frame_t *frame = nullptr;
        uvc_error_t res = uvc_stream_get_frame(g_strmh, &frame, 500000);
        if (!g_stream_running.load()) break;
        if (res == UVC_SUCCESS && frame) {
            timeout_count = 0;
            draw_frame(frame);
        } else if (res == UVC_ERROR_TIMEOUT) {
            timeout_count++;
            if (timeout_count == 1 || timeout_count % 10 == 0) {
                LOGE("uvc_stream_get_frame timeout count=%u", timeout_count);
            }
        } else {
            LOGE("uvc_stream_get_frame failed: %d", res);
            break;
        }
    }
    LOGI("stream polling thread stopped");
}

static void cleanup_partial() {
    if (g_stream_running.load()) {
        g_stream_running.store(false);
        if (g_stream_thread.joinable()) g_stream_thread.join();
    }
    if (g_strmh)   { uvc_stream_stop(g_strmh); uvc_stream_close(g_strmh); g_strmh = nullptr; }
    if (g_devh)    { uvc_close(g_devh);  g_devh = nullptr; }
    if (g_uvc_ctx) { uvc_exit(g_uvc_ctx); g_uvc_ctx = nullptr; }
    if (g_usb_events_running.load()) {
        g_usb_events_running.store(false);
        if (g_usb_event_thread.joinable()) g_usb_event_thread.join();
    }
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
    libusb_set_log_cb(g_usb_ctx, libusb_log_callback, LIBUSB_LOG_CB_CONTEXT);
    libusb_set_option(g_usb_ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_INFO);

    // Share the libusb context with libuvc so both see the same wrapped device.
    uvc_error_t ures = uvc_init(&g_uvc_ctx, g_usb_ctx);
    if (ures != UVC_SUCCESS) {
        LOGE("uvc_init failed: %d", ures);
        cleanup_partial();
        return JNI_FALSE;
    }
    g_usb_events_running.store(true);
    g_usb_event_thread = std::thread(usb_event_loop);

    // uvc_wrap takes the system fd, wraps it via libusb_wrap_sys_device internally,
    // and produces a ready-to-use uvc_device_handle_t.
    ures = uvc_wrap(fd, g_uvc_ctx, &g_devh);
    if (ures != UVC_SUCCESS) {
        LOGE("uvc_wrap failed: %d", ures);
        cleanup_partial();
        return JNI_FALSE;
    }

    log_formats();
    if (!try_stream_ctrl(UVC_FRAME_FORMAT_YUYV, 160, 120, 15)
            && !try_stream_ctrl(UVC_FRAME_FORMAT_YUYV, 176, 144, 15)
            && !try_stream_ctrl(UVC_FRAME_FORMAT_YUYV, 320, 240, 15)
            && !try_stream_ctrl(UVC_FRAME_FORMAT_MJPEG, 160, 120, 15)
            && !try_stream_ctrl(UVC_FRAME_FORMAT_MJPEG, 176, 144, 15)
            && !try_stream_ctrl(UVC_FRAME_FORMAT_MJPEG, 320, 240, 15)
            && !try_stream_ctrl(UVC_FRAME_FORMAT_MJPEG, 640, 480, 15)) {
        LOGE("no compatible format/size/fps");
        cleanup_partial();
        return JNI_FALSE;
    }
    apply_bandwidth_fix();
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_xyz_xrtc_uvcstreamer_UVCCamera_nativeStartStream(
        JNIEnv *env,
        jobject /*thiz*/,
        jobject surface,
        jobject preview_surface) {
    if (!g_devh) return JNI_FALSE;

    if (g_window) {
        ANativeWindow_release(g_window);
        g_window = nullptr;
    }
    if (g_preview_window) {
        ANativeWindow_release(g_preview_window);
        g_preview_window = nullptr;
    }
    g_window = ANativeWindow_fromSurface(env, surface);
    if (!g_window) return JNI_FALSE;
    if (preview_surface) {
        g_preview_window = ANativeWindow_fromSurface(env, preview_surface);
    }
    g_frame_count = 0;
    g_logged_convert_error = false;
    g_logged_lock_error = false;
    g_logged_buffer_info = false;

    int geometry_result = ANativeWindow_setBuffersGeometry(
        g_window,
        0,
        0,
        WINDOW_FORMAT_RGBA_8888);
    if (geometry_result != 0) {
        LOGE("ANativeWindow_setBuffersGeometry(format only) failed: %d", geometry_result);
    }
    if (g_preview_window) {
        int preview_geometry_result = ANativeWindow_setBuffersGeometry(
            g_preview_window,
            0,
            0,
            WINDOW_FORMAT_RGBA_8888);
        if (preview_geometry_result != 0) {
            LOGE("ANativeWindow_setBuffersGeometry(preview format only) failed: %d",
                 preview_geometry_result);
        }
    }

    uvc_error_t res = uvc_start_iso_streaming(g_devh, &g_ctrl, frame_callback, nullptr);
    if (res != UVC_SUCCESS) {
        LOGE("uvc_start_iso_streaming failed: %d; trying default callback", res);
        res = uvc_start_streaming(g_devh, &g_ctrl, frame_callback, nullptr, 0);
        if (res != UVC_SUCCESS) {
            LOGE("uvc_start_streaming failed: %d", res);
            ANativeWindow_release(g_window);
            g_window = nullptr;
            if (g_preview_window) {
                ANativeWindow_release(g_preview_window);
                g_preview_window = nullptr;
            }
            return JNI_FALSE;
        }
        LOGI("uvc_start_streaming succeeded mode=default-callback");
    } else {
        LOGI("uvc_start_iso_streaming succeeded mode=iso-callback");
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_xyz_xrtc_uvcstreamer_UVCCamera_nativeStopStream(JNIEnv * /*env*/, jobject /*thiz*/) {
    if (g_stream_running.load()) {
        g_stream_running.store(false);
        if (g_stream_thread.joinable()) g_stream_thread.join();
    }
    if (g_strmh) {
        uvc_stream_stop(g_strmh);
        uvc_stream_close(g_strmh);
        g_strmh = nullptr;
    }
    if (g_devh) uvc_stop_streaming(g_devh);
    if (g_window) {
        ANativeWindow_release(g_window);
        g_window = nullptr;
    }
    if (g_preview_window) {
        ANativeWindow_release(g_preview_window);
        g_preview_window = nullptr;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_xyz_xrtc_uvcstreamer_UVCCamera_nativeRelease(JNIEnv * /*env*/, jobject /*thiz*/) {
    cleanup_partial();
}
