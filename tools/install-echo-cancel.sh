#!/usr/bin/env bash
# Install, remove or check the head unit's echo cancellation.
#
#   tools/install-echo-cancel.sh            install and restart the audio server
#   tools/install-echo-cancel.sh --status   report whether it is in place and working
#   tools/install-echo-cancel.sh --remove   take it out again
#
# A head unit that does not cancel echo sends the far end of every call back to whoever
# is on it, a few hundred milliseconds late. The reasoning is in the config file itself
# and in docs/echo-cancellation.md; this script only puts it where PipeWire looks.
#
# This is a per user PipeWire drop-in, not a system change and not a package. It needs no
# root, it survives reboots, and --remove undoes it completely.
#
# Worth knowing before running it on a general purpose laptop: it makes the echo
# cancelled microphone the default input for *every* application on the machine, not just
# this one. On a head unit that is exactly right. On a daily driver laptop it means your
# browser and your meeting software get it too, which is usually welcome and is
# occasionally a surprise.

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="$REPO/tools/config/60-headunit-echo-cancel.conf"
TARGET_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/pipewire/pipewire.conf.d"
TARGET="$TARGET_DIR/60-headunit-echo-cancel.conf"
NODE=headunit_aec_source

say() { printf '%s\n' "$*"; }

restart_audio() {
  say "restarting the audio server"
  systemctl --user restart pipewire pipewire-pulse wireplumber 2>/dev/null \
    || { say "  could not restart PipeWire through systemd. Log out and back in, or"
         say "  restart it however this machine manages user services."; return 1; }
  sleep 4
}

check_prerequisites() {
  local missing=0

  if ! command -v pactl >/dev/null 2>&1; then
    say "missing: pactl. Install pulseaudio-utils (or pipewire-pulse)."
    missing=1
  fi

  # The module and the canceller itself live under different library paths per
  # architecture, so look for them by name rather than guessing x86_64.
  if ! find /usr/lib /usr/lib64 -name 'libpipewire-module-echo-cancel.so' \
       -print -quit 2>/dev/null | grep -q .; then
    say "missing: libpipewire-module-echo-cancel.so."
    say "  It ships with PipeWire itself on most distributions. On Debian and Ubuntu it"
    say "  is in the pipewire package; some distributions split it into pipewire-audio."
    missing=1
  fi

  if ! find /usr/lib /usr/lib64 -name 'libspa-aec-webrtc.so' \
       -print -quit 2>/dev/null | grep -q .; then
    say "missing: libspa-aec-webrtc.so, the canceller this config asks for."
    say "  Debian and Ubuntu: libspa-0.2-bluetooth pulls it in; otherwise look for a"
    say "  pipewire echo cancel or webrtc-audio-processing package."
    missing=1
  fi

  return $missing
}

status() {
  if [ -f "$TARGET" ]; then
    say "config:      installed at $TARGET"
  else
    say "config:      not installed"
  fi

  if ! command -v pactl >/dev/null 2>&1; then
    say "audio:       pactl not available, cannot check"
    return
  fi

  if pactl list sources short 2>/dev/null | grep -q "$NODE"; then
    say "source:      $NODE is present"
  else
    say "source:      $NODE is NOT present. If the config is installed, the audio server"
    say "             either has not been restarted or refused the module. Check with:"
    say "               journalctl --user -u pipewire -n 50"
    return
  fi

  local default
  default=$(pactl info 2>/dev/null | sed -n 's/^Default Source: //p')
  if [ "$default" = "$NODE" ]; then
    say "default:     yes, so the call uplink and the Assistant both use it"
  else
    say "default:     NO, the default source is '$default'."
    say "             Echo cancellation exists but nothing is routed through it. Something"
    say "             else has claimed the default, most likely a saved choice. Fix with:"
    say "               pactl set-default-source $NODE"
  fi

  if command -v pw-link >/dev/null 2>&1; then
    if pw-link -l 2>/dev/null | grep -q 'headunit_aec_capture'; then
      say "microphone:  connected to a real capture device"
    else
      say "microphone:  NOT connected to anything. There may be no microphone on this"
      say "             machine, in which case the uplink will be silent."
    fi
    if pw-link -l 2>/dev/null | grep -q 'echo-cancel-sink'; then
      say "reference:   taking the speakers' monitor, which is what it cancels against"
    else
      say "reference:   NOT connected. Without it this cancels nothing."
    fi
  fi
}

case "${1:-}" in
  --status)
    status
    ;;

  --remove)
    if [ ! -f "$TARGET" ]; then
      say "nothing to remove, $TARGET does not exist"
      exit 0
    fi
    rm -f "$TARGET"
    say "removed $TARGET"
    restart_audio
    say "the default input is now '$(pactl info 2>/dev/null | sed -n 's/^Default Source: //p')'"
    ;;

  "")
    if [ ! -f "$SOURCE" ]; then
      say "cannot find $SOURCE"
      exit 1
    fi
    if ! check_prerequisites; then
      say
      say "install the missing pieces and run this again. Nothing has been changed."
      exit 1
    fi
    mkdir -p "$TARGET_DIR"
    cp "$SOURCE" "$TARGET"
    say "installed $TARGET"
    restart_audio || exit 1
    say
    status
    ;;

  *)
    say "usage: tools/install-echo-cancel.sh [--status|--remove]"
    exit 1
    ;;
esac
