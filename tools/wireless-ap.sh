#!/usr/bin/env bash
# Bring this machine up as the Wi-Fi access point a phone projects over, or put it
# back the way it was.
#
#   tools/wireless-ap.sh up [ssid] [passphrase]
#   tools/wireless-ap.sh check [ssid] [passphrase]
#   tools/wireless-ap.sh down
#   tools/wireless-ap.sh status
#
# This is configuration, not code, and it is deliberately not in the plugin. Hosting a
# network is the machine's business: an infotainment image will have its own opinion
# about interfaces, channels and regulatory domains, and a plugin that reconfigured
# networking behind a host app's back would be a worse neighbour than one that reads
# what is already there. The same reasoning as tools/install-echo-cancel.sh, written
# up in docs/echo-cancellation.md.
#
# What the plugin needs from whatever set the network up is one thing only: the
# passphrase. Everything else, the SSID, the access point's MAC and the address the
# phone dials, it reads off the interface itself.
#
# Two warnings that are easy to learn the hard way:
#
#   - Most laptop radios cannot host an access point and stay joined to someone else's
#     network at the same time. Bringing this up will drop whatever the machine was
#     connected to, this machine's internet included.
#   - A phone hosting its own hotspot cannot join an access point without dropping the
#     hotspot. If the machine's only internet is that hotspot, wireless projection and
#     that internet are mutually exclusive.

# Deliberately not `set -e`.
#
# This script is mostly best effort diagnostics: deleting a profile that is not
# there, grepping a journal that has nothing in it, asking nmcli about a connection
# that has gone. Under `set -e` every one of those is fatal, and fatal here means the
# script stops mid way having printed everything up to that point and nothing after,
# which reads as "it does nothing" and is very hard to see. It did exactly that at
# the `connection delete` below.
#
# Every command whose failure actually matters is checked explicitly instead.
set -uo pipefail

ACTION="${1:-status}"
SSID="${2:-HeadUnit}"
PASSPHRASE="${3:-headunit1234}"
CONNECTION="android-auto-ap"

command -v nmcli >/dev/null || {
  echo "This script drives NetworkManager and nmcli is not installed." >&2
  echo "Set an access point up however this machine normally does, then tell the" >&2
  echo "plugin the passphrase. It reads everything else off the interface." >&2
  exit 1
}

device() {
  nmcli -t -f DEVICE,TYPE device | awk -F: '$2=="wifi" {print $1; exit}'
}

# Every nmcli here runs under sudo, and each one would prompt separately. That matters
# more than usual: `up` deletes the old profile before adding the new one, so somebody
# who cancels at the second prompt is left with neither, on a machine whose only link
# may be the Wi-Fi that just went away. Find out once, before anything is touched.
require_sudo() {
  local why="$1"
  if ! command -v sudo >/dev/null; then
    echo "sudo is not installed, and root is needed to $why." >&2
    exit 1
  fi
  sudo -n true 2>/dev/null && return 0
  if [ -t 0 ]; then
    echo "Root is needed to $why."
    sudo -v && return 0
    echo "Could not get root. Nothing has been changed." >&2
    exit 1
  fi
  echo "Root is needed to $why, and there is no terminal to ask for a password on." >&2
  echo "  Run 'sudo -v' first, then this again. Nothing has been changed." >&2
  exit 1
}

case "$ACTION" in
  up)
    DEV="$(device || true)"
    [ -n "$DEV" ] || { echo "No wireless interface." >&2; exit 1; }
    if [ ${#PASSPHRASE} -lt 8 ]; then
      echo "WPA2 needs a passphrase of at least eight characters." >&2
      exit 1
    fi
    require_sudo "reconfigure this machine's wireless interface through NetworkManager"

    # Built as an explicit profile rather than with `nmcli device wifi hotspot`.
    #
    # That convenience command picks the security mode itself, and on NetworkManager
    # 1.54 it picks something a phone told "WPA2 personal" cannot join: the phone
    # associates, fails, and reports STATUS_WIFI_INCORRECT_CREDENTIALS, which reads as
    # a wrong passphrase and is nothing of the kind. The head unit says WPA2 personal
    # on the wire because that is what the protocol's enumeration offers, so the
    # access point has to actually be that: RSN, CCMP, plain PSK, and management
    # frame protection off rather than required.
    # Which band can actually carry an access point, asked of the kernel rather than
    # assumed. A channel the regulatory domain marks "no IR" cannot be beaconed on,
    # whatever the card is capable of, and on iwlwifi that is every 5 GHz channel
    # even with a country set. Getting this wrong is expensive: NetworkManager blocks
    # for ninety seconds trying to start an access point the driver will never bring
    # up, with no output at all.
    #
    # Two traps in reading `iw phy`, both of which this got wrong first time round:
    # channels marked "(disabled)" do not say "no IR" and are not usable either, and
    # the 6 GHz band's frequencies start at 5955 MHz, so a match on 5xxx picks them up
    # as well.
    # Prints the lowest 5 GHz channel this machine may beacon on, or nothing.
    # Hardcoding one does not work: on this card 36 through 64 are all no-IR and only
    # 149 upwards are permitted, and which those are depends on the regulatory domain
    # and on what the card has heard.
    usable_5ghz_channel() {
      iw phy phy0 info 2>/dev/null \
        | awk '/^[[:space:]]*\* 5[0-8][0-9][0-9]\.[0-9] MHz/ {
                 if ($0 !~ /no IR/ && $0 !~ /disabled/ && $0 !~ /radar/) {
                   if (match($0, /\[[0-9]+\]/)) {
                     print substr($0, RSTART + 1, RLENGTH - 2)
                     exit
                   }
                 }
               }'
    }

    BAND=bg
    CHANNEL=6
    FIVE="$(usable_5ghz_channel || true)"
    if [ -n "$FIVE" ]; then
      BAND=a
      CHANNEL="$FIVE"
    else
      echo "This machine may not beacon on any 5 GHz channel: they are all marked"
      echo "no-IR, disabled, or radar. Falling back to 2.4 GHz."
      echo
      echo "Projection will work and the video will be worse than it has to be, because"
      echo "2.4 GHz has neither the bandwidth nor the quiet for a 720p stream. The way"
      echo "to a good picture is then the other arrangement: do not host at all. Put"
      echo "this machine on the network over Ethernet, leave the phone on the same"
      echo "Wi-Fi, and give the plugin that passphrase."
      echo
    fi

    echo "Bringing up '$SSID' on $DEV, band $BAND channel $CHANNEL."
    echo "Anything this machine is connected to over Wi-Fi, its internet included, is"
    echo "about to go away."
    # `|| true` is load bearing. Deleting a profile that is not there exits non-zero,
    # and this line is what made the script run under `set -u -o pipefail` and not
    # `set -e`: with `-e` the whole thing stopped dead here, having printed everything
    # above and nothing after, which looks exactly like doing nothing at all.
    sudo nmcli connection delete "$CONNECTION" >/dev/null 2>&1 || true
    sudo nmcli connection add type wifi ifname "$DEV" con-name "$CONNECTION" \
      autoconnect no ssid "$SSID" \
      802-11-wireless.mode ap \
      802-11-wireless.band "$BAND" \
      802-11-wireless.channel "$CHANNEL" \
      802-11-wireless-security.key-mgmt wpa-psk \
      802-11-wireless-security.proto rsn \
      802-11-wireless-security.pairwise ccmp \
      802-11-wireless-security.group ccmp \
      802-11-wireless-security.pmf 1 \
      802-11-wireless-security.psk "$PASSPHRASE" \
      wifi.cloned-mac-address permanent \
      ipv4.method shared \
      ipv6.method ignore >/dev/null || exit 1
    # --wait, because the default is ninety seconds of silence when the driver will
    # not beacon, and a script that looks hung is a script nobody trusts again.
    if ! sudo nmcli --wait 25 connection up "$CONNECTION"; then
      if [ "$BAND" = a ]; then
        echo
        echo "It would not start on 5 GHz. Falling back to 2.4 GHz."
        sudo nmcli connection modify "$CONNECTION" \
          802-11-wireless.band bg 802-11-wireless.channel 6 || true
        sudo nmcli --wait 25 connection up "$CONNECTION" || {
          echo "It would not start on 2.4 GHz either. What the driver says:" >&2
          journalctl -u NetworkManager --since "-1 min" --no-pager 2>/dev/null \
            | grep -iE "supplicant|hostapd|ap mode|failed" | tail -10 >&2 || true
          exit 1
        }
      else
        echo "The access point would not start. What the driver says:" >&2
        journalctl -u NetworkManager --since "-1 min" --no-pager 2>/dev/null \
          | grep -iE "supplicant|hostapd|ap mode|failed" | tail -10 >&2 || true
        exit 1
      fi
    fi
    echo
    echo "Up. What the phone will be told, and what it will actually find:"
    nmcli -f 802-11-wireless.ssid,802-11-wireless.band,802-11-wireless.channel,\
802-11-wireless-security.key-mgmt,802-11-wireless-security.proto,\
802-11-wireless-security.pmf connection show "$CONNECTION" 2>/dev/null \
      | sed 's/^/  /' || true
    echo
    echo "Give the plugin this passphrase and nothing else:"
    echo "  AndroidAutoWirelessConfig(passphrase: '$PASSPHRASE')"
    ;;

  check)
    # Everything `up` does except the one step that takes the machine off its
    # network. For proving the profile is built the way it should be, on a machine
    # that cannot afford to find out the hard way.
    DEV="$(device || true)"
    [ -n "$DEV" ] || { echo "No wireless interface." >&2; exit 1; }
    CHECK="${CONNECTION}-check"
    require_sudo "build a throwaway NetworkManager profile"
    sudo nmcli connection delete "$CHECK" >/dev/null 2>&1 || true
    sudo nmcli connection add type wifi ifname "$DEV" con-name "$CHECK" \
      autoconnect no ssid "$SSID" \
      802-11-wireless.mode ap \
      802-11-wireless.band a \
      802-11-wireless.channel 149 \
      802-11-wireless-security.key-mgmt wpa-psk \
      802-11-wireless-security.proto rsn \
      802-11-wireless-security.pairwise ccmp \
      802-11-wireless-security.group ccmp \
      802-11-wireless-security.pmf 1 \
      802-11-wireless-security.psk "$PASSPHRASE" \
      wifi.cloned-mac-address permanent \
      ipv4.method shared \
      ipv6.method ignore >/dev/null || {
        echo "The profile would not even be created. That is the thing to fix." >&2
        exit 1
      }
    echo "The profile builds. What an access point would be, minus actually starting:"
    nmcli -f 802-11-wireless.mode,802-11-wireless.band,802-11-wireless.channel,\
802-11-wireless-security.key-mgmt,802-11-wireless-security.proto,\
802-11-wireless-security.pairwise,802-11-wireless-security.pmf,ipv4.method \
      connection show "$CHECK" | sed 's/^/  /'
    sudo nmcli connection delete "$CHECK" >/dev/null 2>&1 || true
    echo
    echo "Nothing was activated and nothing was left behind. 'up' is the real thing."
    ;;

  down)
    require_sudo "take the access point down again"
    sudo nmcli connection down "$CONNECTION" 2>/dev/null || true
    sudo nmcli connection delete "$CONNECTION" 2>/dev/null || true
    echo "Down. Reconnect to a network the usual way."
    ;;
  status)
    DEV="$(device || true)"
    echo "wireless interface: ${DEV:-none}"
    [ -n "$DEV" ] && nmcli -f GENERAL.STATE,GENERAL.CONNECTION,IP4.ADDRESS \
      device show "$DEV" 2>/dev/null || true
    if nmcli -t -f NAME connection show --active 2>/dev/null | grep -qx "$CONNECTION"; then
      echo
      echo "Access point profile, the part that decides whether a phone can join:"
      nmcli -f 802-11-wireless.band,802-11-wireless.channel,\
802-11-wireless-security.key-mgmt,802-11-wireless-security.proto,\
802-11-wireless-security.pmf connection show "$CONNECTION" 2>/dev/null | sed 's/^/  /'
    fi
    echo
    echo "What the head unit would offer a phone, as the plugin reads it:"
    if [ -n "$DEV" ]; then
      # Read the way the plugin reads it: off the interface, not out of a config
      # file. nmcli rather than iwgetid, which lives in wireless-tools and is often
      # not installed. nmcli escapes the colons inside a BSSID, hence the unescaping.
      nmcli -t -f ACTIVE,SSID,BSSID device wifi list ifname "$DEV" 2>/dev/null \
        | awk -F: '$1=="yes" {print "  ssid:  " $2; print "  bssid: " substr($0, index($0,$3))}' \
        | sed 's/\\//g' || true
      ip -4 -br addr show "$DEV" | awk '{print "  ip:    " $3}'
    fi
    ;;
  *)
    sed -n '2,8p' "$0"
    exit 1
    ;;
esac
