#!/usr/bin/env bash
# Drive a real wireless Android Auto test on this machine, and say in plain words how
# far it got.
#
#   tools/wireless-test.sh up [ssid] [passphrase]   access point up, head unit running
#   tools/wireless-test.sh join <passphrase> [ssid] stay on the current network instead
#   tools/wireless-test.sh pair                     open a pairing window on this machine
#   tools/wireless-test.sh nudge                    make the phone reconsider, no pairing
#   tools/wireless-test.sh watch                    live status, Ctrl-C to leave
#   tools/wireless-test.sh down                     head unit stopped, network restored
#
# Hosting an access point costs this machine whatever Wi-Fi it was using, which on a
# development machine is often its only way online, so `watch` explains each stage and
# what to do next rather than printing a log and leaving the reader to interpret it.
#
# Background: docs/wireless.md, including why a phone hosting a hotspot cannot join an
# access point.

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG=/tmp/aa-example.log
SSID="${2:-HeadUnit}"
PASSPHRASE="${3:-headunit1234}"

say()  { printf '%s\n' "$*"; }
step() { printf '\n== %s ==\n' "$*"; }

head_unit_running() {
  pgrep -f "bundle/android_auto_ex""ample" >/dev/null 2>&1
}

start_head_unit() {
  step "Starting the head unit, wireless only"
  # Deliberately not AA_LOG_LEVEL=DEBUG. That turns on the video frame log at thirty
  # lines a second, which buries the handshake. Every line `watch` needs is at info,
  # the AAW messages included. Add it by hand for a second run if a phone says
  # something unexpected.
  AA_TRANSPORTS=wireless \
  AA_WIRELESS_SSID="$1" \
  AA_WIRELESS_PASSPHRASE="$2" \
    "$REPO/tools/run-example.sh" --bundle
  sleep 6
  if grep -aq "\[Wireless\] ready on" "$LOG"; then
    grep -a "\[Wireless\] ready on" "$LOG" | tail -1 | sed 's/.*\[AASDK\] //'
  else
    say "The head unit did not come up. Last lines of $LOG:"
    tail -5 "$LOG"
    return 1
  fi
}

case "${1:-watch}" in
  up)
    step "Bringing up the access point '$SSID'"
    say "This machine is about to lose whatever Wi-Fi it is on."
    "$REPO/tools/wireless-ap.sh" up "$SSID" "$PASSPHRASE" || exit 1
    sleep 3
    start_head_unit "$SSID" "$PASSPHRASE" || exit 1
    say ""
    say "On the phone: turn the hotspot off, then Settings, Connected devices,"
    say "Android Auto, and check that wireless Android Auto is on."
    say "If nothing happens within a minute, run: tools/wireless-test.sh pair"
    say ""
    say "Then watch it with: tools/wireless-test.sh watch"
    ;;

  join)
    # For when the machine is already on a network the phone can also be on, which is
    # the arrangement to prefer: no access point, and the machine keeps its internet.
    #
    # The SSID is optional and only needed when this machine is wired rather than on
    # the Wi-Fi itself. It can read a network it is joined to; it cannot read one it
    # has only heard of.
    PASSPHRASE="${2:-}"
    JOIN_SSID="${3:-}"
    [ -n "$PASSPHRASE" ] || { say "Give the passphrase of the Wi-Fi the phone is on."; exit 1; }
    start_head_unit "$JOIN_SSID" "$PASSPHRASE" || exit 1
    say ""
    say "Both ends have to be on the same network. Check the line above names the one"
    say "the phone is on, then: tools/wireless-test.sh watch"
    ;;

  pair)
    # The likeliest reason a phone ignores a head unit it is already paired with: it
    # cached the service list at pairing time, before this machine advertised anything
    # Android Auto related. Pairing again is what makes it look afresh.
    #
    # The head unit has to be running throughout, and this refuses to open the window
    # otherwise. The phone reads the service list during pairing, so pairing against a
    # machine that is not advertising teaches it that there is nothing there; and a
    # phone that has just been paired tries to connect within seconds, so a head unit
    # started afterwards misses the attempt with nothing anywhere to say why.
    if ! head_unit_running; then
      say "The head unit is not running, so this machine is not advertising Android"
      say "Auto and pairing now would teach the phone that it has none."
      say ""
      say "Start it first:"
      say "  tools/wireless-test.sh up        (hosting an access point)"
      say "  tools/wireless-test.sh join ...  (already on the phone's network)"
      exit 1
    fi
    step "Pairing window open for three minutes"
    say "This only makes the machine visible. It changes no existing pairing until"
    say "the phone actually pairs again."
    bluetoothctl discoverable-timeout 180 >/dev/null
    bluetoothctl pairable on >/dev/null
    bluetoothctl discoverable on >/dev/null
    say ""
    say "On the phone, in this order:"
    say "  1. Bluetooth settings, find this machine, forget it."
    say "  2. Scan, and pair with it again. Accept the code on both screens."
    say "  3. Watch for an Android Auto prompt. The phone usually tries within"
    say "     seconds of pairing, so leave the head unit running and watch it in"
    say "     another terminal: tools/wireless-test.sh watch"
    say ""
    say "Adapter: $(bluetoothctl show | awk -F': ' '/Name:/{print $2; exit}')"
    say "Leave it with: bluetoothctl discoverable off"
    ;;

  nudge)
    # A phone decides whether to project when it connects over Bluetooth, so the
    # cheapest way to make it decide again is to make it connect again. Cheaper than
    # pairing and it changes nothing: no bond is touched.
    #
    # The head unit prods the phone by itself eight seconds after it starts offering,
    # if nothing has asked by then, so this is for prodding again without restarting
    # a session.
    if ! head_unit_running; then
      say "The head unit is not running, so there is nothing for the phone to find."
      exit 1
    fi
    ADDRESS="${2:-$(bluetoothctl devices Paired | awk '{print $2; exit}')}"
    [ -n "$ADDRESS" ] || { say "No paired device to nudge."; exit 1; }
    step "Making $ADDRESS connect again"
    bluetoothctl disconnect "$ADDRESS" >/dev/null 2>&1
    sleep 3
    bluetoothctl connect "$ADDRESS" 2>&1 | tail -1
    say ""
    say "The phone decides within a few seconds. Watch it:"
    say "  tools/wireless-test.sh watch"
    say ""
    say "Nothing at all means the phone is not asking, which is a Bluetooth matter"
    say "rather than a Wi-Fi one: it has most likely not been paired since this"
    say "machine started advertising the service. Try: tools/wireless-test.sh pair"
    ;;

  watch)
    step "Live status, Ctrl-C to leave"
    say "Stages, in the order they have to happen:"
    say "  1. offering      the head unit found a network and published Bluetooth"
    say "  2. bluetooth     the phone opened the Bluetooth channel"
    say "  3. offered       the phone was told which network to join"
    say "  4. accepted      the phone said it could do it"
    say "  5. dialled in    the phone connected over Wi-Fi"
    say "  6. projecting    video is arriving"
    say ""
    if ! head_unit_running; then
      say "WARNING: the head unit is not running. Nothing can happen until it is,"
      say "and a phone that tries while it is down gets no answer and gives up."
      say ""
    fi
    say "Stuck at 1 for more than a minute is the phone not deciding this is a car."
    say "  Try: tools/wireless-test.sh pair, with the head unit left running."
    say "Stuck at 4 is a Wi-Fi problem at the phone's end. Its hotspot is the usual"
    say "  culprit: a phone that is tethering cannot join an access point."
    say ""
    # From the start of the log, so a stage that already happened is still shown, and
    # line buffered, so it appears as it happens rather than in four kilobyte lumps.
    tail -n +1 -f "$LOG" 2>/dev/null | stdbuf -oL awk '
      /\[Wireless\] ready on/           { sub(/.*\[AASDK\] /, ""); print "1. offering   " $0; next }
      /opened the Bluetooth channel/    { sub(/.*\[AASDK\] /, ""); print "2. bluetooth  " $0; next }
      /\[Wireless\] offered /           { sub(/.*\[AASDK\] /, ""); print "3. offered    " $0; next }
      /the phone reports:/              { sub(/.*\[AASDK\] /, ""); print "4. phone says " $0; next }
      /start response:/                 { sub(/.*\[AASDK\] /, ""); print "4. phone says " $0; next }
      /phone dialled in from/           { sub(/.*\[AASDK\] /, ""); print "5. dialled in " $0; next }
      /negotiating protocol version/    { print "5. handshaking, SSL next"; next }
      /Video: .*decoded by/             { sub(/.*\[AASDK\] /, ""); print "6. PROJECTING " $0; next }
      /Video: .*frames/                 { sub(/.*\[AASDK\] /, ""); print "6. projecting " $0; next }
      /dialled in while a session/      { sub(/.*\[AASDK\] /, ""); print "!  " $0; next }
      /\[Wireless\].*(could not|refused|would not)/ { sub(/.*\[AASDK\] /, ""); print "!  " $0; next }
      /Bluetooth channel closed/        { sub(/.*\[AASDK\] /, ""); print "!  " $0; next }
    '
    ;;

  down)
    step "Stopping"
    for pid in $(pgrep -f "bundle/android_auto_ex""ample" 2>/dev/null); do
      kill "$pid" 2>/dev/null
    done
    bluetoothctl discoverable off >/dev/null 2>&1
    "$REPO/tools/wireless-ap.sh" down
    say "Reconnect this machine to a network the usual way, or turn the phone's"
    say "hotspot back on."
    ;;

  *)
    sed -n '2,10p' "$0"
    exit 1
    ;;
esac
