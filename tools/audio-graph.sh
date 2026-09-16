#!/usr/bin/env bash
# Dump the audio graph, for answering where call audio is actually going.
#
#   tools/audio-graph.sh            one snapshot to stdout
#   tools/audio-graph.sh --watch    a snapshot every 2 s until interrupted
#
# Android Auto does not carry phone call audio over the projection link: calls go over
# Bluetooth HFP, with this machine as the hands free device. The downlink (the far end's
# voice) is routed to the speakers by WirePlumber on its own. The uplink (this machine's
# microphone to the phone) has no such automatic route, and that is the question this
# script exists to answer: run it during a live call and look at the "links" section.
#
# What each section is for:
#   defaults   what an app that names no device gets. A Bluetooth entry here is why the
#              head unit pins its own devices, see DefaultHeadUnitDevice in pulse_sink.cc
#   card       the bluez card's active profile. headset-head-unit means HFP is carrying
#              a call; a2dp-sink means it is only music
#   streams    which stream landed on which device, so a head unit stream that got moved
#              onto Bluetooth is visible
#   links      the actual graph edges on the bluez nodes. An uplink that works has
#              something feeding bluez_output/bluez_sink; an empty list is the answer
#   sco        the adapter's SCO byte counters. They only move while a call's audio link
#              is up, which distinguishes "no audio" from "no link at all"

set -uo pipefail

snapshot() {
  echo "=================== $(date '+%H:%M:%S') ==================="

  echo "--- defaults ---"
  pactl info 2>/dev/null | grep -E "Default (Sink|Source)"

  echo "--- card ---"
  for card in $(pactl list cards short 2>/dev/null | awk '{print $2}' | grep '^bluez'); do
    echo "$card"
    pactl list cards 2>/dev/null | grep -A1 "Name: $card" | grep -E "Active Profile" \
      || pactl list cards 2>/dev/null | sed -n "/Name: $card/,/^Card/p" \
         | grep -E "Active Profile"
  done
  pactl list cards short 2>/dev/null | grep -q '^.*bluez' || echo "no bluez card, nothing connected"

  echo "--- sinks and sources ---"
  pactl list sinks short 2>/dev/null
  pactl list sources short 2>/dev/null | grep -v '\.monitor'

  echo "--- streams ---"
  pactl list sink-inputs 2>/dev/null \
    | grep -E "Sink Input #|Sink:|application\.name|media\.name" | sed 's/^\s*/  /'
  pactl list source-outputs 2>/dev/null \
    | grep -E "Source Output #|Source:|application\.name|media\.name" | sed 's/^\s*/  /'

  echo "--- links on bluez nodes ---"
  if pw-link -l 2>/dev/null | grep -q bluez; then
    pw-link -l 2>/dev/null | grep -A2 bluez | grep -vE "^--$"
  else
    echo "  none: no bluez node is in the graph"
  fi

  echo "--- sco ---"
  hciconfig -a hci0 2>/dev/null | grep -E "RX bytes|TX bytes"

  echo
}

if [ "${1:-}" = "--watch" ]; then
  while true; do
    snapshot
    sleep 2
  done
else
  snapshot
fi
