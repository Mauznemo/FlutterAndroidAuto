// SPDX-License-Identifier: GPL-3.0-or-later
//
// The simulator is what every host app developed on macOS or Windows runs against, so
// it has to behave like the real thing in the ways an app can observe: the connection
// sequence, the picture size the view is given, touches arriving as projected video
// pixels, and metadata only on the channels the config offered.

import 'dart:async';
import 'dart:ui' as ui;

import 'package:android_auto/android_auto.dart';
import 'package:android_auto_platform_interface/android_auto_platform_interface.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  group('picture size', () {
    // The same answers video_margins.cc gives, so a layout tried on a Mac is the
    // layout the car will get.
    test('a view that fits a frame gets a picture its exact size', () {
      const view = Size(1280, 680);
      final frame = AndroidAutoSimulator.frameForView(view, true);
      expect(frame, const Size(1280, 720));
      expect(AndroidAutoSimulator.visibleForView(frame, view), view);
    });

    test('a narrow view skips a frame whose picture would be stretched', () {
      const view = Size(780, 680);
      final frame = AndroidAutoSimulator.frameForView(view, true);
      expect(frame, const Size(1280, 720));
      expect(AndroidAutoSimulator.visibleForView(frame, view), view);
    });

    test('a view larger than every frame gets the largest, in its shape', () {
      const view = Size(2560, 1600);
      final frame = AndroidAutoSimulator.frameForView(view, true);
      expect(frame, const Size(1920, 1080));
      expect(AndroidAutoSimulator.visibleForView(frame, view), const Size(1728, 1080));
    });
  });

  group('session', () {
    late AndroidAutoSimulator simulator;
    late AndroidAutoController controller;

    Future<void> pumpView(
      WidgetTester tester, {
      AndroidAutoConfig config = const AndroidAutoConfig(),
    }) async {
      tester.view.physicalSize = const Size(1280, 680);
      tester.view.devicePixelRatio = 1.0;
      addTearDown(tester.view.reset);
      simulator = AndroidAutoSimulator(
        searchTime: const Duration(milliseconds: 10),
        handshakeTime: const Duration(milliseconds: 10),
        firstFrameTime: const Duration(milliseconds: 10),
      );
      AndroidAutoPlatform.instance = simulator;
      controller = AndroidAutoController(config: config);
      await tester.pumpWidget(
        Directionality(
          textDirection: TextDirection.ltr,
          child: AndroidAutoView(controller: controller),
        ),
      );
    }

    Future<void> connect(WidgetTester tester) async {
      await controller.start();
      await tester.pump(const Duration(milliseconds: 40));
      await tester.pump();
    }

    Future<void> tearDownSession(WidgetTester tester) async {
      await controller.stop();
      controller.dispose();
      // Lets the last ripple's timer run out.
      await tester.pump(const Duration(seconds: 1));
    }

    testWidgets('start walks through the states a phone would', (tester) async {
      await pumpView(tester);
      final states = <AndroidAutoConnectionState>[];
      final subscription = controller.events.listen((e) => states.add(e.state));

      await connect(tester);

      expect(states, [
        AndroidAutoConnectionState.searching,
        AndroidAutoConnectionState.handshaking,
        AndroidAutoConnectionState.connected,
        // The first frame, which is news on the connected state rather than a new one.
        AndroidAutoConnectionState.connected,
      ]);
      expect(controller.textureId, isNotNull);
      expect(controller.hasVideo, isTrue);
      expect(controller.videoInfo?.width, 1280);
      expect(controller.videoInfo?.height, 680);
      expect(find.text('Simulated phone'), findsOneWidget);

      // Not awaited: a broadcast subscription's cancel completes outside the test's
      // fake clock, and resuming there would leave every later await stranded.
      unawaited(subscription.cancel());
      await tearDownSession(tester);
      expect(controller.state, AndroidAutoConnectionState.idle);
      expect(controller.textureId, isNull);
      expect(controller.hasVideo, isFalse);
    });

    testWidgets('the picture comes a moment after connected, as a phone\'s does', (
      tester,
    ) async {
      await pumpView(tester);
      await controller.start();
      // Searching and handshaking are done, the first frame is not.
      await tester.pump(const Duration(milliseconds: 25));
      await tester.pump();

      expect(controller.state, AndroidAutoConnectionState.connected);
      expect(controller.hasVideo, isFalse);
      expect(controller.videoInfo, isNull);
      expect(find.text('Simulated phone'), findsNothing);

      await tester.pump(const Duration(milliseconds: 20));
      await tester.pump();
      expect(controller.hasVideo, isTrue);
      expect(find.text('Simulated phone'), findsOneWidget);
      await tearDownSession(tester);
    });

    testWidgets('a tap reaches the pretend phone through sendTouch', (tester) async {
      await pumpView(tester);
      await connect(tester);
      expect(find.text('Midnight Drive'), findsNothing);

      await tester.tapAt(tester.getCenter(find.byIcon(Icons.headphones)));
      await tester.pump();

      expect(find.text('Midnight Drive'), findsOneWidget);
      await tearDownSession(tester);
    });

    testWidgets('guidance arrives on the navigation stream', (tester) async {
      await pumpView(tester);
      await connect(tester);
      final updates = <AndroidAutoNavigation>[];
      final subscription = controller.navigation.listen(updates.add);

      AndroidAutoSimulator.current!.startRoute();
      await tester.pump(const Duration(seconds: 2));

      expect(updates, isNotEmpty);
      expect(updates.last.isGuiding, isTrue);
      expect(updates.last.maneuver, AndroidAutoManeuver.turnNormalRight);
      expect(updates.last.road, 'Station Road');
      expect(controller.lastNavigation, same(updates.last));

      AndroidAutoSimulator.current!.stopRoute();
      await tester.pump();
      expect(updates.last.isGuiding, isFalse);

      // Not awaited: a broadcast subscription's cancel completes outside the test's
      // fake clock, and resuming there would leave every later await stranded.
      unawaited(subscription.cancel());
      await tearDownSession(tester);
    });

    testWidgets('the cover art is an image Flutter can decode', (tester) async {
      await pumpView(tester);
      await connect(tester);

      final art = controller.lastMediaInfo?.albumArt;
      expect(art, isNotNull);
      final image = await tester.runAsync(() async {
        final codec = await ui.instantiateImageCodec(art!);
        return (await codec.getNextFrame()).image;
      });
      expect(image?.width, 128);
      expect(image?.height, 128);
      image?.dispose();

      await tearDownSession(tester);
    });

    testWidgets('nothing arrives on a channel the config did not offer', (
      tester,
    ) async {
      await pumpView(
        tester,
        config: const AndroidAutoConfig(metadata: {AndroidAutoMetadata.phone}),
      );
      final media = <AndroidAutoMediaInfo>[];
      final subscription = controller.mediaPlayback.listen(media.add);
      await connect(tester);

      controller.pressKey(AndroidAutoKey.play);
      await tester.pump(const Duration(seconds: 2));

      expect(media, isEmpty);
      expect(controller.lastMediaInfo, isNull);
      expect(controller.lastPhoneStatus, isNotNull);

      // Not awaited: a broadcast subscription's cancel completes outside the test's
      // fake clock, and resuming there would leave every later await stranded.
      unawaited(subscription.cancel());
      await tearDownSession(tester);
    });

    testWidgets('a ringing call can be answered with the call key', (tester) async {
      await pumpView(tester);
      await connect(tester);

      AndroidAutoSimulator.current!.simulateIncomingCall();
      await tester.pump();
      expect(
        controller.lastPhoneStatus?.activeCall?.state,
        AndroidAutoCallState.incoming,
      );

      controller.pressKey(AndroidAutoKey.call);
      await tester.pump();
      expect(
        controller.lastPhoneStatus?.activeCall?.state,
        AndroidAutoCallState.inCall,
      );

      await tearDownSession(tester);
    });
  });
}
