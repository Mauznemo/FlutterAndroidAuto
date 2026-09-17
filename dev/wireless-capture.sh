#!/usr/bin/env bash
# Record what the phone actually does over Bluetooth during a wireless Android Auto
# attempt, so a failed attempt leaves evidence instead of a shrug.
#
#   dev/wireless-capture.sh start [ssid] [passphrase]
#   dev/wireless-capture.sh stop
#   dev/wireless-capture.sh report
#
# The head unit's own log can only show what reached it. When nothing does, the
# question is whether the phone asked at all, what it asked for, and who said no, and
# none of that is visible above the controller. btmon sees every packet on the
# adapter, so it answers all three.
#
# Writes /tmp/aa-bt.txt (decoded) and /tmp/aa-bt.btsnoop (loadable in Wireshark), plus
# /tmp/aa-wifi.pcap for the Wi-Fi side. All three can be read afterwards, which matters
# because hosting the access point usually costs this machine its own network.

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEXT=/tmp/aa-bt.txt
SNOOP=/tmp/aa-bt.btsnoop
PIDFILE=/tmp/aa-btmon.pid
PCAP=/tmp/aa-wifi.pcap
PCAPPID=/tmp/aa-tcpdump.pid
AAW_UUID_SHORT="4de17a00"

say()  { printf '%s\n' "$*"; }
step() { printf '\n== %s ==\n' "$*"; }

# btmon and tcpdump both need root, and both are started backgrounded with their output
# redirected. Backgrounded sudo cannot ask for a password: it writes its error into the
# redirect and exits, so the capture silently never happens and `report` later says the
# phone did nothing. Ask once, up front, where there is still a terminal to ask on.
require_sudo() {
  local why="$1"
  if ! command -v sudo >/dev/null; then
    say "sudo is not installed, and root is needed to $why."
    exit 1
  fi
  sudo -n true 2>/dev/null && return 0
  if [ -t 0 ]; then
    say "Root is needed to $why."
    sudo -v && return 0
    say "Could not get root, so there is nothing to record with."
    exit 1
  fi
  say "Root is needed to $why, and there is no terminal to ask for a password on."
  say "  Run 'sudo -v' first, then this again."
  exit 1
}

case "${1:-report}" in
  start)
    step "Recording Bluetooth"
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
      say "Already recording. Stop it first."
      exit 1
    fi
    # btmon needs root to open the monitor socket, and tcpdump to open the interface.
    require_sudo "run btmon and tcpdump"
    sudo setsid stdbuf -oL btmon -w "$SNOOP" >"$TEXT" 2>&1 &
    sleep 1
    pgrep -x btmon | head -1 > "$PIDFILE"
    if ! [ -s "$PIDFILE" ]; then
      say "btmon did not start. Check: sudo btmon"
      exit 1
    fi
    say "btmon running, writing $TEXT and $SNOOP"

    # And the Wi-Fi side. The Bluetooth half can succeed completely and the phone
    # still never dial in, and from above the transport those two look identical:
    # nothing happens. Only a packet capture says whether the phone sent a SYN at
    # all, whether it went to the right address, and who answered.
    DEV="$(nmcli -t -f DEVICE,TYPE device 2>/dev/null | awk -F: '$2=="wifi" {print $1; exit}')"
    if command -v tcpdump >/dev/null && [ -n "$DEV" ]; then
      sudo setsid tcpdump -i "$DEV" -n -s 128 -U -w "$PCAP" \
        "tcp port 5288 or icmp or arp" >/dev/null 2>&1 &
      sleep 1
      pgrep -x tcpdump | head -1 > "$PCAPPID"
      say "tcpdump running on $DEV, writing $PCAP"
    else
      say "tcpdump is not installed, so the Wi-Fi side will not be recorded."
      say "  sudo apt install tcpdump"
    fi

    "$REPO/dev/wireless-test.sh" up "${2:-HeadUnit}" "${3:-headunit1234}" || exit 1

    say ""
    step "Now, in this order"
    say "  1. Turn the phone's hotspot OFF."
    say "  2. dev/wireless-test.sh nudge"
    say "  3. Wait a full minute, watching the phone for any Android Auto notice."
    say "  4. If nothing, re-pair: dev/wireless-test.sh pair"
    say "  5. Wait another minute."
    say "  6. dev/wireless-capture.sh stop"
    say ""
    say "Then put the network back and the capture can be read at leisure:"
    say "  dev/wireless-test.sh down"
    say "  dev/wireless-capture.sh report"
    ;;

  stop)
    step "Stopping the recording"
    require_sudo "kill the capture processes, which are running as root"
    if [ -f "$PIDFILE" ]; then
      sudo kill "$(cat "$PIDFILE")" 2>/dev/null
      rm -f "$PIDFILE"
    fi
    # Resolved to a pid first rather than pattern killed. pkill -f would match this
    # script's own command line, which on this machine has taken an agent's shell down
    # with it more than once.
    for pid in $(pgrep -x btmon 2>/dev/null); do
      sudo kill "$pid" 2>/dev/null
    done
    for pid in $(pgrep -x tcpdump 2>/dev/null); do
      sudo kill "$pid" 2>/dev/null
    done
    rm -f "$PCAPPID"
    sleep 1
    say "Captured $(wc -l < "$TEXT" 2>/dev/null || echo 0) lines into $TEXT"
    say "Read it with: dev/wireless-capture.sh report"
    ;;

  report)
    [ -s "$TEXT" ] || { say "No capture at $TEXT. Run: dev/wireless-capture.sh start"; exit 1; }
    step "What the phone asked for"
    say ""
    say "-- SDP: did the phone go looking for services at all --"
    grep -nE "SDP|Service Search|Service Attribute" "$TEXT" | head -20 \
      || say "   none. The phone never queried this machine's service list."
    say ""
    say "-- the Android Auto UUID anywhere in the capture --"
    if grep -qi "$AAW_UUID_SHORT" "$TEXT"; then
      grep -niE -B2 -A2 "$AAW_UUID_SHORT" "$TEXT" | head -40
    else
      say "   never appears. Either the phone did not read our service list, or it"
      say "   read it and the record did not contain the UUID."
    fi
    say ""
    say "-- RFCOMM: channels anyone tried to open --"
    grep -nE "RFCOMM|Channel:" "$TEXT" | head -30 \
      || say "   no RFCOMM activity at all."
    say ""
    say "-- connection refusals --"
    grep -nE "Refused|refused|Reject|reject|Error|error|Connection Request" "$TEXT" \
      | grep -viE "no error" | head -25 || say "   none."
    say ""
    step "The Wi-Fi side: did the phone ever dial in"
    if [ -s "$PCAP" ]; then
      # tcpdump wrote the file as root, so reading it needs root too. Without this the
      # reads below print nothing and the report concludes the phone never dialled in,
      # which is a wrong answer rather than a missing one.
      require_sudo "read $PCAP, which tcpdump wrote as root"
      say ""
      say "-- anything to or from port 5288 --"
      sudo tcpdump -r "$PCAP" -n "tcp port 5288" 2>/dev/null | head -20 \
        || say "   nothing."
      if ! sudo tcpdump -r "$PCAP" -n "tcp port 5288" 2>/dev/null | grep -q .; then
        say "   Not one packet. The phone joined the network and never tried to"
        say "   connect, so the fault is in what it was told rather than in the link."
      fi
      say ""
      say "-- did the phone reach this machine at all: arp and ping --"
      sudo tcpdump -r "$PCAP" -n "arp or icmp" 2>/dev/null | head -10 || say "   nothing."
    else
      say "No packet capture. tcpdump was not running."
    fi
    say ""
    step "The head unit's own view over the same period"
    grep -aE "\[Wireless\]|\[Bluetooth\]" /tmp/aa-example.log 2>/dev/null \
      | sed 's/.*\[AASDK\] //' | tail -15
    ;;

  *)
    sed -n '2,9p' "$0"
    exit 1
    ;;
esac
