#!/usr/bin/env bash
# Build and run the example app on this machine.
#
#   tools/run-example.sh              debug run, logs to /tmp/aa-example.log
#   tools/run-example.sh --release
#   tools/run-example.sh --bg         run detached, so the agent can screenshot it
#   tools/run-example.sh --bundle     run the built binary directly, detached
#
# --bundle exists because `flutter run` pipes the app's stdout through the flutter tool,
# which block buffers it. Native logging (AA_LOG_LEVEL=DEBUG) then arrives in 8 KB lumps
# minutes late, which is useless for watching a protocol exchange. Running the bundle
# straight gives line buffered output and needs `flutter build linux` first.
#
# Every mode kills any instance already running. Two head units fighting over the same
# phone produce exactly the symptoms of a protocol bug: handshakes that half complete,
# reads that time out, a phone that goes silent. Do not skip this.
#
# There is deliberately no renderer switch. Impeller is the only renderer on Linux as
# of Flutter 3.47, and --no-enable-impeller is a verified no-op here.
#
# The window is placed at a known position and size so screenshot coordinates in
# tools/ui.sh line up across runs.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG=/tmp/aa-example.log
MODE=--debug
EXTRA=()
BG=0
BUNDLE=0

for arg in "$@"; do
  case "$arg" in
    --release)     MODE=--release ;;
    --profile)     MODE=--profile ;;
    --bg)          BG=1 ;;
    --bundle)      BUNDLE=1 ;;
    *)             EXTRA+=("$arg") ;;
  esac
done

# flutter build linux writes to build/linux/<arch>/<mode>/bundle, so both halves have to
# follow the arguments. Hardcoding them meant --bundle --release launched a stale debug
# binary, and nothing worked on ARM64 at all.
case "$(uname -m)" in
  x86_64)          ARCH=x64 ;;
  aarch64|arm64)   ARCH=arm64 ;;
  *)               echo "unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac
BINARY="$REPO/example/build/linux/$ARCH/${MODE#--}/bundle/android_auto_example"

# Kill anything already running. Matched on the built binary's path rather than its
# name: pgrep -x cannot match a name this long, and a bare -f pattern would match this
# script's own command line and take the shell down with it.
for pid in $(pgrep -f "bundle/android_auto_ex""ample" || true); do
  [ "$pid" = "$$" ] && continue
  kill "$pid" 2>/dev/null || true
done

cd "$REPO/example"
flutter pub get

if [ "$BUNDLE" = 1 ]; then
  flutter build linux "$MODE" >/dev/null
  setsid stdbuf -oL -eL "$BINARY" >"$LOG" 2>&1 </dev/null &
  echo "example running from the bundle, log: $LOG"
  echo "set AA_LOG_LEVEL=DEBUG in the environment for aasdk's own protocol logging"
elif [ "$BG" = 1 ]; then
  setsid flutter run -d linux "$MODE" "${EXTRA[@]}" >"$LOG" 2>&1 </dev/null &
  echo "example running in the background, log: $LOG"
  echo "wait for 'Flutter run key commands' in the log, then screenshot with tools/ui.sh shot"
else
  flutter run -d linux "$MODE" "${EXTRA[@]}" 2>&1 | tee "$LOG"
fi
