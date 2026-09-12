# Standalone build of the vendored aasdk plus a smoke test.
#
# Kept separate from the Flutter plugin's CMakeLists so milestone M1 stays testable on
# its own: if this builds and the smoke binary prints OK, the Boost port is intact.
#
#   cmake -S packages/android_auto_linux/linux/smoke -B build/aasdk-smoke -G Ninja
#   cmake --build build/aasdk-smoke && ./build/aasdk-smoke/aasdk_smoke
#
# tools/build-aasdk.sh wraps all of that.

cmake_minimum_required(VERSION 3.16)
project(aasdk_smoke CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Use the system protobuf and Abseil rather than letting aasdk build its own copy of
# protobuf 30. Verified working against Ubuntu 26.04's protobuf 3.21.
set(SKIP_BUILD_PROTOBUF ON  CACHE BOOL "" FORCE)
set(SKIP_BUILD_ABSL     ON  CACHE BOOL "" FORCE)
set(AASDK_TEST          OFF CACHE BOOL "" FORCE)
set(AASDK_BENCHMARK     OFF CACHE BOOL "" FORCE)

add_subdirectory(../third_party/aasdk ${CMAKE_BINARY_DIR}/aasdk)

add_executable(aasdk_smoke aasdk_smoke.cpp)
target_link_libraries(aasdk_smoke PRIVATE aasdk aap_protobuf)
