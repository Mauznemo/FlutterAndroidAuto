# Wireless Android Auto

How the plugin gets a phone projecting without a cable, and the handful of rules that
are not obvious from the protobuf. `PLAN.md` under M10 has the milestone checklist.

## Three stages, and only the middle one is new

```
Bluetooth    the head unit publishes an RFCOMM service and the phone opens it
AAW          on that channel: which Wi-Fi network to be on, which address to dial
TCP          the phone joins the network and connects to port 5288
```

Everything above that TCP connection is byte for byte what runs over the cable. The
SSL handshake, service discovery, every channel from video to the metadata decoders:
none of it knows which transport it is on, which is why the wireless code ends at
producing an `aasdk::transport::ITransport` and hands it to the same `ProtocolSession`
the USB path uses.

| Stage | File |
|---|---|
| BlueZ over D-Bus, paired devices, the RFCOMM socket | `src/bluetooth/bluez_client.*` |
| The AAW messages on that socket | `src/wireless/aaw_handshake.*` |
| Which SSID, BSSID and address to name | `src/wireless/wifi_network.*` |
| The acceptor on 5288 and the lifecycle around it | `src/session/wireless_connector.*` |

## The head unit listens, on both legs

Neither direction is obvious and both were established by looking rather than reading.

**Bluetooth: the head unit is the RFCOMM server.** The phone advertises no Android Auto
UUID of its own, so the head unit publishes `4de17a00-52cb-11e6-bdf4-0800200c9a66` and
waits. There is nothing on the phone to connect to: `Device1.ConnectProfile` on that
UUID fails with "No more profiles to connect to".

**Wi-Fi: the head unit is the TCP server.** `WifiStartRequest` carries an `ip_address`
and a `port`, and the head unit is the side that sends it.

## What it takes to be found at all

Two things, and getting either wrong looks identical from above: the phone says nothing
and no log anywhere records a reason.

**The phone must have been paired since the machine started advertising.** It reads the
service list at pairing time and decides from it that a machine is a wireless car. A
phone paired before the service existed never asks, however long it is advertised
afterwards. Re-pairing with the head unit running is what introduces them.

**The SDP record must name an RFCOMM channel, which means passing one to BlueZ.** With
no `Channel` in the `RegisterProfile` options, BlueZ publishes a record whose protocol
descriptor list is L2CAP and nothing else: no RFCOMM entry, no channel number. The
phone asks for the UUID, reads that back, finds nothing to dial, and disconnects
without another packet. This is why every working implementation hardcodes a channel
number, and it is not because the phone expects a particular one: it reads the number
out of the record. `BluezClient` asks for 8 and walks upwards if BlueZ says it is
taken.

## What the phone is told, and where it comes from

Three of the four fields are read off the machine and the fourth cannot be.

| Field | Where it comes from |
|---|---|
| SSID | `SIOCGIWESSID`, or named by the host app |
| BSSID | `SIOCGIWAP`, or `SIOCGIFHWADDR` when hosting |
| address | `getifaddrs`, falling back to the default route's interface |
| passphrase | the host app, and nowhere else |

Nothing here can discover a passphrase: the kernel does not keep one and the network
manager's copy is behind a privileged interface. So a head unit already on the network
it wants the phone on needs exactly one thing configured, which is what makes
`AndroidAutoWirelessConfig` eight fields with seven defaults.

The ioctls are wireless extensions rather than netlink. They are the old interface, but
reading an SSID and a BSSID is exactly what cfg80211 still answers through the
compatibility layer, and the alternatives were libnl as a new dependency or several
hundred lines of hand rolled netlink for two strings. Every use is read only.

**The wireless extensions cannot describe an access point.** `SIOCGIWESSID` and
`SIOCGIWAP` are implemented for station and ad hoc interfaces only; an interface in AP
mode answers `EINVAL` to both. `SIOCGIWMODE` works for every type, which is what makes
it easy to miss: a hosting head unit correctly knows it is hosting and reads an empty
BSSID in the same breath.

An empty BSSID is silently fatal. The phone rejects the offer without scanning and
without associating, and calls it `STATUS_WIFI_INCORRECT_CREDENTIALS`, which sends
whoever is debugging it after the passphrase. So when hosting, the BSSID comes from
`SIOCGIFHWADDR`: in AP mode this machine's MAC is the BSSID, and it matches what
`wpa_supplicant` beacons. Only when hosting, because falling back to the local MAC on
an unassociated station interface would offer this machine's address as the address of
an access point somewhere else. The head unit refuses to start without a BSSID rather
than offering a nameless network.

The SSID cannot be recovered the same way, and where that matters it does not need to
be: a head unit hosting a network is one whose host app created that network, so it
knows the name. The other case is a head unit wired to the LAN with the phone on the
house Wi-Fi, where the machine genuinely cannot know what the phone is connected to and
has to be told.

## Nothing here changes how the machine presents itself on Bluetooth

This matters for a head unit whose own software already pairs the phone for music and
hands free calling, which is most of them, and it is a rule rather than an accident:

- One UUID is added to the adapter's service record. Nothing is removed.
- Pairing, the pairing agent, discoverability, the adapter alias and the adapter class
  are not touched.
- The profile is registered with `Role: server` and **no** `AutoConnect`. AutoConnect
  would have BlueZ reach out to the phone whenever it connects, changing the behaviour
  of a link that other software owns, for no gain: the phone decides when to project.
- A private D-Bus connection is used rather than the process wide shared one, so
  nothing else in the application can end up dispatching BlueZ's calls.

That other software is worth keeping rather than merely tolerating. A phone decides a
machine is a car partly from the hands free profile it offers, and this plugin offers
none.

## Four states, and why not projecting means refusing

Once a phone has been introduced to a machine as a wireless car, it asks for the
service over SDP **every five seconds for as long as it is connected over Bluetooth**,
and it does not stop. What the driver sees meanwhile is a notification saying the phone
is connecting, while nothing is.

Withdrawing the service does not help: it stops the asking being answered, not the
asking. Publishing the service and refusing does.

| | Phone's behaviour |
|---|---|
| Nothing advertised | asks every 5.1 seconds, indefinitely |
| Advertised and refused | asks once or twice, then stops |

Measured over a hundred seconds with a phone connected and the head unit refusing: zero
service queries, one refusal sent, eleven ACL packets on the link in total. Refusing is
not merely tidier, it is by a wide margin the cheaper of the two for the phone's
battery.

So there are four states, and a head unit spends most of its life in the first:

| | Bluetooth service | Projection port | A phone that asks |
|---|---|---|---|
| Application open, not started | published | closed | refused |
| Started | published | open | offered |
| Stopped | published | closed | refused |
| Application gone | withdrawn | closed | unanswered, and it keeps asking |

Constructing an `AndroidAutoController` is what puts it into the first, through
`AndroidAutoPlatform.initialize`. Nothing is projected and no hardware is touched; the
point is only to be able to say no. Starting stays an explicit act, for wireless
exactly as for the cable. The service is never published at all when the host app
leaves wireless out of its transports, which matters more than it looks: a phone paired
in that state never learns the machine can project, so it never asks. That is the
configuration for a head unit that wants Bluetooth music and nothing else.

**Refusing works well enough to need undoing.** A refused phone stops asking and does
not notice when the answer changes, so a head unit that was refusing and is then
started advertises to a phone that has stopped listening. What makes a phone look again
is the Bluetooth link coming up, because that is when it re-reads the service list, so
`WirelessConnector` drops and remakes the link eight seconds after it starts offering,
and only if nothing has asked by then. That costs the phone's Bluetooth audio a few
seconds, which is why it is behind the grace period: a phone that has not been refused
recently is asking every five seconds anyway and is never disturbed.

## The start request is an instruction, not an announcement

`WifiStartRequest` carries the head unit's address, and a phone that receives it
connects there. So the order follows: credentials, the phone joins, it says so with
`WifiConnectionStatus`, and only then is there any point telling it where to dial.

Sent when the Bluetooth channel opens, before the phone has a network, it is answered
`accepted` and forgotten: the phone joins, holds its lease, and never connects. Sent
again when the phone reports it is on the network, the connection happens at once.

**And exactly once.** The phone reports its network status whenever it feels like it,
including while it is projecting, and answering that with another start request is an
instruction it obeys by tearing the working session down and building another. From the
driver's seat that is the Android Auto splash screen appearing and vanishing every few
seconds. So the handshake is told when a phone has dialled in and stays quiet from then
on, and sends the nudge at most once per Bluetooth channel regardless.

## Two things a phone reports as something else

**`STATUS_WIFI_INCORRECT_CREDENTIALS` is the phone's only word for an association that
failed**, whatever the reason, and the passphrase is rarely it. The head unit therefore
logs the whole offer, BSSID and security mode included, on every start. The decisive
check is whether the phone even tried: `journalctl -t wpa_supplicant` showing no
association attempt means it rejected the offer itself rather than failing to join.

The usual cause when it did try is the access point not being the kind the head unit
said it was. The protocol's `WifiSecurityMode` here says WPA2 personal, so the access
point has to be exactly that: RSN, CCMP pairwise and group, plain PSK, management frame
protection off. `nmcli device wifi hotspot` picks its own mode and on NetworkManager
1.54 picks one a phone told WPA2 cannot join.

**An access point also has to be on a channel the kernel will beacon on**, which is a
narrower set than the card supports. A channel the regulatory domain marks `no IR`
cannot be beaconed on at all, and on Intel cards that is typically every 5 GHz channel
below 149 even with a country set. Asking NetworkManager for one of those does not fail
quickly, it blocks for ninety seconds in silence. Two traps in reading `iw phy`: a
`(disabled)` channel does not say `no IR` and is not usable either, and the 6 GHz band's
frequencies start at 5955 MHz, so matching on 5xxx finds "usable" channels in a band the
card cannot use.

## What is not settled

**The version response is a guess.** `WifiVersionResponse` has four fields the schema
calls `unknown_value_a` through `unknown_value_d`, three of them required. No phone has
asked for one across any session, so what goes out is `1, 0, 0` and a warning in the
log. If a phone ever does ask, that line and whatever `WifiConnectionStatus` comes back
are the evidence to fix it with.

**The refusal does not take down a notification the phone has already shown.** It
clears on a real session, on a Bluetooth disconnect, or when swiped away, and swiping
works because a refused phone does not retry. The likeliest explanation is that there
is nothing here to fix: a real car never refuses, so Android has probably not been
walked through this, and the spinner waits for an outcome it still considers pending.

## A phone hosting a hotspot cannot join the head unit

Worth stating because it makes a whole class of test impossible rather than merely
awkward. A phone tethering its mobile data cannot reliably join an access point at the
same time, so a development machine whose only internet is that hotspot cannot have
both. The two workable arrangements are:

- **Joined**: machine and phone on an ordinary network, over Ethernet at the machine's
  end if its radio is needed for nothing else. `access_point_type` comes out `STATIC`.
- **Hosting**: the machine runs the access point and the phone joins it. `DYNAMIC`.
  Most laptop radios cannot host and stay joined to something else at once, so this
  costs the machine its own Wi-Fi. Bluetooth tethering is a way back online that leaves
  the phone's Wi-Fi free.

## Testing it

`tools/wireless-test.sh` brings an access point up, starts the head unit and reports how
far a real phone gets, in named stages. `tools/wireless-capture.sh` records Bluetooth
and the Wi-Fi side while it happens, which is the only way to tell a phone that never
asked from one that asked and walked away: nothing above the transport can see the
difference.

Everything except the phone can be exercised on one machine. The head unit will take an
RFCOMM socket from anywhere, so a script can play the phone's Bluetooth part:

```bash
AA_TRANSPORTS=wireless AA_WIRELESS_FAKE_PHONE=/tmp/aaw.sock tools/run-example.sh --bundle
tools/fake-wireless-phone.py /tmp/aaw.sock
```

It stops at the TCP connection: only a phone has what it takes to get through the SSL
handshake, so this proves the plumbing and not the projection. `AA_TRANSPORTS` exists
for the same test, because a wireless connection is never allowed to displace a session
that is already connected and unplugging is not always possible.

## Losing the link

Much less to do than the USB path. There is no device to bounce and no accessory mode
to leave, so a dead transport means opening the acceptor again and waiting: the
Bluetooth channel usually survives a Wi-Fi blip, and a phone that comes back finds the
port already open. The address is re-read on every re-arm and on every Bluetooth
connection, because a renewed lease between two connections would otherwise send the
phone to an address nothing answers on.
