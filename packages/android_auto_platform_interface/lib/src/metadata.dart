// SPDX-License-Identifier: GPL-3.0-or-later
/// What the phone tells the head unit about itself.
///
/// The mirror image of the sensors: those are the car describing itself to the phone,
/// these are the phone describing itself to the car. They are what a host app draws its
/// own turn card, now playing bar and call banner from, rather than only mirroring the
/// projected pixels.
///
/// Every one of these is built from a JSON object the native layer sends, and every
/// field the phone did not send is null rather than zero or empty. That distinction is
/// load bearing: a track with no album and a track whose album is called "" are
/// different, and so are a route with no estimated arrival time and one arriving at
/// midnight.
library;

import 'dart:convert';
import 'dart:typed_data';

/// Reads one of the base64 picture fields, or null when the phone sent none.
Uint8List? _image(Map<String, dynamic> json, String key) {
  final value = json[key];
  if (value is! String || value.isEmpty) {
    return null;
  }
  try {
    return base64Decode(value);
  } on FormatException {
    // A picture that will not decode is a picture that cannot be drawn, and there is
    // nothing useful a host app could do with the raw string. Treated as absent.
    return null;
  }
}

String? _string(Map<String, dynamic> json, String key) {
  final value = json[key];
  return value is String && value.isNotEmpty ? value : null;
}

int? _int(Map<String, dynamic> json, String key) {
  final value = json[key];
  return value is num ? value.toInt() : null;
}

bool? _bool(Map<String, dynamic> json, String key) {
  final value = json[key];
  return value is bool ? value : null;
}

List<Map<String, dynamic>> _objects(Map<String, dynamic> json, String key) {
  final value = json[key];
  if (value is! List) {
    return const <Map<String, dynamic>>[];
  }
  return value.whereType<Map<String, dynamic>>().toList(growable: false);
}

/// Which of the five metadata channels something is about.
///
/// Declaring one in [AndroidAutoConfig.metadata] advertises the channel to the phone.
/// Unlike a sensor that is not a promise of anything: the phone pushes what it has and
/// a head unit that never reads it is simply a head unit with no turn card.
///
/// The order is part of the platform boundary rather than an internal detail: each
/// entry is one bit, in this order, and implementations map it positionally. Adding an
/// entry anywhere but the end changes what every existing implementation means.
enum AndroidAutoMetadata {
  /// Turn by turn guidance: the next maneuver, its distance, the lanes, the
  /// destination.
  navigation,

  /// The track playing, its artwork, and whether it is playing at all.
  media,

  /// Calls in progress and who they are with. Read only: a call's audio goes over
  /// Bluetooth and never touches the projection link.
  phone,

  /// Messages the phone asks the head unit to show.
  notification,

  /// The phone's media library, asked for a node at a time.
  browse;

  /// The bit this channel occupies in the mask the native layer takes.
  int get bit => 1 << index;
}

/// Whether the phone is guiding anyone anywhere.
///
/// Check this before drawing a turn card. An instruction left on screen after guidance
/// has ended is worse than no card at all, and it is the one mistake a head unit can
/// make that actively misleads a driver.
enum AndroidAutoNavigationStatus {
  /// The phone has nothing to say about navigation.
  unavailable,

  /// Guidance is running.
  active,

  /// Navigation exists but is not guiding.
  inactive,

  /// The route is being recalculated.
  rerouting,
}

/// The unit a distance should be shown in, as the phone's own settings chose.
///
/// The `OneDecimal` forms mean the value should be printed with one digit after the
/// point, which is how "0.4 km" and "400 m" come from the same route a moment apart.
enum AndroidAutoDistanceUnit {
  /// Whole metres.
  metres,

  /// Whole kilometres.
  kilometres,

  /// Kilometres with one decimal place.
  kilometresOneDecimal,

  /// Whole miles.
  miles,

  /// Miles with one decimal place.
  milesOneDecimal,

  /// Whole feet.
  feet,

  /// Whole yards.
  yards,
}

/// How far away something is, as a number and as the phone would print it.
///
/// Both halves matter. [metres] is what to compare against and count down with;
/// [displayValue] and [unit] are what the phone's own screen would show, already
/// rounded and already converted. A head unit that recomputes "0.4 km" from 412 metres
/// will sooner or later disagree with the phone sitting next to it.
class AndroidAutoDistance {
  /// The distance in metres, or null when the phone did not give one.
  final int? metres;

  /// The number to print, already rounded, without its unit.
  final String? displayValue;

  /// The unit [displayValue] is in.
  final AndroidAutoDistanceUnit? unit;

  /// Creates a distance.
  const AndroidAutoDistance({this.metres, this.displayValue, this.unit});

  /// Whether the phone gave anything at all.
  bool get isEmpty => metres == null && displayValue == null;

  /// The distance as a person should read it: `450 m`, `2,0 km`, `1.2 mi`.
  ///
  /// Built from the phone's own number and unit wherever there is one, because that is
  /// already rounded and already in the units the phone's user chose, decimal comma and
  /// all. Falling back to [metres] only when the phone sent no display value, which is
  /// what the older distance message does.
  String get display {
    final value = displayValue;
    if (value == null) {
      return metres == null ? '' : '$metres m';
    }
    return unit == null ? value : '$value ${_unitSuffixes[unit]}';
  }

  /// Builds one from the flattened keys inside a navigation object.
  factory AndroidAutoDistance.fromJson(
    Map<String, dynamic> json, {
    required String metresKey,
    required String textKey,
    required String unitKey,
  }) => AndroidAutoDistance(
    metres: _int(json, metresKey),
    displayValue: _string(json, textKey),
    unit: _distanceUnits[_string(json, unitKey)],
  );

  @override
  String toString() => 'AndroidAutoDistance(${display.isEmpty ? "?" : display})';
}

/// What each unit is called on a dashboard. Short, because a turn card has no room for
/// "kilometres" and nobody in a car reads it anyway.
const Map<AndroidAutoDistanceUnit, String> _unitSuffixes = {
  AndroidAutoDistanceUnit.metres: 'm',
  AndroidAutoDistanceUnit.kilometres: 'km',
  AndroidAutoDistanceUnit.kilometresOneDecimal: 'km',
  AndroidAutoDistanceUnit.miles: 'mi',
  AndroidAutoDistanceUnit.milesOneDecimal: 'mi',
  AndroidAutoDistanceUnit.feet: 'ft',
  AndroidAutoDistanceUnit.yards: 'yd',
};

/// The protocol's unit names, which are American, to this package's, which are not.
const Map<String, AndroidAutoDistanceUnit> _distanceUnits = {
  'meters': AndroidAutoDistanceUnit.metres,
  'kilometers': AndroidAutoDistanceUnit.kilometres,
  'kilometersP1': AndroidAutoDistanceUnit.kilometresOneDecimal,
  'miles': AndroidAutoDistanceUnit.miles,
  'milesP1': AndroidAutoDistanceUnit.milesOneDecimal,
  'feet': AndroidAutoDistanceUnit.feet,
  'yards': AndroidAutoDistanceUnit.yards,
};

/// The shape of the next turn.
///
/// The protocol's own list, which is long because it distinguishes things a driver
/// distinguishes: a slight left is a different arrow from a normal left, and leaving a
/// motorway is a different arrow from turning off a street. A head unit that does not
/// want all of it can group them with [turnsLeft], [turnsRight], [isRoundabout] and
/// [isDestination] rather than switching on forty three cases.
enum AndroidAutoManeuver {
  /// The phone did not say, or said something this version has no name for.
  unknown,

  /// Setting off at the start of the route.
  depart,

  /// The road changes name without a turn.
  nameChange,

  /// Keep left where the road divides.
  keepLeft,

  /// Keep right where the road divides.
  keepRight,

  /// A shallow left.
  turnSlightLeft,

  /// A shallow right.
  turnSlightRight,

  /// An ordinary left.
  turnNormalLeft,

  /// An ordinary right.
  turnNormalRight,

  /// A tight left.
  turnSharpLeft,

  /// A tight right.
  turnSharpRight,

  /// Turn back, to the left.
  uTurnLeft,

  /// Turn back, to the right.
  uTurnRight,

  /// Onto a slip road, bearing left.
  onRampSlightLeft,

  /// Onto a slip road, bearing right.
  onRampSlightRight,

  /// Onto a slip road, turning left.
  onRampNormalLeft,

  /// Onto a slip road, turning right.
  onRampNormalRight,

  /// Onto a slip road, turning sharply left.
  onRampSharpLeft,

  /// Onto a slip road, turning sharply right.
  onRampSharpRight,

  /// Onto a slip road that doubles back to the left.
  onRampUTurnLeft,

  /// Onto a slip road that doubles back to the right.
  onRampUTurnRight,

  /// Off a slip road, bearing left.
  offRampSlightLeft,

  /// Off a slip road, bearing right.
  offRampSlightRight,

  /// Off a slip road, turning left.
  offRampNormalLeft,

  /// Off a slip road, turning right.
  offRampNormalRight,

  /// Take the left branch where the road forks.
  forkLeft,

  /// Take the right branch where the road forks.
  forkRight,

  /// Merge from the left.
  mergeLeft,

  /// Merge from the right.
  mergeRight,

  /// Merge, with no side given.
  mergeSideUnspecified,

  /// Enter a roundabout.
  roundaboutEnter,

  /// Leave a roundabout.
  roundaboutExit,

  /// Enter and leave a clockwise roundabout.
  roundaboutEnterAndExitCw,

  /// Enter and leave a clockwise roundabout, with an exit angle given.
  roundaboutEnterAndExitCwWithAngle,

  /// Enter and leave an anticlockwise roundabout.
  roundaboutEnterAndExitCcw,

  /// Enter and leave an anticlockwise roundabout, with an exit angle given.
  roundaboutEnterAndExitCcwWithAngle,

  /// Carry straight on.
  straight,

  /// Board a ferry.
  ferryBoat,

  /// Board a car carrying train.
  ferryTrain,

  /// The destination is here.
  destination,

  /// The destination is straight ahead.
  destinationStraight,

  /// The destination is on the left.
  destinationLeft,

  /// The destination is on the right.
  destinationRight;

  /// Whether this maneuver goes to the left, for a head unit that draws two arrows
  /// rather than forty three.
  ///
  /// The roundabout direction is matched anywhere in the name rather than at the end,
  /// because two of them carry an exit angle and so end in `WithAngle`. Matching the
  /// end alone made those two the only maneuvers that were neither left nor right,
  /// which for a head unit drawing two arrows means drawing neither.
  bool get turnsLeft => name.endsWith('Left') || name.contains('Ccw');

  /// Whether this maneuver goes to the right.
  bool get turnsRight =>
      name.endsWith('Right') || (name.contains('Cw') && !name.contains('Ccw'));

  /// Whether this maneuver is about a roundabout.
  bool get isRoundabout => name.startsWith('roundabout');

  /// Whether this maneuver is the end of the route.
  bool get isDestination => name.startsWith('destination');
}

/// Which way one lane of the road leads.
enum AndroidAutoLaneShape {
  /// Not given.
  unknown,

  /// Straight on.
  straight,

  /// A shallow left.
  slightLeft,

  /// A shallow right.
  slightRight,

  /// An ordinary left.
  normalLeft,

  /// An ordinary right.
  normalRight,

  /// A tight left.
  sharpLeft,

  /// A tight right.
  sharpRight,

  /// Back to the left.
  uTurnLeft,

  /// Back to the right.
  uTurnRight,
}

/// One arrow painted on one lane.
class AndroidAutoLaneDirection {
  /// Which way this arrow points.
  final AndroidAutoLaneShape shape;

  /// Whether this is a lane the driver should be in for the next maneuver.
  final bool highlighted;

  /// Creates one lane arrow.
  const AndroidAutoLaneDirection({
    this.shape = AndroidAutoLaneShape.unknown,
    this.highlighted = false,
  });

  /// Builds one from the native layer's JSON.
  factory AndroidAutoLaneDirection.fromJson(Map<String, dynamic> json) =>
      AndroidAutoLaneDirection(
        shape: _byName(
          AndroidAutoLaneShape.values,
          _string(json, 'shape'),
          AndroidAutoLaneShape.unknown,
        ),
        highlighted: _bool(json, 'highlighted') ?? false,
      );
}

/// One lane of the road ahead, with every direction it serves.
class AndroidAutoLane {
  /// The arrows painted on this lane, left to right.
  final List<AndroidAutoLaneDirection> directions;

  /// Creates one lane.
  const AndroidAutoLane({this.directions = const <AndroidAutoLaneDirection>[]});

  /// Whether the driver should be in this lane.
  bool get isHighlighted => directions.any((direction) => direction.highlighted);

  /// Builds one from the native layer's JSON.
  factory AndroidAutoLane.fromJson(Map<String, dynamic> json) => AndroidAutoLane(
    directions: _objects(json, 'directions')
        .map(AndroidAutoLaneDirection.fromJson)
        .toList(growable: false),
  );
}

/// Where the route ends, and how far off that is.
class AndroidAutoDestination {
  /// The address, as the phone would print it.
  final String? address;

  /// How far there is to go.
  final AndroidAutoDistance distance;

  /// The arrival time, already formatted and already in the phone's time zone. A
  /// string rather than a [DateTime] because that is what the protocol carries, and
  /// reconstructing an instant from it would mean guessing at a time zone.
  final String? etaText;

  /// How long there is to go.
  final Duration? timeToArrival;

  /// Creates a destination.
  const AndroidAutoDestination({
    this.address,
    this.distance = const AndroidAutoDistance(),
    this.etaText,
    this.timeToArrival,
  });

  /// Builds one from the native layer's JSON.
  factory AndroidAutoDestination.fromJson(Map<String, dynamic> json) {
    final seconds = _int(json, 'secondsToArrival');
    return AndroidAutoDestination(
      address: _string(json, 'address'),
      distance: AndroidAutoDistance.fromJson(
        json,
        metresKey: 'distanceMetres',
        textKey: 'distanceText',
        unitKey: 'distanceUnit',
      ),
      etaText: _string(json, 'etaText'),
      timeToArrival: seconds == null ? null : Duration(seconds: seconds),
    );
  }

  @override
  String toString() => 'AndroidAutoDestination(${address ?? "?"}, $distance)';
}

/// Everything the phone has said about the guidance in progress.
///
/// Built from several protocol messages merged together, because the phone sends the
/// shape of the turn and the distance to it separately and a head unit wants one
/// object. A field is null when nothing has arrived for it yet, not when the phone said
/// zero.
class AndroidAutoNavigation {
  /// Whether guidance is running at all. Check this first.
  final AndroidAutoNavigationStatus? status;

  /// The shape of the next turn.
  final AndroidAutoManeuver? maneuver;

  /// Which exit to take, on a roundabout.
  final int? roundaboutExitNumber;

  /// The angle of that exit, in degrees.
  final int? roundaboutExitAngle;

  /// The road the next turn leads onto.
  final String? road;

  /// The road the car is on now, which is not the same question.
  final String? currentRoad;

  /// The phone's own wording for the instruction, longest first. What to show when
  /// there is room for a sentence rather than an arrow.
  final List<String> cue;

  /// The lanes of the road ahead, left to right.
  final List<AndroidAutoLane> lanes;

  /// How far to the next turn.
  final AndroidAutoDistance stepDistance;

  /// How long to the next turn.
  final Duration? timeToStep;

  /// Where the route ends. Usually one, but a route with waypoints has several.
  final List<AndroidAutoDestination> destinations;

  /// A rendered arrow for the next turn, when the phone sent one.
  ///
  /// Only older phones do. A phone on the current protocol describes the maneuver
  /// instead and leaves the drawing to the head unit, which is what this plugin asks
  /// for, so treat this as a fallback rather than the normal case.
  final Uint8List? maneuverImage;

  /// Creates a navigation snapshot.
  const AndroidAutoNavigation({
    this.status,
    this.maneuver,
    this.roundaboutExitNumber,
    this.roundaboutExitAngle,
    this.road,
    this.currentRoad,
    this.cue = const <String>[],
    this.lanes = const <AndroidAutoLane>[],
    this.stepDistance = const AndroidAutoDistance(),
    this.timeToStep,
    this.destinations = const <AndroidAutoDestination>[],
    this.maneuverImage,
  });

  /// Whether the phone is guiding right now, which is the one test worth making before
  /// drawing anything.
  bool get isGuiding =>
      status == AndroidAutoNavigationStatus.active ||
      status == AndroidAutoNavigationStatus.rerouting;

  /// Where the route ends, or null on a route with no destination yet.
  AndroidAutoDestination? get destination =>
      destinations.isEmpty ? null : destinations.first;

  /// Builds one from the native layer's JSON.
  factory AndroidAutoNavigation.fromJson(Map<String, dynamic> json) {
    final seconds = _int(json, 'secondsToStep');
    return AndroidAutoNavigation(
      status: _byNameOrNull(
        AndroidAutoNavigationStatus.values,
        _string(json, 'status'),
      ),
      maneuver: json['maneuver'] == null
          ? null
          : _byName(
              AndroidAutoManeuver.values,
              _string(json, 'maneuver'),
              AndroidAutoManeuver.unknown,
            ),
      roundaboutExitNumber: _int(json, 'roundaboutExitNumber'),
      roundaboutExitAngle: _int(json, 'roundaboutExitAngle'),
      road: _string(json, 'road'),
      currentRoad: _string(json, 'currentRoad'),
      cue: (json['cue'] as List?)?.whereType<String>().toList(growable: false) ??
          const <String>[],
      lanes: _objects(json, 'lanes')
          .map(AndroidAutoLane.fromJson)
          .toList(growable: false),
      stepDistance: AndroidAutoDistance.fromJson(
        json,
        metresKey: 'stepDistanceMetres',
        textKey: 'stepDistanceText',
        unitKey: 'stepDistanceUnit',
      ),
      timeToStep: seconds == null ? null : Duration(seconds: seconds),
      destinations: _objects(json, 'destinations')
          .map(AndroidAutoDestination.fromJson)
          .toList(growable: false),
      maneuverImage: _image(json, 'maneuverImage'),
    );
  }

  @override
  String toString() =>
      'AndroidAutoNavigation(${status?.name ?? "?"}, ${maneuver?.name ?? "-"} '
      'onto ${road ?? "?"} in $stepDistance)';
}

/// Whether the phone is playing anything.
enum AndroidAutoPlaybackState {
  /// Nothing is loaded.
  stopped,

  /// Audio is playing.
  playing,

  /// Something is loaded and paused.
  paused,
}

/// The track the phone is playing, and what is being done with it.
class AndroidAutoMediaInfo {
  /// The track title.
  final String? song;

  /// Who it is by.
  final String? artist;

  /// What it is from.
  final String? album;

  /// The playlist or station it came from.
  final String? playlist;

  /// How long the track is.
  final Duration? duration;

  /// The phone's own star rating, when it has one.
  final int? rating;

  /// The cover image, in whatever format the phone chose. Decode it with
  /// `Image.memory`.
  final Uint8List? albumArt;

  /// Whether it is playing.
  final AndroidAutoPlaybackState? state;

  /// Which app is playing, as the phone names it.
  final String? source;

  /// How far into the track playback has reached.
  ///
  /// Sent when it changes rather than continuously, so a head unit with a progress bar
  /// should run its own clock from here rather than waiting for the next update.
  final Duration? position;

  /// Whether shuffle is on.
  final bool? shuffle;

  /// Whether repeat is on.
  final bool? repeat;

  /// Whether repeat one track is on.
  final bool? repeatOne;

  /// Creates a media snapshot.
  const AndroidAutoMediaInfo({
    this.song,
    this.artist,
    this.album,
    this.playlist,
    this.duration,
    this.rating,
    this.albumArt,
    this.state,
    this.source,
    this.position,
    this.shuffle,
    this.repeat,
    this.repeatOne,
  });

  /// Whether there is anything worth showing.
  bool get isEmpty => song == null && artist == null && album == null;

  /// Whether audio is playing right now.
  bool get isPlaying => state == AndroidAutoPlaybackState.playing;

  /// Builds one from the native layer's JSON.
  factory AndroidAutoMediaInfo.fromJson(Map<String, dynamic> json) {
    final duration = _int(json, 'durationSeconds');
    final position = _int(json, 'positionSeconds');
    return AndroidAutoMediaInfo(
      song: _string(json, 'song'),
      artist: _string(json, 'artist'),
      album: _string(json, 'album'),
      playlist: _string(json, 'playlist'),
      duration: duration == null ? null : Duration(seconds: duration),
      rating: _int(json, 'rating'),
      albumArt: _image(json, 'albumArt'),
      state: _byNameOrNull(AndroidAutoPlaybackState.values, _string(json, 'state')),
      source: _string(json, 'source'),
      position: position == null ? null : Duration(seconds: position),
      shuffle: _bool(json, 'shuffle'),
      repeat: _bool(json, 'repeat'),
      repeatOne: _bool(json, 'repeatOne'),
    );
  }

  @override
  String toString() =>
      'AndroidAutoMediaInfo(${song ?? "?"} by ${artist ?? "?"}, ${state?.name ?? "?"})';
}

/// What one call is doing.
enum AndroidAutoCallState {
  /// The phone did not say.
  unknown,

  /// Connected and talking.
  inCall,

  /// Connected and on hold.
  onHold,

  /// Not in use.
  inactive,

  /// Ringing.
  incoming,

  /// Part of a conference call.
  conferenced,

  /// Connected with the microphone muted.
  muted,
}

/// One call on the phone.
class AndroidAutoCall {
  /// What this call is doing.
  final AndroidAutoCallState state;

  /// How long it has been going.
  final Duration duration;

  /// The number, when the phone gave one.
  final String? number;

  /// The name from the phone's contacts.
  final String? callerId;

  /// What kind of number it is, as free text from the phone: `mobile`, `home` and so
  /// on.
  final String? numberType;

  /// The contact's photo.
  final Uint8List? thumbnail;

  /// Creates one call.
  const AndroidAutoCall({
    this.state = AndroidAutoCallState.unknown,
    this.duration = Duration.zero,
    this.number,
    this.callerId,
    this.numberType,
    this.thumbnail,
  });

  /// The best name for this caller: the contact if there is one, otherwise the number.
  String get displayName => callerId ?? number ?? 'Unknown';

  /// Builds one from the native layer's JSON.
  factory AndroidAutoCall.fromJson(Map<String, dynamic> json) => AndroidAutoCall(
    state: _byName(
      AndroidAutoCallState.values,
      _string(json, 'state'),
      AndroidAutoCallState.unknown,
    ),
    duration: Duration(seconds: _int(json, 'durationSeconds') ?? 0),
    number: _string(json, 'number'),
    callerId: _string(json, 'callerId'),
    numberType: _string(json, 'numberType'),
    thumbnail: _image(json, 'thumbnail'),
  );

  @override
  String toString() => 'AndroidAutoCall($displayName, ${state.name})';
}

/// What the phone's telephony is doing.
///
/// Read only, deliberately. A call's audio never touches the projection link: it goes
/// over Bluetooth hands free, with this machine as the hands free unit, and answering or
/// hanging up belongs there rather than here. A head unit carrying calls also has to
/// cancel its own echo, or it sends the far end back to itself: see
/// [docs/echo-cancellation.md](https://github.com/Mauznemo/FlutterAndroidAuto/blob/main/docs/echo-cancellation.md).
class AndroidAutoPhoneStatus {
  /// Every call the phone has. Usually none or one; two while one is on hold.
  final List<AndroidAutoCall> calls;

  /// The phone's signal strength, on whatever scale it chose.
  final int? signalStrength;

  /// Creates a telephony snapshot.
  const AndroidAutoPhoneStatus({
    this.calls = const <AndroidAutoCall>[],
    this.signalStrength,
  });

  /// The call worth showing, which is the ringing one if there is one and otherwise the
  /// first. Null when nothing is happening.
  AndroidAutoCall? get activeCall {
    if (calls.isEmpty) {
      return null;
    }
    for (final call in calls) {
      if (call.state == AndroidAutoCallState.incoming) {
        return call;
      }
    }
    return calls.first;
  }

  /// Builds one from the native layer's JSON.
  factory AndroidAutoPhoneStatus.fromJson(Map<String, dynamic> json) =>
      AndroidAutoPhoneStatus(
        calls: _objects(json, 'calls')
            .map(AndroidAutoCall.fromJson)
            .toList(growable: false),
        signalStrength: _int(json, 'signalStrength'),
      );

  @override
  String toString() => 'AndroidAutoPhoneStatus(${calls.length} calls)';
}

/// A message the phone asked the head unit to show.
///
/// An event rather than a state: each one arrives once and is acknowledged by the
/// plugin as soon as it has been handed over. An unacknowledged notification is one the
/// phone sends again, so the acknowledgement is not optional and not something a host
/// app has to do.
class AndroidAutoNotification {
  /// The phone's own identifier for this message.
  final String? id;

  /// What to show.
  final String? text;

  /// The icon that goes with it.
  final Uint8List? icon;

  /// Creates a notification.
  const AndroidAutoNotification({this.id, this.text, this.icon});

  /// Builds one from the native layer's JSON.
  factory AndroidAutoNotification.fromJson(Map<String, dynamic> json) =>
      AndroidAutoNotification(
        id: _string(json, 'id'),
        text: _string(json, 'text'),
        icon: _image(json, 'icon'),
      );

  @override
  String toString() => 'AndroidAutoNotification(${text ?? "?"})';
}

/// What kind of collection a browse entry is.
enum AndroidAutoBrowseListType {
  /// The phone did not say.
  unknown,

  /// A playlist.
  playlist,

  /// An album.
  album,

  /// An artist.
  artist,

  /// A radio station.
  station,

  /// A genre.
  genre,
}

/// One playable item in the phone's media library.
class AndroidAutoBrowseSong {
  /// What to hand to [AndroidAutoPlatform.browseSelect] to play it.
  final String path;

  /// The track title.
  final String name;

  /// Who it is by.
  final String? artist;

  /// What it is from.
  final String? album;

  /// How long it is, on a single song node.
  final Duration? duration;

  /// The cover image, on a single song node.
  final Uint8List? art;

  /// Creates one song.
  const AndroidAutoBrowseSong({
    required this.path,
    required this.name,
    this.artist,
    this.album,
    this.duration,
    this.art,
  });

  /// Builds one from the native layer's JSON.
  factory AndroidAutoBrowseSong.fromJson(Map<String, dynamic> json) {
    final seconds = _int(json, 'durationSeconds');
    return AndroidAutoBrowseSong(
      path: _string(json, 'path') ?? '',
      name: _string(json, 'name') ?? '',
      artist: _string(json, 'artist'),
      album: _string(json, 'album'),
      duration: seconds == null ? null : Duration(seconds: seconds),
      art: _image(json, 'art'),
    );
  }

  @override
  String toString() => 'AndroidAutoBrowseSong($name)';
}

/// One collection in the phone's media library.
class AndroidAutoBrowseList {
  /// What to hand to [AndroidAutoPlatform.browse] to open it.
  final String path;

  /// What kind of collection it is.
  final AndroidAutoBrowseListType type;

  /// What to call it.
  final String? name;

  /// Its cover image, when the phone sent one.
  final Uint8List? art;

  /// Creates one list.
  const AndroidAutoBrowseList({
    required this.path,
    this.type = AndroidAutoBrowseListType.unknown,
    this.name,
    this.art,
  });

  /// Builds one from the native layer's JSON.
  factory AndroidAutoBrowseList.fromJson(Map<String, dynamic> json) =>
      AndroidAutoBrowseList(
        path: _string(json, 'path') ?? '',
        type: _byName(
          AndroidAutoBrowseListType.values,
          _string(json, 'type'),
          AndroidAutoBrowseListType.unknown,
        ),
        name: _string(json, 'name'),
        art: _image(json, 'art'),
      );

  @override
  String toString() => 'AndroidAutoBrowseList(${name ?? path})';
}

/// One media app on the phone.
class AndroidAutoBrowseSource {
  /// What to hand to [AndroidAutoPlatform.browse] to open it.
  final String path;

  /// The app's name.
  final String? name;

  /// The app's icon, when the phone sent one.
  final Uint8List? art;

  /// Creates one source.
  const AndroidAutoBrowseSource({required this.path, this.name, this.art});

  /// Builds one from the native layer's JSON.
  factory AndroidAutoBrowseSource.fromJson(Map<String, dynamic> json) =>
      AndroidAutoBrowseSource(
        path: _string(json, 'path') ?? '',
        name: _string(json, 'name'),
        art: _image(json, 'art'),
      );

  @override
  String toString() => 'AndroidAutoBrowseSource(${name ?? path})';
}

/// Which of the four shapes a browse answer has.
enum AndroidAutoBrowseNodeKind {
  /// The top of the library: a list of media apps.
  root,

  /// One media app: a list of collections.
  source,

  /// One collection: a list of songs.
  list,

  /// One song.
  song,
}

/// One answer to a browse request.
///
/// The phone's media library is a tree asked for a node at a time, and the protocol has
/// a different reply message for each level of it, so which of the collections below is
/// filled in depends on [kind].
class AndroidAutoBrowseNode {
  /// Which level of the tree this is.
  final AndroidAutoBrowseNodeKind? kind;

  /// This node's own path.
  final String path;

  /// What to call it.
  final String? name;

  /// What kind of collection it is, on a [AndroidAutoBrowseNodeKind.list] node.
  final AndroidAutoBrowseListType? type;

  /// The offset of this slice into a long list, when the phone chose to page it.
  final int? start;

  /// How many entries there are in total.
  final int? total;

  /// The media apps, on a [AndroidAutoBrowseNodeKind.root] node.
  final List<AndroidAutoBrowseSource> sources;

  /// The collections, on a [AndroidAutoBrowseNodeKind.source] node.
  final List<AndroidAutoBrowseList> lists;

  /// The songs, on a [AndroidAutoBrowseNodeKind.list] node.
  final List<AndroidAutoBrowseSong> songs;

  /// The song, on a [AndroidAutoBrowseNodeKind.song] node.
  final AndroidAutoBrowseSong? song;

  /// Creates a browse answer.
  const AndroidAutoBrowseNode({
    this.kind,
    this.path = '',
    this.name,
    this.type,
    this.start,
    this.total,
    this.sources = const <AndroidAutoBrowseSource>[],
    this.lists = const <AndroidAutoBrowseList>[],
    this.songs = const <AndroidAutoBrowseSong>[],
    this.song,
  });

  /// Builds one from the native layer's JSON.
  factory AndroidAutoBrowseNode.fromJson(Map<String, dynamic> json) =>
      AndroidAutoBrowseNode(
        kind: _byNameOrNull(AndroidAutoBrowseNodeKind.values, _string(json, 'kind')),
        path: _string(json, 'path') ?? '',
        name: _string(json, 'name'),
        type: _byNameOrNull(AndroidAutoBrowseListType.values, _string(json, 'type')),
        start: _int(json, 'start'),
        total: _int(json, 'total'),
        sources: _objects(json, 'sources')
            .map(AndroidAutoBrowseSource.fromJson)
            .toList(growable: false),
        lists: _objects(json, 'lists')
            .map(AndroidAutoBrowseList.fromJson)
            .toList(growable: false),
        songs: _objects(json, 'songs')
            .map(AndroidAutoBrowseSong.fromJson)
            .toList(growable: false),
        song: json['song'] is Map<String, dynamic>
            ? AndroidAutoBrowseSong.fromJson(json['song'] as Map<String, dynamic>)
            : null,
      );

  @override
  String toString() => 'AndroidAutoBrowseNode(${kind?.name ?? "?"} at $path)';
}

/// Looks an enum value up by name, falling back rather than throwing.
///
/// A phone on a newer protocol can send a name this build has never heard of, and a
/// head unit that crashed on one would be a head unit that stopped working when the
/// phone was updated.
T _byName<T extends Enum>(List<T> values, String? name, T fallback) {
  if (name == null) {
    return fallback;
  }
  for (final value in values) {
    if (value.name == name) {
      return value;
    }
  }
  return fallback;
}

T? _byNameOrNull<T extends Enum>(List<T> values, String? name) {
  if (name == null) {
    return null;
  }
  for (final value in values) {
    if (value.name == name) {
      return value;
    }
  }
  return null;
}
