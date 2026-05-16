package xyz.xrtc.uvcstreamer

import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.view.Surface

class UVCCamera {

    init {
        System.loadLibrary("uvc_native")
    }

    private var usbConnection: UsbDeviceConnection? = null

    /**
     * Opens [device] through Android's [UsbManager] and hands the file descriptor
     * to libusb via libusb_wrap_sys_device. The caller is responsible for having
     * already obtained USB permission for the device.
     */
    fun connect(usbManager: UsbManager, device: UsbDevice): Boolean {
        val conn = usbManager.openDevice(device) ?: return false
        usbConnection = conn
        val ok = nativeConnect(conn.fileDescriptor)
        if (!ok) {
            conn.close()
            usbConnection = null
        }
        return ok
    }

    fun startStream(surface: Surface): Boolean = nativeStartStream(surface)

    fun stopStream() = nativeStopStream()

    fun release() {
        nativeRelease()
        usbConnection?.close()
        usbConnection = null
    }

    private external fun nativeConnect(fd: Int): Boolean
    private external fun nativeStartStream(surface: Surface): Boolean
    private external fun nativeStopStream()
    private external fun nativeRelease()
}
