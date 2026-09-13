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

# Any new source files that you add to the plugin should be added here.
list(APPEND PLUGIN_SOURCES
  "android_auto_linux_plugin.cc"
  "src/aa_core.cc"
  "src/event_bus.cc"
  "src/frame_ring.cc"
  "src/test_pattern.cc"
  "src/present/gl_adapter.cc"
  "src/present/texture_registry.cc"
  "src/session/usb_context.cc"
  "src/session/usb_connector.cc"
  "src/session/service_discovery.cc"
  "src/session/input_channel.cc"
  "src/session/protocol_session.cc"
  "src/session/support_channels.cc"
  "src/session/video_channel.cc"
  "src/video/video_decoder.cc"
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

# drm_fourcc.h is a header of macros with no library behind it, and it ships with the
# kernel headers rather than with libdrm, so there is nothing to find_package and
# nothing extra for an end user to install.

# === aasdk ===
#
# Vendored as a submodule and patched, see docs/aasdk-port-notes.md. It does not build
# unpatched against Boost 1.87 or newer. tools/build-aasdk.sh applies the patch; this
# build fails with a clear message rather than a wall of compiler errors if it has not
# been run.
set(AASDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/aasdk")
if(NOT EXISTS "${AASDK_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "The aasdk submodule is missing. Run:\n"
    "  git submodule update --init --recursive")
endif()
if(NOT EXISTS "${AASDK_DIR}/include/aasdk/Common/Strand.hpp")
  message(FATAL_ERROR
    "aasdk is not patched and will not build against this Boost. Run:\n"
    "  tools/build-aasdk.sh")
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
add_subdirectory("${AASDK_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/aasdk" EXCLUDE_FROM_ALL)

target_link_libraries(${PLUGIN_NAME} PRIVATE aasdk aap_protobuf)

# List of absolute paths to libraries that should be bundled with the plugin.
# aasdk and its generated protobuf are shared libraries, so they have to travel with
# the app rather than being resolved from the system.
#
# TARGET_SONAME_FILE, not TARGET_FILE: the plugin's DT_NEEDED records the SONAME
# (libaasdk.so.2026), while TARGET_FILE is the fully versioned real file
# (libaasdk.so.2026.09.13+git...). Bundling the latter puts a file in the bundle that
# the loader will never look for, and the app only keeps working because its RPATH
# still points back into the build tree. Move the bundle elsewhere and it breaks.
set(android_auto_linux_bundled_libraries
  "$<TARGET_SONAME_FILE:aasdk>"
  "$<TARGET_SONAME_FILE:aap_protobuf>"
  PARENT_SCOPE
)
