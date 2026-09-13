#!/usr/bin/env bash
# Build the vendored aasdk and run the milestone M1 smoke test.
#
#   tools/build-aasdk.sh           configure, build, run the smoke test
#   tools/build-aasdk.sh --clean   throw the build directory away first
#
# Takes care of the submodule and the Boost port patch, so a fresh clone only needs
# tools/setup-dev-machine.sh --build-deps before this works.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AASDK="$REPO/packages/android_auto_linux/linux/third_party/aasdk"
PATCH="$REPO/packages/android_auto_linux/linux/patches/0001-port-to-boost-asio-io_context.patch"
BUILD="$REPO/build/aasdk-smoke"

[ "${1:-}" = "--clean" ] && rm -rf "$BUILD"

if [ ! -f "$AASDK/CMakeLists.txt" ]; then
  echo "==> fetching the aasdk submodule"
  git -C "$REPO" submodule update --init --recursive
fi

# aasdk does not build against Boost 1.87 or newer unedited. The patch is idempotent in
# practice because a clean tree is the only state it applies to, so check first.
if ! grep -q 'aasdk/Common/Strand.hpp' "$AASDK/include/aasdk/Messenger/Messenger.hpp" 2>/dev/null; then
  echo "==> applying the io_context port"
  git -C "$AASDK" apply "$PATCH"
else
  echo "==> io_context port already applied"
fi

echo "==> configuring"
# Resolve protoc here rather than leaving it to aasdk's protobuf subproject, which uses
# include(FindProtobuf) instead of find_package and loses it on a reconfigure, failing
# with "protoc executable not found" even though it is installed. The plugin's own
# CMakeLists already pins it the same way for the same reason.
PROTOC="$(command -v protoc || true)"
if [ -z "$PROTOC" ]; then
  echo "protoc is not installed, run: tools/setup-dev-machine.sh --build-deps" >&2
  exit 1
fi
cmake -S "$REPO/packages/android_auto_linux/linux/smoke" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DProtobuf_PROTOC_EXECUTABLE="$PROTOC" \
  -DPROTOBUF_PROTOC_EXECUTABLE="$PROTOC" >/dev/null

echo "==> building"
cmake --build "$BUILD" 2>&1 | grep -vE "^\[[0-9]+/[0-9]+\]" | grep -iE "error|warning: unused" || true
cmake --build "$BUILD" >/dev/null

echo "==> smoke test"
"$BUILD/aasdk_smoke"
