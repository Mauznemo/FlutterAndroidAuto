// SPDX-License-Identifier: GPL-3.0-or-later
import 'dart:async';

import 'package:android_auto_platform_interface/android_auto_platform_interface.dart';
import 'package:flutter/foundation.dart';

/// Starts, stops and observes a head unit session.
///
/// A host app normally creates one of these for the lifetime of the app and hands it
/// to an [AndroidAutoView].
class AndroidAutoController extends ChangeNotifier {
  /// How the head unit describes itself to the phone.
  final AndroidAutoConfig config;

  late final StreamSubscription<AndroidAutoEvent> _subscription;
  final List<StreamSubscription<void>> _metadataSubscriptions = [];
  AndroidAutoConnectionState _state = AndroidAutoConnectionState.idle;
  String? _message;
  int? _textureId;
  AndroidAutoVideoInfo? _videoInfo;

  /// Creates a controller. Nothing is projected until [start] is called.
  ///
  /// One thing does happen straight away, and only when [AndroidAutoConfig.transports]
  /// includes [AndroidAutoTransport.wireless]: the Bluetooth service is published and
  /// every phone that asks for it is refused. A phone that knows this machine as a
  /// wireless car asks every five seconds for as long as Bluetooth is connected, and
  /// shows its driver a notification saying it is connecting for as long as nothing
  /// answers, so a head unit that is open and not projecting should be saying no
  /// rather than saying nothing. See [AndroidAutoPlatform.initialize].
  AndroidAutoController({this.config = const AndroidAutoConfig()}) {
    _subscription = _platform.events.listen(_onEvent);
    // Deliberately not awaited: a constructor cannot, and nothing else here depends
    // on it having finished.
    unawaited(_platform.initialize(config));
    // Subscribed here rather than left to the host app, so that a widget which only
    // listens to this ChangeNotifier repaints when a track or a turn changes. The
    // streams are broadcast, so an app that wants the values themselves still
    // subscribes to them directly.
    _metadataSubscriptions.addAll([
      _platform.navigation.listen((_) => notifyListeners()),
      _platform.mediaPlayback.listen((_) => notifyListeners()),
      _platform.phoneStatus.listen((_) => notifyListeners()),
    ]);
  }

  AndroidAutoPlatform get _platform => AndroidAutoPlatform.instance;

  /// Lifecycle and error events from the native session.
  Stream<AndroidAutoEvent> get events => _platform.events;

  /// Where the session is in its lifecycle.
  AndroidAutoConnectionState get state => _state;

  /// Detail for the current [state], typically only set when it is
  /// [AndroidAutoConnectionState.error].
  String? get message => _message;

  /// Id of the texture carrying the projected video, or null while there is none.
  int? get textureId => _textureId;

  /// The size and decoder of the incoming video, or null before the first frame.
  ///
  /// Prefer this over [config] when laying the projection out: the config is what was
  /// asked for, this is what arrived.
  AndroidAutoVideoInfo? get videoInfo => _videoInfo;

  /// Begins looking for a phone.
  Future<void> start() async {
    await _platform.start(config);
    await _refreshVideoState();
  }

  /// Ends the session.
  Future<void> stop() async {
    await _platform.stop();
    _textureId = null;
    _videoInfo = null;
    notifyListeners();
  }

  /// Every phone this machine is paired with over Bluetooth.
  ///
  /// Describes the machine rather than the session, so it answers before [start] and
  /// after [stop]. Use it to fill a picker whose choice goes into
  /// [AndroidAutoWirelessConfig.phoneAddress].
  Future<List<AndroidAutoBluetoothDevice>> pairedPhones() =>
      _platform.pairedPhones();

  /// Publishes the Bluetooth service and opens the projection port.
  ///
  /// Done for you by [start] when [AndroidAutoConfig.transports] includes
  /// [AndroidAutoTransport.wireless]. Call this directly to offer wireless as
  /// something the driver can switch on and off.
  Future<void> startWireless() async {
    await _platform.startWireless();
    notifyListeners();
  }

  /// Stops offering wireless. A phone already projecting keeps its connection.
  ///
  /// The Bluetooth service stays published and phones that ask are refused, which is
  /// what stops the phone showing a permanent "connecting to Android Auto" notice.
  /// See [AndroidAutoPlatform.stopWireless].
  Future<void> stopWireless() async {
    await _platform.stopWireless();
    notifyListeners();
  }

  /// Changes which Wi-Fi network a phone is sent to. Applies the next time wireless
  /// starts, so it never disturbs a phone that is projecting.
  void setWirelessConfig(AndroidAutoWirelessConfig config) {
    _platform.setWirelessConfig(config);
    notifyListeners();
  }

  /// Whether wireless is being offered right now.
  bool get wirelessActive => _platform.wirelessActive;

  /// What wireless resolved to, or null when it is not running.
  ///
  /// The thing to put on screen when a phone will not connect: it says whether the
  /// machine found a network, what it is telling phones to join, and how far the
  /// phone has got.
  AndroidAutoWirelessStatus? get wirelessStatus => _platform.wirelessStatus;

  /// Drives the video path from a generated pattern instead of a phone.
  ///
  /// Useful for laying out an overlay before any hardware is involved.
  Future<void> startTestPattern() async {
    await _platform.startTestPattern();
    await _refreshVideoState();
  }

  /// Stops the pattern started by [startTestPattern].
  Future<void> stopTestPattern() => _platform.stopTestPattern();

  /// Reports a touch to the phone.
  ///
  /// [AndroidAutoView] calls this for every pointer event over the projection, so an
  /// app that uses the view has nothing to do here. Calling it directly is for head
  /// units with a real touchscreen wired up some other way, and it is then the
  /// caller's job to put the coordinates in projected video pixels: see
  /// [AndroidAutoTouchPoint].
  void sendTouch(
    AndroidAutoTouchAction action,
    List<AndroidAutoTouchPoint> pointers, {
    int actionIndex = 0,
  }) => _platform.sendTouch(action, pointers, actionIndex: actionIndex);

  /// Presses and releases a hardware key.
  ///
  /// This is how a head unit's steering wheel and dashboard buttons reach the phone.
  void pressKey(AndroidAutoKey key) => _platform.pressKey(key);

  /// Reports one half of a key press, for a button that can be held.
  void sendKey(AndroidAutoKey key, {required bool down, bool longPress = false}) =>
      _platform.sendKey(key, down: down, longPress: longPress);

  /// Reports rotary encoder movement, in detents, positive clockwise.
  ///
  /// Pair it with [pressKey] and [AndroidAutoKey.enter] for the encoder's push.
  void sendRotary(int steps) => _platform.sendRotary(steps);

  /// Playback volume of one audio stream, 0.0 to 1.0.
  double volume(AndroidAutoAudioStream stream) => _platform.audioVolume(stream);

  /// Sets the playback volume of one audio stream.
  ///
  /// The three streams are separate because the phone sends them separately. Media is
  /// ducked automatically while speech is playing, so an app does not have to do that
  /// itself.
  void setVolume(AndroidAutoAudioStream stream, double volume) {
    _platform.setAudioVolume(stream, volume);
    notifyListeners();
  }

  /// Whether one audio stream is muted.
  bool muted(AndroidAutoAudioStream stream) => _platform.audioMuted(stream);

  /// Mutes or unmutes one audio stream.
  void setMuted(AndroidAutoAudioStream stream, bool muted) {
    _platform.setAudioMuted(stream, muted);
    notifyListeners();
  }

  /// The audio outputs this machine offers, for presenting a picker.
  Future<List<AndroidAutoAudioDevice>> audioDevices() => _platform.audioDevices();

  /// The selected output's [AndroidAutoAudioDevice.name], or empty for the default.
  String get audioDevice => _platform.audioDevice;

  /// Chooses the audio output. Null or empty means the head unit's own speakers: the
  /// system default, except that a Bluetooth device is never picked for it. A phone
  /// paired for hands free calling moves that default, and the car's speakers are not
  /// the phone's to move. Naming a Bluetooth device explicitly still works.
  void setAudioDevice(String? name) {
    _platform.setAudioDevice(name);
    notifyListeners();
  }

  /// Whether the plugin plays the phone's audio itself.
  bool get audioOutputEnabled => _platform.audioOutputEnabled;

  /// Turns the plugin's own playback off without touching the protocol.
  ///
  /// For an infotainment system that routes audio through its own amplifier, or an app
  /// that mixes [audioBuffers] itself.
  void setAudioOutputEnabled(bool enabled) {
    _platform.setAudioOutputEnabled(enabled);
    notifyListeners();
  }

  /// Which audio backend is playing: `PulseAudio`, or `none` before the first buffer.
  String get audioBackend => _platform.audioBackend;

  /// Times a stream came close to running the speakers dry since the session started.
  /// Should stay at zero.
  int audioUnderruns(AndroidAutoAudioStream stream) =>
      _platform.audioUnderruns(stream);

  /// Buffers thrown away because the phone sent faster than they could be played.
  int audioDropped(AndroidAutoAudioStream stream) => _platform.audioDropped(stream);

  /// How far behind the head unit the speakers are.
  Duration audioLatency(AndroidAutoAudioStream stream) =>
      _platform.audioLatency(stream);

  /// The phone's PCM, before this head unit touches it.
  ///
  /// Only worth listening to for an app that mixes the audio itself, which should pair
  /// it with `setAudioOutputEnabled(false)`. Nothing is delivered while nothing is
  /// listening.
  Stream<AndroidAutoAudioBuffer> get audioBuffers => _platform.audioBuffers;

  /// Whether the phone has the head unit's microphone open right now.
  ///
  /// Goes true when the Assistant is invoked, by "Hey Google" or by
  /// [AndroidAutoKey.microphone], and false when it has finished. Show it: this is the
  /// only signal a person in the car has that the machine is listening.
  ///
  /// There is no call to turn it on. The microphone is opened when the phone asks and at
  /// no other time.
  bool get microphoneActive => _platform.microphoneActive;

  /// Peak level of the most recently captured audio, 0.0 to 1.0, for a level meter.
  /// Zero while [microphoneActive] is false.
  ///
  /// Read it on a timer rather than waiting to be notified: it changes with every 32 ms
  /// buffer, which is far too often to rebuild a widget tree for.
  double get microphoneLevel => _platform.microphoneLevel;

  /// Bytes captured since the session started. Tells a microphone that is missing apart
  /// from one whose audio is not reaching the phone.
  int get microphoneBytes => _platform.microphoneBytes;

  /// The inputs this machine offers, for presenting a picker.
  Future<List<AndroidAutoAudioDevice>> microphoneDevices() =>
      _platform.microphoneDevices();

  /// The selected input's [AndroidAutoAudioDevice.name], or empty for the default.
  String get microphoneDevice => _platform.microphoneDevice;

  /// Chooses the input to capture from. Null or empty means the head unit's own
  /// microphone: the system default, with Bluetooth devices ruled out for the reason
  /// [setAudioDevice] gives.
  ///
  /// Takes effect the next time the phone asks for the microphone, which is the only
  /// moment the plugin opens one.
  void setMicrophoneDevice(String? name) {
    _platform.setMicrophoneDevice(name);
    notifyListeners();
  }

  /// Which capture backend is running: `PulseAudio`, or `none` before the first capture.
  String get microphoneBackend => _platform.microphoneBackend;

  /// Whether the head unit is telling the phone it is dark outside.
  bool get nightMode => _platform.nightMode;

  /// Says whether it is dark outside, which drives the phone's own light and dark
  /// theme.
  ///
  /// Nothing in this plugin reads a light sensor. The app is the thing running in the
  /// vehicle, so where this comes from, a sunset table, a photodiode or the headlight
  /// switch, is the app's choice.
  void setNightMode(bool night) {
    _platform.setNightMode(night);
    notifyListeners();
  }

  /// What the car currently forbids the phone to do. Empty means parked.
  Set<AndroidAutoDrivingRestriction> get drivingRestrictions =>
      _platform.drivingRestrictions;

  /// Says what the car forbids right now.
  ///
  /// The phone locks the matching parts of its interface at once and nobody in the car
  /// can override it, so this is a safety decision rather than a preference. Use
  /// [setParked] for the usual two cases.
  void setDrivingRestrictions(Set<AndroidAutoDrivingRestriction> restrictions) {
    _platform.setDrivingRestrictions(restrictions);
    notifyListeners();
  }

  /// Parked lifts every restriction; moving applies video, keyboard and configuration.
  void setParked(bool parked) {
    _platform.setParked(parked);
    notifyListeners();
  }

  /// The last position reported with [setLocation], or null if there has been none.
  AndroidAutoLocation? get location => _platform.location;

  /// Reports where the car is.
  ///
  /// Only meaningful when [AndroidAutoSensor.location] is in
  /// [AndroidAutoConfig.sensors], and then it matters: the phone has stopped using its
  /// own receiver and is navigating from these.
  void setLocation(AndroidAutoLocation location) {
    _platform.setLocation(location);
    notifyListeners();
  }

  /// Road speed in metres per second.
  void setSpeed(double metresPerSecond) => _platform.setSpeed(metresPerSecond);

  /// Engine speed in revolutions per minute.
  void setRpm(double rpm) => _platform.setRpm(rpm);

  /// Tank level as a percentage of full, remaining range in metres, and whether the low
  /// fuel warning is lit.
  void setFuel({
    required double levelPercent,
    required double rangeMetres,
    bool low = false,
  }) => _platform.setFuel(
    levelPercent: levelPercent,
    rangeMetres: rangeMetres,
    low: low,
  );

  /// Whether the parking brake is engaged.
  void setParkingBrake(bool engaged) => _platform.setParkingBrake(engaged);

  /// The selected gear: 0 neutral, 1 to 10 the numbered gears, 100 drive, 101 park,
  /// 102 reverse.
  void setGear(int gear) => _platform.setGear(gear);

  /// Which way the car points, in degrees clockwise from north. Not the direction it is
  /// moving, which is [AndroidAutoLocation.bearingDegrees].
  void setCompass(double bearingDegrees) => _platform.setCompass(bearingDegrees);

  /// Outside temperature in degrees Celsius and barometric pressure in kilopascals.
  void setEnvironment({double? temperatureCelsius, double? pressureKpa}) =>
      _platform.setEnvironment(
        temperatureCelsius: temperatureCelsius,
        pressureKpa: pressureKpa,
      );

  /// Total distance travelled, in kilometres.
  void setOdometer(double kilometres) => _platform.setOdometer(kilometres);

  /// Whether a toll transponder is in the car.
  void setTollCard(bool present) => _platform.setTollCard(present);

  /// Which sensors the phone has subscribed to, empty when none is connected.
  ///
  /// Never the same as [AndroidAutoConfig.sensors]: a phone takes what it wants from
  /// what was offered, and the gap between the two is the first thing to check when a
  /// value is being set and nothing on the phone changes.
  Set<AndroidAutoSensor> get sensorSubscriptions => _platform.sensorSubscriptions;

  /// Sensor readings written to the phone since the session was created.
  int get sensorBatches => _platform.sensorBatches;

  // === metadata ===
  //
  // What the phone is doing rather than what it looks like. These are what a host app
  // draws its own turn card, now playing bar and call banner from.
  //
  // The three snapshots go null when a session ends, because a phone that has been
  // unplugged is not playing anything, and every one of them notifies this controller
  // so a widget that only listens here repaints without subscribing to a stream.

  /// Turn by turn guidance, as it changes.
  ///
  /// Check [AndroidAutoNavigation.isGuiding] before drawing: an instruction left on
  /// screen after guidance has ended is the one mistake a head unit can make that
  /// actively misleads a driver.
  Stream<AndroidAutoNavigation> get navigation => _platform.navigation;

  /// The latest guidance, or null while the phone has said nothing.
  AndroidAutoNavigation? get lastNavigation => _platform.lastNavigation;

  /// The track playing and what is being done with it, as it changes.
  Stream<AndroidAutoMediaInfo> get mediaPlayback => _platform.mediaPlayback;

  /// The latest track, or null while the phone has said nothing.
  AndroidAutoMediaInfo? get lastMediaInfo => _platform.lastMediaInfo;

  /// Calls in progress, as they come and go.
  ///
  /// Read only: a call's audio goes over Bluetooth hands free and never touches the
  /// projection link, so answering and hanging up belong there.
  Stream<AndroidAutoPhoneStatus> get phoneStatus => _platform.phoneStatus;

  /// The latest telephony state, or null while the phone has said nothing.
  AndroidAutoPhoneStatus? get lastPhoneStatus => _platform.lastPhoneStatus;

  /// Messages the phone asks the head unit to show. Needs
  /// [AndroidAutoMetadata.notification] in [AndroidAutoConfig.metadata].
  Stream<AndroidAutoNotification> get notifications => _platform.notifications;

  /// Answers to [browse].
  Stream<AndroidAutoBrowseNode> get browseResults => _platform.browseResults;

  /// Asks the phone for one node of its media library.
  ///
  /// [path] is empty for the root and otherwise a path out of a previous answer. The
  /// answer arrives on [browseResults] rather than being returned, because it is a
  /// round trip to the phone. Needs [AndroidAutoMetadata.browse] in
  /// [AndroidAutoConfig.metadata].
  bool browse({String path = '', int start = 0}) =>
      _platform.browse(path: path, start: start);

  /// Tells the phone the user picked [path] in the media library, which is what makes
  /// it play.
  bool browseSelect(String path) => _platform.browseSelect(path);

  /// Which metadata channels the phone actually opened, empty when none is connected.
  ///
  /// Never the same as [AndroidAutoConfig.metadata]: a phone takes what it wants from
  /// what was offered, and the gap is the first thing to check when nothing arrives.
  Set<AndroidAutoMetadata> get metadataChannels => _platform.metadataChannels;

  /// Updates received on one metadata channel since the session was created.
  int metadataUpdates(AndroidAutoMetadata kind) => _platform.metadataUpdates(kind);

  /// Re-reads the texture id and the incoming video description from the platform.
  Future<void> _refreshVideoState() async {
    final id = await _platform.textureId;
    final info = await _platform.videoInfo;
    if (id != _textureId || info != _videoInfo) {
      _textureId = id;
      _videoInfo = info;
      notifyListeners();
    }
  }

  void _onEvent(AndroidAutoEvent event) {
    _state = event.state;
    _message = event.message;
    notifyListeners();
    // The texture is registered natively the moment video starts, so an event is the
    // earliest point at which asking for the id is worthwhile. The video size arrives
    // later still, on the event the decoder raises once it has a frame.
    _refreshVideoState();
  }

  @override
  void dispose() {
    _subscription.cancel();
    for (final subscription in _metadataSubscriptions) {
      subscription.cancel();
    }
    _metadataSubscriptions.clear();
    _platform.dispose();
    super.dispose();
  }
}
