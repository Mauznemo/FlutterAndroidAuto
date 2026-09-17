#!/usr/bin/env bash
# Drive this machine's GUI from a terminal.
#
# Exists so the coding agent can run the example app, look at it, click around in it
# and read the result back, without a human at the keyboard.
#
# The host is KDE Plasma on Wayland, so the usual X11 tools (xdotool, scrot, wmctrl)
# do not work. Screenshots come from spectacle, synthetic input from ydotool.
#
# Usage:
#   dev/ui.sh setup                 start ydotoold and make pointer motion 1:1
#   dev/ui.sh shot [file]           full screen png, prints the path
#   dev/ui.sh shotwin [file]        active window only
#   dev/ui.sh crop <f> <x> <y> <w> <h> [out]
#   dev/ui.sh move <x> <y>          absolute pointer position
#   dev/ui.sh click <x> <y>         left click at an absolute position
#   dev/ui.sh rclick <x> <y>        right click
#   dev/ui.sh drag <x1> <y1> <x2> <y2>
#   dev/ui.sh scroll <x> <y> <amount>   negative scrolls up
#   dev/ui.sh type <text>           type text (see the layout warning below)
#   dev/ui.sh paste <text>          put text on the clipboard and press ctrl+v
#   dev/ui.sh key <name...>         e.g. esc, enter, tab, ctrl+c
#   dev/ui.sh windows               list open windows
#
# Layout warning: ydotool injects raw evdev keycodes, and this machine's compositor
# layout is German (QWERTZ). "type" therefore mangles y/z and most symbols. Use
# "paste" for anything that has to arrive verbatim.

set -euo pipefail

SOCKET="${YDOTOOL_SOCKET:-/run/user/$(id -u)/.ydotool_socket}"
export YDOTOOL_SOCKET="$SOCKET"
SHOT_DIR="${AA_SHOT_DIR:-/tmp/aa-shots}"
mkdir -p "$SHOT_DIR"

die() { echo "ui.sh: $*" >&2; exit 1; }

# Only `setup` needs root, and it needs it for two commands in a row, the second of
# which is backgrounded with stdin on /dev/null. Backgrounded sudo cannot ask for a
# password: it writes its error into the redirected log and exits, so the daemon simply
# never appears. Ask once, here, where there is still a terminal to ask on.
require_sudo() {
  local why="$1"
  command -v sudo >/dev/null || die "sudo is not installed, and root is needed to $why"
  sudo -n true 2>/dev/null && return 0
  if [ -t 0 ]; then
    echo "ui.sh: root is needed to $why."
    sudo -v || die "could not get root, so ydotoold cannot be started"
    return 0
  fi
  die "root is needed to $why, and there is no terminal to ask for a password on.
  Run 'sudo -v' first, or give this user passwordless sudo for ydotoold."
}

need_daemon() {
  [ -S "$SOCKET" ] || die "ydotoold is not running, run: dev/ui.sh setup"
}

# ydotool's --absolute is unreliable here, so absolute positioning is done by slamming
# the pointer into the top left corner and then moving relatively. That is only exact
# while the virtual device has a flat acceleration profile, which cmd_setup arranges.
move_abs() {
  local x="$1" y="$2"
  ydotool mousemove -- -20000 -20000
  sleep 0.05
  ydotool mousemove -- "$x" "$y"
  sleep 0.05
}

# Find the KWin input device path for the ydotool virtual pointer and force its
# pointer acceleration to flat/0 so one relative unit equals one pixel.
flatten_pointer_accel() {
  local i path name
  for i in $(seq 0 40); do
    path="/org/kde/KWin/InputDevice/event$i"
    name=$(gdbus call --session --dest org.kde.KWin --object-path "$path" \
      --method org.freedesktop.DBus.Properties.Get org.kde.KWin.InputDevice name 2>/dev/null || true)
    case "$name" in
      *ydotoold*)
        gdbus call --session --dest org.kde.KWin --object-path "$path" \
          --method org.freedesktop.DBus.Properties.Set org.kde.KWin.InputDevice \
          pointerAccelerationProfileFlat "<true>" >/dev/null
        gdbus call --session --dest org.kde.KWin --object-path "$path" \
          --method org.freedesktop.DBus.Properties.Set org.kde.KWin.InputDevice \
          pointerAcceleration "<0.0>" >/dev/null
        echo "ui.sh: flattened pointer acceleration on $path"
        return 0
        ;;
    esac
  done
  echo "ui.sh: warning, could not find the ydotool virtual device in KWin" >&2
  return 1
}

# Translate friendly key names into the press/release keycode pairs ydotool wants.
# "ctrl+c" becomes "29:1 46:1 46:0 29:0". Bare numbers are passed through.
keyspec() {
  local spec down up part code
  for spec in "$@"; do
    down=""; up=""
    IFS='+' read -ra parts <<< "$spec"
    for part in "${parts[@]}"; do
      case "${part,,}" in
        ctrl|control) code=29 ;;  shift) code=42 ;;  alt) code=56 ;;
        super|meta)   code=125 ;; esc|escape) code=1 ;;
        enter|return) code=28 ;;  tab) code=15 ;;    space) code=57 ;;
        backspace)    code=14 ;;  delete) code=111 ;;
        up)           code=103 ;; down) code=108 ;;  left) code=105 ;; right) code=106 ;;
        home)         code=102 ;; end) code=107 ;;
        pageup)       code=104 ;; pagedown) code=109 ;;
        f1) code=59 ;; f2) code=60 ;; f3) code=61 ;; f4) code=62 ;;
        f5) code=63 ;; f6) code=64 ;; f7) code=65 ;; f8) code=66 ;;
        f9) code=67 ;; f10) code=68 ;; f11) code=87 ;; f12) code=88 ;;
        [a-z])
          # KEY_A..KEY_Z are not contiguous, so use an explicit table.
          local letters=" a:30 b:48 c:46 d:32 e:18 f:33 g:34 h:35 i:23 j:36 k:37 l:38 m:50 "
          letters+="n:49 o:24 p:25 q:16 r:19 s:31 t:20 u:22 v:47 w:17 x:45 y:21 z:44 "
          code=$(printf '%s' "$letters" | tr ' ' '\n' | grep "^${part,,}:" | cut -d: -f2)
          ;;
        [0-9]|[0-9][0-9]|[0-9][0-9][0-9]) code="$part" ;;
        *) die "unknown key name: $part" ;;
      esac
      down+="$code:1 "
      up="$code:0 $up"
    done
    printf '%s%s' "$down" "$up"
  done
}

cmd_setup() {
  # A socket file left behind by a killed daemon looks alive but is not, so key off
  # the process instead.
  if ! pgrep -x ydotoold >/dev/null; then
    require_sudo "start ydotoold, which opens /dev/uinput"
    sudo rm -f "$SOCKET"
    sudo setsid ydotoold --socket-path="$SOCKET" --socket-perm=0600 \
      --socket-own="$(id -u):$(id -g)" >/tmp/ydotoold.log 2>&1 </dev/null &
    for _ in $(seq 1 30); do [ -S "$SOCKET" ] && break; sleep 0.2; done
  fi
  [ -S "$SOCKET" ] || die "ydotoold failed to start, see /tmp/ydotoold.log"
  echo "ui.sh: ydotoold listening on $SOCKET"
  # The device only appears to KWin after the first event.
  ydotool mousemove -- 1 0 >/dev/null 2>&1 || true
  sleep 0.3
  flatten_pointer_accel || true
}

cmd_shot() {
  local out="${1:-$SHOT_DIR/shot-$(date +%H%M%S).png}"
  spectacle -b -n -f -o "$out" >/dev/null 2>&1
  echo "$out"
}

cmd_shotwin() {
  local out="${1:-$SHOT_DIR/win-$(date +%H%M%S).png}"
  spectacle -b -n -a -o "$out" >/dev/null 2>&1
  echo "$out"
}

cmd_crop() {
  local f="$1" x="$2" y="$3" w="$4" h="$5"
  local out="${6:-${f%.png}-crop.png}"
  python3 -c "
from PIL import Image
Image.open('$f').crop(($x, $y, $x+$w, $y+$h)).save('$out')"
  echo "$out"
}

cmd_windows() {
  gdbus call --session --dest org.kde.KWin --object-path /KWin \
    --method org.kde.KWin.supportInformation 2>/dev/null \
    | tr ',' '\n' | grep -iE "Caption|resourceName|Geometry" | head -60
}

case "${1:-}" in
  setup)   shift; cmd_setup "$@" ;;
  shot)    shift; cmd_shot "$@" ;;
  shotwin) shift; cmd_shotwin "$@" ;;
  crop)    shift; cmd_crop "$@" ;;
  windows) shift; cmd_windows "$@" ;;
  move)    need_daemon; move_abs "$2" "$3" ;;
  click)   need_daemon; move_abs "$2" "$3"; ydotool click 0xC0 >/dev/null ;;
  rclick)  need_daemon; move_abs "$2" "$3"; ydotool click 0xC1 >/dev/null ;;
  drag)
    need_daemon
    move_abs "$2" "$3"
    ydotool click 0x40 >/dev/null          # left down
    sleep 0.1
    ydotool mousemove -- "$(( $4 - $2 ))" "$(( $5 - $3 ))"
    sleep 0.1
    ydotool click 0x80 >/dev/null          # left up
    ;;
  scroll)  need_daemon; move_abs "$2" "$3"; ydotool mousemove --wheel -- 0 "$4" >/dev/null ;;
  type)    need_daemon; shift; ydotool type -- "$*" >/dev/null ;;
  paste)
    need_daemon; shift
    command -v wl-copy >/dev/null || die "wl-clipboard is not installed"
    printf '%s' "$*" | wl-copy
    ydotool key 29:1 47:1 47:0 29:0 >/dev/null   # ctrl+v
    ;;
  key)
    need_daemon; shift
    ydotool key $(keyspec "$@") >/dev/null
    ;;
  *)
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
