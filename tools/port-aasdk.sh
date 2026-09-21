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
# component that Boost 1.90 no longer ships, puts aasdk's own include directory on the
# aasdk target so a parent project can actually use it, adds the includes
# IInputSourceServiceEventHandler.hpp forgot, and fixes two crashes: a use after free in
# AOAPDevice, unguarded promise dereferences in the message streams, and bulk endpoints
# left halted by a previous session.
#
# It also lets the parent project choose the library type, because a shared aasdk names
# itself after the day it was built and cannot be redistributed without carrying that
# symlink, and stops Boost.Test from reaching every consumer's DT_NEEDED.
#
# Note on reading aasdk's USB_TRANSFER errors: "Native Code" is a libusb_transfer_status,
# so 2 is TIMED_OUT and 4 is STALL. It is not a libusb_error.
#
#   tools/port-aasdk.sh apply     redo the port in the working tree, from the patch
#   tools/port-aasdk.sh regen     redo it from the transforms below, for a new aasdk pin
#   tools/port-aasdk.sh patch     regenerate linux/patches/ from the working tree
#   tools/port-aasdk.sh reset     throw the working tree away, back to the pinned commit
#
# The patch under linux/patches/ is the committed source of truth, and the submodule is
# never committed dirty.
#
# `apply` and `regen` are not the same thing, and the difference matters. Parts of the
# port are hand written rather than scripted: the USBEndpoint transfer retry, the halt
# clearing and the fault injection knob are in the patch and in no function below. So
# `regen` produces less than the patch holds, and `patch` run straight after it would
# throw the hand written work away. Use `regen` only when the submodule has been moved
# to a commit the patch no longer applies to, and expect to put the rest back by hand.

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

# AOAPDevice keeps a pointer into a libusb config descriptor that it does not own. The
# descriptor is a local in create(), so it is freed the moment create() returns, and
# ~AOAPDevice then reads bInterfaceNumber out of freed memory to release the interface.
#
# It usually survives the first time, because the freed block still holds its old
# contents. Connect and disconnect a couple of times and the allocator hands that memory
# to someone else, and the app dies in ~AOAPDevice. Give the device ownership of the
# descriptor instead.
port_aoap_device_lifetime() {
  python3 - "$AASDK" <<'PYEOF'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])

header = root / "include/aasdk/USB/AOAPDevice.hpp"
text = header.read_text()
if "configDescriptorHandle_" not in text:
    text = text.replace(
        """      AOAPDevice(IUSBWrapper &usbWrapper, boost::asio::io_context &ioService, DeviceHandle handle,
                 const libusb_interface_descriptor *interfaceDescriptor);""",
        """      AOAPDevice(IUSBWrapper &usbWrapper, boost::asio::io_context &ioService, DeviceHandle handle,
                 ConfigDescriptorHandle configDescriptorHandle,
                 const libusb_interface_descriptor *interfaceDescriptor);""")
    text = text.replace(
        """      DeviceHandle handle_;
      const libusb_interface_descriptor *interfaceDescriptor_;""",
        """      DeviceHandle handle_;
      // Owns the memory interfaceDescriptor_ points into. Do not remove: without it the
      // descriptor is freed when create() returns, and the destructor reads it.
      ConfigDescriptorHandle configDescriptorHandle_;
      const libusb_interface_descriptor *interfaceDescriptor_;""")
    header.write_text(text)

source = root / "src/USB/AOAPDevice.cpp"
text = source.read_text()
if "configDescriptorHandle_" not in text:
    text = text.replace(
        """    AOAPDevice::AOAPDevice(IUSBWrapper &usbWrapper, boost::asio::io_context &ioService, DeviceHandle handle,
                           const libusb_interface_descriptor *interfaceDescriptor)
        : usbWrapper_(usbWrapper), handle_(std::move(handle)), interfaceDescriptor_(interfaceDescriptor) {""",
        """    AOAPDevice::AOAPDevice(IUSBWrapper &usbWrapper, boost::asio::io_context &ioService, DeviceHandle handle,
                           ConfigDescriptorHandle configDescriptorHandle,
                           const libusb_interface_descriptor *interfaceDescriptor)
        : usbWrapper_(usbWrapper), handle_(std::move(handle)),
          configDescriptorHandle_(std::move(configDescriptorHandle)),
          interfaceDescriptor_(interfaceDescriptor) {""")
    text = text.replace(
        """      return std::make_unique<AOAPDevice>(usbWrapper, ioService, std::move(handle), interfaceDescriptor);""",
        """      return std::make_unique<AOAPDevice>(usbWrapper, ioService, std::move(handle),
                                          std::move(configDescriptorHandle), interfaceDescriptor);""")
    source.write_text(text)
PYEOF
}

# A USB bulk endpoint that was in use when a session ended can be left halted, and every
# transfer on it then fails. aasdk claims the interface and starts writing without
# clearing that state.
#
# This is defensive hygiene rather than a fix for a symptom we saw: the reconnect
# failures turned out to be LIBUSB_TRANSFER_TIMED_OUT, not STALL. Clearing the halt
# after claiming is standard practice and costs nothing, so it stays.
port_clear_endpoint_halt() {
  python3 - "$AASDK" <<'PYEOF'
import pathlib
import sys

source = pathlib.Path(sys.argv[1]) / "src/USB/AOAPDevice.cpp"
text = source.read_text()
if "libusb_clear_halt" in text:
    sys.exit(0)

needle = """      if (result != 0) {
        throw error::Error(error::ErrorCode::USB_CLAIM_INTERFACE, result);
      }
"""
replacement = """      if (result != 0) {
        throw error::Error(error::ErrorCode::USB_CLAIM_INTERFACE, result);
      }

      // A previous session can leave the bulk endpoints halted, and every transfer on a
      // halted endpoint fails with LIBUSB_TRANSFER_STALL. Clearing the halt here is what
      // makes reconnecting to a phone that was already projecting work.
      for (int i = 0; i < interfaceDescriptor->bNumEndpoints; ++i) {
        libusb_clear_halt(handle.get(), interfaceDescriptor->endpoint[i].bEndpointAddress);
      }
"""
if needle not in text:
    raise SystemExit("AOAPDevice::create claim block not found, patch needs updating")
source.write_text(text.replace(needle, replacement))
PYEOF
}

# The message streams dereference promise_ without ever checking it for null. There are
# ten such sites across MessageInStream and MessageOutStream and not one is guarded.
#
# Most of the time the promise is there. During teardown it is not: a transport that is
# being stopped rejects its pending receive, the rejection handler runs, and promise_ has
# already been reset by another path. The result is a null dereference inside
# Promise::reject, which is a segfault in an io_context thread.
#
# Guarding the dereference is the whole fix. Resetting an already null shared_ptr is a
# no op, so the lines that follow need no change.
port_promise_null_guards() {
  python3 - "$AASDK" <<'PYEOF'
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
pattern = re.compile(r"^(\s*)(promise_->[^\n]*;)\s*$")

for name in ("src/Messenger/MessageInStream.cpp", "src/Messenger/MessageOutStream.cpp"):
    path = root / name
    if not path.exists():
        continue
    out = []
    changed = False
    for line in path.read_text().splitlines():
        match = pattern.match(line)
        if match and "if (promise_" not in line:
            indent, statement = match.groups()
            out.append(f"{indent}if (promise_ != nullptr) {{")
            out.append(f"{indent}  {statement}")
            out.append(f"{indent}}}")
            changed = True
        else:
            out.append(line)
    if changed:
        path.write_text("\n".join(out) + "\n")
PYEOF
}

# aasdk exposes its own headers through a directory scoped include_directories(), which
# does not reach a parent project's targets. Anyone adding aasdk with add_subdirectory,
# which is exactly how the Flutter plugin consumes it, cannot find aasdk/... headers.
# Put the include directory on the target where it belongs.
port_target_includes() {
  sed -i 's|target_include_directories(aasdk PUBLIC|target_include_directories(aasdk PUBLIC\n        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>\n        $<INSTALL_INTERFACE:include>|g' \
    "$AASDK/CMakeLists.txt"
}

# IInputSourceServiceEventHandler.hpp names error::Error and std::shared_ptr without
# including either. It only compiles upstream because every other aasdk header happens
# to be included first; a translation unit that reaches for the input channel on its own
# fails outright. Same shape of bug as the missing target include directory above.
port_input_source_includes() {
  local f="$AASDK/include/aasdk/Channel/InputSource/IInputSourceServiceEventHandler.hpp"
  [ -f "$f" ] || return 0
  grep -q 'aasdk/Error/Error.hpp' "$f" && return 0
  sed -i 's|#include <stdint.h>|#include <stdint.h>\n#include <memory>|' "$f"
  sed -i 's|#include <aap_protobuf/service/media/sink/message/KeyBindingRequest.pb.h>|#include <aap_protobuf/service/media/sink/message/KeyBindingRequest.pb.h>\n#include "aasdk/Error/Error.hpp"|' "$f"
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

# aasdk hardcodes a shared library on everything but macOS, and a shared aasdk is the
# reason a built bundle cannot be moved off the machine that built it. Its SONAME is
# date based (libaasdk.so.2026), so what the plugin records in DT_NEEDED is a symlink
# whose target is named after the day of the build, and Flutter's install step copies
# the symlink rather than the file behind it.
#
# Rather than teach the bundling step about versioned symlinks, let the library type be
# chosen by the parent project. The plugin asks for STATIC and ships one .so.
port_library_type() {
  python3 - "$AASDK" <<'PYEOF'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])

option = (
    "# The parent project decides. aasdk linked into another shared library wants\n"
    "# STATIC: the date based SONAME below makes a shared aasdk awkward to redistribute.\n"
    'set(AASDK_LIBRARY_TYPE "SHARED" CACHE STRING "aasdk library type: SHARED or STATIC")\n'
    "\n")

main = root / "CMakeLists.txt"
text = main.read_text()
if "AASDK_LIBRARY_TYPE" not in text:
    needle = (
        'if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")  # macOS\n'
        '    message(NOTICE "Configuring STATIC Library for MacOS")\n'
        "    add_library(aasdk STATIC\n"
        "            ${source_files}\n"
        "            ${include_files})\n"
        "else()\n"
        '    message(NOTICE "Configuring SHARED Library")\n'
        "    add_library(aasdk SHARED\n"
        "            ${source_files}\n"
        "            ${include_files})\n"
        "endif()")
    if needle not in text:
        raise SystemExit("aasdk add_library block not found, the port needs updating")
    replacement = option + needle.replace(
        'message(NOTICE "Configuring SHARED Library")',
        'message(NOTICE "Configuring ${AASDK_LIBRARY_TYPE} Library")').replace(
        "add_library(aasdk SHARED", "add_library(aasdk ${AASDK_LIBRARY_TYPE}")
    main.write_text(text.replace(needle, replacement, 1))

proto = root / "protobuf/CMakeLists.txt"
text = proto.read_text()
if "AASDK_LIBRARY_TYPE" not in text:
    needle = (
        'if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")  # macOS\n'
        '    message(NOTICE "Configuring STATIC Library for MacOS")\n'
        "    add_library(aap_protobuf STATIC ${PROTO_SRCS} ${PROTO_HDRS})\n"
        "else()\n"
        '    message(NOTICE "Configuring SHARED Library")\n'
        "    add_library(aap_protobuf SHARED ${PROTO_SRCS} ${PROTO_HDRS})\n"
        "endif()")
    if needle not in text:
        raise SystemExit("aap_protobuf add_library block not found, the port needs updating")
    replacement = (
        "# Inherited from the parent project when there is one, SHARED standalone.\n"
        'set(AASDK_LIBRARY_TYPE "SHARED" CACHE STRING "aasdk library type: SHARED or STATIC")\n'
        "\n") + needle.replace(
        'message(NOTICE "Configuring SHARED Library")',
        'message(NOTICE "Configuring ${AASDK_LIBRARY_TYPE} Library")').replace(
        "add_library(aap_protobuf SHARED", "add_library(aap_protobuf ${AASDK_LIBRARY_TYPE}")
    proto.write_text(text.replace(needle, replacement, 1))
PYEOF
}

# Boost.Test is asked for unconditionally and lands in Boost_LIBRARIES, which aasdk then
# puts in its PUBLIC link interface. So every consumer of aasdk carries a DT_NEEDED on
# libboost_unit_test_framework whether the tests were built or not. Ask for it only when
# they are.
port_boost_test_component() {
  python3 - "$AASDK" <<'PYEOF'
import pathlib
import sys

path = pathlib.Path(sys.argv[1]) / "CMakeLists.txt"
text = path.read_text()
needle = ("find_package(Boost REQUIRED COMPONENTS log_setup log "
          "OPTIONAL_COMPONENTS unit_test_framework)")
if needle not in text:
    sys.exit(0)
replacement = (
    "if(AASDK_TEST)\n"
    "    " + needle + "\n"
    "else()\n"
    "    # Boost.Test is a test only dependency, and asking for it here would put it in\n"
    "    # aasdk's PUBLIC link interface and so in every consumer's DT_NEEDED.\n"
    "    find_package(Boost REQUIRED COMPONENTS log_setup log)\n"
    "endif()")
path.write_text(text.replace(needle, replacement, 1))
PYEOF
}

# Redo the port in the working tree the way a fresh clone gets it: by applying the
# committed patch. This is what CMake does at configure time too.
# aasdk overrides CMAKE_CXX_FLAGS_RELEASE with "-g -O3 -DNDEBUG", so a release build
# carries full DWARF. Linked into the plugin that was 23 MB of the 26 MB the shipped
# .so weighed, for a configuration nothing asked for: CMake already has RelWithDebInfo
# for anyone who wants an optimised build with symbols.
port_release_debug_info() {
  sed -i 's/^set(CMAKE_CXX_FLAGS_RELEASE "-g -O3 -DNDEBUG")$/set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")/' \
    "$AASDK/CMakeLists.txt"
}

cmd_apply() {
  local patch="$PATCHES/0001-port-to-boost-asio-io_context.patch"
  [ -f "$patch" ] || {
    echo "$patch is missing. Use 'regen' to rebuild the port from the transforms." >&2
    exit 1
  }
  if ! git -C "$AASDK" apply --check "$patch" 2>/dev/null; then
    echo "The committed patch does not apply to the submodule as it stands." >&2
    echo "Reset it first (tools/port-aasdk.sh reset), or, if the pin has moved," >&2
    echo "rebuild the port with 'regen' and put the hand written parts back." >&2
    exit 1
  fi
  git -C "$AASDK" apply "$patch"
  echo "applied $patch"
}

# Rebuild the scripted half of the port from source. See the warning in the header:
# this does not reproduce the hand written USBEndpoint work.
cmd_regen() {
  write_strand_header
  port_cmake_minimum
  port_boost_components
  port_boost_test_component
  port_library_type
  port_release_debug_info
  port_target_includes
  port_input_source_includes
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

  # Has to run after the substitutions above: it matches signatures that mention
  # io_context, which only exist once io_service has been renamed.
  port_aoap_device_lifetime
  port_promise_null_guards
  port_clear_endpoint_halt

  echo "ported $(echo "$files" | wc -l) files"
  echo "remaining io_service references: $(grep -rc 'io_service' "$AASDK/include" "$AASDK/src" 2>/dev/null | grep -v ':0$' | wc -l) files"
  echo
  echo "This is the scripted half of the port only. The USBEndpoint transfer retry, the"
  echo "halt clearing and the AA_FAULT_TRANSFER_AFTER knob are hand written and are not"
  echo "reproduced here. Put them back before running 'patch', or they are lost."
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
  regen) cmd_regen ;;
  patch) cmd_patch ;;
  reset) cmd_reset ;;
  *) sed -n '2,41p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
