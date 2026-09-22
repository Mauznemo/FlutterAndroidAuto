# The Flutter tooling requires that developers have CMake 3.10 or later
# installed. You should not increase this version, as doing so will cause
# the plugin to fail to compile for some customers of the plugin.
cmake_minimum_required(VERSION 3.10)

# Project-level configuration.
set(PROJECT_NAME "android_auto_linux")
project(${PROJECT_NAME} LANGUAGES CXX)

# This value is used when generating builds using this plugin, so it must
# not be changed.
set(PLUGIN_NAME "android_auto_linux_plugin")

# === build caching ===
#
# aasdk and its generated protobuf are the overwhelming majority of this build, and
# `flutter clean` throws the whole object tree away, so a clean rebuild pays for all of
# it again. ccache keys on preprocessed source rather than on timestamps, and its cache
# lives in the user's home directory, so it survives the clean that the build directory
# does not.
#
# Set here rather than in the app's CMakeLists so it covers this plugin and the aasdk
# subproject below, which is where the time actually goes, and leaves the host
# application's own build alone.
#
# CMAKE_<LANG>_COMPILER_LAUNCHER initialises a target property when the target is
# created, so this has to come before add_library and before add_subdirectory.
option(AA_USE_CCACHE "Compile through ccache when it is installed" ON)
if(AA_USE_CCACHE AND NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
  find_program(AA_CCACHE_EXECUTABLE NAMES ccache)
  if(AA_CCACHE_EXECUTABLE)
    set(CMAKE_C_COMPILER_LAUNCHER "${AA_CCACHE_EXECUTABLE}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${AA_CCACHE_EXECUTABLE}")
    message(STATUS "android_auto: compiling through ${AA_CCACHE_EXECUTABLE}")
  else()
    message(STATUS "android_auto: ccache not installed, a clean rebuild will be slow")
  endif()
endif()

# Any new source files that you add to the plugin should be added here.
list(APPEND PLUGIN_SOURCES
  "android_auto_linux_plugin.cc"
  "src/aa_core.cc"
  "src/audio/audio_input.cc"
  "src/audio/audio_output.cc"
  "src/audio/pulse_sink.cc"
  "src/audio/pulse_source.cc"
  "src/bluetooth/bluez_client.cc"
  "src/event_bus.cc"
  "src/frame_ring.cc"
  "src/test_pattern.cc"
  "src/metadata/json.cc"
  "src/metadata/metadata_state.cc"
  "src/present/gl_adapter.cc"
  "src/present/texture_registry.cc"
  "src/sensors/sensor_state.cc"
  "src/session/usb_context.cc"
  "src/session/usb_connector.cc"
  "src/session/wireless_connector.cc"
  "src/session/audio_channels.cc"
  "src/session/service_discovery.cc"
  "src/session/input_channel.cc"
  "src/session/metadata_channel.cc"
  "src/session/metadata_channels.cc"
  "src/session/microphone_channel.cc"
  "src/session/protocol_session.cc"
  "src/session/sensor_channel.cc"
  "src/session/video_channel.cc"
  "src/video/video_decoder.cc"
  "src/video/video_margins.cc"
  "src/wireless/aaw_handshake.cc"
  "src/wireless/wifi_network.cc"
)

# Define the plugin library target. Its name must not be changed (see comment
# on PLUGIN_NAME above).
add_library(${PLUGIN_NAME} SHARED
  ${PLUGIN_SOURCES}
)

# Apply a standard set of build settings that are configured in the
# application-level CMakeLists.txt. This can be removed for plugins that want
# full control over build settings.
apply_standard_settings(${PLUGIN_NAME})

# apply_standard_settings pins C++14, but aasdk's headers are C++17. target_compile_features
# takes the higher of the two, so this has to come after.
target_compile_features(${PLUGIN_NAME} PRIVATE cxx_std_17)

# Symbols are hidden by default to reduce the chance of accidental conflicts
# between plugins. This should not be removed; any symbols that should be
# exported should be explicitly exported with the FLUTTER_PLUGIN_EXPORT macro.
#
# The C ABI in src/aa_core.h opts back in with AA_EXPORT, because Dart has to be able
# to dlsym it.
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)

# CXX_VISIBILITY_PRESET only reaches this target's own objects. aasdk and its generated
# protobuf come in as static archives compiled by their own target, so their symbols
# arrive with default visibility and are exported from the plugin: 6159 of them against
# the 65 that are the interface. A Flutter app loads all its plugins into one address
# space and the first definition of a symbol wins, so that is a real hazard for any
# other plugin carrying its own protobuf or Boost. The version script says what is
# public; see the file itself for the list.
#
# LINK_FLAGS rather than target_link_options, which needs CMake 3.13 and this file is
# pinned at 3.10 on purpose, see the note at the top.
set(PLUGIN_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/android_auto_linux_plugin.map")
set_property(TARGET ${PLUGIN_NAME} APPEND_STRING PROPERTY
  LINK_FLAGS " -Wl,--version-script=${PLUGIN_VERSION_SCRIPT}")
set_property(TARGET ${PLUGIN_NAME} APPEND PROPERTY
  LINK_DEPENDS "${PLUGIN_VERSION_SCRIPT}")

# === test bench surface ===
#
# Some of the environment knobs are for provoking faults rather than for configuring a
# head unit: stalling Start(), killing a live transport on a timer, standing in for a
# paired phone on a Unix socket, and overriding the Wi-Fi passphrase the host app set.
# Every one of them is valuable while the protocol is being worked on and none of them
# belongs in a head unit sitting in a car, where the environment is not a trust boundary
# the way a host app's configuration is.
#
# So they are compiled out unless asked for. The default follows the build type rather
# than being a flat OFF, because a debug build is the test bench, and needing an extra
# -D to reproduce a crash is how a reproduction stops being used. A release build has
# them out whatever else is passed, unless somebody explicitly says otherwise.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(AA_FAULT_INJECTION_DEFAULT ON)
else()
  set(AA_FAULT_INJECTION_DEFAULT OFF)
endif()
option(AA_ENABLE_FAULT_INJECTION
  "Compile in the AA_FAULT_* knobs, the fake wireless phone and the Wi-Fi passphrase override"
  ${AA_FAULT_INJECTION_DEFAULT})
if(AA_ENABLE_FAULT_INJECTION)
  message(STATUS "android_auto: fault injection compiled in (AA_ENABLE_FAULT_INJECTION=ON)")
  target_compile_definitions(${PLUGIN_NAME} PRIVATE AA_FAULT_INJECTION)
endif()

# Source include directories and library dependencies. Add any plugin-specific
# dependencies here.
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::GTK)

# libepoxy is how GTK itself resolves GL entry points, so use the same thing rather
# than linking libGL directly and risking a different dispatch table than Flutter's.
find_package(PkgConfig REQUIRED)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::EPOXY)

# aasdk's public headers include <libusb.h>, but aasdk only puts libusb on the include
# path with a directory scoped include_directories(), which does not reach this target.
# Same shape of bug as the one patched in docs/aasdk-port-notes.md.
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::LIBUSB)

# === video decode ===
#
# libavcodec does the H.264, libavutil owns the VA-API device and the DRM prime export
# that makes the zero copy path possible, libswscale converts to RGBA on the software
# fallback. libavformat is deliberately not here: there is no container to demux, the
# phone sends raw Annex-B.
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::LIBAV)

# === audio in and out ===
#
# libpulse-simple plays the phone's PCM and reads the microphone, and libpulse enumerates
# the outputs and the inputs. That is the point of choosing this API: the same code runs
# on a PipeWire desktop, a PulseAudio one, and the compatibility layer most infotainment
# images ship. See src/audio/pcm_sink.h and
# src/audio/pcm_source.h for where a second backend would go.
pkg_check_modules(LIBPULSE REQUIRED IMPORTED_TARGET libpulse libpulse-simple)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::LIBPULSE)

# === wireless ===
#
# Wireless Android Auto starts over Bluetooth, and the only supported way to publish an
# SDP record on a modern system is BlueZ's D-Bus interface. GDBus is the D-Bus client
# that comes with GLib, which is already linked through GTK above, so this costs
# nothing beyond naming gio explicitly rather than relying on GTK to drag it in.
# libbluetooth's own SDP API is deprecated and its development package is not a
# dependency this project wants to add.
#
# gio-unix is what carries the RFCOMM file descriptor across D-Bus. Once it has
# arrived it is an ordinary socket and nothing else here touches GLib.
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::GIO)

# Reading the SSID and BSSID off the wireless interface needs linux/wireless.h, which
# like drm_fourcc.h below is kernel headers and no library.

# drm_fourcc.h is a header of macros with no library behind it, and it ships with the
# kernel headers rather than with libdrm, so there is nothing to find_package and
# nothing extra for an end user to install.

# === aasdk ===
#
# Vendored as a submodule and patched, see docs/aasdk-port-notes.md. It does not build
# unpatched against Boost 1.87 or newer, so the patch is applied here, at configure time,
# rather than being a manual step a fresh clone has to know about.
set(AASDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/aasdk")
set(AASDK_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/patches/0001-port-to-boost-asio-io_context.patch")
# Created by the patch, so its presence is the same question as "is this patched".
set(AASDK_PATCH_MARKER "${AASDK_DIR}/include/aasdk/Common/Strand.hpp")

if(NOT EXISTS "${AASDK_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "The aasdk submodule is missing. Run:\n"
    "  git submodule update --init --recursive")
endif()

if(NOT EXISTS "${AASDK_PATCH_MARKER}")
  find_program(AA_GIT_EXECUTABLE NAMES git)
  if(NOT AA_GIT_EXECUTABLE)
    message(FATAL_ERROR
      "aasdk needs patching before it will build against Boost 1.87 or newer, and git "
      "was not found to apply the patch with. Install git, or apply it by hand:\n"
      "  git -C ${AASDK_DIR} apply ${AASDK_PATCH}")
  endif()
  message(STATUS "Applying the aasdk io_context port")
  # git apply is all or nothing, so a failure here leaves the submodule as it was and the
  # message can say so without qualification. The usual cause is a submodule someone has
  # already edited by hand.
  execute_process(
    COMMAND "${AA_GIT_EXECUTABLE}" -C "${AASDK_DIR}" apply "${AASDK_PATCH}"
    RESULT_VARIABLE AA_PATCH_RESULT
    ERROR_VARIABLE AA_PATCH_ERROR)
  if(NOT AA_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR
      "Could not apply the aasdk port patch. Nothing was changed.\n"
      "git said: ${AA_PATCH_ERROR}\n"
      "If the submodule has been edited, reset it and try again:\n"
      "  tools/port-aasdk.sh reset")
  endif()
  if(NOT EXISTS "${AASDK_PATCH_MARKER}")
    message(FATAL_ERROR
      "The aasdk patch applied but did not produce ${AASDK_PATCH_MARKER}. The patch and "
      "the pinned submodule commit have diverged; regenerate it with:\n"
      "  tools/port-aasdk.sh reset && tools/port-aasdk.sh apply && tools/port-aasdk.sh patch")
  endif()
endif()

# aasdk's protobuf subproject uses include(FindProtobuf) rather than find_package, which
# has been seen to lose protoc on a reconfigure and fail with "protoc executable not
# found" even though it is installed. Resolving it once here makes the build
# reproducible instead of order dependent.
find_program(AA_PROTOC_EXECUTABLE NAMES protoc REQUIRED)
set(Protobuf_PROTOC_EXECUTABLE "${AA_PROTOC_EXECUTABLE}" CACHE FILEPATH "" FORCE)
set(PROTOBUF_PROTOC_EXECUTABLE "${AA_PROTOC_EXECUTABLE}" CACHE FILEPATH "" FORCE)

# Use the system protobuf and Abseil rather than letting aasdk build protobuf 30 from
# source, which turns a one minute build into a ten minute one.
set(SKIP_BUILD_PROTOBUF ON  CACHE BOOL "" FORCE)
set(SKIP_BUILD_ABSL     ON  CACHE BOOL "" FORCE)
set(AASDK_TEST          OFF CACHE BOOL "" FORCE)
set(AASDK_BENCHMARK     OFF CACHE BOOL "" FORCE)

# aasdk and its protobuf go in statically, so the plugin is one self contained .so and
# there is nothing beside it to bundle. The reason is aasdk's versioning: it names
# itself after the day it was built, libaasdk.so.2026 pointing at
# libaasdk.so.2026.09.17+git.9bf6adf. A shared build means shipping that symlink and
# its target, keeping the two in step, and getting the SONAME rather than the real file
# into the bundle, which is a class of mistake with no symptom until the bundle is moved
# to another machine. None of that buys anything back: nothing else on the system links
# aasdk, and the plugin is GPL-3.0-or-later either way. Written up in docs/packaging.md.
#
# Static libraries are linked into a shared object, so they need position independent
# code. Set on the directory rather than on the targets, because they belong to aasdk.
set(AASDK_LIBRARY_TYPE "STATIC" CACHE STRING "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory("${AASDK_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/aasdk" EXCLUDE_FROM_ALL)

target_link_libraries(${PLUGIN_NAME} PRIVATE aasdk aap_protobuf)

# The port adds one fault knob of its own, AA_FAULT_TRANSFER_AFTER in USBEndpoint.cpp,
# so aasdk needs the same define as the plugin or that one ships while the other three
# do not. Set after add_subdirectory, where the target exists.
if(AA_ENABLE_FAULT_INJECTION)
  target_compile_definitions(aasdk PRIVATE AA_FAULT_INJECTION)
endif()

# List of absolute paths to libraries that should be bundled with the plugin.
#
# Empty, and that is the point of linking aasdk statically above: everything the plugin
# needs beyond the system libraries is inside libandroid_auto_linux_plugin.so. Flutter
# still requires the variable to exist.
#
# It was not always empty, and the way it failed is worth keeping. aasdk was shared, so
# the plugin's DT_NEEDED held its SONAME, libaasdk.so.2026, while the real file carried
# the full date and git hash. Bundling TARGET_FILE put a file in the bundle that the
# loader never looks for; bundling TARGET_SONAME_FILE put a symlink there pointing at a
# file that was not. Neither showed a symptom on the machine that built it, because the
# plugin's RUNPATH still reached into the build tree. The failure only appeared once the
# bundle was copied somewhere else, which is to say, once it was shipped.
set(android_auto_linux_bundled_libraries
  ""
  PARENT_SCOPE
)
