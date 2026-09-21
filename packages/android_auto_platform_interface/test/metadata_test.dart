// SPDX-License-Identifier: GPL-3.0-or-later
//
// The metadata types are decoded from JSON that `linux/src/metadata/json.cc` writes,
// so these tests are about one seam: the key names, the shapes and the absences on one
// side matching what the other side reads. A mismatch there is silent. Nothing throws,
// nothing logs, a turn card just never fills in.
//
// The JSON in here is written the way the native writer writes it, flattened keys and
// all, rather than the way it would be nice to receive.

import 'dart:convert';

import 'package:android_auto_platform_interface/android_auto_platform_interface.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  group('AndroidAutoDistance', () {
    test('prefers the phone\'s own rounding and units over the raw metres', () {
      final distance = AndroidAutoDistance.fromJson(
        {
          'stepDistanceMetres': 412,
          'stepDistanceText': '0,4',
          'stepDistanceUnit': 'kilometersP1',
        },
        metresKey: 'stepDistanceMetres',
        textKey: 'stepDistanceText',
        unitKey: 'stepDistanceUnit',
      );

      expect(distance.metres, 412);
      expect(distance.unit, AndroidAutoDistanceUnit.kilometresOneDecimal);
      // Not "0.4 km": the phone's own string keeps the decimal comma its user chose.
      expect(distance.display, '0,4 km');
    });

    test('falls back to metres when the phone sent no display value', () {
      final distance = AndroidAutoDistance.fromJson(
        {'m': 450},
        metresKey: 'm',
        textKey: 'text',
        unitKey: 'unit',
      );

      expect(distance.display, '450 m');
      expect(distance.isEmpty, isFalse);
    });

    test('is empty, and prints nothing, when the phone sent neither', () {
      const distance = AndroidAutoDistance();

      expect(distance.isEmpty, isTrue);
      expect(distance.display, '');
    });

    test('ignores a unit name this version does not know', () {
      final distance = AndroidAutoDistance.fromJson(
        {'m': 1, 'text': '3', 'unit': 'furlongs'},
        metresKey: 'm',
        textKey: 'text',
        unitKey: 'unit',
      );

      expect(distance.unit, isNull);
      expect(distance.display, '3');
    });
  });

  group('AndroidAutoManeuver', () {
    test('groups every turn into left or right, including the angled roundabouts', () {
      expect(AndroidAutoManeuver.turnSharpLeft.turnsLeft, isTrue);
      expect(AndroidAutoManeuver.turnSharpLeft.turnsRight, isFalse);
      expect(AndroidAutoManeuver.turnNormalRight.turnsRight, isTrue);

      // An anticlockwise roundabout goes left, a clockwise one goes right, and the two
      // that carry an exit angle have to agree with the two that do not.
      expect(AndroidAutoManeuver.roundaboutEnterAndExitCcw.turnsLeft, isTrue);
      expect(AndroidAutoManeuver.roundaboutEnterAndExitCcw.turnsRight, isFalse);
      expect(AndroidAutoManeuver.roundaboutEnterAndExitCw.turnsRight, isTrue);
      expect(AndroidAutoManeuver.roundaboutEnterAndExitCw.turnsLeft, isFalse);
      expect(AndroidAutoManeuver.roundaboutEnterAndExitCcwWithAngle.turnsLeft, isTrue);
      expect(
        AndroidAutoManeuver.roundaboutEnterAndExitCcwWithAngle.turnsRight,
        isFalse,
      );
      expect(AndroidAutoManeuver.roundaboutEnterAndExitCwWithAngle.turnsRight, isTrue);
      expect(AndroidAutoManeuver.roundaboutEnterAndExitCwWithAngle.turnsLeft, isFalse);
    });

    test('never calls a maneuver both left and right', () {
      for (final maneuver in AndroidAutoManeuver.values) {
        expect(
          maneuver.turnsLeft && maneuver.turnsRight,
          isFalse,
          reason: '${maneuver.name} claims to go both ways',
        );
      }
    });

    test('recognises its roundabouts and its destinations', () {
      for (final maneuver in AndroidAutoManeuver.values) {
        expect(
          maneuver.isRoundabout,
          maneuver.name.startsWith('roundabout'),
          reason: maneuver.name,
        );
        expect(
          maneuver.isDestination,
          maneuver.name.startsWith('destination'),
          reason: maneuver.name,
        );
      }
    });
  });

  group('AndroidAutoNavigation.fromJson', () {
    test('reads a full guidance update', () {
      final navigation = AndroidAutoNavigation.fromJson({
        'status': 'active',
        'maneuver': 'turnNormalLeft',
        'road': 'Hauptstraße',
        'currentRoad': 'Bahnhofstraße',
        'cue': ['In 300 metres, turn left onto Hauptstraße', 'Turn left'],
        'stepDistanceMetres': 300,
        'stepDistanceText': '300',
        'stepDistanceUnit': 'meters',
        'secondsToStep': 45,
        'lanes': [
          {
            'directions': [
              {'shape': 'straight', 'highlighted': false},
              {'shape': 'normalLeft', 'highlighted': true},
            ],
          },
        ],
        'destinations': [
          {
            'address': 'Hauptstraße 1, Berlin',
            'distanceMetres': 4200,
            'distanceText': '4,2',
            'distanceUnit': 'kilometersP1',
            'etaText': '18:42',
            'secondsToArrival': 600,
          },
        ],
      });

      expect(navigation.status, AndroidAutoNavigationStatus.active);
      expect(navigation.isGuiding, isTrue);
      expect(navigation.maneuver, AndroidAutoManeuver.turnNormalLeft);
      expect(navigation.maneuver!.turnsLeft, isTrue);
      expect(navigation.road, 'Hauptstraße');
      expect(navigation.currentRoad, 'Bahnhofstraße');
      expect(navigation.cue, hasLength(2));
      expect(navigation.cue.first, startsWith('In 300 metres'));
      expect(navigation.stepDistance.display, '300 m');
      expect(navigation.timeToStep, const Duration(seconds: 45));
      expect(navigation.lanes, hasLength(1));
      expect(navigation.lanes.single.directions, hasLength(2));
      expect(navigation.lanes.single.directions.last.highlighted, isTrue);
      expect(
        navigation.lanes.single.directions.last.shape,
        AndroidAutoLaneShape.normalLeft,
      );
      expect(navigation.destination?.address, 'Hauptstraße 1, Berlin');
      expect(navigation.destination?.distance.display, '4,2 km');
      expect(navigation.destination?.etaText, '18:42');
      expect(navigation.destination?.timeToArrival, const Duration(minutes: 10));
    });

    test('rerouting counts as guiding, inactive does not', () {
      expect(
        AndroidAutoNavigation.fromJson({'status': 'rerouting'}).isGuiding,
        isTrue,
      );
      expect(
        AndroidAutoNavigation.fromJson({'status': 'inactive'}).isGuiding,
        isFalse,
      );
    });

    test('an empty object decodes to an empty snapshot rather than throwing', () {
      final navigation = AndroidAutoNavigation.fromJson(const {});

      expect(navigation.status, isNull);
      expect(navigation.maneuver, isNull);
      expect(navigation.isGuiding, isFalse);
      expect(navigation.cue, isEmpty);
      expect(navigation.lanes, isEmpty);
      expect(navigation.destinations, isEmpty);
      expect(navigation.destination, isNull);
      expect(navigation.stepDistance.isEmpty, isTrue);
      expect(navigation.timeToStep, isNull);
      expect(navigation.maneuverImage, isNull);
    });

    test('a maneuver name from a newer schema becomes unknown, not null', () {
      // The distinction matters: null means the phone said nothing about the turn,
      // unknown means it named one this version has no arrow for.
      final navigation = AndroidAutoNavigation.fromJson({
        'maneuver': 'turnIntoTheSeaSlightly',
      });

      expect(navigation.maneuver, AndroidAutoManeuver.unknown);
    });

    test('survives values of the wrong type where a number was expected', () {
      final navigation = AndroidAutoNavigation.fromJson({
        'stepDistanceMetres': 'three hundred',
        'secondsToStep': null,
        'cue': ['ok', 7, null],
        'lanes': 'not a list',
      });

      expect(navigation.stepDistance.metres, isNull);
      expect(navigation.timeToStep, isNull);
      expect(navigation.cue, ['ok']);
      expect(navigation.lanes, isEmpty);
    });

    test('decodes a base64 maneuver image and drops one that will not decode', () {
      final bytes = <int>[0x89, 0x50, 0x4e, 0x47];
      final good = AndroidAutoNavigation.fromJson({
        'maneuverImage': base64Encode(bytes),
      });
      expect(good.maneuverImage, bytes);

      final bad = AndroidAutoNavigation.fromJson({
        'maneuverImage': 'not base64 at all!!',
      });
      expect(bad.maneuverImage, isNull);

      final empty = AndroidAutoNavigation.fromJson({'maneuverImage': ''});
      expect(empty.maneuverImage, isNull);
    });
  });

  group('AndroidAutoMediaInfo.fromJson', () {
    test('reads a now playing update', () {
      final media = AndroidAutoMediaInfo.fromJson({
        'song': 'Teardrop',
        'artist': 'Massive Attack',
        'album': 'Mezzanine',
        'state': 'playing',
        'durationSeconds': 330,
        'positionSeconds': 41,
        'albumArt': base64Encode(<int>[1, 2, 3]),
      });

      expect(media.song, 'Teardrop');
      expect(media.artist, 'Massive Attack');
      expect(media.album, 'Mezzanine');
      expect(media.state, AndroidAutoPlaybackState.playing);
      expect(media.isPlaying, isTrue);
      expect(media.duration, const Duration(seconds: 330));
      expect(media.position, const Duration(seconds: 41));
      expect(media.albumArt, <int>[1, 2, 3]);
    });

    test('an empty string is an absent field, not an empty one', () {
      // The native writer emits a key with an empty value for a string the phone did
      // not send, so a head unit that checked for null alone would draw a blank line
      // where it meant to draw nothing.
      final media = AndroidAutoMediaInfo.fromJson({
        'song': 'Teardrop',
        'artist': '',
      });

      expect(media.song, 'Teardrop');
      expect(media.artist, isNull);
    });
  });

  group('AndroidAutoPhoneStatus.fromJson', () {
    test('reads the whole call list', () {
      final status = AndroidAutoPhoneStatus.fromJson({
        'calls': [
          {
            'state': 'inCall',
            'callerId': 'Anna',
            'number': '+49301234567',
            'durationSeconds': 92,
          },
          {'state': 'onHold', 'number': '+49309999999'},
        ],
      });

      expect(status.calls, hasLength(2));
      expect(status.calls.first.state, AndroidAutoCallState.inCall);
      expect(status.calls.first.callerId, 'Anna');
      expect(status.calls.first.displayName, 'Anna');
      expect(status.calls.first.duration, const Duration(seconds: 92));
      expect(status.calls.last.state, AndroidAutoCallState.onHold);
      expect(status.calls.last.callerId, isNull);
      // No contact match, so the number is the best name there is.
      expect(status.calls.last.displayName, '+49309999999');
    });

    test('no calls is an empty list, which is how a call ending arrives', () {
      // PhoneStatus replaces rather than merges, precisely so that this clears the
      // previous call instead of leaving it on screen.
      final status = AndroidAutoPhoneStatus.fromJson(const {'calls': []});

      expect(status.calls, isEmpty);
    });
  });

  group('AndroidAutoBrowseNode.fromJson', () {
    test('reads a nested browse result', () {
      final node = AndroidAutoBrowseNode.fromJson({
        'kind': 'list',
        'type': 'album',
        'songs': [
          {
            'path': '/spotify/mezzanine/1',
            'name': 'Teardrop',
            'artist': 'Massive Attack',
            'durationSeconds': 330,
          },
        ],
        'sources': [
          {'path': '/spotify', 'name': 'Spotify'},
        ],
      });

      expect(node.kind, AndroidAutoBrowseNodeKind.list);
      expect(node.type, AndroidAutoBrowseListType.album);
      expect(node.songs, hasLength(1));
      expect(node.songs.single.name, 'Teardrop');
      expect(node.songs.single.path, '/spotify/mezzanine/1');
      expect(node.songs.single.duration, const Duration(seconds: 330));
      expect(node.sources.single.name, 'Spotify');
      expect(node.lists, isEmpty);
    });
  });
}
