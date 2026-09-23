// SPDX-License-Identifier: GPL-3.0-or-later
import 'dart:async';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:android_auto_platform_interface/android_auto_platform_interface.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/widgets.dart';

import 'simulated_art.dart';

/// The screens the simulated phone can show, one per button on its rail.
enum SimulatedApp {
  /// The map, with guidance when a route is running.
  maps,

  /// Now playing.
  media,

  /// Contacts, and the call screen while a call is up.
  phone,

  /// The grid of apps.
  launcher,
}

/// Something on the simulated screen that reacts to a tap.
///
/// Put on the screen inside a [MetaData] widget, which is what lets [SimulatedPhone]
/// find it with an ordinary hit test at the position a touch report names. So a tap
/// reaches it the way it reaches a real phone: as projected video pixels through
/// `sendTouch`, never as a Flutter gesture.
class SimulatedTarget {
  /// Stable across rebuilds, so a press and its release can be matched up.
  final String id;

  /// What the phone does when this is tapped.
  final VoidCallback onTap;

  /// Creates a tap target.
  const SimulatedTarget(this.id, this.onTap);
}

/// A tap that has just lifted, drawn as a short ring so a tap is visible after the fact.
class SimulatedRipple {
  /// Distinguishes two ripples at the same place.
  final int id;

  /// Where it lifted, in the screen's layout coordinates.
  final Offset position;

  /// Creates a ripple.
  const SimulatedRipple(this.id, this.position);
}

/// One stretch of the simulated route: a road driven along, and the turn at its end.
class SimulatedLeg {
  /// The road being driven along.
  final String road;

  /// Its length.
  final int metres;

  /// How fast it is driven, in metres per second.
  final double speed;

  /// The turn at its end.
  final AndroidAutoManeuver maneuver;

  /// The road that turn leads onto.
  final String next;

  /// The instruction as the phone would word it.
  final String cue;

  /// Which exit, when [maneuver] is a roundabout.
  final int? exit;

  /// The lanes approaching the turn, left to right.
  final List<AndroidAutoLane> lanes;

  /// Creates a leg.
  const SimulatedLeg({
    required this.road,
    required this.metres,
    required this.speed,
    required this.maneuver,
    required this.next,
    required this.cue,
    this.exit,
    this.lanes = const <AndroidAutoLane>[],
  });
}

/// One track in the simulated library.
class SimulatedTrack {
  /// Title.
  final String song;

  /// Artist.
  final String artist;

  /// Album.
  final String album;

  /// Length.
  final Duration duration;

  /// The two colours its cover is drawn in.
  final List<Color> colors;

  /// Creates a track.
  const SimulatedTrack(this.song, this.artist, this.album, this.duration, this.colors);
}

/// One entry in the simulated contact list.
class SimulatedContact {
  /// Name.
  final String name;

  /// Number, from the ranges set aside for fiction so none of them rings anybody.
  final String number;

  /// Creates a contact.
  const SimulatedContact(this.name, this.number);
}

/// What a connected phone is doing, standing in for a real one.
///
/// Holds the state the simulated screen draws, reacts to touches and keys the way a
/// phone would, and reports what it is doing through the same metadata a real phone
/// sends, so a host app's own turn card, now playing bar and call banner can be built
/// against it. Nothing here touches audio or hardware: the music is silent and the
/// Assistant listens to nothing.
class SimulatedPhone extends ChangeNotifier {
  /// Called with every guidance update, as the navigation channel would deliver it.
  final void Function(AndroidAutoNavigation navigation) onNavigation;

  /// Called with every playback update.
  final void Function(AndroidAutoMediaInfo media) onMedia;

  /// Called with every telephony update.
  final void Function(AndroidAutoPhoneStatus status) onPhoneStatus;

  /// The route every simulated guidance session drives, a few minutes of it.
  static const List<SimulatedLeg> route = [
    SimulatedLeg(
      road: 'Main Street',
      metres: 300,
      speed: 12,
      maneuver: AndroidAutoManeuver.turnNormalRight,
      next: 'Station Road',
      cue: 'Turn right onto Station Road',
    ),
    SimulatedLeg(
      road: 'Station Road',
      metres: 520,
      speed: 13,
      maneuver: AndroidAutoManeuver.roundaboutEnterAndExitCcw,
      next: 'Ring Road',
      cue: 'At the roundabout, take the 2nd exit onto Ring Road',
      exit: 2,
    ),
    SimulatedLeg(
      road: 'Ring Road',
      metres: 700,
      speed: 16,
      maneuver: AndroidAutoManeuver.keepLeft,
      next: 'A1',
      cue: 'Keep left to join the A1',
      lanes: [
        AndroidAutoLane(
          directions: [
            AndroidAutoLaneDirection(
              shape: AndroidAutoLaneShape.slightLeft,
              highlighted: true,
            ),
          ],
        ),
        AndroidAutoLane(
          directions: [
            AndroidAutoLaneDirection(
              shape: AndroidAutoLaneShape.slightLeft,
              highlighted: true,
            ),
            AndroidAutoLaneDirection(shape: AndroidAutoLaneShape.straight),
          ],
        ),
        AndroidAutoLane(
          directions: [AndroidAutoLaneDirection(shape: AndroidAutoLaneShape.straight)],
        ),
      ],
    ),
    SimulatedLeg(
      road: 'A1',
      metres: 2400,
      speed: 30,
      maneuver: AndroidAutoManeuver.offRampNormalRight,
      next: 'Airport Road',
      cue: 'Take exit 12 towards Airport Road',
    ),
    SimulatedLeg(
      road: 'Airport Road',
      metres: 600,
      speed: 15,
      maneuver: AndroidAutoManeuver.turnNormalLeft,
      next: 'Park Lane',
      cue: 'Turn left onto Park Lane',
    ),
    SimulatedLeg(
      road: 'Park Lane',
      metres: 250,
      speed: 9,
      maneuver: AndroidAutoManeuver.destinationRight,
      next: '12 Park Lane',
      cue: 'Your destination is on the right',
    ),
  ];

  /// Where [route] ends.
  static const String destination = '12 Park Lane';

  /// How long [route] takes to drive, rounded up the way a phone rounds an arrival.
  static int get routeMinutes =>
      (route.fold<double>(0, (sum, leg) => sum + leg.metres / leg.speed) / 60).ceil();

  /// The simulated music library. Made up, so nothing here is anybody's recording.
  static const List<SimulatedTrack> tracks = [
    SimulatedTrack(
      'Midnight Drive',
      'The Simulators',
      'Test Signals',
      Duration(minutes: 3, seconds: 34),
      [Color(0xFF3F51B5), Color(0xFFE91E63)],
    ),
    SimulatedTrack(
      'Open Road',
      'Placeholder Band',
      'Mock Sessions',
      Duration(minutes: 3, seconds: 7),
      [Color(0xFF00897B), Color(0xFFFFC107)],
    ),
    SimulatedTrack(
      'Signal Lost',
      'Null Pointer',
      'Stack Traces',
      Duration(minutes: 4, seconds: 3),
      [Color(0xFF6D4C41), Color(0xFF26C6DA)],
    ),
  ];

  /// The simulated contact list.
  static const List<SimulatedContact> contacts = [
    SimulatedContact('Alex Morgan', '+44 20 7946 0018'),
    SimulatedContact('Sam Taylor', '+44 20 7946 0257'),
    SimulatedContact('Jordan Lee', '+44 20 7946 0633'),
  ];

  /// Who rings when [simulateIncomingCall] is used.
  static const SimulatedContact caller = SimulatedContact(
    'Robin Park',
    '+44 20 7946 0941',
  );

  /// Cover art per track, drawn once per process.
  static final Map<int, Uint8List> _art = {};

  SimulatedApp _app = SimulatedApp.maps;
  bool _night = false;
  bool _connected = false;
  bool _disposed = false;
  Timer? _clock;
  Timer? _assistantTimer;

  int? _leg;
  double _legTravelled = 0;
  AndroidAutoNavigation? _navigation;

  int _track = 0;
  bool _playing = false;
  bool _resumeAfterCall = false;
  Duration _position = Duration.zero;

  AndroidAutoCall? _call;

  bool _listening = false;
  DateTime _listeningSince = DateTime.now();

  final Map<int, Offset> _fingers = {};
  final List<SimulatedRipple> _ripples = [];
  int _rippleCount = 0;
  String? _pressed;
  BuildContext? _canvas;

  /// Creates a phone that is not connected yet.
  SimulatedPhone({
    required this.onNavigation,
    required this.onMedia,
    required this.onPhoneStatus,
  });

  /// Whether the phone is projecting.
  bool get connected => _connected;

  /// Which screen is in front.
  SimulatedApp get app => _app;

  /// Whether the head unit last said it is dark outside. Only the map follows it,
  /// which is what Android Auto does: its own chrome is dark either way.
  bool get night => _night;
  set night(bool value) {
    if (value != _night) {
      _night = value;
      notifyListeners();
    }
  }

  /// The guidance in progress, or null when there is none.
  AndroidAutoNavigation? get navigation => _navigation;

  /// Which leg of [route] the car is on, or null when not guiding.
  int? get leg => _leg;

  /// How far along the current leg the car is, 0.0 to 1.0.
  double get legProgress =>
      _leg == null ? 0 : (_legTravelled / route[_leg!].metres).clamp(0.0, 1.0);

  /// The loaded track.
  SimulatedTrack get track => tracks[_track];

  /// Whether the (silent) music is playing.
  bool get playing => _playing;

  /// How far into [track] playback is.
  Duration get position => _position;

  /// The call in progress, or null.
  AndroidAutoCall? get call => _call;

  /// Whether the Assistant has the microphone open. Nothing is captured.
  bool get listening => _listening;

  /// A made up level for a meter while [listening], so one can be seen moving.
  double get microphoneLevel {
    if (!_listening) {
      return 0;
    }
    final t = DateTime.now().difference(_listeningSince).inMilliseconds / 110;
    return 0.15 + 0.35 * (math.sin(t) * math.sin(t * 0.37)).abs();
  }

  /// Every finger currently down, in the screen's layout coordinates.
  Map<int, Offset> get fingers => Map.unmodifiable(_fingers);

  /// Taps that have just lifted.
  List<SimulatedRipple> get ripples => List.unmodifiable(_ripples);

  /// The [SimulatedTarget.id] under a finger that has not lifted yet, for drawing the
  /// pressed state.
  String? get pressed => _pressed;

  /// The phone has connected: start its clock and say what it is playing, as a real
  /// phone does the moment the media channel opens.
  void connect() {
    if (_connected) {
      return;
    }
    _connected = true;
    _app = SimulatedApp.maps;
    _clock = Timer.periodic(const Duration(seconds: 1), (_) => _tick());
    _emitMedia();
    _emitPhone();
    notifyListeners();
  }

  /// The phone has gone: forget everything it was doing.
  void disconnect() {
    _clock?.cancel();
    _clock = null;
    _assistantTimer?.cancel();
    _assistantTimer = null;
    _connected = false;
    _leg = null;
    _navigation = null;
    _playing = false;
    _resumeAfterCall = false;
    _call = null;
    _listening = false;
    _fingers.clear();
    _pressed = null;
    if (!_disposed) {
      notifyListeners();
    }
  }

  /// Brings [app] to the front.
  void open(SimulatedApp app) {
    _app = app;
    notifyListeners();
  }

  // === touch ===

  /// Called by the screen when it is built, so touches can be hit tested against it.
  void attach(BuildContext canvas) => _canvas = canvas;

  /// Called by the screen when it goes away.
  void detach(BuildContext canvas) {
    if (identical(_canvas, canvas)) {
      _canvas = null;
    }
  }

  /// One touch report, with every finger that is down in layout coordinates and the
  /// one this report is about named by [subject].
  ///
  /// Behaves like a tap on Android: the target is the thing under the finger when it
  /// went down, it fires when the same finger lifts over the same thing, and a second
  /// finger or sliding off cancels it.
  void touch(AndroidAutoTouchAction action, Map<int, Offset> fingers, int subject) {
    final position = fingers[subject];
    switch (action) {
      case AndroidAutoTouchAction.down:
        _fingers
          ..clear()
          ..addAll(fingers);
        _pressed = position == null ? null : _targetAt(position)?.id;
      case AndroidAutoTouchAction.pointerDown:
        _fingers
          ..clear()
          ..addAll(fingers);
        _pressed = null;
      case AndroidAutoTouchAction.move:
        _fingers
          ..clear()
          ..addAll(fingers);
        if (_pressed != null &&
            position != null &&
            _targetAt(position)?.id != _pressed) {
          _pressed = null;
        }
      case AndroidAutoTouchAction.pointerUp:
        _fingers
          ..clear()
          ..addAll(fingers)
          ..remove(subject);
      case AndroidAutoTouchAction.up:
        _fingers.clear();
        if (position != null) {
          _ripple(position);
          final target = _targetAt(position);
          if (target != null && target.id == _pressed) {
            target.onTap();
          }
        }
        _pressed = null;
    }
    notifyListeners();
  }

  SimulatedTarget? _targetAt(Offset position) {
    final box = _canvas?.findRenderObject();
    if (box is! RenderBox || !box.attached || !box.hasSize) {
      return null;
    }
    final result = BoxHitTestResult();
    if (!box.hitTest(result, position: position)) {
      return null;
    }
    for (final entry in result.path) {
      final target = entry.target;
      if (target is RenderMetaData && target.metaData is SimulatedTarget) {
        return target.metaData as SimulatedTarget;
      }
    }
    return null;
  }

  void _ripple(Offset position) {
    final ripple = SimulatedRipple(_rippleCount++, position);
    _ripples.add(ripple);
    Timer(const Duration(milliseconds: 450), () {
      _ripples.remove(ripple);
      if (!_disposed) {
        notifyListeners();
      }
    });
  }

  // === keys ===

  /// A hardware key going down. The phone acts on the press, not the release.
  void key(AndroidAutoKey key) {
    if (!_connected) {
      return;
    }
    switch (key) {
      case AndroidAutoKey.home:
        open(SimulatedApp.launcher);
      case AndroidAutoKey.back:
        open(SimulatedApp.maps);
      case AndroidAutoKey.call:
        if (_call?.state == AndroidAutoCallState.incoming) {
          answer();
        } else {
          open(SimulatedApp.phone);
        }
      case AndroidAutoKey.endCall:
        hangUp();
      case AndroidAutoKey.playPause:
        playPause();
      case AndroidAutoKey.play:
        _setPlaying(true);
      case AndroidAutoKey.pause:
        _setPlaying(false);
      case AndroidAutoKey.next:
        skip(1);
      case AndroidAutoKey.previous:
        skip(-1);
      case AndroidAutoKey.microphone:
        startAssistant();
      case AndroidAutoKey.up:
      case AndroidAutoKey.down:
      case AndroidAutoKey.left:
      case AndroidAutoKey.right:
      case AndroidAutoKey.enter:
        break;
    }
  }

  // === navigation ===

  /// Starts guidance to [destination] from the beginning of [route].
  void startRoute() {
    if (!_connected) {
      return;
    }
    _leg = 0;
    _legTravelled = 0;
    _app = SimulatedApp.maps;
    _emitNavigation();
    notifyListeners();
  }

  /// Ends guidance, as the driver pressing the cross on the phone's map does.
  void stopRoute() {
    if (_leg == null) {
      return;
    }
    _leg = null;
    _navigation = const AndroidAutoNavigation(
      status: AndroidAutoNavigationStatus.inactive,
    );
    onNavigation(_navigation!);
    notifyListeners();
  }

  void _advanceRoute(double seconds) {
    var index = _leg!;
    _legTravelled += route[index].speed * seconds;
    while (_legTravelled >= route[index].metres) {
      _legTravelled -= route[index].metres;
      index++;
      if (index == route.length) {
        // Arrived. The phone says guidance is over, which is what makes a head unit
        // take its turn card down.
        stopRoute();
        return;
      }
    }
    _leg = index;
    _emitNavigation();
  }

  void _emitNavigation() {
    final index = _leg!;
    final current = route[index];
    final toStep = current.metres - _legTravelled;
    var toEnd = toStep;
    var secondsToEnd = toStep / current.speed;
    for (final later in route.skip(index + 1)) {
      toEnd += later.metres;
      secondsToEnd += later.metres / later.speed;
    }
    final arrival = DateTime.now().add(Duration(seconds: secondsToEnd.round()));
    _navigation = AndroidAutoNavigation(
      status: AndroidAutoNavigationStatus.active,
      maneuver: current.maneuver,
      roundaboutExitNumber: current.exit,
      road: current.next,
      currentRoad: current.road,
      cue: [current.cue],
      lanes: current.lanes,
      stepDistance: _distance(toStep),
      timeToStep: Duration(seconds: (toStep / current.speed).round()),
      destinations: [
        AndroidAutoDestination(
          address: destination,
          distance: _distance(toEnd),
          etaText: clockText(arrival),
          timeToArrival: Duration(seconds: secondsToEnd.round()),
        ),
      ],
    );
    onNavigation(_navigation!);
  }

  /// A distance rounded the way the phone rounds one for a driver.
  static AndroidAutoDistance _distance(double metres) {
    final whole = metres.round();
    if (metres >= 10000) {
      return AndroidAutoDistance(
        metres: whole,
        displayValue: (metres / 1000).round().toString(),
        unit: AndroidAutoDistanceUnit.kilometres,
      );
    }
    if (metres >= 1000) {
      return AndroidAutoDistance(
        metres: whole,
        displayValue: (metres / 1000).toStringAsFixed(1),
        unit: AndroidAutoDistanceUnit.kilometresOneDecimal,
      );
    }
    final step = metres >= 300 ? 50 : 10;
    return AndroidAutoDistance(
      metres: whole,
      displayValue: ((metres / step).round() * step).toString(),
      unit: AndroidAutoDistanceUnit.metres,
    );
  }

  /// A time of day as a head unit shows one.
  static String clockText(DateTime time) =>
      '${time.hour.toString().padLeft(2, '0')}:'
      '${time.minute.toString().padLeft(2, '0')}';

  // === media ===

  /// Toggles playback.
  void playPause() => _setPlaying(!_playing);

  /// Moves [delta] tracks along. Back restarts the track first unless it has only
  /// just begun, as every phone's player does.
  void skip(int delta) {
    if (!_connected) {
      return;
    }
    if (delta < 0 && _position > const Duration(seconds: 3)) {
      _position = Duration.zero;
    } else {
      _track = (_track + delta) % tracks.length;
      _position = Duration.zero;
    }
    _emitMedia();
    notifyListeners();
  }

  void _setPlaying(bool playing) {
    if (!_connected || playing == _playing) {
      return;
    }
    _playing = playing;
    _emitMedia();
    notifyListeners();
  }

  void _emitMedia() {
    final track = tracks[_track];
    onMedia(
      AndroidAutoMediaInfo(
        song: track.song,
        artist: track.artist,
        album: track.album,
        playlist: 'Simulator Mix',
        duration: track.duration,
        albumArt: _art.putIfAbsent(
          _track,
          () => simulatedCoverArt(track.colors),
        ),
        state: _playing
            ? AndroidAutoPlaybackState.playing
            : AndroidAutoPlaybackState.paused,
        source: 'Simulated Music',
        position: _position,
        shuffle: false,
        repeat: false,
        repeatOne: false,
      ),
    );
  }

  // === telephony ===

  /// Rings, as a call arriving on the phone would.
  void simulateIncomingCall() {
    if (!_connected || _call != null) {
      return;
    }
    _startCall(
      AndroidAutoCall(
        state: AndroidAutoCallState.incoming,
        number: caller.number,
        callerId: caller.name,
        numberType: 'mobile',
      ),
    );
  }

  /// Rings [contact] from the phone's own dialler.
  void callContact(SimulatedContact contact) {
    if (!_connected || _call != null) {
      return;
    }
    _startCall(
      AndroidAutoCall(
        state: AndroidAutoCallState.inCall,
        number: contact.number,
        callerId: contact.name,
        numberType: 'mobile',
      ),
    );
    _app = SimulatedApp.phone;
    notifyListeners();
  }

  void _startCall(AndroidAutoCall call) {
    // A phone pauses its music for a call and picks it up again afterwards.
    _resumeAfterCall = _playing;
    _setPlaying(false);
    _call = call;
    _emitPhone();
    notifyListeners();
  }

  /// Answers a ringing call.
  void answer() {
    final call = _call;
    if (call == null || call.state != AndroidAutoCallState.incoming) {
      return;
    }
    _call = _copyCall(call, state: AndroidAutoCallState.inCall);
    _app = SimulatedApp.phone;
    _emitPhone();
    notifyListeners();
  }

  /// Ends or declines the call.
  void hangUp() {
    if (_call == null) {
      return;
    }
    _call = null;
    _emitPhone();
    if (_resumeAfterCall) {
      _resumeAfterCall = false;
      _setPlaying(true);
    }
    notifyListeners();
  }

  static AndroidAutoCall _copyCall(
    AndroidAutoCall call, {
    AndroidAutoCallState? state,
    Duration? duration,
  }) => AndroidAutoCall(
    state: state ?? call.state,
    duration: duration ?? call.duration,
    number: call.number,
    callerId: call.callerId,
    numberType: call.numberType,
  );

  void _emitPhone() {
    final call = _call;
    onPhoneStatus(
      AndroidAutoPhoneStatus(
        calls: call == null ? const <AndroidAutoCall>[] : [call],
        signalStrength: 4,
      ),
    );
  }

  // === assistant ===

  /// Opens the microphone for a few seconds, as the Assistant does for one query.
  void startAssistant() {
    if (!_connected) {
      return;
    }
    _listening = true;
    _listeningSince = DateTime.now();
    _assistantTimer?.cancel();
    _assistantTimer = Timer(const Duration(seconds: 4), () {
      _listening = false;
      if (!_disposed) {
        notifyListeners();
      }
    });
    notifyListeners();
  }

  // === clock ===

  void _tick() {
    if (_leg != null) {
      _advanceRoute(1);
    }
    if (_playing) {
      _position += const Duration(seconds: 1);
      if (_position >= tracks[_track].duration) {
        skip(1);
      } else {
        _emitMedia();
      }
    }
    final call = _call;
    if (call != null && call.state == AndroidAutoCallState.inCall) {
      _call = _copyCall(call, duration: call.duration + const Duration(seconds: 1));
      _emitPhone();
    }
    // Every second whatever happened, for the clock on the rail.
    notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    disconnect();
    super.dispose();
  }
}
