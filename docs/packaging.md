# Packaging

What a built application actually contains, what a machine needs installed before it
will run, and why the native library is linked the way it is.

For building the repository itself see the README. This file is about the other end:
shipping the result to a machine that is not the one it was built on.

## What a release build produces

```
build/linux/<arch>/release/bundle/
  android_auto_example                 the executable
  data/                                Flutter assets and the ICU data
  lib/
    libandroid_auto_linux_plugin.so    the head unit, aasdk inside it
    libapp.so                          the app's compiled Dart
    libflutter_linux_gtk.so            the Flutter engine
```

Three libraries, and the plugin is one of them. Nothing else belongs in `lib/`. If a
`libaasdk.so.*` ever appears there again, something has gone wrong; see
[Static, not shared](#static-not-shared) below.

The bundle is relocatable: copy the directory anywhere, on any machine with the runtime
dependencies below, and it runs. That is worth testing rather than assuming, because
the failure is invisible on the machine that did the build:

```bash
cp -a build/linux/x64/release/bundle /tmp/relocation-test
ldd /tmp/relocation-test/lib/libandroid_auto_linux_plugin.so | grep 'not found'
```

Silence is a pass. A dangling entry means something is being resolved out of the build
tree through an absolute `RUNPATH`, which will keep working until the moment it ships.

## Static, not shared

**aasdk and its generated protobuf are linked into the plugin statically.** They are the
only two libraries this is true of; everything else comes from the system.

The decision is about aasdk's versioning rather than about linking in the abstract.
aasdk names its releases after the day they were built, so a shared build produces
`libaasdk.so.2026.09.17+git.9bf6adf` with a `libaasdk.so.2026` symlink beside it, and
`libaasdk.so.2026` is what lands in the plugin's `DT_NEEDED`. Shipping that means
shipping both names and keeping them in step, through a Flutter bundling step that
copies files and has no concept of a versioned symlink. Both of the obvious ways to
write it are wrong:

- bundling the real file puts a name in `lib/` that the loader never looks for, and
- bundling the SONAME puts a symlink there whose target is not in `lib/` at all.

Neither shows a symptom while the build tree is still on disk, because the plugin's
`RUNPATH` reaches back into it. The first time either is noticed is on the target
machine.

Against that, a shared aasdk buys nothing. Nothing else on a head unit links it, so
there is no copy to share, and the licence position is the same either way: the plugin
is GPL-3.0-or-later whether aasdk is inside it or beside it.

So `AASDK_LIBRARY_TYPE` is set to `STATIC` in the plugin's CMake, aasdk's own
CMakeLists takes the library type from the parent project (part of the port, see
[`aasdk-port-notes.md`](aasdk-port-notes.md)), and
`android_auto_linux_bundled_libraries` is empty.

Two things follow from static linking that had to be handled:

- **Position independent code.** A static archive linked into a shared object needs it.
  `CMAKE_POSITION_INDEPENDENT_CODE` is set on the directory before `add_subdirectory`.
- **Symbol visibility.** `CXX_VISIBILITY_PRESET hidden` only reaches the plugin's own
  objects. The archives were compiled by aasdk's target with aasdk's flags, so their
  symbols arrived with default visibility and the plugin exported 6159 of them where 65
  are the interface. A Flutter application loads every plugin into one address space and
  the first definition of a symbol wins for all of them, so a second plugin carrying its
  own protobuf or Boost would have bound to this one's. `android_auto_linux_plugin.map`
  is a linker version script that exports the `aa_*` C ABI and the GTK registrar entry
  point and nothing else.

## Runtime dependencies

These are packages, not headers: what a machine needs to *run* a built bundle. Names are
Ubuntu 26.04's. The version numbers in several of them are part of the package name and
will differ on another release, which is the point of the warning at the end.

```bash
sudo apt-get install \
  libavcodec62 libavutil60 libswscale9 \
  libpulse0 libusb-1.0-0 libssl3t64 libprotobuf32t64 \
  libboost-log1.90.0 libboost-thread1.90.0 libboost-filesystem1.90.0 \
  libboost-regex1.90.0 libboost-serialization1.90.0 libboost-date-time1.90.0 \
  libboost-chrono1.90.0 libboost-atomic1.90.0 libboost-container1.90.0
```

| Package | Needed for |
|---|---|
| `libavcodec`, `libavutil`, `libswscale` | H.264 decode, the VA-API device and the DRM prime export, and RGBA conversion on the software fallback |
| `libpulse0` | audio out and the microphone. PipeWire answering to PulseAudio's API counts |
| `libusb-1.0-0` | the USB transport and the AOAP handshake |
| `libssl3t64` | the projection link's TLS |
| `libprotobuf32t64` | the protocol messages |
| `libboost-*` | aasdk's asio, logging and threading |

Beyond those, a Flutter Linux application already needs GTK 3, GLib, cairo, pango,
harfbuzz, gdk-pixbuf, libepoxy and a C++ runtime, and those are not listed here because
nothing about this plugin adds them.

Two more that are not library dependencies and so do not appear in `ldd`:

- **A VA-API driver**, for hardware video decode: `libva-drm2` plus the driver for the
  GPU (`intel-media-va-driver`, `mesa-va-drivers`). Without one the decoder falls back
  to software, which works and costs CPU. `AA_VIDEO_DECODER=software` forces the
  fallback, which is how to tell a driver problem from a decoder problem.
- **BlueZ**, for wireless Android Auto only. The plugin talks to it over D-Bus; there is
  no library dependency and no build time dependency. A USB-only head unit needs none of
  it.

**The Boost and protobuf versions are pinned by the build, not chosen at runtime.** The
plugin records `libprotobuf.so.32` and `libboost_log.so.1.90.0` in its `DT_NEEDED`, so a
bundle built on Ubuntu 26.04 will not start on 24.04. Build on the distribution being
shipped to. Linking those statically as well would lift the restriction and is the
obvious next move if a single portable bundle is ever wanted; it is not done today
because it trades a ten minute protobuf build back into every clean build, and because
static Boost.Log needs its own set of defines to behave.

## Build caching

aasdk and its generated protobuf are 346 translation units and the overwhelming
majority of the build. `flutter clean` deletes the object tree, so without help every
clean rebuild pays for all of them again.

The plugin's CMake routes compiles through **ccache** when it is installed, for itself
and for the aasdk subproject, and leaves the host application's own build alone. ccache
keys on preprocessed source rather than timestamps and its cache lives in the user's
home directory, so it survives the clean that the build directory does not.

Measured on an 8 core x86_64 laptop, `flutter clean` followed by
`flutter build linux --debug`:

| | Wall clock |
|---|---|
| Cold cache, 346 compiles, 0 hits | 196 s |
| Warm cache, 346 compiles, 346 hits | 20 s |

Install it with `tools/setup-dev-machine.sh --build-deps`, or `apt install ccache`.
Nothing requires it: CMake says so at configure time and builds without it. Pass
`-DAA_USE_CCACHE=OFF` to turn it off when it is installed.

In CI the same mechanism works, with the cache directory restored from and saved to the
Actions cache; see `.github/workflows/ci.yml`.

## Size

A release bundle is about 27 MB, of which the plugin is 3.3 MB.

It was 26 MB until aasdk stopped forcing `-g` into release builds. aasdk overrides
`CMAKE_CXX_FLAGS_RELEASE` with `-g -O3 -DNDEBUG`, which put 23 MB of DWARF into a
shipped library for a configuration nobody asked for, CMake having `RelWithDebInfo` for
exactly that. Undoing it is part of the port.

The plugin is still not stripped, deliberately: 3.3 MB is small enough that the symbol
table is worth keeping, and a crash in a car is much easier to read with one.

## ARM64

The target is a mini PC, and the package layout, the build and the tooling are all
architecture agnostic: `dev/run-example.sh` resolves `x64` or `arm64` from `uname -m`,
and nothing in the native code assumes a word size or an endianness.

**It has not been run on ARM64 yet, but nothing structural is in the way.**

One thing about installing Flutter there is worth knowing, because it looks like a
blocker and is not. Flutter publishes prebuilt SDK archives for **x64 only**:
`releases_linux.json` has no arm64 entry, so anything that downloads from it, including
`subosito/flutter-action`, fails on an ARM64 machine with

```
Unable to determine Flutter version for channel: stable version: 3.47.4 architecture: arm64
```

That is a gap in the prebuilt archives, not in Flutter. **Installing from a git clone
works**: the engine artifacts exist for `linux-arm64` in all three build modes, along
with the arm64 Dart SDK, and `flutter precache --linux` fetches them.

```bash
git clone --depth 1 --branch 3.47.4 https://github.com/flutter/flutter.git ~/flutter
export PATH="$HOME/flutter/bin:$PATH"
flutter config --enable-linux-desktop
flutter precache --linux
```

CI builds `aarch64` this way on every release. A build that links is still not a head
unit that projects, so what remains owed is the real check on the device: a phone, a
cable, and the video, audio, input and sensor paths exercised there. The VA-API path is
a different driver stack on ARM and is the most likely thing to need work; there is a
software fallback if it does.
