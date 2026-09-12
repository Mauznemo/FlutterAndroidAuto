## Style

- Never use em dashes (—) or en dashes (–), anywhere: not in UI strings, dart docs, normal comments, or markdown. Use a comma, parentheses, or a separate sentence instead.
- When defining widgets or classes put all the vars on top of the constructor and not the other way around.
- Update this or other CLAUDE.mds if the info in here changes or something new is worth adding if it will be needed for every later session. Do not clutter it with one of info or things that are just common sense or easy to figure out without having it here. If you think something one off needs explaining the dart docs with /// is the right place, not this file.

## What this project is

A Flutter plugin that runs an Android Auto head unit inside a Flutter app, rendering the
projected phone screen into a Flutter `Texture` so the host app can draw widgets on top.
Linux first, federated package layout so Android can be added later.

**Read `PLAN.md` first in every session.** It holds the milestones with checkboxes and is
the source of truth for what is done and what is next. Tick a box only when the thing is
verified on this machine, not when it merely compiles. Also update the status table at
the top of `PLAN.md` when a milestone changes state.

Background reading, only when relevant: `docs/research.md` (protocol and library
evaluation), `docs/architecture.md` (how the pieces fit), `docs/dev-environment.md`
(machine specifics).

## Layout

| Path | What |
|---|---|
| `packages/android_auto` | app facing API, pure Dart, no GPL code |
| `packages/android_auto_platform_interface` | the contract, pure Dart, no GPL code |
| `packages/android_auto_linux` | Linux implementation, links aasdk, **GPL-3.0** |
| `example/` | test bench app, run this to verify anything visually |
| `tools/` | `ui.sh`, `run-example.sh`, `setup-dev-machine.sh` |

Keep aasdk code out of the two pure Dart packages. That split is what keeps a future
permissive implementation possible.

## You can drive this machine yourself

KDE Plasma on **Wayland**, 1920x1080 at scale 1, so screenshot pixels equal screen
coordinates. X11 tools (xdotool, scrot, wmctrl) do not work. sudo is passwordless.

```bash
tools/ui.sh setup              # once per boot: starts ydotoold, flattens pointer accel
tools/ui.sh shot               # full screen png, prints the path, then Read it
tools/ui.sh crop <f> <x> <y> <w> <h>
tools/ui.sh click <x> <y>      # also: move, rclick, drag, scroll
tools/ui.sh key ctrl+s         # also: esc, enter, tab, f1..f12
tools/ui.sh paste "text"       # use this, not `type`, see below
tools/run-example.sh --bg      # build and launch the test bench detached
```

Three things that will bite otherwise:

- **`tools/ui.sh setup` is required after every reboot.** Without the flat pointer
  acceleration it sets on the ydotool virtual device, clicks land in the wrong place.
- **The keyboard layout is German (QWERTZ)** and ydotool sends raw keycodes, so
  `ui.sh type` mangles y/z and symbols. Use `ui.sh paste` for exact text.
- **Never run `pkill -f <pattern>`**, it matches the agent's own shell command line and
  kills the session. Resolve the pid first with `pgrep` into a variable built by
  concatenation, then `kill` it.

## Build and run

```bash
cd example && flutter pub get && flutter build linux --debug
tools/run-example.sh --no-impeller   # escape hatch if external textures misbehave
```

Flutter 3.47.4 stable via snap at `~/snap/flutter/common/flutter`.

## Native notes that keep coming back

- aasdk is vendored as a submodule under
  `packages/android_auto_linux/linux/third_party/aasdk`. Changes to it live as patches in
  `linux/patches/`, never as edits committed inside the submodule.
- aasdk uses `boost::asio::io_service`, removed in Boost 1.87. This machine has Boost
  1.90, so the port to `io_context` is mandatory. Details in `docs/research.md`.
- `android_auto_linux` must keep `pluginClass` in its pubspec even though it is mostly
  FFI. The GTK registration entry point is the only way to get the `FlTextureRegistrar`.
- The `io_context` thread pool must never run on Flutter's platform thread, and nothing
  outside the raster thread may call into Flutter's GL context without making the shared
  context current first.
