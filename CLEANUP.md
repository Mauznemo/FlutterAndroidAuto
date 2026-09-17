# Cleanup audit

A full read of the repository ahead of M11, looking for anything that would confuse,
mislead or block somebody who is not the person who built it. Written 2026-09-17,
against commit `8cd6041`.

The test applied throughout: **would this still make sense to a stranger on different
hardware, after `PLAN.md` is deleted?**

Each item is a checkbox so this can be worked through and ticked off the way `PLAN.md`
was. Sections are being worked in order.

| Section | Items | Worst severity | State |
|---|---|---|---|
| A. Things that are actually broken | 6 | **high** | done, 2026-09-17 |
| B. Privileged commands with no error path | 4 | **high** | done, 2026-09-17 |
| C. Tied to this machine or this phone | 9 | medium | done, 2026-09-17 |
| D. Milestone references | 3 | medium | done, bar one line left to E1 |
| E. Documentation that is stale or wrong | 12 | **high** | E4 done in D |
| F. Packaging and structure | 9 | medium | F9 half done in C |
| G. Debug surface compiled into release | 4 | medium | not started |
| H. CLAUDE.md | 8 | low | not started |

---

## A. Things that are actually broken

### A1. `certificatePath` is documented everywhere and does nothing

**High. This is the most serious finding.**

The config field is copied into the session struct and then never read by anything:

- `linux/src/aa_core.cc:621` writes `session->config.certificate_path`
- nothing anywhere reads it back (`grep -rn certificate_path linux/src` returns three
  hits: the declaration, the doc comment and that one write)
- `ProtocolSession` builds `aasdk::transport::SSLWrapper` with no arguments
  (`protocol_session.cc:197`), and aasdk bakes `cert/headunit.crt` in at build time
  through its own CMake

So an integrator who supplies their own certificate silently gets the bundled one.
No log line, no error, no state change. It fails in exactly the way that is hardest
to attribute.

Four places promise this works:

- [x] `README.md:69` "It is loaded from a configurable path so integrators can supply their own."
- [x] `docs/research.md:161` "The plugin loads it from a configurable path so integrators can substitute their own."
- [x] `packages/android_auto_platform_interface/lib/android_auto_platform_interface.dart:61` `AndroidAutoConfig.certificatePath`, "Overrides the bundled head unit certificate and key."
- [x] `linux/src/aa_core.h:177` "Directory holding headunit.crt and headunit.key. NULL uses the bundled pair."

**Resolved 2026-09-17: removed.** The field is gone from `AndroidAutoConfig`, from
`AaConfig`, from the FFI bindings and from `aa_core.cc`, and the four claims now say
what is true: aasdk compiles the certificate and key in as string literals in
`Messenger/Cryptor.cpp`, so substituting one means rebuilding aasdk. `PLAN.md` records
the same under M3.

### A2. Dead error check in the fake phone listener

`linux/src/session/wireless_connector.cc:245-253`

```cpp
boost::system::error_code ec;
fake_phone_ = std::make_unique<boost::asio::local::stream_protocol::acceptor>(
    io_context_, boost::asio::local::stream_protocol::endpoint(path));
if (ec) { ... }
```

`ec` is default constructed and never passed to anything. That acceptor constructor is
the **throwing** overload, so `if (ec)` is always false: the whole error branch is
unreachable, and a bind failure throws out of an io thread instead of being logged.

The real acceptor in the same file (`wireless_connector.cc:109-126`) does it correctly,
passing `ec` into `open`, `bind` and `listen`. Make this one match.

- [x] Fix, or delete along with the rest of the fake phone path (see G1). Fixed: opened,
      bound and listened in three steps taking an error code, matching the projection
      acceptor above it. Kept rather than deleted, because G1 guards this path rather
      than removing it.

### A3. An unsupported resolution silently becomes 720p

`linux/src/session/service_discovery.cc:44-58`

`ResolutionFor` matches 800x480, 1280x720, 1920x1080, 2560x1440 and 3840x2160, and
returns `VIDEO_1280x720` for anything else with no warning. 1024x600 is a very common
head unit panel and lands here.

The consequence is not only the wrong advertisement. `AndroidAutoConfig.width/height`
still say 1024x600, and that is what `AndroidAutoView` uses for touch mapping until the
first frame arrives and `videoInfo` takes over. Between start and first frame, taps land
in the wrong place.

- [x] Log a warning naming the requested size and the substituted one. **Not verified
      at runtime**: `ResolutionFor` is only reached during service discovery, so it
      needs a phone and a deliberately odd configured size to fire.
- [x] `AndroidAutoConfig.width` doc says "One of 800, 1280 or 1920 in practice", which
      understates the set (2560 and 3840 work) and does not say what happens otherwise.
      List the five and state the fallback.

### A4. `run-example.sh --bundle` runs the wrong binary

`tools/run-example.sh:46`

```bash
BINARY="$REPO/example/build/linux/x64/debug/bundle/android_auto_example"
```

`debug` and `x64` are hardcoded, but `--release` and `--profile` are accepted and passed
to `flutter build`. So `tools/run-example.sh --bundle --release` builds release and then
launches the stale debug binary, or fails outright if there is not one. It also cannot
work on ARM64, which `README.md:10` names as a target.

- [x] Derive the path from `$MODE` and the host architecture. `x86_64` and `aarch64`
      are mapped, anything else refuses rather than guessing.

### A5. `setup-dev-machine.sh` pins a GCC version that will not exist elsewhere

`tools/setup-dev-machine.sh:24`

```
libstdc++-16-dev
```

The comment directly above it explains that clang targets *the newest installed GCC*,
which is the reason the package is needed at all. Hardcoding `16` means the script
fails on any machine with a different GCC, and under `set -euo pipefail` that takes the
whole `--build-deps` run down after the `apt-get update` has already run.

- [x] Detect the version instead. `gcc -dumpversion` turned out to be the wrong probe:
      on this machine it answers 15 while clang selects 16, which is exactly the case
      the package is needed for. `newest_gcc_major` asks clang what it picked
      (`Selected GCC installation:`), falls back to the newest directory under
      `/usr/lib/gcc/*/`, and only then to `gcc -dumpversion`. It is installed after the
      rest of `--build-deps`, so whatever GCC they pulled in is there to be detected,
      and a machine with no matching package gets a warning rather than a failed run.

### A6. `example/pubspec.lock` is gitignored

`.gitignore:7` ignores `pubspec.lock` everywhere. That is right for the three library
packages and wrong for `example/`, which is an application: applications commit their
lockfile so a build is reproducible.

- [x] Negate the pattern for `example/pubspec.lock`. The file is now untracked rather
      than ignored, so it wants adding in the cleanup commit.

---

## B. Privileged commands with no error path

**Resolved 2026-09-17.** The four scripts that need root now each carry a `require_sudo`
preflight: `sudo -n true` first, then `sudo -v` to prompt up front when there is a
terminal, and otherwise a message naming the command, why it needs root and what to run
instead. Nothing is touched before it passes. It is a small function repeated per script
rather than a shared file, because F9 is going to split `tools/` into shipped
infrastructure and the author's own tools, and these four fall on both sides of that line.

The audit as written:

You asked specifically about this. **There is no sudo check anywhere in `tools/`.** Not
one `sudo -n true` probe, no EUID test, no message explaining what is needed and why.
The only acknowledgement is a comment stating the assumption:

`tools/wireless-capture.sh:38` "Needs root to open the monitor socket. sudo is
passwordless on this machine."

That comment is the whole of the design. On a machine where sudo prompts, or where the
user is not a sudoer, four different failure shapes occur:

### B1. Backgrounded sudo cannot prompt, so it fails invisibly

- `tools/ui.sh:118` `sudo setsid ydotoold ... >/tmp/ydotoold.log 2>&1 </dev/null &`
- `tools/wireless-capture.sh:39` `sudo setsid stdbuf -oL btmon -w "$SNOOP" ... &`
- `tools/wireless-capture.sh:54` `sudo setsid tcpdump ... &`

Stdin is `/dev/null` and the job is backgrounded, so sudo cannot ask for a password. It
writes its error into the redirected log and exits. `ui.sh` at least then says
"ydotoold failed to start, see /tmp/ydotoold.log", which is survivable.
`wireless-capture.sh` says "btmon did not start. Check: sudo btmon" only after a sleep,
and the tcpdump path says nothing useful at all.

- [x] Probe once at the top of each script with `sudo -n true`, and on failure either
      run `sudo -v` to prompt interactively up front, or exit with a message naming
      exactly which command needs root and why. Both, in that order. `wireless-capture.sh`
      also preflights `stop` (the capture processes are root-owned) and the `report`
      read of `/tmp/aa-wifi.pcap`: without root that read printed nothing and the report
      concluded the phone never dialled in, which is a wrong answer rather than a
      missing one. The stale "sudo is passwordless on this machine" comment is gone.

### B2. `ui.sh setup` prompts halfway through

`tools/ui.sh:117` runs `sudo rm -f "$SOCKET"` **before** the backgrounded daemon start.
That one is foreground and will prompt, so on a password-requiring machine the script
blocks at a prompt the caller did not expect, then silently fails at the next line.

- [x] Same preflight, inside the branch that starts the daemon, so the other
      sub-commands stay root-free.

### B3. `wireless-ap.sh` mutates network state before it knows it can finish

`tools/wireless-ap.sh:128-129` deletes an existing NetworkManager connection and then
adds a new one. Each `sudo nmcli` may prompt independently. A user who cancels at the
second prompt is left with the old connection deleted and no new one, which on a machine
whose only link is that Wi-Fi means no way back online. The script does not `set -e`
(deliberately, per its own header), so it carries on through the wreckage.

- [x] Preflight sudo before the first destructive `nmcli`. It sits directly after the
      argument checks in `up`, so a refusal costs nothing and prints "Nothing has been
      changed". `check` and `down` preflight too, `down` because it is the way back.
- [x] Consider making `up` verify it can acquire root before deleting anything. Same
      preflight: it runs before the channel probe, so nothing is printed about bringing
      an access point up that then cannot be brought up.

Also fixed in passing, because it sits on the line this touches: the comment above the
`connection delete` claimed "This script runs under `set -e`", which the header four
lines earlier says it deliberately does not.

### B4. `setup-dev-machine.sh` documents sudo in one line and never checks

`tools/setup-dev-machine.sh:9` says "Written for Ubuntu/Debian. Uses sudo." That is the
whole of it. It is also the only thing `README.md` tells a new user to run.

- [x] Preflight, and say up front which of the three sub-commands need root. All three
      do today, and the usage block now names what each one changes. Unknown options are
      rejected before the preflight, so a typo never costs a password prompt.
      `--agent-tools` is left in place here; whether it should exist at all is C4.

**The model to copy is already in the repo.** `tools/install-echo-cancel.sh` needs no
root, checks its prerequisites by name across library paths rather than guessing an
architecture, and prints a distribution-specific hint for each missing piece
(`install-echo-cancel.sh:39-60`). Every other script should behave like that one.

---

## C. Tied to this machine, this phone or this agent

**Resolved 2026-09-17.** A new top-level `dev/` holds everything that drives the author's
own machine: `ui.sh`, `run-example.sh`, `wireless-test.sh`, `wireless-capture.sh`,
`fake-wireless-phone.py`, `audio-graph.sh` and `dev-environment.md`, with a
`dev/README.md` saying plainly that none of it ships and that deleting the directory
leaves the plugin building and running as before. `tools/` keeps what anyone cloning the
repository needs: `setup-dev-machine.sh`, `build-aasdk.sh`, `port-aasdk.sh`,
`install-echo-cancel.sh`, `wireless-ap.sh` and `config/`. That also settles the first
half of F9, which asked for exactly this split.

### C1. `docs/dev-environment.md` should not ship

The entire file is one machine on one date, plus how a coding agent drives it. It opens
"What was found on the development machine on 2026-09-12, and how the coding agent drives
it without a human at the keyboard", and includes a table with `sudo | passwordless`, the
display's refresh rate, and a "Verified agent capabilities" matrix.

It is also stale: "Still to confirm" lists an Android phone and VA-API zero copy, both
of which have been working for milestones.

- [x] Delete it, or move it out of `docs/` into something clearly marked as the
      maintainer's own notes. Moved to `dev/dev-environment.md`, with an opening
      paragraph saying it is one laptop rather than a requirement. Its stale "Still to
      confirm" section is now "Confirmed since", recording that both questions (a test
      phone, VA-API zero copy) were answered milestones ago. `README.md` no longer lists
      it under `docs/`.

### C2. `tools/ui.sh` is KDE plus Wayland plus a German keyboard

191 lines that only work on KWin: it drives `org.kde.KWin.InputDevice` over D-Bus to
flatten pointer acceleration (`ui.sh:53-74`), shells out to Spectacle for screenshots,
and warns that `type` mangles y and z because the compositor layout is QWERTZ
(`ui.sh:25-27`).

This is agent tooling for one desktop. It has nothing to do with the plugin.

- [x] Move out of the repository, or into a clearly separate directory that the README
      and CLAUDE.md both describe as "for driving the author's own machine, not part of
      the project". Now `dev/ui.sh`. `README.md` and `CLAUDE.md` both carry the line, and
      CLAUDE.md adds the rule that nothing in `tools/` may call into `dev/`.

### C3. `wireless-capture.sh` is missing from every index

It exists, it is referenced from `CLAUDE.md` in the wireless section and from
`docs/wireless.md:237`, but it is absent from the `tools/` row of the CLAUDE.md layout
table, which lists the other ten.

- [x] Add it, or decide it goes with `ui.sh`. It went with `ui.sh`, and the `dev/` row
      of the CLAUDE.md layout table lists it along with the rest of that directory.

### C4. `--agent-tools` installs desktop software

`tools/setup-dev-machine.sh:27`

```
AGENT_TOOLS=(ydotool kde-spectacle wl-clipboard python3-pil)
```

`kde-spectacle` pulls KDE onto a machine that may not have it. This flag exists purely
to support `ui.sh`.

- [x] Remove along with C2, or at minimum stop advertising it in the script's own
      usage block. Kept, since `dev/ui.sh` was kept, but demoted: it is listed last, its
      line reads "KDE desktop software, only for dev/", and the paragraph under the usage
      block says building and running the project needs the other two and that this one
      pulls KDE onto a machine that may not have it. `--all` now says "all three, KDE
      included" rather than quietly doing it.

### C5. "This Pixel" as a statement of fact

Six places treat one test phone's behaviour as the protocol:

- [x] `linux/src/session/metadata_channels.cc:337` "This Pixel never opens the notification or the media browser"
- [x] `linux/src/bluetooth/bluez_client.h:45` "this Pixel advertises no ..."
- [x] `linux/src/session/microphone_channel.h:132` "the Pixel tested against asks for two"
- [x] `linux/src/aa_core.h:611` "Measured on a Pixel 8 Pro: a query every 5.1 seconds"
- [x] `linux/src/wireless/aaw_handshake.h:55` "Measured on a Pixel 8 Pro"
- [x] `docs/architecture.md:76` "measured against a Pixel 8 Pro at 1280x720"

The measurements are worth keeping, the framing is not. "One phone tested" reads very
differently from "this Pixel", and a reader whose phone behaves otherwise needs to know
which they are looking at. Rephrase as "observed on one Android 15 phone" or similar.

All six now read "the one phone tested, a Pixel 8 Pro". The phone's Android version is
not recorded anywhere in the repository, so the model is kept and the framing changed
rather than inventing a version. A seventh the audit did not list,
`linux/src/session/usb_connector.cc:12` "Measured on a Pixel", got the same treatment.
The remaining "this Pixel" lines are in `PLAN.md` and `CLAUDE.md`, which are D and H.

### C6. Laptop as the reference acoustic environment

- [x] `docs/echo-cancellation.md:25` "Measured on the reference machine, laptop speakers
      at 85% with the built-in microphone roughly 30 cm away". The table now opens with a
      paragraph saying it is one measurement on one machine, that a car's speakers, cabin
      and microphone placement will give different numbers, and that what carries over is
      the shape rather than the figures.

The document already handles this well in its "Verifying it on other hardware" section,
which says to repeat the measurement. The table just needs a one line preamble making
clear the numbers are an example rather than a specification.

### C7. `.gitignore` carries agent scratch

`.gitignore:27-28`

```
# Agent scratch
/tmp-shots/
```

- [x] Remove. Gone. The `custom_headunit.crt`/`.key` patterns are left in place: they
      are a safety net rather than a machine-specific entry, even though A1 removed the
      feature that named them.

### C8. `--bg` is described in terms of the agent

`tools/run-example.sh:6` "run detached, so the agent can screenshot it"

- [x] Reword to what it does: run detached and log to a file. The line the script prints
      afterwards said "then screenshot with ui.sh shot" and now says to wait for the
      window.

### C9. "On this machine" in shipped build and source comments

Harmless in API docs where "this machine" means the head unit, but wrong in three places
where it means the author's laptop:

- [x] `linux/CMakeLists.txt:106` "On this machine that is PipeWire answering to
      PulseAudio's API" (the following sentence already makes the general point, so the
      clause can just go). Gone, as suggested.
- [x] `linux/src/aa_core.cc:142` "on this machine the USB port is the flaky part". Now
      "a marginal USB port is its own source of confusion".
- [x] `linux/src/audio/pcm_sink.h:5` and `linux/src/audio/pulse_sink.cc:3`, same clause.
      Now "on a PipeWire host" and "on most current Linux systems". `pulse_source.cc:3`
      carried the same clause and was not listed; it got the same fix.
- [x] `example/pubspec.yaml:2` "Runs a head unit on this machine". Clause dropped.

Left alone: `linux/src/wireless/wifi_network.h:35` "a configuration problem on this
machine", where "this machine" means the head unit, which is the reading the audit
itself calls harmless.

---

## D. Milestone references

All of these become meaningless the moment `PLAN.md` goes.

**Resolved 2026-09-17.** The decision taken: `PLAN.md` stays as the project's own record,
and nothing else is allowed to depend on it. So every milestone number is gone from
source, build files and `docs/`, and every "see `PLAN.md` under Mx" either had its
substance inlined or was dropped where the surrounding comment already said enough.
`CLAUDE.md` still uses `PLAN.md`, which is what it is for, and is H's business.

### D1. In source files

- [x] `linux/src/test_pattern.h:1,5,6` (see also E4)
- [x] `linux/src/frame_ring.h:4,21`
- [x] `linux/src/sensors/sensor_state.h:13` "See PLAN.md under M8"
- [x] `linux/src/aa_core.h:646` "Started life as M2 scaffolding"
- [x] `linux/src/aa_core.cc:214` "the io_context thread pool that aasdk will run on from M3"
- [x] `linux/src/session/sensor_channel.h:28` "lived in support_channels.cc until M8"
- [x] `linux/src/session/sensor_channel.cc:84` "see PLAN.md under M8"
- [x] `linux/src/session/microphone_channel.h:26,135` "until M7", "in PLAN.md under M7"
- [x] `linux/src/session/audio_channels.h:21` "what M6 replaced"
- [x] `linux/src/session/video_channel.cc:220` "which is M9's problem"
- [x] `linux/src/session/protocol_session.h:254` "a later milestone will let the host app"
- [x] `linux/src/session/metadata_channels.h:3` "Everything before M9"
- [x] `linux/src/session/metadata_channel.h:3` "The five M9 channels"
- [x] `linux/src/metadata/json.h:3,9` "M9 carries", "what M8 learned"
- [x] `linux/src/metadata/metadata_state.h:3,25` "the API agnostic seam for M9", "for the reason M8 spells"
- [x] `lib/src/bindings/aa_core_bindings.dart:1137` (regenerate after fixing `aa_core.h:646`)
- [x] `example/lib/main.dart:163,253,599,722`

Most read fine with the milestone simply deleted: "the API agnostic seam for M9" becomes
"the API agnostic seam". The `PLAN.md` cross-references need their content inlined or
dropped.

That is how it went. Three needed more than a deletion:

- `test_pattern.h` is rewritten rather than trimmed, since it told the reader to delete
  what is now supported API in three layers. That closes **E4** as well.
- `sensor_channel.cc:84` "see PLAN.md under M8 for what this one sends" now states what
  the phone sent: 0 for most sensors and 3 for speed and compass, with the limiter never
  having fired under the microsecond reading.
- `sensor_state.h:13` now says what the defaults buy, rather than pointing at where it
  was written down.

Three more were history rather than information, and went entirely: "lived in
support_channels.cc until M8", the same for M7, and "what M6 replaced". Where a
comment described a future milestone (`video_channel.cc`, `protocol_session.h`) it now
describes the present state instead.

### D2. In build files

- [x] `tools/build-aasdk.sh:2` "run the milestone M1 smoke test"
- [x] `tools/setup-dev-machine.sh:4` "native toolchain and libraries (M1 onward)"
- [x] `linux/smoke/CMakeLists.txt:3` "so milestone M1 stays testable"

### D3. In documentation

- [ ] `README.md:13,48`. Line 48 is a plain listing of a file that exists, so it stays.
      Line 13 makes the reader depend on `PLAN.md` for the project's status, inside the
      same three lines **E1** rewrites for being false. Left there rather than fixed
      twice.
- [x] `docs/research.md:107` "every channel M3 to M10 needs"
- [x] `docs/aasdk-port-notes.md:23,108`
- [x] `docs/wireless.md:4` "`PLAN.md` under M10 has the milestone checklist"
- [x] `docs/architecture.md:12,73,198`
- [x] `docs/dev-environment.md:24,117,118` (moot if C1 is done). Two of the three were
      indeed moot: C1 replaced the "Still to confirm" section. The third, "needed from
      M1", now reads "needed to build".
- [x] `packages/android_auto/README.md:5` and `packages/android_auto_linux/README.md:5`
      both link to `PLAN.md`. Links dropped. Giving the three packages real READMEs is
      **F4** and is still open.

---

## E. Documentation that is stale or wrong

### E1. The README says the project does not work

**High. This is the first thing anybody reads.**

`README.md:12-14`

> **Status: early. Nothing works yet.** The protocol research, architecture and
> milestone plan are done, the native head unit core has not been built.

Everything through M10 is done and verified. The README also links to `PLAN.md` as the
source of truth for status, which is about to be deleted.

- [ ] Rewrite the status block.
- [ ] The "What it will look like" heading and its future tense should become "Usage".
      The snippet itself is accurate against the current API.
- [ ] "Repository layout" omits `docs/echo-cancellation.md`, `docs/wireless.md`,
      `docs/aasdk-port-notes.md` and eight of the eleven tools.
- [ ] "Getting started" is two lines, one of which is a dev machine provisioner. An
      end user needs: add the dependency, the GPL consequence, minimum config, and the
      fact that `tools/build-aasdk.sh` has to be run before the first build (see F9).

### E2. The C ABI section of `docs/architecture.md` is fiction

**High.** `docs/architecture.md:136-169` presents itself as the ABI ("Sketch, not final")
and every function in it is wrong. Verified against `aa_core.h`:

| Documented | Reality |
|---|---|
| `aa_session_create(const AaConfig*, int64_t dart_port)` | second parameter is `AaEventCallback on_event` |
| `aa_touch(...)` | does not exist; it is `aa_session_send_touch` with a different shape |
| `aa_key(...)` | does not exist; `aa_session_send_key` |
| `aa_set_night_mode(...)` | does not exist; `aa_session_set_night_mode` |
| `aa_set_driving_status(session, int32_t parked)` | `aa_session_set_driving_status(session, int32_t restrictions)`, a bitmask, not a bool |
| `aa_set_location(session, lat, lon, bearing, speed)` | `aa_session_set_location(session, const AaLocation*)` |
| `AaConfig.cert_path`, `uint32_t enabled_services` | `certificate_path`, and three masks: `sensors`, `metadata`, `transports` |

Nothing in the section survives contact with the header.

- [ ] Delete the section and point at `linux/src/aa_core.h`, which is thoroughly
      documented and cannot drift from itself.

### E3. `architecture.md` lists a file that does not exist

`docs/architecture.md:45` puts `present/vk_adapter.cc` in the layer diagram with no
qualifier. Line 89 later says "when it lands", but the diagram reads as inventory.

- [ ] Mark it clearly as not yet present, or take it out of the diagram.

### E4. `test_pattern.h` tells you to delete a public API

`linux/src/test_pattern.h:1-6`

> Milestone M2 only: ... Delete it once M4 is done.

M4 is long done, and the test pattern is now shipped public API in three layers:
`aa_session_start_test_pattern`, `AndroidAutoPlatform.startTestPattern` and
`AndroidAutoController.startTestPattern`, each with its own doc comment describing it as
a supported way to lay out an overlay without hardware. `aa_core.h:646` even says it
"earned its keep".

The header and the ABI directly contradict each other.

- [x] Rewrite the header comment to describe what it is now. **Done in D**, since the
      same comment carried two of D1's milestone references. It now says the pattern was
      built as scaffolding for the video path and kept because it earns its place: the
      real decoder publishes into the same ring, so it tells a video problem from a
      presentation one.

### E5. `run-example.sh` describes behaviour that does not exist

`tools/run-example.sh:21-22`

> The window is placed at a known position and size so screenshot coordinates in
> tools/ui.sh line up across runs.

There is no window placement code in the script, in the example app, or anywhere else.
`my_application.cc:55` sets a default size of 1280x720 and no position.

- [ ] Delete the comment.

### E6. Three headers reference a file that was deleted

`support_channels.cc` no longer exists. It is cited as history in:

- [ ] `linux/src/session/audio_channels.h:20`
- [ ] `linux/src/session/sensor_channel.h:28`
- [ ] `linux/src/session/microphone_channel.h:26`

Refactor archaeology that a new reader cannot verify or use.

### E7. `docs/research.md` is a dated log presented as reference

It opens "Everything here was verified on 2026-09-12" and carries open questions that
have since been answered:

- [ ] `research.md:135` "try `-DSKIP_BUILD_PROTOBUF=ON` first" is settled; both
      `aasdk-port-notes.md:21` and `linux/CMakeLists.txt:162` confirm it works
- [ ] `research.md:128` "Escape hatches if this goes badly: pin and build Boost 1.86"
      is moot, the port worked
- [ ] `research.md:257` "though `vainfo` is not installed yet" is stale
- [ ] `research.md:265` "Revisit with native libpipewire if latency is not good enough"
      reads as an open action item

`packages/android_auto_linux/lib/android_auto_linux.dart:4` points host app authors at
this file "for what that means for host apps", which sends them into a research log to
find a licensing answer.

- [ ] Split the settled conclusions out, or retitle the whole file so nobody mistakes
      it for current guidance.

### E8. `aasdk-port-notes.md` is titled for one distribution

"Porting aasdk to Ubuntu 26.04". The fixes apply to any system with Boost 1.87 or newer
and CMake 4, which is most of them by now.

- [ ] Retitle to the actual constraint. Keep the version table as "verified against".

### E9. Every CHANGELOG says "Initial scaffolding"

All three packages, at `0.0.1`, after ten milestones.

- [ ] Write real entries before publishing.

### E10. `example/README.md` is the Flutter template default

Entire contents: "# android_auto_example / A new Flutter project."

The example is the best documentation in the repo (1364 lines exercising every channel,
with the sensor and wireless panels) and its README says nothing about it.

- [ ] Describe what the test bench does and which panels exercise what.

### E11. "over USB" in seventeen transport-agnostic places

Wireless landed in M10 and the projection link can now be TCP, but the comments still
name the cable. The user-facing ones matter most, because they tell an app author that
a call is a USB round trip when it may not be:

- [ ] `android_auto_platform_interface.dart:1173` `browse` doc
- [ ] `android_auto_controller.dart:421` `browse` doc
- [ ] `aa_core.h:551` `aa_session_browse` doc

Internal ones, lower priority, "the transport" would read correctly for all of them:

`audio_output.h:9`, `audio_input.h:12`, `audio_input.cc:15`, `pcm_sink.h:12`,
`pcm_source.h:15`, `video_decoder.h:15`, `video_channel.h:14`, `input_channel.h:146`,
`microphone_channel.h:166`, `metadata_channels.h:80`, `audio_channels.cc:273`,
`android_auto_linux.dart:291`.

### E12. `CLAUDE.md` gets the Boost version wrong

`CLAUDE.md:132` "Boost 1.90 removed `io_service`". It was removed in **1.87**; 1.90 is
merely what this machine ships. Three other places have it right:
`docs/research.md:114`, `tools/build-aasdk.sh:24`, `linux/CMakeLists.txt:137`.

- [ ] Fix the one that is wrong.

---

## F. Packaging and structure

### F1. The two "permissive" packages are licensed GPL-3.0

All four `LICENSE` files are byte identical GPL-3.0 (`md5 1ebbd3e3...`). Meanwhile:

- `README.md:64` "those two packages can be relicensed without untangling anything"
- `docs/architecture.md:186` "can stay permissive"
- `docs/research.md:150` "can be permissive"

The *code separation* permits relicensing. The *licence grant* does not: once there are
outside contributors, relicensing needs every one of them to agree. Right now there are
none, which makes this the cheapest moment in the project's life to decide.

- [ ] Either put an actual permissive licence on `android_auto` and
      `android_auto_platform_interface` now, or reword all three claims to say the
      separation preserves the *option* subject to contributor agreement.

### F2. Two of seventy one source files carry a licence header

For GPL-3.0 the licence's own instructions ask for a per file notice, and downstream
consumers of a single file have no way to know its terms.

- [ ] Add SPDX headers (`// SPDX-License-Identifier: GPL-3.0-or-later`) across
      `packages/*/lib/**` and `packages/android_auto_linux/linux/src/**`.

### F3. No `license:` field in any pubspec, and `publish_to: none` everywhere

All three packages are unpublishable as configured.

- [ ] Decide whether M11 publishes to pub.dev. If yes, drop `publish_to: none`, add
      `license:`, and note that pub.dev scores an example, a CHANGELOG and a README,
      all three of which are currently placeholders (E9, E10, F4).

### F4. Package READMEs are three lines and link to `PLAN.md`

`packages/android_auto/README.md` and `packages/android_auto_linux/README.md` both point
at `PLAN.md`. `packages/android_auto_platform_interface/README.md` points at the
repository README with a relative path.

On pub.dev the package README **is** the landing page, and `../../README.md` does not
resolve there.

- [ ] Give each package a real README. Drop the `PLAN.md` links.

### F5. API docs point at files that are not in the package

Published packages do not carry the repository's `docs/` directory, so these are dead
references for anyone consuming from pub.dev:

- [ ] `android_auto.dart:4` and `android_auto_platform_interface.dart:5` point at
      `docs/architecture.md`
- [ ] `metadata.dart:785` points at `docs/echo-cancellation.md`
- [ ] `android_auto_linux.dart:4` points at `docs/research.md`

Either inline the relevant sentence or use an absolute repository URL.

### F6. The platform interface reaches into the Linux implementation

The contract package is meant to be implementation agnostic (its own library doc says
so), but three doc comments name Linux internals:

- [ ] `android_auto_platform_interface.dart:397` "`AaSensor` in `linux/src/aa_core.h`"
- [ ] `metadata.dart:63` "`AaMetadata` in `linux/src/aa_core.h`"
- [ ] `metadata.dart:526` "see the ENUM instrument cluster type in `service_discovery.cc`"

The first two are load bearing (the enum order really is part of the FFI boundary) and
should be reworded to state the constraint without naming the file: "the order is part
of the platform boundary, implementations map it positionally". The third is an
implementation detail and can go.

Same class: `AndroidAutoKey`'s doc says "Adding to this list means adding to
`SupportedKeycodes()` in the Linux implementation as well".

### F7. Source file comment cites `CLAUDE.md`

`linux/src/session/metadata_channel.cc:54` "The cycle warning in CLAUDE.md is about ..."

Agent instructions cited from shipping source.

- [ ] Restate the rule in place, or point at the header that documents it.

### F8. `ffigen.yaml` include lists are stale

`packages/android_auto_linux/ffigen.yaml` names eight types. The header now declares
thirteen. Missing: `AaLocation`, `AaWirelessConfig`, `AaSensor`, `AaMetadata`,
`AaTransport`, `AaWifiSecurity`, `AaAccessPointType`, `AaDrivingRestriction`, and the
`AaMetadataCallback` typedef.

They generate correctly today because ffigen pulls them in transitively through the
functions that use them, so nothing is broken. But the config no longer describes the
ABI, and a type that is not yet reachable from an included function would silently not
generate.

- [ ] Bring the lists up to date. Bindings themselves are current and complete,
      verified: every `aa_*` function in the header is present in
      `lib/src/bindings/aa_core_bindings.dart`.

### F9. The build depends on `tools/`, which changes what `tools/` is

`linux/CMakeLists.txt:146-150` fails the build with
"aasdk is not patched and will not build against this Boost. Run: tools/build-aasdk.sh".

So `build-aasdk.sh` and `port-aasdk.sh` are **not** developer conveniences that can be
deleted. They are required build infrastructure, and a fresh clone cannot compile
without them.

Worse, `build-aasdk.sh` does much more than patch: it configures and builds a whole
separate smoke target before the plugin can build at all. That is a poor first run for
somebody who just wants to depend on the package.

- [x] Decide which tools are shipped infrastructure (`build-aasdk.sh`, `port-aasdk.sh`,
      `install-echo-cancel.sh`, `wireless-ap.sh`) and which are the author's own
      (`ui.sh`, `run-example.sh`, `wireless-capture.sh`, `wireless-test.sh`,
      `fake-wireless-phone.py`, `audio-graph.sh`), and separate them. **Done in C**, as
      `tools/` and `dev/`. `setup-dev-machine.sh` counts as shipped: `README.md` tells a
      new user to run it. The second bullet below is still open.
- [ ] Better: apply the patch from CMake directly, so a clean clone builds with no
      manual step. Then `build-aasdk.sh` is genuinely optional.

### F10. `flutter analyze` at the repository root fails

There is no root `pubspec.yaml`, so the root `analysis_options.yaml` cannot resolve
`package:flutter_lints/flutter.yaml`:

```
warning • The URI 'package:flutter_lints/flutter.yaml' ... can't be found
```

Each package analyzes clean individually (verified: all four report "No issues found").
Only the root invocation is broken, and that is the obvious thing for a contributor or
a CI job to run.

- [ ] Add a root workspace pubspec, or a melos setup, or document the per package
      invocation. M11 lists CI, so this needs settling anyway.

---

## G. Debug surface compiled into release builds

### G1. Fault injection ships to end users

Three knobs read straight from the environment with no compile time guard:

- `AA_FAULT_SLOW_START` (`protocol_session.cc:41`) sleeps inside `Start()`
- `AA_FAULT_TRANSPORT_AFTER` (`protocol_session.cc:339`) kills the transport on a timer
- `AA_WIRELESS_FAKE_PHONE` (`wireless_connector.cc:240`) opens a Unix socket at an
  arbitrary path and accepts anything that connects as if it were a paired phone

The last one also calls `::remove(path)` on an attacker-controllable path before
binding, and is the one carrying the dead error check from A2. The comment says "It
cannot be reached from Dart and there is deliberately no way to turn it on from the host
app", which is true of the *host app* and not of anybody with environment access.

They are well documented and were clearly valuable. They should not be in a shipped
head unit.

- [ ] Guard all three behind `#ifndef NDEBUG` or a dedicated CMake option
      (`AA_ENABLE_FAULT_INJECTION`, default off).

### G2. A Wi-Fi passphrase can be overridden from the environment

`aa_core.cc:175-186`. The comment is candid: "A passphrase in an environment variable is
a test bench convenience and nothing more. A real head unit gets it from its host app."

It still silently overrides what the host app configured, in release.

- [ ] Same guard as G1.

### G3. `headunit1234` as a default in three tracked files

- `example/lib/main.dart:72` `AndroidAutoWirelessConfig(passphrase: 'headunit1234')`
- `tools/wireless-test.sh:24`
- `tools/wireless-ap.sh:44`, and `wireless-ap.sh:172` prints it back as a copy-paste
  snippet for the host app

Not a secret, but it is the string an integrator copies out of the example.

- [ ] In the example, read it from an environment variable or a text field with an
      obvious placeholder, so nothing invites copying it verbatim.

### G4. Shipped source comments quote dev tool command lines

`AA_SERVICES`, `AA_LOG_LEVEL`, `AA_FAULT_*` and `AA_WIRELESS_FAKE_PHONE` are documented
in the source with invocations like `AA_LOG_LEVEL=DEBUG tools/run-example.sh` and
`tools/run-example.sh --bundle`. If those scripts move or go, the comments dangle.

- [ ] Document the variable, not the script. `aa_core.cc:198`,
      `protocol_session.cc:37,336`, `wireless_connector.cc:233`.

Related: `example/lib/main.dart:117` cites `tools/wireless-test.sh` in a comment
explaining `AA_AUTOSTART`.

Minor, same area: `ApplyTransportOverride` (`aa_core.cc:158-162`) uses a plain substring
`find` where `ApplyServiceOverride` carefully matches whole comma separated entries.
Harmless as the two names do not overlap, but inconsistent.

---

## H. CLAUDE.md

You asked for a review of this specifically. It is 660 lines and about 44 KB, which is
large enough that the important rules compete with the narrative around them.

The technical content is excellent and mostly earns its place. The problems are what it
depends on and what it duplicates.

### H1. It depends on `PLAN.md`, which is being deleted

- [ ] Line 13 "**Read `PLAN.md` first in every session.**" is the second instruction in
      the file.
- [ ] Line 16 "update the status table at the top of `PLAN.md`"
- [ ] Line 396 "`PLAN.md` under M3 has the full reasoning" (the stop and resume
      ordering, which is genuinely important and needs inlining rather than dropping)
- [ ] Line 493 "listed in `PLAN.md` under M3"

### H2. Milestone numbering throughout

Lines 83, 126, 149, 166, 170, 182, 375, 437, 468, 471. Same treatment as D1: most read
fine with the reference removed.

### H3. The "You can drive this machine yourself" section

Lines 36 to 62. KDE Plasma, Wayland, 1920x1080, ydotool, German QWERTZ, passwordless
sudo. This is agent environment, it duplicates `docs/dev-environment.md`, and it will be
wrong for anybody else.

- [ ] Move to a separate local file that is not checked in, or gate it clearly as
      "the author's machine only".

### H4. Hardware specific claims stated as project facts

- [ ] Line 93 "Flutter 3.47.4 stable via snap at `~/snap/flutter/common/flutter`"
- [ ] Line 359 "This Pixel subscribes to eight of the twelve"
- [ ] Line 534 "This Pixel opens three of the five"
- [ ] Line 314 "through the laptop's own speakers and microphone"

### H5. Factual error

- [ ] Line 132 "Boost 1.90 removed `io_service`". See E12.

### H6. Incomplete index

- [ ] The `tools/` row (line 33) lists ten of the eleven scripts; `wireless-capture.sh`
      is missing, and the wireless section later tells you to use it.

### H7. Verification recipes that belong in `docs/`

The `parecord` command and per-100 ms RMS method (lines 320 to 330), the `spd-say`
recipe (line 314), the "-2.2 dB for a -12 dB volume change" anecdote. All useful, none
of it a rule for writing code.

- [ ] Move the measurement methods into `docs/echo-cancellation.md`, which already has a
      "Verifying it on other hardware" section they would fit inside.

### H8. Typo

- [ ] Line 124 "Do NOT git commit unless you are **toled** to do so!" → "told".

---

## What I did not check

- **Nothing was run against a phone.** Every functional claim above was established by
  reading, except the analyzer runs and the file inventory. A1 in particular is worth
  confirming on hardware: point `certificatePath` at a directory with a deliberately
  invalid certificate and confirm the session still connects, which is what the code
  says will happen.
- **The aasdk submodule and its patch** were treated as vendored and out of scope,
  beyond confirming that the certificate is compiled in (`third_party/aasdk/CMakeLists.txt:396`).
- **Generated files** (`lib/src/bindings/`, `example/linux/flutter/ephemeral/`) were
  checked for freshness but not reviewed for content.
- **Runtime behaviour of the tools.** None of the shell scripts were executed; the sudo
  findings in section B come from reading the invocations and their redirections.
