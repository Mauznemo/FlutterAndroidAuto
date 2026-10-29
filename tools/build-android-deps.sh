#!/usr/bin/env bash
# Cross compile what aasdk links against, for Android.
#
#   tools/build-android-deps.sh                 every ABI in ABIS below
#   tools/build-android-deps.sh arm64-v8a       just the one
#   tools/build-android-deps.sh --clean ...     throw the build tree away first
#
# On Linux these come from the distribution. Android has no such thing, so they are
# built here from pinned upstream releases, statically, into
# packages/android_auto_linux/android/deps/<abi>/, which the plugin's Android CMake
# reads. The result is a few hundred megabytes of objects and is never committed.
#
# Needs the Android NDK (ANDROID_NDK_HOME, or the version below under ANDROID_HOME),
# cmake, ninja, make, perl (for OpenSSL's Configure) and a host protoc 3.21. The host
# protoc has to match the protobuf built here, because generated code only links against
# the runtime of the release that generated it.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$REPO/build/android-deps"
PREFIX_ROOT="$REPO/packages/android_auto_linux/android/deps"

# Matches Flutter's own default, so a host app and this plugin agree on one NDK.
NDK_VERSION="28.2.13676358"
# The oldest these libraries need. The plugin itself asks for 28, see its build.gradle.kts,
# and archives built for an older API link into a newer one unchanged.
API=26
ABIS=(arm64-v8a x86_64)

BOOST_VERSION=1.90.0
PROTOBUF_VERSION=3.21.12
OPENSSL_VERSION=3.5.9
LIBUSB_VERSION=1.0.29

if [ "${1:-}" = "--clean" ]; then
  rm -rf "$WORK"
  shift
fi
if [ $# -gt 0 ]; then
  ABIS=("$@")
fi

ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
NDK="${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/$NDK_VERSION}"
if [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
  echo "No NDK at $NDK. Install it with:" >&2
  echo "  sdkmanager \"ndk;$NDK_VERSION\"" >&2
  exit 1
fi
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
LLVM="$NDK/toolchains/llvm/prebuilt/linux-x86_64"

PROTOC="$(command -v protoc || true)"
if [ -z "$PROTOC" ] || ! "$PROTOC" --version | grep -q "${PROTOBUF_VERSION#3.}"; then
  echo "Need a host protoc $PROTOBUF_VERSION, found: ${PROTOC:-none}" >&2
  exit 1
fi

JOBS="$(nproc)"
mkdir -p "$WORK/downloads" "$WORK/src"

# Each archive is unpacked into a directory of its own and built from outside it.
fetch() {
  local name="$1" url="$2" file="$WORK/downloads/${2##*/}"
  if [ ! -f "$file" ]; then
    echo "==> downloading $name"
    curl -fsSL -o "$file.part" "$url"
    mv "$file.part" "$file"
  fi
  if [ ! -d "$WORK/src/$name" ]; then
    mkdir -p "$WORK/src/$name"
    tar -xf "$file" -C "$WORK/src/$name" --strip-components=1
  fi
}

fetch boost "https://github.com/boostorg/boost/releases/download/boost-$BOOST_VERSION/boost-$BOOST_VERSION-cmake.tar.xz"
fetch protobuf "https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOBUF_VERSION#3.}/protobuf-cpp-$PROTOBUF_VERSION.tar.gz"
fetch openssl "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz"
fetch libusb "https://github.com/libusb/libusb/releases/download/v$LIBUSB_VERSION/libusb-$LIBUSB_VERSION.tar.bz2"

# libusb ships an ndk-build makefile and no CMake, so it gets a small one of its own.
# The source list is the Linux backend's, which is what Android runs.
mkdir -p "$WORK/libusb-cmake"
cat > "$WORK/libusb-cmake/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.16)
project(libusb C)
set(SRC "$WORK/src/libusb/libusb")
add_library(usb-1.0 STATIC
  \${SRC}/core.c \${SRC}/descriptor.c \${SRC}/hotplug.c \${SRC}/io.c \${SRC}/strerror.c
  \${SRC}/sync.c \${SRC}/os/linux_usbfs.c \${SRC}/os/linux_netlink.c
  \${SRC}/os/events_posix.c \${SRC}/os/threads_posix.c)
target_include_directories(usb-1.0 PRIVATE "$WORK/src/libusb/android" \${SRC} \${SRC}/os)
target_link_libraries(usb-1.0 PRIVATE log)
install(TARGETS usb-1.0 ARCHIVE DESTINATION lib)
install(FILES \${SRC}/libusb.h DESTINATION include/libusb-1.0)
EOF

cmake_android() {
  local abi="$1" src="$2" build="$3"
  shift 3
  cmake -S "$src" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DANDROID_ABI="$abi" \
    -DANDROID_PLATFORM="android-$API" \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_FIND_ROOT_PATH="$PREFIX" \
    "$@" >"$build.log" 2>&1 || { tail -40 "$build.log"; exit 1; }
  cmake --build "$build" -j "$JOBS" >>"$build.log" 2>&1 || { tail -40 "$build.log"; exit 1; }
  cmake --install "$build" >>"$build.log" 2>&1
}

openssl_target() {
  case "$1" in
    arm64-v8a) echo android-arm64 ;;
    armeabi-v7a) echo android-arm ;;
    x86_64) echo android-x86_64 ;;
    x86) echo android-x86 ;;
    *) echo "unknown ABI $1" >&2; exit 1 ;;
  esac
}

for ABI in "${ABIS[@]}"; do
  PREFIX="$PREFIX_ROOT/$ABI"
  BUILD="$WORK/build/$ABI"
  mkdir -p "$PREFIX" "$BUILD"
  echo "== $ABI =="

  if [ ! -f "$PREFIX/lib/libusb-1.0.a" ]; then
    echo "==> libusb $LIBUSB_VERSION"
    cmake_android "$ABI" "$WORK/libusb-cmake" "$BUILD/libusb"
  fi

  if [ ! -f "$PREFIX/lib/libprotobuf.a" ]; then
    echo "==> protobuf $PROTOBUF_VERSION"
    cmake_android "$ABI" "$WORK/src/protobuf" "$BUILD/protobuf" \
      -Dprotobuf_BUILD_TESTS=OFF \
      -Dprotobuf_BUILD_PROTOC_BINARIES=OFF \
      -Dprotobuf_BUILD_SHARED_LIBS=OFF \
      -Dprotobuf_WITH_ZLIB=OFF
  fi

  # aasdk still calls ENGINE_cleanup, so the deprecated ENGINE API stays in.
  if [ ! -f "$PREFIX/lib/libssl.a" ]; then
    echo "==> openssl $OPENSSL_VERSION"
    # OpenSSL builds in its source tree, so each ABI gets a copy of its own.
    rm -rf "$BUILD/openssl"
    cp -r "$WORK/src/openssl" "$BUILD/openssl"
    (
      cd "$BUILD/openssl"
      export ANDROID_NDK_ROOT="$NDK"
      export PATH="$LLVM/bin:$PATH"
      ./Configure "$(openssl_target "$ABI")" -D__ANDROID_API__="$API" \
        no-shared no-tests no-apps no-docs no-module \
        --prefix="$PREFIX" --libdir=lib >"$BUILD/openssl.log" 2>&1
      make -j "$JOBS" build_libs >>"$BUILD/openssl.log" 2>&1 ||
        { tail -40 "$BUILD/openssl.log"; exit 1; }
      make install_dev >>"$BUILD/openssl.log" 2>&1
    )
  fi

  if [ ! -f "$PREFIX/lib/libboost_log.a" ]; then
    echo "==> boost $BOOST_VERSION"
    # Static, like everything here. aasdk asks for BOOST_ALL_DYN_LINK, which would put
    # it in Boost.Log's shared library namespace; the Android CMake takes that define
    # back off aasdk rather than this build putting it on, because Boost refuses to be
    # built static with it.
    cmake_android "$ABI" "$WORK/src/boost" "$BUILD/boost" \
      -DBUILD_SHARED_LIBS=OFF \
      -DBOOST_INCLUDE_LIBRARIES="log;asio;circular_buffer;endian;algorithm;core" \
      -DBOOST_ENABLE_PYTHON=OFF \
      -DBOOST_ENABLE_MPI=OFF
  fi
  echo "==> $ABI done: $PREFIX"
done
