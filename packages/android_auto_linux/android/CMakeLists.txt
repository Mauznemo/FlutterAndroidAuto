# The native core on Android: the same sources the Linux plugin builds, with the
# Android backends in place of the Linux ones.
#
#   Linux                         Android
#   present/gl_adapter.cc         present/gl_adapter_android.cc   EGL into a SurfaceProducer
#   video/libav_decoder.cc        video/mediacodec_decoder.cc     MediaCodec, AHardwareBuffer
#   audio/pulse_sink.cc           audio/aaudio_sink.cc
#   audio/pulse_source.cc         audio/aaudio_source.cc
#   bluetooth/bluez_client.cc     bluetooth/bluez_client_android.cc  (not implemented yet)
#   session/usb_connector.cc      session/usb_connector_android.cc   devices from UsbManager
#   android_auto_linux_plugin.cc  platform/android_jni.cc         the plugin's entry point
#
# Everything else, aasdk, the protocol session and every channel, is shared.
cmake_minimum_required(VERSION 3.22)
project(android_auto_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(AA_PACKAGE "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(AA_SRC "${AA_PACKAGE}/linux/src")
set(AASDK_DIR "${AA_PACKAGE}/linux/third_party/aasdk")
set(AASDK_PATCH "${AA_PACKAGE}/linux/patches/0001-port-to-boost-asio-io_context.patch")
set(AA_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/deps/${ANDROID_ABI}")

option(AA_USE_CCACHE "Compile through ccache when it is installed" ON)
if(AA_USE_CCACHE AND NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
  find_program(AA_CCACHE_EXECUTABLE NAMES ccache)
  if(AA_CCACHE_EXECUTABLE)
    set(CMAKE_C_COMPILER_LAUNCHER "${AA_CCACHE_EXECUTABLE}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${AA_CCACHE_EXECUTABLE}")
  endif()
endif()

# === dependencies ===
#
# Android has no package manager to provide Boost, protobuf, OpenSSL and libusb, so they
# are cross compiled from pinned releases by tools/build-android-deps.sh into deps/.
if(NOT EXISTS "${AA_DEPS}/lib/libboost_log.a")
  message(FATAL_ERROR
    "The Android dependencies for ${ANDROID_ABI} have not been built. Run:\n"
    "  tools/build-android-deps.sh ${ANDROID_ABI}")
endif()

# === aasdk ===
#
# The same patch the Linux build applies, keyed on the same marker, so whichever builds
# first does it.
if(NOT EXISTS "${AASDK_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "The aasdk submodule is missing. Run:\n  git submodule update --init --recursive")
endif()
if(NOT EXISTS "${AASDK_DIR}/include/aasdk/Common/Strand.hpp")
  find_program(AA_GIT_EXECUTABLE NAMES git REQUIRED)
  message(STATUS "Applying the aasdk io_context port")
  execute_process(
    COMMAND "${AA_GIT_EXECUTABLE}" -C "${AASDK_DIR}" apply "${AASDK_PATCH}"
    RESULT_VARIABLE AA_PATCH_RESULT
    ERROR_VARIABLE AA_PATCH_ERROR)
  if(NOT AA_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not apply the aasdk port patch: ${AA_PATCH_ERROR}")
  endif()
endif()

# The finders aasdk uses look in /usr, which on a cross build is the host's. Each is
# handed the answer instead, and pkg-config, which FindOpenSSL consults for its static
# link flags, only sees the Android prefix: left alone it read the host's openssl.pc and
# linked an Android library against x86 Ubuntu's libzstd.
set(ENV{PKG_CONFIG_LIBDIR} "${AA_DEPS}/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
set(Boost_DIR "${AA_DEPS}/lib/cmake/Boost-1.90.0" CACHE PATH "" FORCE)
set(OPENSSL_ROOT_DIR "${AA_DEPS}" CACHE PATH "" FORCE)
set(OPENSSL_USE_STATIC_LIBS TRUE)
set(LIBUSB_1_INCLUDE_DIRS "${AA_DEPS}/include/libusb-1.0" CACHE PATH "" FORCE)
set(LIBUSB_1_LIBRARIES "${AA_DEPS}/lib/libusb-1.0.a" CACHE FILEPATH "" FORCE)
set(Protobuf_INCLUDE_DIR "${AA_DEPS}/include" CACHE PATH "" FORCE)
set(Protobuf_LIBRARY "${AA_DEPS}/lib/libprotobuf.a" CACHE FILEPATH "" FORCE)
set(Protobuf_LIBRARIES "${AA_DEPS}/lib/libprotobuf.a" CACHE FILEPATH "" FORCE)
set(PROTOBUF_LIBRARY "${AA_DEPS}/lib/libprotobuf.a" CACHE FILEPATH "" FORCE)
set(PROTOBUF_INCLUDE_DIR "${AA_DEPS}/include" CACHE PATH "" FORCE)
# protoc runs on the build machine, so it is the host's, and it has to be the release
# the Android libprotobuf was built from.
find_program(AA_PROTOC_EXECUTABLE NAMES protoc NO_CMAKE_FIND_ROOT_PATH REQUIRED)
set(Protobuf_PROTOC_EXECUTABLE "${AA_PROTOC_EXECUTABLE}" CACHE FILEPATH "" FORCE)
set(PROTOBUF_PROTOC_EXECUTABLE "${AA_PROTOC_EXECUTABLE}" CACHE FILEPATH "" FORCE)
list(APPEND CMAKE_FIND_ROOT_PATH "${AA_DEPS}")

set(SKIP_BUILD_PROTOBUF ON CACHE BOOL "" FORCE)
set(SKIP_BUILD_ABSL ON CACHE BOOL "" FORCE)
set(AASDK_TEST OFF CACHE BOOL "" FORCE)
set(AASDK_BENCHMARK OFF CACHE BOOL "" FORCE)
set(AASDK_LIBRARY_TYPE "STATIC" CACHE STRING "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory("${AASDK_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/aasdk" EXCLUDE_FROM_ALL)

# === the core ===
add_library(android_auto_native SHARED
  "${AA_SRC}/aa_core.cc"
  "${AA_SRC}/audio/audio_input.cc"
  "${AA_SRC}/audio/audio_output.cc"
  "${AA_SRC}/audio/aaudio_sink.cc"
  "${AA_SRC}/audio/aaudio_source.cc"
  "${AA_SRC}/bluetooth/bluez_client_android.cc"
  "${AA_SRC}/event_bus.cc"
  "${AA_SRC}/frame_ring.cc"
  "${AA_SRC}/test_pattern.cc"
  "${AA_SRC}/metadata/json.cc"
  "${AA_SRC}/metadata/metadata_state.cc"
  "${AA_SRC}/platform/android_jni.cc"
  "${AA_SRC}/present/gl_adapter_android.cc"
  "${AA_SRC}/sensors/sensor_state.cc"
  "${AA_SRC}/session/usb_context.cc"
  "${AA_SRC}/session/usb_connector_android.cc"
  "${AA_SRC}/session/wireless_connector.cc"
  "${AA_SRC}/session/audio_channels.cc"
  "${AA_SRC}/session/service_discovery.cc"
  "${AA_SRC}/session/input_channel.cc"
  "${AA_SRC}/session/metadata_channel.cc"
  "${AA_SRC}/session/metadata_channels.cc"
  "${AA_SRC}/session/microphone_channel.cc"
  "${AA_SRC}/session/protocol_session.cc"
  "${AA_SRC}/session/sensor_channel.cc"
  "${AA_SRC}/session/video_channel.cc"
  "${AA_SRC}/video/mediacodec_decoder.cc"
  "${AA_SRC}/video/video_margins.cc"
  "${AA_SRC}/wireless/aaw_handshake.cc"
  "${AA_SRC}/wireless/wifi_network.cc"
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_definitions(android_auto_native PRIVATE AA_FAULT_INJECTION)
  target_compile_definitions(aasdk PRIVATE AA_FAULT_INJECTION)
endif()

# aasdk's public headers include <libusb.h> without putting it on the include path, the
# same gap the Linux build fills from pkg-config.
target_include_directories(android_auto_native PRIVATE "${AA_SRC}" "${AA_DEPS}/include/libusb-1.0")
set_target_properties(android_auto_native PROPERTIES CXX_VISIBILITY_PRESET hidden)

# What Dart and Java call and nothing else, for the same reason as on Linux: see the
# version script.
set(AA_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/android_auto_native.map")
target_link_options(android_auto_native PRIVATE
  "-Wl,--version-script=${AA_VERSION_SCRIPT}"
  # 16 KB pages, which Google Play requires of native code from Android 15 devices on.
  "-Wl,-z,max-page-size=16384")
set_property(TARGET android_auto_native APPEND PROPERTY LINK_DEPENDS "${AA_VERSION_SCRIPT}")

target_link_libraries(android_auto_native PRIVATE
  aasdk
  aap_protobuf
  aaudio
  android
  EGL
  GLESv3
  log
  mediandk
  nativewindow)
