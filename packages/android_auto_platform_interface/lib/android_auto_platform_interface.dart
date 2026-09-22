// SPDX-License-Identifier: GPL-3.0-or-later
/// Platform interface for the `android_auto` plugin.
///
/// Contains no native code and no aasdk derived code, so a future permissive
/// implementation could replace `android_auto_linux` without any host app change. This
/// package is itself GPL-3.0-or-later, like the rest of the repository.
library;

import 'dart:typed_data';

import 'package:plugin_platform_interface/plugin_platform_interface.dart';

import 'src/metadata.dart';

export 'src/metadata.dart';

/// Where a head unit session is in its lifecycle.
enum AndroidAutoConnectionState {
  /// Nothing is running.
  idle,

  /// Looking for a phone on USB, or waiting for one to connect over Wi-Fi.
  searching,

  /// A phone was found, the transport and SSL handshake are in progress.
  handshaking,

  /// Channels are open and the phone is projecting.
  connected,

  /// The session ended because of an error. See [AndroidAutoEvent.message].
  error,
}

/// How the head unit describes itself to the phone during service discovery.
///
/// The values matter: the phone picks its layout and video encoding from them, and
/// some of them show up in the phone's own UI.
class AndroidAutoConfig {
  /// Width in pixels of the frame the phone encodes, or null to let the head unit
  /// choose.
  ///
  /// Left null, which is the default, the head unit picks per connection the smallest
  /// of 800x480, 1280x720 and 1920x1080 whose picture covers the view in physical
  /// pixels. The picture is then shown one to one or shrunk, which the plugin does
  /// cleanly, and never stretched, which looks soft and blocky whatever does it: detail
  /// the phone never encoded cannot be put back. The choice is made when a phone
  /// connects, so a view made much larger afterwards is stretched until the next
  /// connection.
  ///
  /// Set [width] and [height] together to name a size instead. The protocol only has
  /// names for five, so they must be 800x480, 1280x720, 1920x1080, 2560x1440 or
  /// 3840x2160. Anything else is advertised to the phone as 1280x720, with a warning in
  /// the log, and the phone then projects at that size rather than the one asked for.
  ///
  /// All five are 16:9. For a view of another shape, see [matchViewAspectRatio]: this
  /// is then the size of the frame the phone encodes, and what it draws in is the part
  /// of it with the view's shape.
  final int? width;

  /// Height in pixels of the frame the phone encodes, or null to let the head unit
  /// choose. See [width].
  final int? height;

  /// Target frame rate the head unit advertises. 30 or 60.
  final int fps;

  /// Screen density the phone should lay out for.
  final int dpi;

  /// Whether the phone lays its interface out to fit the view it is shown in.
  ///
  /// The protocol only offers 16:9 frame sizes, and a head unit screen rarely is 16:9
  /// once the host app has put a status bar above the projection. With this on, the
  /// view tells the head unit its size and the phone is asked to leave margins round its
  /// interface so that what is inside them is the view's size, or its shape for a view
  /// larger than the frame. The margins are cropped off before the frame reaches
  /// Flutter, so the texture fills the view one to one, with nothing wasted, no bars of
  /// its own, and the phone's text at the size it drew it.
  ///
  /// The size is read when a phone connects. A view that changes size while one is
  /// connected asks the phone to lay out again once it has held still for a moment,
  /// which restarts the phone's video stream: the picture freezes for about a second.
  ///
  /// Off, the phone always draws in the whole 16:9 frame and the view letterboxes it.
  final bool matchViewAspectRatio;

  /// Shown on the phone while Android Auto is active.
  final String headUnitName;

  /// Vehicle make, model and year, reported during service discovery.
  final String carModel;

  /// Vehicle model year, reported during service discovery.
  final String carYear;

  /// Which sensors this head unit tells the phone the car has.
  ///
  /// Read once, when the session starts, because service discovery happens once per
  /// connection. The two defaults are the ones that are not optional: a head unit that
  /// cannot answer the driving status subscription leaves the phone with most of its
  /// interface locked.
  ///
  /// Add to it only for sensors the app will actually feed, through
  /// [AndroidAutoPlatform.setLocation] and its neighbours. Offering one and then never
  /// sending a reading is worse than not offering it, most of all for
  /// [AndroidAutoSensor.location]: the phone stops using its own position as soon as
  /// the car claims to have one.
  final Set<AndroidAutoSensor> sensors;

  /// Which metadata channels this head unit offers the phone.
  ///
  /// Read once, when the session starts, for the same reason the sensors are. Unlike a
  /// sensor this is not a promise: the phone pushes what it has and a head unit that
  /// never reads a channel it advertised simply draws no turn card. The cost of one is
  /// a channel that is opened and answered, which is why the default is the three the
  /// phone pushes on its own and not the two the head unit has to drive.
  ///
  /// [AndroidAutoMetadata.browse] is worth adding for an app that wants to show the
  /// phone's media library, and [AndroidAutoMetadata.notification] for one that wants
  /// to show its notifications. An empty set advertises no metadata channel at all,
  /// which is a real choice and costs nothing else: unlike the sensors, there is no
  /// metadata channel a phone insists on.
  final Set<AndroidAutoMetadata> metadata;

  /// How a phone may reach this head unit.
  ///
  /// The cable only, unless the app asks for more. Wireless costs a Bluetooth service
  /// and an open TCP port, and no head unit should acquire either by accident.
  ///
  /// Adding [AndroidAutoTransport.wireless] publishes one extra UUID in the machine's
  /// Bluetooth service record and takes nothing away, so a head unit whose own
  /// software pairs the phone for music and hands free calling keeps working exactly
  /// as it did. That software is worth keeping: the hands free profile is part of how
  /// a phone decides a machine is a car.
  final Set<AndroidAutoTransport> transports;

  /// Which network to send a phone to when [transports] includes
  /// [AndroidAutoTransport.wireless]. Ignored otherwise.
  ///
  /// Read when the session starts. [AndroidAutoPlatform.setWirelessConfig] changes it
  /// afterwards, which takes effect the next time wireless starts.
  final AndroidAutoWirelessConfig? wireless;

  /// Creates a head unit description. The defaults are a 30 fps head unit whose frame
  /// size follows its view, which every phone accepts.
  const AndroidAutoConfig({
    this.width,
    this.height,
    this.fps = 30,
    this.dpi = 140,
    this.matchViewAspectRatio = true,
    this.headUnitName = 'Flutter Head Unit',
    this.carModel = 'Universal',
    this.carYear = '2026',
    this.sensors = const {
      AndroidAutoSensor.nightMode,
      AndroidAutoSensor.drivingStatus,
    },
    this.metadata = const {
      AndroidAutoMetadata.navigation,
      AndroidAutoMetadata.media,
      AndroidAutoMetadata.phone,
    },
    this.transports = const {AndroidAutoTransport.usb},
    this.wireless,
  });
}

/// What the phone is actually sending, once video is flowing.
///
/// Distinct from [AndroidAutoConfig] on purpose. The config is what the head unit asked
/// for; this is what turned up. The phone chooses from the video configurations the
/// head unit advertised, and it is allowed to change its mind mid session, so a host
/// app that lays out from the config alone will letterbox the projection wrongly the
/// first time a phone does something unexpected.
class AndroidAutoVideoInfo {
  /// Width of the video in pixels, which is the width of the texture.
  ///
  /// Less than the phone's frame when it was asked to leave margins, see
  /// [AndroidAutoConfig.matchViewAspectRatio]: this is the part it draws in, and the
  /// part touch positions are measured in.
  final int width;

  /// Height of the video in pixels, which is the height of the texture.
  final int height;

  /// Which decoder is doing the work: `VA-API`, `software`, or `none` before the first
  /// frame has been decoded.
  final String decoder;

  /// Creates a description of the incoming video stream.
  const AndroidAutoVideoInfo({
    required this.width,
    required this.height,
    required this.decoder,
  });

  /// Width divided by height, for laying the projection out.
  double get aspectRatio => height == 0 ? 0 : width / height;

  @override
  bool operator ==(Object other) =>
      other is AndroidAutoVideoInfo &&
      other.width == width &&
      other.height == height &&
      other.decoder == decoder;

  @override
  int get hashCode => Object.hash(width, height, decoder);

  @override
  String toString() => 'AndroidAutoVideoInfo($width x $height, $decoder)';
}

/// What a touch report describes.
///
/// The names and the order come from Android's own `MotionEvent`, which is what the
/// protocol carries, so the head unit has to follow the same rules Android does: the
/// first finger down is [down] and the last one up is [up], while the ones in between
/// are [pointerDown] and [pointerUp].
enum AndroidAutoTouchAction {
  /// The first finger touched the screen.
  down,

  /// The last finger left the screen.
  up,

  /// At least one finger moved.
  move,

  /// Another finger touched a screen that was already being touched.
  pointerDown,

  /// One of several fingers left the screen, and others are still down.
  pointerUp,
}

/// One finger in a touch report.
///
/// [x] and [y] are in **projected video pixels**, not logical pixels and not
/// normalised, measured from the top left corner of the texture: the
/// [AndroidAutoVideoInfo.width] by [AndroidAutoVideoInfo.height] picture the phone drew.
/// The phone reads them against that, so a widget local position has to be mapped
/// through however the projection is fitted on screen first.
/// `AndroidAutoView` in the `android_auto` package does that; an app sending its own
/// touches has to do it itself.
class AndroidAutoTouchPoint {
  /// Identifies this finger for as long as it stays down. Small indices, the way
  /// Android numbers pointers, rather than an ever growing counter.
  final int id;

  /// Horizontal position in projected video pixels.
  final int x;

  /// Vertical position in projected video pixels.
  final int y;

  /// Creates one finger of a touch report.
  const AndroidAutoTouchPoint({required this.id, required this.x, required this.y});

  @override
  String toString() => 'AndroidAutoTouchPoint(#$id at $x,$y)';
}

/// A hardware key a head unit can report.
///
/// These are the keys advertised during service discovery, which is a promise that the
/// head unit can produce every one of them rather than a request to receive them. A
/// phone may bind any of them and route it itself. The list is part of the platform
/// boundary: an implementation advertises exactly these to the phone, so adding an entry
/// here without teaching every implementation about it means sending a key the phone was
/// never told the head unit had.
enum AndroidAutoKey {
  /// Go back one screen.
  back(4),

  /// Return to the Android Auto home screen.
  home(3),

  /// Answer a call, or open the dialler.
  call(5),

  /// Hang up.
  endCall(6),

  /// Toggle playback.
  playPause(85),

  /// Start playback.
  play(126),

  /// Pause playback.
  pause(127),

  /// Skip to the next track.
  next(87),

  /// Skip to the previous track.
  previous(88),

  /// Start the Assistant. The protocol calls this SEARCH; on a head unit it is the
  /// microphone button, not a text search.
  microphone(84),

  /// Move the selection up, on a head unit with a D-pad.
  up(19),

  /// Move the selection down.
  down(20),

  /// Move the selection left.
  left(21),

  /// Move the selection right.
  right(22),

  /// Activate the selection. This is also the rotary encoder's push.
  enter(23);

  /// The protocol's keycode, which is Android's `KeyEvent` code.
  final int code;

  const AndroidAutoKey(this.code);
}

/// One of the three audio streams a head unit plays.
///
/// Android Auto sends these separately and leaves the mixing to the head unit, which is
/// why they have separate volumes rather than one. It is also why the head unit is the
/// one that has to duck: by the time the music and the navigation prompt leave the
/// phone they are already on different channels, so nothing on the phone can turn one
/// down under the other.
enum AndroidAutoAudioStream {
  /// Music, podcasts, anything the phone calls media. 48 kHz stereo.
  ///
  /// Turned down automatically while [speech] is playing, and back up afterwards.
  media,

  /// Interface and notification sounds. 16 kHz mono.
  system,

  /// Navigation prompts and the Assistant. 16 kHz mono.
  speech,
}

/// An audio device the machine can play through or listen with.
///
/// An infotainment system often routes audio itself, through an amplifier the head unit
/// does not control, and its microphone is rarely the one the operating system would
/// pick by default, so choosing both is the host app's business rather than something
/// this plugin should decide. The same class describes an output and an input; which one
/// it is depends on whether it came from [AndroidAutoPlatform.audioDevices] or
/// [AndroidAutoPlatform.microphoneDevices].
class AndroidAutoAudioDevice {
  /// What to hand to [AndroidAutoPlatform.setAudioDevice] or
  /// [AndroidAutoPlatform.setMicrophoneDevice]. Stable, not human friendly.
  final String name;

  /// What to show a person, such as `Built-in Audio Analogue Stereo`.
  final String description;

  /// Whether the audio server would have picked this one anyway.
  final bool isDefault;

  /// Creates a description of one device.
  const AndroidAutoAudioDevice({
    required this.name,
    required this.description,
    this.isDefault = false,
  });

  @override
  bool operator ==(Object other) =>
      other is AndroidAutoAudioDevice &&
      other.name == name &&
      other.description == description &&
      other.isDefault == isDefault;

  @override
  int get hashCode => Object.hash(name, description, isDefault);

  @override
  String toString() => 'AndroidAutoAudioDevice($description)';
}

/// One buffer of PCM, exactly as the phone sent it.
///
/// Delivered to apps that want to mix the audio themselves. It is the raw stream,
/// before this head unit's volume or ducking, so an app reading these should also turn
/// the built in output off with
/// [AndroidAutoPlatform.setAudioOutputEnabled] or it will hear both.
class AndroidAutoAudioBuffer {
  /// Which of the three streams this belongs to.
  final AndroidAutoAudioStream stream;

  /// Samples per second, 48000 for media and 16000 for the other two.
  final int sampleRate;

  /// 2 for media, 1 for the other two.
  final int channels;

  /// Signed 16 bit little endian samples, interleaved when there is more than one
  /// channel. Owned by the caller, and a fresh copy per buffer.
  final Uint8List samples;

  /// Creates one buffer of raw PCM.
  const AndroidAutoAudioBuffer({
    required this.stream,
    required this.sampleRate,
    required this.channels,
    required this.samples,
  });

  /// How long this buffer takes to play.
  Duration get duration => Duration(
    microseconds:
        sampleRate == 0 || channels == 0
            ? 0
            : samples.lengthInBytes * 1000000 ~/ (sampleRate * channels * 2),
  );

  @override
  String toString() =>
      'AndroidAutoAudioBuffer(${stream.name}, ${samples.lengthInBytes} bytes)';
}

/// A sensor a head unit can report to the phone.
///
/// Declaring one in [AndroidAutoConfig.sensors] is a statement that the car has it, not
/// a feature switch. The phone subscribes to everything that is offered and then waits
/// for readings, and for [location] it stops using its own position the moment it sees
/// the entry, so a head unit that offers a fix it cannot supply has taken navigation
/// away from a phone that was managing without it. Offer what the app can feed.
///
/// The order is part of the platform boundary rather than an internal detail: each
/// entry is one bit, in this order, and implementations map it positionally. Adding an
/// entry anywhere but the end changes what every existing implementation means.
enum AndroidAutoSensor {
  /// Whether it is dark outside, which drives the phone's own light and dark theme.
  nightMode,

  /// What the car forbids while it is moving. See [AndroidAutoDrivingRestriction].
  drivingStatus,

  /// The car's position, from the car's own receiver rather than the phone's.
  location,

  /// Road speed.
  speed,

  /// Engine speed.
  rpm,

  /// Tank level, remaining range and the low fuel warning.
  fuel,

  /// Whether the parking brake is engaged.
  parkingBrake,

  /// The selected gear.
  gear,

  /// Which way the car is pointing, which is not which way it is moving.
  compass,

  /// Outside temperature and barometric pressure.
  environment,

  /// Total distance travelled.
  odometer,

  /// Whether a toll transponder is in the car.
  tollCard;

  /// The bit this sensor occupies in the mask the native layer takes.
  int get bit => 1 << index;
}

/// What a moving car forbids the phone to do.
///
/// These combine, and a parked car sets none of them: an empty set is what Android Auto
/// calls unrestricted, and it is what this plugin reports until the host app says
/// otherwise. This is the single most consequential thing a head unit tells the phone
/// about itself, because the phone locks the matching parts of its interface and the
/// person in the car has no way to override it.
enum AndroidAutoDrivingRestriction {
  /// No moving pictures. The projection keeps working; video content inside it stops.
  video(1),

  /// No on screen keyboard.
  keyboard(2),

  /// No voice input.
  voice(4),

  /// No settings screens.
  configuration(8),

  /// Long messages are truncated rather than shown in full.
  messageLength(16);

  /// The protocol's DrivingStatus bit.
  final int code;

  const AndroidAutoDrivingRestriction(this.code);
}

/// Where the car is.
///
/// Only [latitude] and [longitude] are required. The other four are null when the host
/// app does not know them, and a null is left off the wire entirely rather than sent as
/// zero, because every one of them has a meaningful zero: a bearing of zero is due
/// north, an altitude of zero is sea level, and a speed of zero is standing still.
class AndroidAutoLocation {
  /// Degrees north, negative for south.
  final double latitude;

  /// Degrees east, negative for west.
  final double longitude;

  /// Radius in metres of the circle the fix is somewhere in.
  final double? accuracyMetres;

  /// Height above sea level in metres.
  final double? altitudeMetres;

  /// Ground speed in metres per second.
  final double? speedMps;

  /// Direction of travel in degrees clockwise from north.
  final double? bearingDegrees;

  /// Creates a position fix.
  const AndroidAutoLocation({
    required this.latitude,
    required this.longitude,
    this.accuracyMetres,
    this.altitudeMetres,
    this.speedMps,
    this.bearingDegrees,
  });

  @override
  bool operator ==(Object other) =>
      other is AndroidAutoLocation &&
      other.latitude == latitude &&
      other.longitude == longitude &&
      other.accuracyMetres == accuracyMetres &&
      other.altitudeMetres == altitudeMetres &&
      other.speedMps == speedMps &&
      other.bearingDegrees == bearingDegrees;

  @override
  int get hashCode => Object.hash(
    latitude,
    longitude,
    accuracyMetres,
    altitudeMetres,
    speedMps,
    bearingDegrees,
  );

  @override
  String toString() =>
      'AndroidAutoLocation($latitude, $longitude'
      '${accuracyMetres == null ? "" : ", +/-${accuracyMetres}m"})';
}

/// Something the native session wants the Dart side to know about.
class AndroidAutoEvent {
  /// The lifecycle state the session moved into.
  final AndroidAutoConnectionState state;

  /// Human readable detail, set when [state] is
  /// [AndroidAutoConnectionState.error].
  final String? message;

  /// Creates an event describing a session state change.
  const AndroidAutoEvent(this.state, [this.message]);
}

/// How a phone may reach the head unit.
///
/// Not alternatives. A head unit is normally both: the cable is what a driver reaches
/// for when the battery is low, and wireless is what they use the rest of the time.
enum AndroidAutoTransport {
  /// A cable, with the phone switched into Android Open Accessory mode.
  usb,

  /// Bluetooth to agree the details, then Wi-Fi to carry the projection.
  wireless;

  /// The bit this transport occupies in the native mask.
  int get bit => 1 << index;
}

/// How the Wi-Fi network the phone is told to join is secured.
///
/// The numbers are the protocol's own `WifiSecurityMode`, which is why they are not
/// consecutive.
enum AndroidAutoWifiSecurity {
  /// No passphrase. A real configuration, and a terrible one for a car.
  open(1),

  /// WPA personal.
  wpaPersonal(4),

  /// WPA2 personal, which is what almost every access point is set to.
  wpa2Personal(5),

  /// A network accepting either WPA or WPA2.
  wpaWpa2Personal(6);

  /// The value the protocol carries.
  final int value;

  const AndroidAutoWifiSecurity(this.value);
}

/// Whether the head unit brought the network up for the phone, or is merely on one.
///
/// The phone is told which, and it is worth getting right: it describes whose network
/// this is, not how good it is.
enum AndroidAutoAccessPointType {
  /// Work it out from whether the wireless interface is hosting the network rather
  /// than joined to it. The right answer almost always.
  automatic(-1),

  /// A network that was already there, which the head unit happens to be joined to.
  /// A workshop's Wi-Fi, or a car with a built in router.
  existing(0),

  /// A network this head unit is hosting for the phone.
  hosted(1);

  /// The value the protocol carries, or -1 for [automatic], which never reaches it.
  final int value;

  const AndroidAutoAccessPointType(this.value);
}

/// Which network to send the phone to, and where to dial once it is on it.
///
/// Every field but the passphrase can be left out and read off the machine. The
/// passphrase cannot: the kernel does not keep one and the network manager's copy is
/// behind a privileged interface, so a head unit sitting on an ordinary Wi-Fi network
/// needs exactly one thing configured.
///
/// Nothing here brings a network up. Hosting an access point, or joining someone
/// else's, is the machine's own configuration, in the same way the echo canceller for
/// phone calls is.
class AndroidAutoWirelessConfig {
  /// The passphrase of the network the phone should join. Empty only makes sense with
  /// [AndroidAutoWifiSecurity.open].
  final String passphrase;

  /// The network's name. Empty reads it off the wireless interface, which is right
  /// whenever the head unit is already on the network it wants the phone on.
  final String ssid;

  /// The access point's MAC. Empty reads it off the interface.
  final String bssid;

  /// Which wireless interface to describe, `wlan0` and the like. Empty picks the
  /// first one with an address.
  final String interfaceName;

  /// The address the phone connects back to. Empty reads the interface's IPv4.
  final String ipAddress;

  /// Which paired phone to prod when wireless starts being offered and nothing asks
  /// for it. Normally left empty, which prods whichever paired phone is connected;
  /// worth naming on a machine several phones are paired with.
  ///
  /// The prod drops and remakes that phone's Bluetooth link, because re-reading the
  /// service list is something a phone only does when the link comes up. It costs
  /// that phone's Bluetooth audio a few seconds, so it only happens when a phone has
  /// gone quiet. Fill it from [AndroidAutoPlatform.pairedPhones].
  final String phoneAddress;

  /// The port the head unit listens on. 5288 is what Android Auto dials.
  final int port;

  /// How the network is secured.
  final AndroidAutoWifiSecurity security;

  /// Whose network this is.
  final AndroidAutoAccessPointType accessPoint;

  /// Describes the network to send a phone to.
  const AndroidAutoWirelessConfig({
    this.passphrase = '',
    this.ssid = '',
    this.bssid = '',
    this.interfaceName = '',
    this.ipAddress = '',
    this.phoneAddress = '',
    this.port = 5288,
    this.security = AndroidAutoWifiSecurity.wpa2Personal,
    this.accessPoint = AndroidAutoAccessPointType.automatic,
  });
}

/// A device this machine is paired with over Bluetooth.
class AndroidAutoBluetoothDevice {
  /// `AA:BB:CC:DD:EE:FF`, and what [AndroidAutoWirelessConfig.phoneAddress] takes.
  final String address;

  /// What the device calls itself.
  final String name;

  /// Whether it is connected over Bluetooth right now. Says nothing about whether it
  /// is projecting.
  final bool connected;

  /// Whether its Bluetooth device class says it is a phone. A hint for sorting a
  /// picker, not a fact to depend on.
  final bool isPhone;

  /// Creates a description of a paired device.
  const AndroidAutoBluetoothDevice({
    required this.address,
    required this.name,
    this.connected = false,
    this.isPhone = false,
  });

  @override
  String toString() => 'AndroidAutoBluetoothDevice($name, $address)';
}

/// What the head unit resolved to and is telling phones.
///
/// The answer to "why is nothing happening". It shows whether the machine found a
/// network at all, and how far a phone has got.
class AndroidAutoWirelessStatus {
  /// The wireless interface being described.
  final String interfaceName;

  /// The network the phone is being told to join.
  final String ssid;

  /// That network's access point.
  final String bssid;

  /// The address the phone is being told to dial.
  final String ipAddress;

  /// The port it is being told to dial.
  final int port;

  /// Whether this machine is hosting the network rather than joined to it.
  final bool hosting;

  /// Whether the Bluetooth service is published. False means no phone can even find
  /// the head unit, which is usually BlueZ not running.
  final bool bluetoothReady;

  /// Whether a phone has opened the Bluetooth channel. True with no connection
  /// following means the phone heard the offer and could not act on it, which is a
  /// Wi-Fi problem rather than a Bluetooth one.
  final bool phoneLinked;

  /// Creates a description of what wireless is currently offering.
  const AndroidAutoWirelessStatus({
    required this.interfaceName,
    required this.ssid,
    required this.bssid,
    required this.ipAddress,
    required this.port,
    required this.hosting,
    required this.bluetoothReady,
    required this.phoneLinked,
  });

  @override
  String toString() =>
      'AndroidAutoWirelessStatus($ssid on $interfaceName, $ipAddress:$port'
      '${hosting ? ", hosting" : ""}'
      '${phoneLinked ? ", phone linked" : ""})';
}

/// The contract every platform implementation fulfils.
///
/// Implementations register themselves by assigning to [instance] from their
/// `registerWith` entry point.
abstract class AndroidAutoPlatform extends PlatformInterface {
  /// Subclasses must call this so [PlatformInterface] can verify them.
  AndroidAutoPlatform() : super(token: _token);

  static final Object _token = Object();

  static AndroidAutoPlatform? _instance;

  /// The implementation registered for the current platform.
  ///
  /// Throws if the platform has no implementation, rather than silently no-opping,
  /// because a head unit that quietly does nothing is worse than one that fails loudly.
  static AndroidAutoPlatform get instance {
    final instance = _instance;
    if (instance == null) {
      throw UnsupportedError(
        'android_auto has no implementation for this platform yet. '
        'Only Linux is supported today.',
      );
    }
    return instance;
  }

  /// Registers [instance] as the implementation for the current platform.
  static set instance(AndroidAutoPlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  /// Makes the head unit exist without starting it.
  ///
  /// Nothing is projected and no hardware is touched. What it does do, when
  /// [AndroidAutoConfig.transports] includes [AndroidAutoTransport.wireless], is
  /// publish the Bluetooth service and refuse every phone that asks for it.
  ///
  /// That is worth doing from the moment the application opens. A phone that knows
  /// this machine as a wireless car asks for the service every five seconds for as
  /// long as Bluetooth is connected, and shows its driver a notification saying it is
  /// connecting for as long as nothing answers. Refusing makes it stop; staying silent
  /// does not. Nothing is published when wireless is not in the transports, so an app
  /// that does not want it pays nothing and a phone paired in that state never learns
  /// the machine can project at all.
  ///
  /// Called for you by `AndroidAutoController`'s constructor, in the `android_auto`
  /// package. Calling [start] without it works exactly as before.
  Future<void> initialize(AndroidAutoConfig config) async {}

  /// Starts looking for a phone. Completes once the session is running, not once a
  /// phone has actually connected. Watch [events] for that.
  Future<void> start(AndroidAutoConfig config);

  /// Tears the session down and releases the transport.
  Future<void> stop();

  /// Every phone this machine is paired with, so an app can present a picker.
  ///
  /// Describes the machine rather than a session, so it works before [start] and
  /// after [stop]. Empty when the Bluetooth daemon cannot be reached, which on a
  /// machine with no Bluetooth is not a fault.
  Future<List<AndroidAutoBluetoothDevice>> pairedPhones() async =>
      const <AndroidAutoBluetoothDevice>[];

  /// Changes which network a phone is sent to.
  ///
  /// Takes effect the next time wireless starts, so a change while a phone is
  /// projecting does not disturb it.
  void setWirelessConfig(AndroidAutoWirelessConfig config) {}

  /// Publishes the Bluetooth service and opens the projection port.
  ///
  /// Called for you by [start] when [AndroidAutoConfig.transports] includes
  /// [AndroidAutoTransport.wireless]. This exists so an app can offer wireless as a
  /// switch the driver can turn off.
  ///
  /// Failures arrive on [events] rather than as an exception, because the interesting
  /// ones are about the machine rather than about the call: no Wi-Fi network, no
  /// address, no Bluetooth.
  Future<void> startWireless() async {}

  /// Stops offering wireless.
  ///
  /// Leaves a phone that is already projecting alone. A driver who switches wireless
  /// off halfway through a journey meant "do not start another one", not "cut this one
  /// off in a tunnel".
  ///
  /// The projection port closes, but the Bluetooth service stays published and phones
  /// that ask for it are told no. That is deliberate, and it is what stops the phone
  /// showing a permanent "connecting to Android Auto" notification: a phone that knows
  /// this machine as a wireless car asks for the service every five seconds for as
  /// long as Bluetooth is connected, and withdrawing the service only stops it being
  /// answered, not asked. Refusing makes it give up.
  ///
  /// To have the service never published at all, leave [AndroidAutoTransport.wireless]
  /// out of [AndroidAutoConfig.transports]. A phone paired while it is out never learns
  /// this machine can project, so it never asks in the first place.
  Future<void> stopWireless() async {}

  /// Whether wireless is being offered right now.
  bool get wirelessActive => false;

  /// What wireless resolved to, or null when it is not running.
  AndroidAutoWirelessStatus? get wirelessStatus => null;

  /// Lifecycle and error events from the native session.
  Stream<AndroidAutoEvent> get events;

  /// Id of the texture the projected video is rendered into, or null while there is
  /// no video stream.
  Future<int?> get textureId;

  /// The size and decoder of the incoming video, or null before the first frame.
  Future<AndroidAutoVideoInfo?> get videoInfo async => null;

  /// Tells the head unit the size of the view the projection is shown in, in physical
  /// pixels.
  ///
  /// Its shape decides the margins, see [AndroidAutoConfig.matchViewAspectRatio], and
  /// its size the frame when [AndroidAutoConfig.width] is left to the head unit.
  /// Callable at any time, before [start] included; the size last given is what a
  /// connecting phone is told. `AndroidAutoView` calls it on every layout.
  void setViewSize(double width, double height) {}

  /// Tells the head unit how many physical pixels the texture is drawn across: the part
  /// of the view it covers once fitted, not the view itself.
  ///
  /// A rendering hint. When the texture is drawn smaller than the video, the head unit
  /// shrinks it itself, averaging every pixel, rather than leaving it to Flutter, whose
  /// sampling reads four pixels per screen pixel and skips the rest, which is what makes
  /// small text and compression noise look grainy. `AndroidAutoView` calls this on every
  /// layout.
  void setDisplaySize(int width, int height) {}

  /// Feeds the video path from a generated pattern instead of a phone.
  ///
  /// Exists so the texture pipeline can be exercised without hardware, and so a host
  /// app can check its own overlay layout before a phone is ever plugged in. Does
  /// nothing once real video is running.
  Future<void> startTestPattern() async {}

  /// Stops the pattern started by [startTestPattern].
  Future<void> stopTestPattern() async {}

  /// Reports a touch to the phone.
  ///
  /// [pointers] carries every finger currently down, and [actionIndex] is the index
  /// within it of the finger this report is about, which only means anything for
  /// [AndroidAutoTouchAction.pointerDown] and [AndroidAutoTouchAction.pointerUp].
  ///
  /// Fire and forget, and synchronous on purpose: this is called straight out of a
  /// pointer callback, the protocol never acknowledges a report, and awaiting one
  /// would only add latency to a path that is measured in milliseconds. Does nothing
  /// while no phone is connected.
  void sendTouch(
    AndroidAutoTouchAction action,
    List<AndroidAutoTouchPoint> pointers, {
    int actionIndex = 0,
  }) {}

  /// Reports one hardware key transition.
  ///
  /// Down and up are separate calls, as they are on Android: a phone that gets a down
  /// and no up believes the button is still held. Use [pressKey] for the common case.
  void sendKey(AndroidAutoKey key, {required bool down, bool longPress = false}) {}

  /// Presses and releases [key].
  void pressKey(AndroidAutoKey key) {
    sendKey(key, down: true);
    sendKey(key, down: false);
  }

  /// Reports rotary encoder movement, in detents, positive clockwise.
  ///
  /// Sent as a relative axis rather than a key, which is what makes the phone scroll a
  /// list by steps instead of treating every detent as a button press.
  void sendRotary(int steps) {}

  /// Playback volume of one stream, 0.0 to 1.0.
  double audioVolume(AndroidAutoAudioStream stream) => 1.0;

  /// Sets the playback volume of one stream. Values outside 0.0 to 1.0 are clamped.
  ///
  /// Applied in software with a short ramp, so a change while music is playing is a
  /// fade rather than a click. Survives a phone disconnecting and reconnecting, because
  /// it describes the head unit rather than the phone.
  void setAudioVolume(AndroidAutoAudioStream stream, double volume) {}

  /// Whether one stream is muted.
  bool audioMuted(AndroidAutoAudioStream stream) => false;

  /// Mutes or unmutes one stream. A muted stream is played as silence rather than not
  /// played at all, so unmuting takes effect at once.
  void setAudioMuted(AndroidAutoAudioStream stream, bool muted) {}

  /// The outputs this machine offers, for a host app that wants to present a picker.
  ///
  /// Blocks briefly in the native layer, so it is a future. Empty when no audio server
  /// can be reached.
  Future<List<AndroidAutoAudioDevice>> audioDevices() async =>
      const <AndroidAutoAudioDevice>[];

  /// The selected output's [AndroidAutoAudioDevice.name], or empty for the default.
  String get audioDevice => '';

  /// Chooses the output to play through. Null or empty means the head unit's own
  /// speakers: the system default, except that a Bluetooth device is never picked for
  /// it, because a phone paired for hands free calling moves that default.
  void setAudioDevice(String? name) {}

  /// Whether the plugin is playing the phone's audio itself.
  bool get audioOutputEnabled => true;

  /// Turns the plugin's own playback off without touching the protocol.
  ///
  /// The phone keeps sending and [audioBuffers] keeps delivering, so this is what an
  /// app doing its own mixing, or an infotainment system routing audio through its own
  /// amplifier, turns off first.
  void setAudioOutputEnabled(bool enabled) {}

  /// Which audio backend is playing: `PulseAudio`, or `none` before the first buffer.
  String get audioBackend => 'none';

  /// Times this stream came close to running the speakers dry since the session
  /// started. Should stay at zero; anything else is an audible glitch.
  int audioUnderruns(AndroidAutoAudioStream stream) => 0;

  /// Buffers thrown away because the phone sent faster than they could be played.
  int audioDropped(AndroidAutoAudioStream stream) => 0;

  /// How far behind the head unit the speakers are.
  Duration audioLatency(AndroidAutoAudioStream stream) => Duration.zero;

  /// The phone's PCM, before this head unit touches it.
  ///
  /// Only worth listening to for an app that mixes the audio itself; the plugin plays
  /// these already. Listening costs a copy per buffer, so nothing is delivered until
  /// something is listening.
  Stream<AndroidAutoAudioBuffer> get audioBuffers =>
      const Stream<AndroidAutoAudioBuffer>.empty();

  /// Whether the phone has the head unit's microphone open right now.
  ///
  /// This is what a "listening" indicator shows, and it is the state of the device
  /// rather than of the channel: a phone that has opened the microphone channel but has
  /// not asked to record reads false. It goes true when the Assistant is invoked, by
  /// "Hey Google" or by [AndroidAutoKey.microphone], and false when it is finished.
  ///
  /// There is no call to turn it on. The phone asks, the head unit captures, and
  /// nothing else opens the device: a head unit whose host app could start recording
  /// would be a different and much worse thing than one that listens when asked.
  bool get microphoneActive => false;

  /// Peak level of the most recently captured buffer, 0.0 to 1.0, for a level meter.
  /// Zero while [microphoneActive] is false.
  double get microphoneLevel => 0.0;

  /// Bytes captured since the session was created.
  ///
  /// Worth showing next to [microphoneActive] while a head unit is being brought up: an
  /// Assistant that hears nothing looks the same whether the microphone is missing or
  /// the audio is not arriving, and this tells the two apart.
  int get microphoneBytes => 0;

  /// The inputs this machine offers, for a host app that wants to present a picker.
  ///
  /// Blocks briefly in the native layer, so it is a future. Empty when no audio server
  /// can be reached. Monitors of outputs are left out: they would let the head unit send
  /// the phone its own audio back.
  Future<List<AndroidAutoAudioDevice>> microphoneDevices() async =>
      const <AndroidAutoAudioDevice>[];

  /// The selected input's [AndroidAutoAudioDevice.name], or empty for the default.
  String get microphoneDevice => '';

  /// Chooses the input to capture from. Null or empty means the head unit's own
  /// microphone: the system default, with Bluetooth devices ruled out for the reason
  /// [setAudioDevice] gives.
  ///
  /// Takes effect the next time the phone asks for the microphone, because that is the
  /// only moment the plugin is allowed to open one.
  void setMicrophoneDevice(String? name) {}

  /// Which capture backend is running: `PulseAudio`, or `none` before the first capture
  /// or on a machine with no microphone.
  String get microphoneBackend => 'none';

  /// Whether the head unit is telling the phone it is dark outside.
  ///
  /// Drives the phone's own light and dark theme, so it is what makes the projection
  /// match a dashboard that dims at dusk. False, meaning day, until the app says
  /// otherwise.
  bool get nightMode => false;

  /// Says whether it is dark outside.
  ///
  /// Nothing here reads a light sensor: this plugin owns no hardware, and the app is
  /// the thing running in the vehicle. A sunset table, a photodiode or the car's own
  /// headlight switch are all reasonable sources, and all of them are the app's to
  /// choose.
  void setNightMode(bool night) {}

  /// What the car currently forbids the phone to do. Empty means parked.
  Set<AndroidAutoDrivingRestriction> get drivingRestrictions =>
      const <AndroidAutoDrivingRestriction>{};

  /// Says what the car forbids right now.
  ///
  /// The phone locks the matching parts of its interface at once and the person in the
  /// car cannot override it, so this is a safety decision rather than a preference.
  /// Empty is a parked car, which is what a head unit reports until told otherwise.
  /// [setParked] covers the usual two cases.
  void setDrivingRestrictions(Set<AndroidAutoDrivingRestriction> restrictions) {}

  /// The usual two cases: parked lifts every restriction, moving applies the set
  /// Android Auto's own guidelines describe.
  ///
  /// Moving is video, keyboard and configuration: no moving pictures, no on screen
  /// keyboard and no settings screens. Voice input and message length are deliberately
  /// left alone, because voice is the one interaction that is safe while driving and
  /// truncating messages is a choice about content rather than about safety. An app
  /// that wants a different combination sets it with [setDrivingRestrictions].
  void setParked(bool parked) => setDrivingRestrictions(
    parked
        ? const <AndroidAutoDrivingRestriction>{}
        : const {
          AndroidAutoDrivingRestriction.video,
          AndroidAutoDrivingRestriction.keyboard,
          AndroidAutoDrivingRestriction.configuration,
        },
  );

  /// The last position given to [setLocation], or null if there has never been one.
  AndroidAutoLocation? get location => null;

  /// Reports where the car is.
  ///
  /// Only meaningful when [AndroidAutoSensor.location] is in
  /// [AndroidAutoConfig.sensors], and then it matters a great deal: the phone will have
  /// stopped using its own receiver and will be navigating from these. Send a fix
  /// whenever one arrives, typically once a second.
  void setLocation(AndroidAutoLocation location) {}

  /// Road speed in metres per second.
  void setSpeed(double metresPerSecond) {}

  /// Engine speed in revolutions per minute.
  void setRpm(double rpm) {}

  /// Tank level as a percentage of full, remaining range in metres, and whether the low
  /// fuel warning is lit.
  void setFuel({
    required double levelPercent,
    required double rangeMetres,
    bool low = false,
  }) {}

  /// Whether the parking brake is engaged.
  void setParkingBrake(bool engaged) {}

  /// The selected gear, as the protocol numbers them: 0 neutral, 1 to 10 the numbered
  /// gears, 100 drive, 101 park, 102 reverse.
  void setGear(int gear) {}

  /// Which way the car is pointing, in degrees clockwise from north.
  ///
  /// Not the same as [AndroidAutoLocation.bearingDegrees], which is the direction it is
  /// moving. A car reversing points one way and travels the other.
  void setCompass(double bearingDegrees) {}

  /// Outside temperature in degrees Celsius and barometric pressure in kilopascals.
  /// Either may be null, which leaves that one out of the reading.
  void setEnvironment({double? temperatureCelsius, double? pressureKpa}) {}

  /// Total distance travelled, in kilometres.
  void setOdometer(double kilometres) {}

  /// Whether a toll transponder is in the car.
  void setTollCard(bool present) {}

  /// Which sensors the phone has subscribed to, empty when none is connected.
  ///
  /// Never the same question as [AndroidAutoConfig.sensors]: a phone takes what it
  /// wants from what was offered. This is the first thing to look at when a value is
  /// being set and nothing on the phone's screen changes.
  Set<AndroidAutoSensor> get sensorSubscriptions => const <AndroidAutoSensor>{};

  /// Sensor readings written to the phone since the session was created.
  ///
  /// The "did anything actually go out" number, which is otherwise only answerable by
  /// watching the phone's own UI.
  int get sensorBatches => 0;

  // === metadata ===
  //
  // What the phone is doing, as opposed to what it looks like. The point of the whole
  // plugin: a host app draws its own turn card, now playing bar and call banner from
  // these rather than only mirroring pixels.
  //
  // None of it survives a disconnection. What the phone is playing stops being true the
  // moment it is unplugged, so the streams fall silent and the snapshots go empty
  // rather than showing a track that finished an hour ago.

  /// Turn by turn guidance, as it changes.
  ///
  /// A snapshot per update, with several protocol messages already merged into one
  /// object: the phone sends the shape of the turn and the distance to it separately.
  /// Check [AndroidAutoNavigation.isGuiding] before drawing anything.
  Stream<AndroidAutoNavigation> get navigation =>
      const Stream<AndroidAutoNavigation>.empty();

  /// The last navigation update, or null if the phone has said nothing.
  AndroidAutoNavigation? get lastNavigation => null;

  /// The track playing and what is being done with it, as it changes.
  Stream<AndroidAutoMediaInfo> get mediaPlayback =>
      const Stream<AndroidAutoMediaInfo>.empty();

  /// The last media update, or null if the phone has said nothing.
  AndroidAutoMediaInfo? get lastMediaInfo => null;

  /// Calls in progress, as they come and go.
  ///
  /// Read only. A call's audio never touches the projection link: it goes over
  /// Bluetooth hands free, so answering and hanging up belong there.
  Stream<AndroidAutoPhoneStatus> get phoneStatus =>
      const Stream<AndroidAutoPhoneStatus>.empty();

  /// The last telephony update, or null if the phone has said nothing.
  AndroidAutoPhoneStatus? get lastPhoneStatus => null;

  /// Messages the phone asks the head unit to show.
  ///
  /// Only delivered when [AndroidAutoMetadata.notification] is in
  /// [AndroidAutoConfig.metadata]. Each one is acknowledged by the plugin as soon as it
  /// is handed over, so a host app has nothing to do but show it.
  Stream<AndroidAutoNotification> get notifications =>
      const Stream<AndroidAutoNotification>.empty();

  /// Answers to [browse].
  Stream<AndroidAutoBrowseNode> get browseResults =>
      const Stream<AndroidAutoBrowseNode>.empty();

  /// Asks the phone for one node of its media library.
  ///
  /// [path] is empty for the root and otherwise a path out of a previous answer;
  /// [start] is the offset into a long list. The answer arrives on [browseResults]
  /// rather than being returned, because it is a round trip to the phone.
  ///
  /// Returns false when the browser channel is not open, which is the normal answer
  /// whenever no phone is connected or [AndroidAutoMetadata.browse] is not in
  /// [AndroidAutoConfig.metadata].
  bool browse({String path = '', int start = 0}) => false;

  /// Tells the phone the user picked [path] in the media library, which is what makes
  /// it play. Returns false on the same terms as [browse].
  bool browseSelect(String path) => false;

  /// Which metadata channels the phone actually opened, empty when none is connected.
  ///
  /// Never the same question as [AndroidAutoConfig.metadata]: a phone takes what it
  /// wants from what was offered. The first thing to look at when nothing is arriving.
  Set<AndroidAutoMetadata> get metadataChannels => const <AndroidAutoMetadata>{};

  /// Updates received on one metadata channel since the session was created.
  ///
  /// The "is anything coming in at all" number, which separates a phone that is not
  /// sending from a host app that is not listening.
  int metadataUpdates(AndroidAutoMetadata kind) => 0;

  /// Releases everything the implementation holds.
  ///
  /// Distinct from [stop]: a stopped session can be started again, a disposed one
  /// cannot. Host apps normally call this only when shutting down.
  Future<void> dispose() async {}
}
