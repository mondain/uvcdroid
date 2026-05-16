# Keep JNI bridge symbols so the native lib can resolve them at runtime.
-keep class xyz.xrtc.uvcstreamer.UVCCamera { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}
