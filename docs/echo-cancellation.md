# Echo cancellation for phone calls

Phone calls in Android Auto do not go over the projection link. They go over Bluetooth
HFP, with the machine running this plugin acting as the hands free unit and the phone as
the audio gateway. Nothing in this repository carries call audio, and nothing needs to:
once the phone is paired, WirePlumber bridges both directions on its own.

What it does not do is cancel echo, and without that a head unit is unpleasant to be
called by. This document is about the one piece of configuration that fixes it.

## The problem

A head unit plays the far end of a call through the car's speakers and listens with a
microphone in the same cabin. The microphone therefore hears the far end along with the
driver, and sends that mixture back up the uplink. The far end hears itself a few
hundred milliseconds late, which is considerably worse than hearing nothing: it is the
single most recognisable symptom of a badly built hands free unit.

The phone will not do this for us, and that is not an oversight. A phone on speakerphone
cancels its own echo, because it knows what its speaker played and when. The moment the
call is routed to a Bluetooth hands free unit it stops, because it no longer knows either
of those things. The `ec_enabled` field the phone sends is asking whether the head unit
has handled this already, not offering to handle it.

One measurement, on one machine, as an illustration of the size of the effect rather
than a specification. A car's speakers, cabin and microphone placement will all give
different numbers; what should carry over is the shape, a large positive gap in the raw
microphone and a suppressed one after. "Verifying it on other hardware" below is how to
take the same measurement where it matters.

Laptop speakers at 85% with the built-in microphone roughly 30 cm away:

| | raw microphone | echo cancelled source |
|---|---|---|
| ambient, nothing playing | -34.4 dBFS | -61.8 dBFS |
| speakers playing | -15.0 dBFS | -71.7 dBFS |

So the microphone picks the speakers up at 19.4 dB above the room floor, which is an
unmistakable echo, and 56.8 dB of it is suppressed. What survives sits below the
uplink's own idle noise floor, which is as good as this measurement can distinguish.

## The fix

`tools/config/60-headunit-echo-cancel.conf`, a PipeWire drop-in that loads
`libpipewire-module-echo-cancel` in monitor mode. Three choices in it matter:

- **The reference is the speakers' monitor**, not one nominated application. So it
  cancels the call audio, the navigation prompts and the media alike, without naming any
  of them. Anything that comes out of the speakers is something the microphone should not
  send back.
- **No device is named anywhere**, which is what makes the same file work on any machine.
  It attaches to whatever the real microphone is and whatever the real output is.
- **The virtual source outranks every real capture device**, so it becomes the system
  default input. That is the whole routing story. The Bluetooth call uplink is an
  ordinary capture stream that follows the default, and so is the Android Auto microphone
  channel, so both end up going through the canceller without either being told to.

The raw microphone is still there and can still be chosen by name, for hardware that
already cancels echo in the device and would only make it worse by doing it twice.

## Installing it

For anyone using this package on its own:

```bash
tools/install-echo-cancel.sh
```

It checks that PipeWire has the pieces, copies the file into the user's PipeWire
configuration directory, restarts the audio server, and then reports whether the result
is actually wired up. `--status` re-runs that report, `--remove` undoes all of it. No root
is needed, nothing outside the user's own configuration is touched, and it survives
reboots.

## Adding it to a host app's setup script

A host app that already runs a one time setup script on each machine should do what the
installer does. It is three steps and no logic:

```bash
# 1. The drop-in. The filename matters only in that PipeWire reads the directory in
#    lexical order, so a 60- prefix lands after the distribution's own defaults.
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/pipewire/pipewire.conf.d"
cp 60-headunit-echo-cancel.conf \
   "${XDG_CONFIG_HOME:-$HOME/.config}/pipewire/pipewire.conf.d/"

# 2. Restart the audio server so it reads the new drop-in.
systemctl --user restart pipewire pipewire-pulse wireplumber

# 3. Confirm it took. This should print headunit_aec_source.
sleep 4 && pactl info | sed -n 's/^Default Source: //p'
```

Ship `60-headunit-echo-cancel.conf` alongside the setup script, or copy it out of this
repository at build time so there is one version of it rather than two.

Two prerequisites are worth checking rather than assuming, because a missing one fails
silently at the PipeWire level and looks like the config being ignored:
`libpipewire-module-echo-cancel.so` and `libspa-aec-webrtc.so`. Both normally ship with
PipeWire itself, but some distributions split them into a separate audio package.
`tools/install-echo-cancel.sh` checks for both by name across library paths rather than
guessing an architecture, which is the part worth copying if the script is reimplemented.

## Verifying it on other hardware

The numbers above are specific to one microphone and one pair of speakers. How much echo
reaches the microphone depends entirely on where the two sit relative to each other, so
repeat the measurement rather than assuming a car will behave like a laptop.

The method, which matters more than the script:

1. Record the raw microphone and the cancelled source **at the same time**, over the same
   passage. Two recordings taken a minute apart are not comparable.
2. Include several seconds of silence first, for a room floor to compare against.
3. Play broadband noise through the speakers, not a tone. A tone is unrepresentatively
   easy for an echo canceller.
4. Compare per-window RMS, not peak.

The trap to avoid: at a quiet playback volume the speakers may not reach the microphone
at all, and then both recordings look identical and the canceller appears to be working
perfectly while doing nothing. Check first that the raw microphone actually rises above
its silent floor while the speakers play. If it does not, turn the volume up until it
does, or there is nothing to measure.

### The recipes, and two traps

Recording is the only way to answer any of this. Listening to speakers and forming an
impression is not a measurement, and the differences involved are a few dB.

```bash
parecord --device=@DEFAULT_MONITOR@ --format=s16le --rate=48000 --channels=2 \
  --file-format=wav /tmp/probe.wav
```

Then take a per-100 ms RMS over the result. Comparisons have to be back to back on the
same passage: two recordings taken a minute apart once gave -2.2 dB for what was
actually a -12 dB volume change, because the track had moved on in between.

To exercise the Assistant's microphone path without a person in the room, speech
synthesis through the speakers is enough to get a query recognised, though it will not
trigger the hotword, because Google's hotword stage does speaker verification:

```bash
spd-say -l de -r -20 -w "Navigiere nach Hamburg"
```

**Do not fake a microphone with `pactl load-module module-null-sink
media.class=Audio/Source`.** On the PipeWire this was tried against it broke recording
machine wide, and the symptom was `pa_simple_new` timing out after 30 seconds against a
device that had worked a minute earlier, which looks exactly like a bug in whatever code
is doing the capturing.

## What is not verified

**That the driver's own voice comes through undamaged.** This cannot be tested from the
machine alone: the only sound source under a test script's control is the speakers, and
anything played there is by definition echo to be cancelled. Near end speech needs a
person in the room and a real two way call. The canceller also runs noise suppression and
gain control, which is why the cancelled source sits well below the raw microphone even
when nothing is playing, so this is worth a deliberate listen rather than an assumption.

**Anything about a real car.** Road noise, a larger cabin, and a microphone further from
the speakers all change the problem.

## Tuning

The settings are spelled out in the config file with the reasoning next to each. The one
likely to need changing is `webrtc.mobile_mode`, which switches to a lighter canceller
built for phone hardware. It is worth turning on only if a weak ARM head unit cannot
afford the full one, and it costs real suppression, so measure before and after rather
than assuming.

`webrtc.voice_detection` is deliberately off. It gates the uplink on whether the
canceller believes someone is talking, and a gate that guesses wrong clips the start of a
word.

## A side effect worth knowing about

This makes the echo cancelled microphone the default input for **every** application on
the machine, not only this one. On a head unit that is exactly right. On a general purpose
laptop used for development it means the browser and meeting software get it too, which is
usually welcome and is occasionally a surprise. `tools/install-echo-cancel.sh --remove`
puts it back.
