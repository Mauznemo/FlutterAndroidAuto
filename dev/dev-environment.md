# Dev environment

What was found on the development machine on 2026-09-12, and how the coding agent
drives it without a human at the keyboard.

None of this is a requirement of the plugin. It is one laptop, recorded so that the
measurements taken on it can be read in context and repeated elsewhere. See
[`README.md`](README.md) in this directory.

## Machine

| | |
|---|---|
| OS | Ubuntu 26.04.1 LTS (Resolute Raccoon) |
| Kernel | 7.0.0-31-generic, x86_64 |
| Desktop | KDE Plasma on **Wayland** (`XDG_SESSION_TYPE=wayland`, KWin) |
| Display | single output `eDP-1`, 1920x1080, **scale 1**, 144 Hz |
| GPU | Intel UHD Graphics (Comet Lake), Mesa 26.0.8, OpenGL 4.6 / GLES 3.2 |
| Audio | PipeWire 1477 with `pipewire-pulse`, so PulseAudio APIs work |
| Bluetooth | Intel AX201, `bluetooth.service` active |
| Keyboard layout | **German (QWERTZ)**, `pc105` |
| sudo | passwordless |

Toolchain already present: Flutter 3.47.4 stable (snap, at
`~/snap/flutter/common/flutter`), Dart 3.13.3, CMake 4.2.3, Ninja 1.13.2, GCC 15.2,
Clang 21.1.8, git 2.53. `flutter doctor` reports the Linux desktop toolchain green.

Not present yet, needed from M1: Boost, libusb, OpenSSL headers, protobuf, ffmpeg,
libva, libpulse. Install with `tools/setup-dev-machine.sh --build-deps`.

## Screenshots

Wayland means no `scrot`, `import`, `maim` or `xwd`. Screenshots come from KDE's
Spectacle in batch mode, which needs no portal dialog:

```bash
spectacle -b -n -f -o out.png     # full screen
spectacle -b -n -a -o out.png     # active window
spectacle -b -n -f -p -o out.png  # include the mouse pointer
```

Screenshot pixels map 1:1 to screen coordinates because the output scale is 1.

Wrapped as `dev/ui.sh shot` / `shotwin` / `crop`.

KWin also exposes `org.kde.KWin.ScreenShot2` over D-Bus (`CaptureWindow`,
`CaptureActiveWindow`, `CaptureArea`, `CaptureScreen`) if Spectacle ever gets in the way.

## Synthetic input

`ydotool` injects events through `/dev/uinput`. Two things had to be sorted out:

**1. Permissions.** `ydotool.service` ships enabled but was failing because the user
could not open `/dev/uinput`. Fixed with a udev rule plus group membership:

```
KERNEL=="uinput", GROUP="input", MODE="0660", OPTIONS+="static_node=uinput"
```

The group change needs a re-login, so `dev/ui.sh setup` starts `ydotoold` under sudo
instead and hands the socket to the user. Nothing else needs root.

**2. Absolute positioning.** `ydotool mousemove --absolute` does not land on the
requested pixel on this compositor. The working approach is to slam the pointer into the
top left with a huge relative move and then move relatively to the target. That is only
exact once pointer acceleration is off for the virtual device.

KWin exposes per device settings on D-Bus, so `dev/ui.sh setup` finds the
`ydotoold virtual device` under `/org/kde/KWin/InputDevice/eventN` and sets:

```
pointerAccelerationProfileFlat = true
pointerAcceleration            = 0.0
```

This only touches the virtual device. The real mouse and touchpad keep their settings.

Verified: after flattening, "home then move by (960, 540)" lands the cursor exactly on
(960, 540). Before flattening the same move overshot to roughly (1890, 1070).

**3. Keyboard layout.** `ydotool` sends raw evdev keycodes and the compositor maps them
through the **German** layout, so `ydotool type "ydotool"` arrives as `zdotool`. Use
`dev/ui.sh paste <text>` (clipboard plus ctrl+v) whenever the text has to be exact.
`dev/ui.sh key ctrl+c` style shortcuts are fine, modifiers and control keys are
layout independent.

## Verified agent capabilities

| Capability | Status | How |
|---|---|---|
| Full screen screenshot | works | `dev/ui.sh shot` |
| Active window screenshot | works | `dev/ui.sh shotwin` |
| Crop a region for a closer look | works | `dev/ui.sh crop` |
| Move the pointer to an exact pixel | works, after `setup` | `dev/ui.sh move` |
| Left / right click, drag, scroll | works | `dev/ui.sh click` etc |
| Type text | works, layout mangled | prefer `dev/ui.sh paste` |
| Key combinations | works | `dev/ui.sh key ctrl+s` |
| Launch and detach a GUI app | works | `dev/run-example.sh --bg` |
| sudo without a password | works | |

Tested end to end by opening Kate, typing into it, and reading the result back from a
screenshot.

## Renderer

Checked by reading the example app's own startup log:

```
[IMPORTANT:...embedder_surface_gl_impeller.cc(126)]
    Using the Impeller rendering backend (OpenGLESSDF).
```

Impeller is the only renderer on Linux as of Flutter 3.47, running its OpenGLES backend.
Passing `--no-enable-impeller` changes nothing, verified: the log is identical with and
without it. There is no Skia fallback. See `docs/research.md` for why that shapes the
video pipeline.

## Confirmed since

Both of the open questions this file recorded on 2026-09-12 have been answered:

- A Pixel 8 Pro projects over the cable and over Wi-Fi, with
  `tools/setup-dev-machine.sh --udev` in place. `adb` is still only useful for telling
  a locked phone from an unresponsive one, since Android Auto logs nothing to `logcat`
  on a production phone.
- VA-API zero copy works on this Intel UHD part. The decoder exports a dmabuf that
  `gl_adapter` imports as `EGLImage`s, measured at 0.9 to 1.1 ms wire to frame against
  2.9 ms for the software fallback.
