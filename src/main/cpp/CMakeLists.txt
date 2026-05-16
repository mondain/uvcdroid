cmake_minimum_required(VERSION 3.22.1)
project("uvc_native")

# CRITICAL: Force 16 KB ELF segment alignment for Android 15 compatibility.
# (NDK r28+ already does this by default; flag is harmless on older NDKs.)
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384")

set(PREBUILT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs/${ANDROID_ABI})
set(INCLUDE_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(NOT EXISTS ${PREBUILT_DIR}/libusb.a OR NOT EXISTS ${PREBUILT_DIR}/libuvc.a)
    message(FATAL_ERROR
        "Missing prebuilt static libraries for ABI=${ANDROID_ABI}.\n"
        "Expected: ${PREBUILT_DIR}/libusb.a and ${PREBUILT_DIR}/libuvc.a\n"
        "Build libusb (Android fork) and libuvc with -Wl,-z,max-page-size=16384 "
        "and drop the .a files plus their public headers under src/main/cpp/include/.")
endif()

# Prebuilt libusb (Android fork that supports opening by file descriptor)
add_library(libusb STATIC IMPORTED)
set_target_properties(libusb PROPERTIES IMPORTED_LOCATION ${PREBUILT_DIR}/libusb.a)

# Prebuilt libuvc
add_library(libuvc STATIC IMPORTED)
set_target_properties(libuvc PROPERTIES IMPORTED_LOCATION ${PREBUILT_DIR}/libuvc.a)

add_library(uvc_native SHARED uvc_jni.cpp)

target_include_directories(uvc_native PRIVATE ${INCLUDE_DIR})

target_link_libraries(uvc_native
        android
        log
        libuvc
        libusb)
