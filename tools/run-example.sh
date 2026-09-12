#!/usr/bin/env bash
# Build and run the example app on this machine.
#
#   tools/run-example.sh              debug run, logs to /tmp/aa-example.log
#   tools/run-example.sh --release
#   tools/run-example.sh --no-impeller   force the Skia GL backend
#   tools/run-example.sh --bg         run detached, so the agent can screenshot it
#
# The window is placed at a known position and size so screenshot coordinates in
# tools/ui.sh line up across runs.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG=/tmp/aa-example.log
MODE=--debug
EXTRA=()
BG=0

for arg in "$@"; do
  case "$arg" in
    --release)     MODE=--release ;;
    --profile)     MODE=--profile ;;
    --no-impeller) EXTRA+=(--no-enable-impeller) ;;
    --bg)          BG=1 ;;
    *)             EXTRA+=("$arg") ;;
  esac
done

cd "$REPO/example"
flutter pub get

if [ "$BG" = 1 ]; then
  setsid flutter run -d linux "$MODE" "${EXTRA[@]}" >"$LOG" 2>&1 </dev/null &
  echo "example running in the background, log: $LOG"
  echo "wait for 'Flutter run key commands' in the log, then screenshot with tools/ui.sh shot"
else
  flutter run -d linux "$MODE" "${EXTRA[@]}" 2>&1 | tee "$LOG"
fi
