# Keep JNI bridge symbols.
-keep class xyz.xrtc.uvcstreamer.** { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}
