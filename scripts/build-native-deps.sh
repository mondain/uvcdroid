#!/usr/bin/env bash
# Build libusb (upstream) + libuvc as static libs for all four Android ABIs.
# Upstream libusb (>= 1.0.24) exposes libusb_wrap_sys_device, which the JNI uses
# via uvc_wrap, so no Android fork is required.
# Output is placed under src/main/cpp/libs/<abi>/ and headers under src/main/cpp/include/.
#
# Requirements:
#   - ANDROID_NDK_HOME pointing at an NDK r28+ install
#   - cmake, ninja, git, autoconf, automake, libtool, pkg-config
set -euo pipefail

: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to your NDK r28+ path}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/build/native"
OUT_LIBS="$ROOT/src/main/cpp/libs"
OUT_INC="$ROOT/src/main/cpp/include"
ABIS=(arm64-v8a armeabi-v7a x86 x86_64)
API=21

LIBUSB_REPO="${LIBUSB_REPO:-https://github.com/libusb/libusb.git}"
LIBUSB_REF="${LIBUSB_REF:-master}"
LIBUVC_REPO="${LIBUVC_REPO:-https://github.com/libuvc/libuvc.git}"
LIBUVC_REF="${LIBUVC_REF:-master}"

LDFLAGS_16K="-Wl,-z,max-page-size=16384"

mkdir -p "$WORK" "$OUT_INC"
cd "$WORK"

[ -d libusb ] || git clone --depth 1 --branch "$LIBUSB_REF" "$LIBUSB_REPO" libusb
[ -d libuvc ] || git clone --depth 1 --branch "$LIBUVC_REF" "$LIBUVC_REPO" libuvc

TOOLCHAIN="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"

for ABI in "${ABIS[@]}"; do
    echo "==> Building libusb for $ABI"
    BUILD_DIR="$WORK/build/libusb-$ABI"
    rm -rf "$BUILD_DIR" && mkdir -p "$BUILD_DIR"

    LIBUSB_CMAKE_DIR="$WORK/build/libusb-cmake"
    rm -rf "$LIBUSB_CMAKE_DIR" && mkdir -p "$LIBUSB_CMAKE_DIR"
    cat > "$LIBUSB_CMAKE_DIR/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.22.1)
project(libusb_android_static C)

set(LIBUSB_SOURCE_DIR "" CACHE PATH "Path to the libusb source checkout")

add_library(usb STATIC
    ${LIBUSB_SOURCE_DIR}/libusb/core.c
    ${LIBUSB_SOURCE_DIR}/libusb/descriptor.c
    ${LIBUSB_SOURCE_DIR}/libusb/hotplug.c
    ${LIBUSB_SOURCE_DIR}/libusb/io.c
    ${LIBUSB_SOURCE_DIR}/libusb/sync.c
    ${LIBUSB_SOURCE_DIR}/libusb/strerror.c
    ${LIBUSB_SOURCE_DIR}/libusb/os/linux_usbfs.c
    ${LIBUSB_SOURCE_DIR}/libusb/os/events_posix.c
    ${LIBUSB_SOURCE_DIR}/libusb/os/threads_posix.c
    ${LIBUSB_SOURCE_DIR}/libusb/os/linux_netlink.c
)

target_include_directories(usb PRIVATE
    ${LIBUSB_SOURCE_DIR}/android
    ${LIBUSB_SOURCE_DIR}/libusb
    ${LIBUSB_SOURCE_DIR}/libusb/os
)

target_compile_options(usb PRIVATE
    -fvisibility=hidden
    -pthread
    -Wall
    -Wextra
    -Wshadow
    -Wunused
    -Wwrite-strings
    -Werror=format-security
    -Werror=implicit-function-declaration
    -Werror=implicit-int
    -Werror=init-self
    -Werror=missing-prototypes
    -Werror=strict-prototypes
    -Werror=undef
    -Werror=uninitialized
)

set_target_properties(usb PROPERTIES OUTPUT_NAME usb)
EOF

    cmake -S "$LIBUSB_CMAKE_DIR" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" \
        -DCMAKE_BUILD_TYPE=Release \
        -DLIBUSB_SOURCE_DIR="$WORK/libusb" \
        -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS_16K" \
        -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS_16K"
    cmake --build "$BUILD_DIR" --parallel
    mkdir -p "$OUT_LIBS/$ABI"
    cp "$BUILD_DIR/libusb.a" "$OUT_LIBS/$ABI/libusb.a"

    PKG_CONFIG_DIR="$BUILD_DIR/pkgconfig"
    mkdir -p "$PKG_CONFIG_DIR"
    cat > "$PKG_CONFIG_DIR/libusb-1.0.pc" <<EOF
prefix=$OUT_LIBS/$ABI
exec_prefix=\${prefix}
libdir=\${prefix}
includedir=$WORK/libusb/libusb

Name: libusb-1.0
Description: libusb static Android build
Version: 1.0
Libs: -L\${libdir} -lusb
Cflags: -I\${includedir}
EOF

    echo "==> Building libuvc for $ABI"
    BUILD_DIR="$WORK/build/libuvc-$ABI"
    rm -rf "$BUILD_DIR" && mkdir -p "$BUILD_DIR"
    PKG_CONFIG_LIBDIR="$PKG_CONFIG_DIR" cmake -S "$WORK/libuvc" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
        -DCMAKE_BUILD_TARGET=Static -DBUILD_EXAMPLE=OFF -DBUILD_TEST=OFF \
        -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS_16K" \
        -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS_16K"
    cmake --build "$BUILD_DIR" --parallel
    find "$BUILD_DIR" -name 'libuvc.a' -exec cp {} "$OUT_LIBS/$ABI/libuvc.a" \;
done

echo "==> Staging public headers"
mkdir -p "$OUT_INC/libuvc"
cp "$WORK/libusb/libusb/libusb.h" "$OUT_INC/"
cp "$WORK/libuvc/include/libuvc/libuvc.h" "$OUT_INC/libuvc/"
cp "$WORK/build/libuvc-${ABIS[0]}/include/libuvc/libuvc_config.h" "$OUT_INC/libuvc/"

echo "Done. Prebuilts under $OUT_LIBS, headers under $OUT_INC."
