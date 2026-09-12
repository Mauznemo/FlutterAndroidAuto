#!/usr/bin/env bash
# Port the vendored aasdk from boost::asio::io_service to io_context.
#
# io_service was deprecated in Boost 1.66 and removed in Boost 1.87. Ubuntu 26.04 ships
# Boost 1.90, so aasdk does not build without this.
#
# The port is deliberately tiny. Rather than rewriting 95 files' worth of construction,
# dispatch and accessor calls, it introduces aasdk::Strand, a thin subclass of
# boost::asio::strand<io_context::executor_type> that keeps the call sites aasdk already
# has (construct from an io_context, one argument dispatch, get_io_service). After that
# only three textual substitutions are needed.
#
# It also raises a couple of cmake_minimum_required() calls, because CMake 4 dropped
# compatibility with anything below 3.5, stops asking CMake for the Boost.System
# component that Boost 1.90 no longer ships, and puts aasdk's own include directory on
# the aasdk target so a parent project can actually use it.
#
#   tools/port-aasdk.sh apply     transform the submodule working tree in place
#   tools/port-aasdk.sh patch     regenerate linux/patches/ from the working tree
#   tools/port-aasdk.sh reset     throw the working tree away, back to the pinned commit
#
# The patch under linux/patches/ is the committed source of truth. The submodule itself
# is never committed dirty.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AASDK="$REPO/packages/android_auto_linux/linux/third_party/aasdk"
PATCHES="$REPO/packages/android_auto_linux/linux/patches"

[ -d "$AASDK/src" ] || {
  echo "aasdk submodule is not checked out. Run: git submodule update --init --recursive" >&2
  exit 1
}

write_strand_header() {
  mkdir -p "$AASDK/include/aasdk/Common"
  cat > "$AASDK/include/aasdk/Common/Strand.hpp" <<'EOF'
// This file is part of aasdk library project.
// Copyright (C) 2018 f1x.studio (Michal Szwaj)
//
// aasdk is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// aasdk is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with aasdk. If not, see <http://www.gnu.org/licenses/>.

#pragma once

#include <boost/asio.hpp>
#include <utility>

namespace aasdk {

// boost::asio::io_service::strand was removed in Boost 1.87. Its replacement,
// boost::asio::strand<Executor>, models the Executor concept instead: it is built from
// an executor rather than an io_context, its dispatch() and post() take an allocator,
// its context() is typed execution_context&, and it has no get_io_service().
//
// aasdk calls strands in the old style in roughly a hundred places. This subclass keeps
// that call style working so the port stays a mechanical type substitution instead of a
// rewrite of every handler dispatch in the library.
class Strand : public boost::asio::strand<boost::asio::io_context::executor_type> {
 public:
  using Base = boost::asio::strand<boost::asio::io_context::executor_type>;

  // The old strand(io_service&) constructor.
  explicit Strand(boost::asio::io_context& ioContext) : Base(ioContext.get_executor()) {}

  // Keep the Executor based constructors usable too.
  explicit Strand(const Base& base) : Base(base) {}

  // The old one argument dispatch() and post(). boost::asio::strand<Executor> requires
  // an allocator on both, the free functions do not.
  template <typename Handler>
  void dispatch(Handler&& handler) {
    boost::asio::dispatch(static_cast<Base&>(*this), std::forward<Handler>(handler));
  }

  template <typename Handler>
  void post(Handler&& handler) {
    boost::asio::post(static_cast<Base&>(*this), std::forward<Handler>(handler));
  }

  // aasdk reaches the owning io_context two ways, guarded on BOOST_VERSION: the old
  // get_io_service() below 1.66, and context() above it. On boost::asio::strand<Executor>
  // context() exists but is typed execution_context&, which does not bind to the
  // io_context& parameters aasdk passes it to. Both accessors are narrowed here.
  //
  // The downcast is sound: every Strand is built from an io_context by the constructor
  // above, so the execution_context really is one.
  boost::asio::io_context& context() {
    return static_cast<boost::asio::io_context&>(Base::context());
  }

  boost::asio::io_context& get_io_service() { return context(); }
};

}  // namespace aasdk
EOF
}

# Boost.System has been header only since Boost 1.69, and Boost 1.90 finally dropped the
# compiled library and its CMake component, so asking for it by name is a hard error now.
# Boost::headers covers what aasdk actually used it for.
port_boost_components() {
  sed -i 's/find_package(Boost REQUIRED COMPONENTS system log_setup log/find_package(Boost REQUIRED COMPONENTS log_setup log/' \
    "$AASDK/CMakeLists.txt"
}

# io_context dropped the post() and dispatch() members that io_service had. The strand
# side is handled by aasdk::Strand, but IOContextWrapper calls them on the io_context
# directly, so those two call sites move to the free functions.
port_io_context_wrapper() {
  local f="$AASDK/include/aasdk/IO/IOContextWrapper.hpp"
  [ -f "$f" ] || return 0
  sed -i \
    -e 's/ioService_->post(std::move(handler));/boost::asio::post(*ioService_, std::move(handler));/' \
    -e 's/ioService_->dispatch(std::move(handler));/boost::asio::dispatch(*ioService_, std::move(handler));/' \
    "$f"
}

# aasdk exposes its own headers through a directory scoped include_directories(), which
# does not reach a parent project's targets. Anyone adding aasdk with add_subdirectory,
# which is exactly how the Flutter plugin consumes it, cannot find aasdk/... headers.
# Put the include directory on the target where it belongs.
port_target_includes() {
  sed -i 's|target_include_directories(aasdk PUBLIC|target_include_directories(aasdk PUBLIC\n        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>\n        $<INSTALL_INTERFACE:include>|g' \
    "$AASDK/CMakeLists.txt"
}

# CMake 4 removed compatibility with cmake_minimum_required(VERSION < 3.5).
port_cmake_minimum() {
  local f
  for f in "$AASDK"/cmake_modules/gitversion.cmake "$AASDK"/cmake_modules_old/gitversion.cmake \
           "$AASDK"/tools/simple/CMakeLists.txt; do
    [ -f "$f" ] || continue
    sed -i -E 's/cmake_minimum_required\(VERSION 3\.(0|0\.0|1|2|3|4|10)\)/cmake_minimum_required(VERSION 3.16)/' "$f"
  done
}

cmd_apply() {
  write_strand_header
  port_cmake_minimum
  port_boost_components
  port_target_includes
  port_io_context_wrapper

  local files
  files=$(grep -rl --include='*.hpp' --include='*.cpp' \
            -e 'io_service' -e 'address::from_string' "$AASDK/include" "$AASDK/src" || true)
  [ -n "$files" ] || { echo "nothing to port, already applied?"; return 0; }

  # Order matters. io_service::strand has to go before the bare io_service rename,
  # otherwise it turns into io_context::strand, which does not exist.
  echo "$files" | xargs sed -i \
    -e 's/boost::asio::io_service::strand/aasdk::Strand/g' \
    -e 's/boost::asio::io_service/boost::asio::io_context/g' \
    -e 's/boost::asio::ip::address::from_string/boost::asio::ip::make_address/g'

  # Every file that now names aasdk::Strand needs the header. Most aasdk files pick up
  # boost/asio.hpp transitively, so do not rely on that here: add the include explicitly
  # after the last existing include. A duplicate include is harmless.
  local f
  for f in $(grep -rl --include='*.hpp' --include='*.cpp' 'aasdk::Strand' \
               "$AASDK/include" "$AASDK/src" || true); do
    grep -q 'aasdk/Common/Strand.hpp' "$f" && continue
    [ "$f" = "$AASDK/include/aasdk/Common/Strand.hpp" ] && continue
    awk '
      /^#include/ { last = NR }
      { lines[NR] = $0 }
      END {
        for (i = 1; i <= NR; i++) {
          print lines[i]
          if (i == last) print "#include <aasdk/Common/Strand.hpp>"
        }
      }' "$f" > "$f.ported" && mv "$f.ported" "$f"
  done

  echo "ported $(echo "$files" | wc -l) files"
  echo "remaining io_service references: $(grep -rc 'io_service' "$AASDK/include" "$AASDK/src" 2>/dev/null | grep -v ':0$' | wc -l) files"
}

cmd_patch() {
  mkdir -p "$PATCHES"
  git -C "$AASDK" add -A
  git -C "$AASDK" diff --cached --binary > "$PATCHES/0001-port-to-boost-asio-io_context.patch"
  git -C "$AASDK" reset -q
  echo "wrote $PATCHES/0001-port-to-boost-asio-io_context.patch"
  wc -l "$PATCHES/0001-port-to-boost-asio-io_context.patch"
}

cmd_reset() {
  git -C "$AASDK" checkout -- .
  git -C "$AASDK" clean -fdq
  echo "aasdk working tree reset to the pinned commit"
}

case "${1:-}" in
  apply) cmd_apply ;;
  patch) cmd_patch ;;
  reset) cmd_reset ;;
  *) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
