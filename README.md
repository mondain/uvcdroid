# uvcdroid

Android UVC (USB Video Class) streaming library built on libusb (Android fork) and
libuvc, with JNI glue that pushes decoded frames straight into an Android `Surface`.

- Package: `xyz.xrtc.uvcstreamer`
- Min SDK: 21
- Compile SDK: 35
- NDK: r28+ (16 KB page alignment required for Android 15)
- ABIs: `arm64-v8a`, `armeabi-v7a`, `x86`, `x86_64`

## Project layout

```
uvcdroid/
  build.gradle.kts        # Android library module
  settings.gradle.kts
  gradle.properties
  gradle/libs.versions.toml
  src/main/
    AndroidManifest.xml
    java/xyz/xrtc/uvcstreamer/UVCCamera.kt
    cpp/
      CMakeLists.txt
      uvc_jni.cpp
      include/                  # libusb.h, libuvc/libuvc.h (added by build script)
      libs/<abi>/libusb.a       # prebuilt static libs (built locally)
      libs/<abi>/libuvc.a
  scripts/build-native-deps.sh
```

## Building the native dependencies

Before the first Gradle build you must produce the static libs:

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r28
./scripts/build-native-deps.sh
```

The script clones upstream libusb + libuvc, builds them for all four ABIs with
`-Wl,-z,max-page-size=16384`, and stages the `.a` files and public headers where
`src/main/cpp/CMakeLists.txt` expects them.

The JNI layer opens the camera with `libusb_wrap_sys_device` + `uvc_wrap`, so
**upstream libusb (>= 1.0.24) works** — no Android-specific fork required.

## Building the AAR

```bash
./gradlew :assembleRelease
```

The AAR lands in `build/outputs/aar/uvcdroid-release.aar`.

## Publishing locally

```bash
./gradlew publishToMavenLocal
```

Then consume from another project:

```kotlin
repositories { mavenLocal() }
dependencies {
    implementation("xyz.xrtc:uvcdroid:0.1.0-SNAPSHOT")
}
```

## Usage

```kotlin
val camera = UVCCamera()
if (camera.connect(usbManager, usbDevice)) {
    camera.startStream(surfaceView.holder.surface)
}
// later
camera.stopStream()
camera.release()
```

You must request USB permission for the device (`UsbManager.requestPermission`)
before calling `connect`.
