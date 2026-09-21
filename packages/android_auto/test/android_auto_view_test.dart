// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input is the one channel that flows outwards, and two things about it are easy to
// break without noticing, because neither the phone nor this code reports an error
// when they are wrong:
//
//   - coordinates are projected video pixels, not logical pixels, so the mapping has
//     to agree with the letterboxing that was actually painted, and
//   - touch follows Android's MotionEvent rules, where the first finger down is a
//     different action from the second and action_index names the one that changed.
//
// These tests hold both. A fake platform stands in for the native side and records
// what it was told.

import 'package:android_auto/android_auto.dart';
import 'package:android_auto_platform_interface/android_auto_platform_interface.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'fake_platform.dart';

void main() {
  late FakeAndroidAutoPlatform platform;
  late AndroidAutoController controller;

  setUp(() {
    platform = FakeAndroidAutoPlatform();
    AndroidAutoPlatform.instance = platform;
    controller = AndroidAutoController(
      config: const AndroidAutoConfig(width: 1280, height: 720),
    );
  });

  tearDown(() async {
    controller.dispose();
    await platform.close();
  });

  /// Puts the view alone on a surface of exactly [size], with a texture already
  /// available.
  ///
  /// The view fills the surface rather than sitting inside a Center, so a widget local
  /// position and the global one the test taps at are the same number. Every
  /// expectation below is written in the view's own coordinates, which is what the
  /// mapping is about.
  Future<void> pumpView(
    WidgetTester tester, {
    Size size = const Size(1280, 720),
    BoxFit fit = BoxFit.contain,
    bool enableTouch = true,
  }) async {
    tester.view.physicalSize = size;
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.reset);
    platform.texture = 7;
    await controller.start();
    await tester.pumpWidget(
      Directionality(
        textDirection: TextDirection.ltr,
        child: AndroidAutoView(
          controller: controller,
          fit: fit,
          enableTouch: enableTouch,
        ),
      ),
    );
    await tester.pump();
  }

  group('coordinate mapping', () {
    testWidgets('a tap on a view the size of the video maps one to one', (
      tester,
    ) async {
      await pumpView(tester);

      await tester.tapAt(const Offset(640, 360));
      await tester.pump();

      expect(platform.touches, isNotEmpty);
      final down = platform.touches.first;
      expect(down.action, AndroidAutoTouchAction.down);
      expect(down.subject.x, 640);
      expect(down.subject.y, 360);
    });

    testWidgets('letterboxing is undone, so the centre is still the centre', (
      tester,
    ) async {
      // 1280x720 fitted into a square: 800x450 painted, with 175 pixel bars above and
      // below. A tap in the middle of the box is the middle of the video.
      await pumpView(tester, size: const Size(800, 800));

      await tester.tapAt(const Offset(400, 400));
      await tester.pump();

      final down = platform.touches.first;
      expect(down.subject.x, 640);
      expect(down.subject.y, 360);
    });

    testWidgets('a tap just inside the top of the projection maps near zero', (
      tester,
    ) async {
      await pumpView(tester, size: const Size(800, 800));

      // The bars are 175 tall, so y = 176 is one pixel inside the video.
      await tester.tapAt(const Offset(400, 176));
      await tester.pump();

      final down = platform.touches.first;
      expect(down.subject.y, lessThan(5));
      expect(down.subject.y, greaterThanOrEqualTo(0));
    });

    testWidgets('a tap on the letterbox bar is dropped, not clamped to the edge', (
      tester,
    ) async {
      await pumpView(tester, size: const Size(800, 800));

      // Well inside the top bar. Clamping this to y=0 would make the phone think the
      // driver touched the top of its screen, which they did not.
      await tester.tapAt(const Offset(400, 40));
      await tester.pump();

      expect(platform.touches, isEmpty);
    });

    testWidgets('a drag that wanders onto the bar keeps tracking the edge', (
      tester,
    ) async {
      await pumpView(tester, size: const Size(800, 800));

      final gesture = await tester.startGesture(const Offset(400, 400));
      await tester.pump();
      await gesture.moveTo(const Offset(400, 40));
      await tester.pump();
      await gesture.up();
      await tester.pump();

      final moves = platform.touches
          .where((touch) => touch.action == AndroidAutoTouchAction.move)
          .toList();
      expect(moves, isNotEmpty);
      // Clamped to the top of the video rather than dropped: the finger is still down
      // and the phone needs to keep hearing about it.
      expect(moves.last.subject.y, 0);
    });

    testWidgets('coordinates never leave the video', (tester) async {
      await pumpView(tester, size: const Size(800, 800));

      final gesture = await tester.startGesture(const Offset(400, 400));
      await tester.pump();
      for (final target in const [
        Offset(0, 400),
        Offset(799, 400),
        Offset(400, 799),
      ]) {
        await gesture.moveTo(target);
        await tester.pump();
      }
      await gesture.up();
      await tester.pump();

      for (final touch in platform.touches) {
        for (final point in touch.pointers) {
          expect(point.x, inInclusiveRange(0, 1279));
          expect(point.y, inInclusiveRange(0, 719));
        }
      }
    });

    testWidgets('the phone\'s own size wins over the one that was asked for', (
      tester,
    ) async {
      // The head unit asked for 1280x720 and the phone chose 1920x1080. A view that
      // laid out from the config would map every touch to the wrong place.
      platform.video = const AndroidAutoVideoInfo(
        width: 1920,
        height: 1080,
        decoder: 'test',
      );
      await pumpView(tester, size: const Size(1920, 1080));

      await tester.tapAt(const Offset(960, 540));
      await tester.pump();

      final down = platform.touches.first;
      expect(down.subject.x, 960);
      expect(down.subject.y, 540);
    });
  });

  group('MotionEvent rules', () {
    testWidgets('one finger is down then up, and both carry it', (tester) async {
      await pumpView(tester);

      final gesture = await tester.startGesture(const Offset(100, 100));
      await tester.pump();
      await gesture.up();
      await tester.pump();

      expect(
        platform.touches.map((touch) => touch.action),
        [AndroidAutoTouchAction.down, AndroidAutoTouchAction.up],
      );
      // The finger going up is still in the report. Android includes it.
      expect(platform.touches.last.pointers, hasLength(1));
    });

    testWidgets('a second finger is pointerDown, and names itself', (tester) async {
      await pumpView(tester);

      final first = await tester.startGesture(const Offset(100, 100));
      await tester.pump();
      final second = await tester.startGesture(const Offset(200, 200));
      await tester.pump();

      final pointerDown = platform.touches.last;
      expect(pointerDown.action, AndroidAutoTouchAction.pointerDown);
      expect(pointerDown.pointers, hasLength(2));
      expect(pointerDown.actionIndex, 1);
      expect(pointerDown.subject.x, 200);

      await second.up();
      await tester.pump();
      // One finger still down, so the other lifting is a pointerUp rather than an up.
      expect(platform.touches.last.action, AndroidAutoTouchAction.pointerUp);
      expect(platform.touches.last.pointers, hasLength(2));

      await first.up();
      await tester.pump();
      expect(platform.touches.last.action, AndroidAutoTouchAction.up);
      expect(platform.touches.last.pointers, hasLength(1));
    });

    testWidgets('slots are small and reused, not Flutter\'s growing pointer ids', (
      tester,
    ) async {
      await pumpView(tester);

      for (var i = 0; i < 3; i++) {
        final gesture = await tester.startGesture(Offset(100.0 + i, 100));
        await tester.pump();
        await gesture.up();
        await tester.pump();
      }

      // Flutter's pointer id has advanced three times; the phone saw slot 0 each time.
      for (final touch in platform.touches) {
        expect(touch.subject.id, 0);
      }
    });

    testWidgets('the lower slot is reused once its finger lifts', (tester) async {
      await pumpView(tester);

      final first = await tester.startGesture(const Offset(100, 100));
      await tester.pump();
      final second = await tester.startGesture(const Offset(200, 200));
      await tester.pump();
      expect(platform.touches.last.subject.id, 1);

      await first.up();
      await tester.pump();
      platform.touches.clear();

      // Slot 0 is free again, so the next finger down takes it while slot 1 is still
      // held by the finger that never lifted.
      final third = await tester.startGesture(const Offset(300, 300));
      await tester.pump();
      expect(platform.touches.last.subject.id, 0);

      await second.up();
      await tester.pump();
      await third.up();
      await tester.pump();
    });
  });

  group('the view itself', () {
    testWidgets('shows the placeholder until there is a texture', (tester) async {
      platform.texture = null;
      await tester.pumpWidget(
        Directionality(
          textDirection: TextDirection.ltr,
          child: AndroidAutoView(
            controller: controller,
            placeholder: const Text('waiting for a phone'),
          ),
        ),
      );

      expect(find.text('waiting for a phone'), findsOneWidget);
      expect(find.byType(Texture), findsNothing);
    });

    testWidgets('renders a Texture once one arrives', (tester) async {
      await pumpView(tester);

      expect(find.byType(Texture), findsOneWidget);
      expect(tester.widget<Texture>(find.byType(Texture)).textureId, 7);
    });

    testWidgets('sends nothing when touch is turned off', (tester) async {
      await pumpView(tester, enableTouch: false);

      await tester.tapAt(const Offset(640, 360));
      await tester.pump();

      expect(platform.touches, isEmpty);
    });
  });
}
