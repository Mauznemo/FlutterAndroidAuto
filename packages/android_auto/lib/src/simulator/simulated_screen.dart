// SPDX-License-Identifier: GPL-3.0-or-later
import 'dart:math' as math;

import 'package:android_auto_platform_interface/android_auto_platform_interface.dart';
import 'package:flutter/material.dart' show Icons;
import 'package:flutter/widgets.dart';

import 'simulated_phone.dart';

/// The colours the simulated screen is drawn in, close enough to Android Auto's own
/// that a host app's overlays can be judged against them.
abstract final class _Palette {
  static const background = Color(0xFF000000);
  static const rail = Color(0xFF1B1C1E);
  static const card = Color(0xFF2A2B2E);
  static const cardHigh = Color(0xFF3A3B3F);
  static const pressed = Color(0x33FFFFFF);
  static const text = Color(0xFFE8EAED);
  static const muted = Color(0xFF9AA0A6);
  static const accent = Color(0xFF8AB4F8);
  static const guidance = Color(0xFF0B8043);
  static const red = Color(0xFFE5534B);
  static const green = Color(0xFF34A853);
}

/// What the simulated phone projects, drawn with widgets instead of decoded video.
///
/// Laid out the way a phone lays out: in density independent pixels at the head
/// unit's advertised dpi, so a picture of a given size holds as much as a real phone
/// would put in it, then scaled to whatever rectangle the view gives it. That is why a
/// retina display shows it small, which is also what a real phone would do there.
class SimulatedScreen extends StatelessWidget {
  /// The phone whose screen this is.
  final SimulatedPhone phone;

  /// The size of the picture in projected video pixels.
  final Size picture;

  /// The density the head unit advertised.
  final int dpi;

  /// Creates the screen.
  const SimulatedScreen({
    super.key,
    required this.phone,
    required this.picture,
    required this.dpi,
  });

  /// Projected video pixels to the phone's layout units. Touch reports are converted
  /// with this too, so what is drawn and what is hit agree by construction.
  static double layoutScale(int dpi) => 160 / (dpi <= 0 ? 160 : dpi);

  @override
  Widget build(BuildContext context) {
    return FittedBox(
      fit: BoxFit.fill,
      child: SizedBox.fromSize(
        size: picture * layoutScale(dpi),
        child: _Canvas(phone: phone),
      ),
    );
  }
}

/// Colour bars, for [AndroidAutoPlatform.startTestPattern] before a phone connects.
class SimulatedTestPattern extends StatelessWidget {
  /// Creates the pattern.
  const SimulatedTestPattern({super.key});

  static const _bars = [
    Color(0xFFC0C0C0),
    Color(0xFFC0C000),
    Color(0xFF00C0C0),
    Color(0xFF00C000),
    Color(0xFFC000C0),
    Color(0xFFC00000),
    Color(0xFF0000C0),
  ];

  @override
  Widget build(BuildContext context) {
    return Stack(
      fit: StackFit.expand,
      children: [
        Row(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            for (final color in _bars) Expanded(child: ColoredBox(color: color)),
          ],
        ),
        const Center(
          child: DecoratedBox(
            decoration: BoxDecoration(color: Color(0xCC000000)),
            child: Padding(
              padding: EdgeInsets.symmetric(horizontal: 16, vertical: 8),
              child: Text(
                'Simulated test pattern',
                style: TextStyle(
                  color: _Palette.text,
                  fontSize: 18,
                  decoration: TextDecoration.none,
                ),
              ),
            ),
          ),
        ),
      ],
    );
  }
}

/// The screen at its layout size. Registers itself with the phone so touch reports
/// can be hit tested against what is drawn.
class _Canvas extends StatefulWidget {
  final SimulatedPhone phone;

  const _Canvas({required this.phone});

  @override
  State<_Canvas> createState() => _CanvasState();
}

class _CanvasState extends State<_Canvas> {
  @override
  void initState() {
    super.initState();
    widget.phone.attach(context);
  }

  @override
  void didUpdateWidget(_Canvas oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.phone != widget.phone) {
      oldWidget.phone.detach(context);
      widget.phone.attach(context);
    }
  }

  @override
  void dispose() {
    widget.phone.detach(context);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final phone = widget.phone;
    return ListenableBuilder(
      listenable: phone,
      builder: (context, _) => LayoutBuilder(
        builder: (context, constraints) {
          final size = constraints.biggest;
          // A narrow picture moves the rail to the bottom, as a real phone does.
          final wide = size.width >= size.height * 1.4;
          final rail = _Rail(phone: phone, vertical: wide);
          final content = _Content(phone: phone);
          return DefaultTextStyle(
            style: const TextStyle(
              color: _Palette.text,
              fontSize: 20,
              decoration: TextDecoration.none,
              fontWeight: FontWeight.w400,
            ),
            child: ColoredBox(
              color: _Palette.background,
              child: Stack(
                fit: StackFit.expand,
                children: [
                  Flex(
                    direction: wide ? Axis.horizontal : Axis.vertical,
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: wide
                        ? [rail, Expanded(child: content)]
                        : [Expanded(child: content), rail],
                  ),
                  IgnorePointer(child: _Touches(phone: phone)),
                ],
              ),
            ),
          );
        },
      ),
    );
  }
}

/// A tap target: found by the phone's hit test, and lit while a finger is on it.
class _Tap extends StatelessWidget {
  final SimulatedPhone phone;
  final String id;
  final VoidCallback onTap;
  final Widget child;
  final BorderRadius radius;

  const _Tap({
    required this.phone,
    required this.id,
    required this.onTap,
    required this.child,
    this.radius = const BorderRadius.all(Radius.circular(16)),
  });

  @override
  Widget build(BuildContext context) {
    return MetaData(
      metaData: SimulatedTarget(id, onTap),
      behavior: HitTestBehavior.opaque,
      child: Stack(
        children: [
          child,
          if (phone.pressed == id)
            Positioned.fill(
              child: DecoratedBox(
                decoration: BoxDecoration(color: _Palette.pressed, borderRadius: radius),
              ),
            ),
        ],
      ),
    );
  }
}

class _Rail extends StatelessWidget {
  final SimulatedPhone phone;
  final bool vertical;

  const _Rail({required this.phone, required this.vertical});

  static const _apps = {
    SimulatedApp.maps: Icons.navigation,
    SimulatedApp.media: Icons.headphones,
    SimulatedApp.phone: Icons.phone,
    SimulatedApp.launcher: Icons.apps,
  };

  @override
  Widget build(BuildContext context) {
    final buttons = [
      for (final entry in _apps.entries)
        _RailButton(
          phone: phone,
          id: 'rail.${entry.key.name}',
          icon: entry.value,
          selected: phone.app == entry.key,
          onTap: () => phone.open(entry.key),
        ),
    ];
    final microphone = _RailButton(
      phone: phone,
      id: 'rail.microphone',
      icon: Icons.mic,
      selected: phone.listening,
      onTap: phone.startAssistant,
    );
    final clock = Padding(
      padding: const EdgeInsets.all(12),
      child: Text(
        SimulatedPhone.clockText(DateTime.now()),
        style: const TextStyle(fontSize: 22, fontWeight: FontWeight.w500),
      ),
    );
    return ColoredBox(
      color: _Palette.rail,
      child: SizedBox(
        width: vertical ? 96 : null,
        height: vertical ? null : 88,
        child: Flex(
          direction: vertical ? Axis.vertical : Axis.horizontal,
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            clock,
            const Spacer(),
            ...buttons,
            const Spacer(),
            microphone,
            const SizedBox.square(dimension: 12),
          ],
        ),
      ),
    );
  }
}

class _RailButton extends StatelessWidget {
  final SimulatedPhone phone;
  final String id;
  final IconData icon;
  final bool selected;
  final VoidCallback onTap;

  const _RailButton({
    required this.phone,
    required this.id,
    required this.icon,
    required this.selected,
    required this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(6),
      child: _Tap(
        phone: phone,
        id: id,
        onTap: onTap,
        radius: const BorderRadius.all(Radius.circular(36)),
        child: Container(
          width: 72,
          height: 72,
          decoration: BoxDecoration(
            color: selected ? _Palette.cardHigh : null,
            shape: BoxShape.circle,
          ),
          child: Icon(
            icon,
            size: 36,
            color: selected ? _Palette.accent : _Palette.text,
          ),
        ),
      ),
    );
  }
}

class _Content extends StatelessWidget {
  final SimulatedPhone phone;

  const _Content({required this.phone});

  @override
  Widget build(BuildContext context) {
    final call = phone.call;
    final ringing = call != null && call.state == AndroidAutoCallState.incoming;
    return Stack(
      fit: StackFit.expand,
      children: [
        switch (phone.app) {
          SimulatedApp.maps => _Maps(phone: phone),
          SimulatedApp.media => _Media(phone: phone),
          SimulatedApp.phone => _Phone(phone: phone),
          SimulatedApp.launcher => _Launcher(phone: phone),
        },
        if (ringing && phone.app != SimulatedApp.phone)
          Positioned(
            top: 16,
            left: 16,
            right: 16,
            child: Center(child: _IncomingBanner(phone: phone, call: call)),
          ),
        if (phone.listening)
          Positioned(
            left: 16,
            right: 16,
            bottom: 16,
            child: _AssistantBar(phone: phone),
          ),
        // Said on the screen itself, so nobody takes a screenshot of this for a phone.
        const Positioned(
          right: 12,
          bottom: 8,
          child: IgnorePointer(
            child: Text(
              'Simulated phone',
              style: TextStyle(fontSize: 14, color: Color(0x99FFFFFF)),
            ),
          ),
        ),
      ],
    );
  }
}

// === maps ===

class _Maps extends StatelessWidget {
  final SimulatedPhone phone;

  const _Maps({required this.phone});

  @override
  Widget build(BuildContext context) {
    final navigation = phone.navigation;
    final guiding = navigation != null && navigation.isGuiding;
    return Stack(
      fit: StackFit.expand,
      children: [
        CustomPaint(
          painter: _MapPainter(
            night: phone.night,
            leg: phone.leg,
            legProgress: phone.legProgress,
          ),
        ),
        if (guiding) ...[
          Positioned(
            left: 16,
            top: 16,
            child: _TurnCard(navigation: navigation),
          ),
          Positioned(
            left: 16,
            bottom: 16,
            child: _ArrivalCard(phone: phone, navigation: navigation),
          ),
        ] else
          Positioned(left: 16, top: 16, child: _StartCard(phone: phone)),
      ],
    );
  }
}

class _TurnCard extends StatelessWidget {
  final AndroidAutoNavigation navigation;

  const _TurnCard({required this.navigation});

  static IconData iconFor(AndroidAutoManeuver? maneuver) => switch (maneuver) {
    AndroidAutoManeuver.turnNormalRight => Icons.turn_right,
    AndroidAutoManeuver.turnNormalLeft => Icons.turn_left,
    AndroidAutoManeuver.turnSlightRight => Icons.turn_slight_right,
    AndroidAutoManeuver.turnSlightLeft => Icons.turn_slight_left,
    AndroidAutoManeuver.keepLeft => Icons.fork_left,
    AndroidAutoManeuver.keepRight => Icons.fork_right,
    AndroidAutoManeuver.offRampNormalRight => Icons.ramp_right,
    AndroidAutoManeuver.offRampNormalLeft => Icons.ramp_left,
    AndroidAutoManeuver.roundaboutEnterAndExitCcw => Icons.roundabout_right,
    AndroidAutoManeuver.roundaboutEnterAndExitCw => Icons.roundabout_left,
    AndroidAutoManeuver.destinationRight ||
    AndroidAutoManeuver.destinationLeft ||
    AndroidAutoManeuver.destination => Icons.flag,
    _ => Icons.straight,
  };

  @override
  Widget build(BuildContext context) {
    final lanes = navigation.lanes;
    return Container(
      width: 340,
      decoration: const BoxDecoration(
        color: _Palette.guidance,
        borderRadius: BorderRadius.all(Radius.circular(20)),
      ),
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        mainAxisSize: MainAxisSize.min,
        children: [
          Row(
            children: [
              Icon(iconFor(navigation.maneuver), size: 56, color: _Palette.text),
              const SizedBox(width: 12),
              Text(
                navigation.stepDistance.display,
                style: const TextStyle(fontSize: 36, fontWeight: FontWeight.w600),
              ),
            ],
          ),
          const SizedBox(height: 8),
          Text(
            navigation.road ?? '',
            style: const TextStyle(fontSize: 26, fontWeight: FontWeight.w500),
          ),
          if (lanes.isNotEmpty) ...[
            const SizedBox(height: 12),
            Row(
              children: [
                for (final lane in lanes)
                  Padding(
                    padding: const EdgeInsets.only(right: 8),
                    child: Icon(
                      lane.directions.any(
                            (d) => d.shape == AndroidAutoLaneShape.slightLeft,
                          )
                          ? Icons.turn_slight_left
                          : Icons.straight,
                      size: 32,
                      color: lane.isHighlighted ? _Palette.text : const Color(0x66FFFFFF),
                    ),
                  ),
              ],
            ),
          ],
        ],
      ),
    );
  }
}

class _ArrivalCard extends StatelessWidget {
  final SimulatedPhone phone;
  final AndroidAutoNavigation navigation;

  const _ArrivalCard({required this.phone, required this.navigation});

  @override
  Widget build(BuildContext context) {
    final destination = navigation.destination;
    final minutes = ((destination?.timeToArrival?.inSeconds ?? 0) / 60).ceil();
    return _Card(
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            mainAxisSize: MainAxisSize.min,
            children: [
              Text(
                destination?.etaText ?? '',
                style: const TextStyle(fontSize: 28, fontWeight: FontWeight.w600),
              ),
              Text(
                '$minutes min · ${destination?.distance.display ?? ''}',
                style: const TextStyle(color: _Palette.muted),
              ),
            ],
          ),
          const SizedBox(width: 24),
          _Tap(
            phone: phone,
            id: 'maps.end',
            onTap: phone.stopRoute,
            radius: const BorderRadius.all(Radius.circular(28)),
            child: const _Round(color: _Palette.red, icon: Icons.close),
          ),
        ],
      ),
    );
  }
}

class _StartCard extends StatelessWidget {
  final SimulatedPhone phone;

  const _StartCard({required this.phone});

  @override
  Widget build(BuildContext context) {
    return _Card(
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          const Icon(Icons.place, size: 36, color: _Palette.accent),
          const SizedBox(width: 12),
          Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            mainAxisSize: MainAxisSize.min,
            children: [
              const Text(SimulatedPhone.destination, style: TextStyle(fontSize: 24)),
              Text(
                'Simulated route, ${SimulatedPhone.routeMinutes} min',
                style: const TextStyle(color: _Palette.muted, fontSize: 18),
              ),
            ],
          ),
          const SizedBox(width: 24),
          _Tap(
            phone: phone,
            id: 'maps.start',
            onTap: phone.startRoute,
            radius: const BorderRadius.all(Radius.circular(28)),
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 14),
              decoration: const BoxDecoration(
                color: _Palette.accent,
                borderRadius: BorderRadius.all(Radius.circular(28)),
              ),
              child: const Text(
                'Start',
                style: TextStyle(color: Color(0xFF0B1F3A), fontWeight: FontWeight.w600),
              ),
            ),
          ),
        ],
      ),
    );
  }
}

/// A made up town with the simulated route drawn through it.
class _MapPainter extends CustomPainter {
  final bool night;
  final int? leg;
  final double legProgress;

  _MapPainter({required this.night, required this.leg, required this.legProgress});

  /// The route's corners, as fractions of the map. One more than there are legs.
  static const _route = [
    Offset(0.30, 0.95),
    Offset(0.30, 0.72),
    Offset(0.52, 0.72),
    Offset(0.52, 0.45),
    Offset(0.74, 0.30),
    Offset(0.74, 0.16),
    Offset(0.86, 0.16),
  ];

  @override
  void paint(Canvas canvas, Size size) {
    final land = night ? const Color(0xFF242F3E) : const Color(0xFFF1F3F4);
    final park = night ? const Color(0xFF263C3F) : const Color(0xFFCDE8C9);
    final water = night ? const Color(0xFF17263C) : const Color(0xFFAAD3F5);
    final road = night ? const Color(0xFF38414E) : const Color(0xFFFFFFFF);
    final routeColor = night ? const Color(0xFF669DF6) : const Color(0xFF1A73E8);
    Offset at(Offset f) => Offset(f.dx * size.width, f.dy * size.height);

    canvas.drawRect(Offset.zero & size, Paint()..color = land);
    canvas.drawRRect(
      RRect.fromRectAndRadius(
        Rect.fromLTRB(size.width * 0.05, size.height * 0.08, size.width * 0.24, size.height * 0.4),
        const Radius.circular(24),
      ),
      Paint()..color = park,
    );
    canvas.drawPath(
      Path()
        ..moveTo(size.width * 0.9, size.height)
        ..quadraticBezierTo(size.width * 0.85, size.height * 0.6, size.width, size.height * 0.45)
        ..lineTo(size.width, size.height)
        ..close(),
      Paint()..color = water,
    );
    final roads = Paint()
      ..color = road
      ..strokeWidth = 10
      ..strokeCap = StrokeCap.round;
    for (final y in const [0.16, 0.45, 0.72, 0.9]) {
      canvas.drawLine(at(Offset(0, y)), at(Offset(1, y)), roads);
    }
    for (final x in const [0.12, 0.30, 0.52, 0.74, 0.86]) {
      canvas.drawLine(at(Offset(x, 0)), at(Offset(x, 1)), roads);
    }
    // The motorway, the one diagonal.
    canvas.drawLine(at(const Offset(0.4, 0.58)), at(const Offset(0.86, 0.26)), roads..strokeWidth = 18);

    final current = leg;
    if (current == null) {
      _car(canvas, at(_route.first), at(_route[1]) - at(_route.first), routeColor);
      return;
    }
    final from = at(_route[current]);
    final to = at(_route[current + 1]);
    final car = Offset.lerp(from, to, legProgress)!;
    final ahead = Path()..moveTo(car.dx, car.dy);
    for (final point in _route.skip(current + 1)) {
      final p = at(point);
      ahead.lineTo(p.dx, p.dy);
    }
    canvas.drawPath(
      ahead,
      Paint()
        ..color = routeColor
        ..style = PaintingStyle.stroke
        ..strokeWidth = 12
        ..strokeJoin = StrokeJoin.round
        ..strokeCap = StrokeCap.round,
    );
    final end = at(_route.last);
    canvas.drawCircle(end, 12, Paint()..color = _Palette.red);
    _car(canvas, car, to - from, routeColor);
  }

  void _car(Canvas canvas, Offset position, Offset heading, Color color) {
    canvas.save();
    canvas.translate(position.dx, position.dy);
    canvas.rotate(math.atan2(heading.dy, heading.dx) + math.pi / 2);
    final chevron = Path()
      ..moveTo(0, -22)
      ..lineTo(16, 16)
      ..lineTo(0, 8)
      ..lineTo(-16, 16)
      ..close();
    canvas.drawPath(
      chevron,
      Paint()
        ..color = const Color(0xFFFFFFFF)
        ..style = PaintingStyle.stroke
        ..strokeWidth = 6
        ..strokeJoin = StrokeJoin.round,
    );
    canvas.drawPath(chevron, Paint()..color = color);
    canvas.restore();
  }

  @override
  bool shouldRepaint(_MapPainter old) =>
      old.night != night || old.leg != leg || old.legProgress != legProgress;
}

// === media ===

class _Media extends StatelessWidget {
  final SimulatedPhone phone;

  const _Media({required this.phone});

  static String _time(Duration d) =>
      '${d.inMinutes}:${(d.inSeconds % 60).toString().padLeft(2, '0')}';

  @override
  Widget build(BuildContext context) {
    final track = phone.track;
    final progress = track.duration.inMilliseconds == 0
        ? 0.0
        : phone.position.inMilliseconds / track.duration.inMilliseconds;
    return Padding(
      padding: const EdgeInsets.all(32),
      child: Row(
        children: [
          ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 320, maxHeight: 320),
            child: AspectRatio(
              aspectRatio: 1,
              child: Container(
              decoration: BoxDecoration(
                borderRadius: const BorderRadius.all(Radius.circular(24)),
                gradient: LinearGradient(
                  colors: track.colors,
                  begin: Alignment.topLeft,
                  end: Alignment.bottomRight,
                ),
              ),
              child: const Icon(Icons.music_note, size: 96, color: Color(0x99FFFFFF)),
            ),
            ),
          ),
          const SizedBox(width: 40),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                const Text(
                  'Simulated Music',
                  style: TextStyle(color: _Palette.muted, fontSize: 18),
                ),
                const SizedBox(height: 8),
                Text(
                  track.song,
                  style: const TextStyle(fontSize: 36, fontWeight: FontWeight.w600),
                ),
                Text(
                  '${track.artist} · ${track.album}',
                  style: const TextStyle(color: _Palette.muted, fontSize: 22),
                ),
                const SizedBox(height: 24),
                ClipRRect(
                  borderRadius: const BorderRadius.all(Radius.circular(3)),
                  child: SizedBox(
                    height: 6,
                    child: Stack(
                      fit: StackFit.expand,
                      children: [
                        const ColoredBox(color: _Palette.cardHigh),
                        FractionallySizedBox(
                          alignment: Alignment.centerLeft,
                          widthFactor: progress.clamp(0.0, 1.0),
                          child: const ColoredBox(color: _Palette.accent),
                        ),
                      ],
                    ),
                  ),
                ),
                const SizedBox(height: 8),
                Row(
                  mainAxisAlignment: MainAxisAlignment.spaceBetween,
                  children: [
                    Text(
                      _time(phone.position),
                      style: const TextStyle(color: _Palette.muted, fontSize: 16),
                    ),
                    Text(
                      _time(track.duration),
                      style: const TextStyle(color: _Palette.muted, fontSize: 16),
                    ),
                  ],
                ),
                const SizedBox(height: 24),
                Row(
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: [
                    _Tap(
                      phone: phone,
                      id: 'media.previous',
                      onTap: () => phone.skip(-1),
                      radius: const BorderRadius.all(Radius.circular(36)),
                      child: const _Round(icon: Icons.skip_previous),
                    ),
                    const SizedBox(width: 32),
                    _Tap(
                      phone: phone,
                      id: 'media.play',
                      onTap: phone.playPause,
                      radius: const BorderRadius.all(Radius.circular(36)),
                      child: _Round(
                        icon: phone.playing ? Icons.pause : Icons.play_arrow,
                        color: _Palette.accent,
                        iconColor: const Color(0xFF0B1F3A),
                        size: 80,
                      ),
                    ),
                    const SizedBox(width: 32),
                    _Tap(
                      phone: phone,
                      id: 'media.next',
                      onTap: () => phone.skip(1),
                      radius: const BorderRadius.all(Radius.circular(36)),
                      child: const _Round(icon: Icons.skip_next),
                    ),
                  ],
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

// === phone ===

class _Phone extends StatelessWidget {
  final SimulatedPhone phone;

  const _Phone({required this.phone});

  @override
  Widget build(BuildContext context) {
    final call = phone.call;
    if (call != null) {
      return _CallScreen(phone: phone, call: call);
    }
    return Padding(
      padding: const EdgeInsets.all(24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          const Padding(
            padding: EdgeInsets.only(left: 8, bottom: 12),
            child: Text(
              'Contacts',
              style: TextStyle(fontSize: 28, fontWeight: FontWeight.w600),
            ),
          ),
          for (final contact in SimulatedPhone.contacts)
            _ListRow(
              phone: phone,
              id: 'phone.${contact.number}',
              icon: Icons.person,
              title: contact.name,
              subtitle: contact.number,
              onTap: () => phone.callContact(contact),
            ),
          _ListRow(
            phone: phone,
            id: 'phone.incoming',
            icon: Icons.call_received,
            title: 'Simulate an incoming call',
            subtitle: 'From ${SimulatedPhone.caller.name}',
            onTap: phone.simulateIncomingCall,
          ),
        ],
      ),
    );
  }
}

class _CallScreen extends StatelessWidget {
  final SimulatedPhone phone;
  final AndroidAutoCall call;

  const _CallScreen({required this.phone, required this.call});

  @override
  Widget build(BuildContext context) {
    final ringing = call.state == AndroidAutoCallState.incoming;
    final seconds = call.duration.inSeconds;
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Container(
            width: 120,
            height: 120,
            alignment: Alignment.center,
            decoration: const BoxDecoration(
              color: _Palette.cardHigh,
              shape: BoxShape.circle,
            ),
            child: Text(
              call.displayName.characters.first,
              style: const TextStyle(fontSize: 56),
            ),
          ),
          const SizedBox(height: 16),
          Text(call.displayName, style: const TextStyle(fontSize: 34)),
          Text(
            ringing
                ? 'Incoming call'
                : '${seconds ~/ 60}:${(seconds % 60).toString().padLeft(2, '0')}',
            style: const TextStyle(color: _Palette.muted),
          ),
          const SizedBox(height: 32),
          Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              if (ringing) ...[
                _Tap(
                  phone: phone,
                  id: 'call.answer',
                  onTap: phone.answer,
                  radius: const BorderRadius.all(Radius.circular(40)),
                  child: const _Round(color: _Palette.green, icon: Icons.call, size: 80),
                ),
                const SizedBox(width: 48),
              ],
              _Tap(
                phone: phone,
                id: 'call.end',
                onTap: phone.hangUp,
                radius: const BorderRadius.all(Radius.circular(40)),
                child: const _Round(color: _Palette.red, icon: Icons.call_end, size: 80),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class _IncomingBanner extends StatelessWidget {
  final SimulatedPhone phone;
  final AndroidAutoCall call;

  const _IncomingBanner({required this.phone, required this.call});

  @override
  Widget build(BuildContext context) {
    return _Card(
      color: _Palette.cardHigh,
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          const Icon(Icons.phone_in_talk, size: 36, color: _Palette.green),
          const SizedBox(width: 16),
          Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            mainAxisSize: MainAxisSize.min,
            children: [
              Text(call.displayName, style: const TextStyle(fontSize: 24)),
              const Text('Incoming call', style: TextStyle(color: _Palette.muted)),
            ],
          ),
          const SizedBox(width: 32),
          _Tap(
            phone: phone,
            id: 'banner.decline',
            onTap: phone.hangUp,
            radius: const BorderRadius.all(Radius.circular(28)),
            child: const _Round(color: _Palette.red, icon: Icons.call_end),
          ),
          const SizedBox(width: 16),
          _Tap(
            phone: phone,
            id: 'banner.answer',
            onTap: phone.answer,
            radius: const BorderRadius.all(Radius.circular(28)),
            child: const _Round(color: _Palette.green, icon: Icons.call),
          ),
        ],
      ),
    );
  }
}

// === launcher ===

class _Launcher extends StatelessWidget {
  final SimulatedPhone phone;

  const _Launcher({required this.phone});

  @override
  Widget build(BuildContext context) {
    final tiles = [
      (SimulatedApp.maps, Icons.navigation, 'Maps', const Color(0xFF1A73E8)),
      (SimulatedApp.media, Icons.headphones, 'Music', const Color(0xFFE8710A)),
      (SimulatedApp.phone, Icons.phone, 'Phone', _Palette.green),
    ];
    return Center(
      child: Wrap(
        spacing: 32,
        runSpacing: 32,
        children: [
          for (final (app, icon, label, color) in tiles)
            _Tap(
              phone: phone,
              id: 'launcher.${app.name}',
              onTap: () => phone.open(app),
              child: SizedBox(
                width: 140,
                child: Column(
                  children: [
                    const SizedBox(height: 8),
                    _Round(icon: icon, color: color, size: 96),
                    const SizedBox(height: 12),
                    Text(label),
                    const SizedBox(height: 8),
                  ],
                ),
              ),
            ),
          _Tap(
            phone: phone,
            id: 'launcher.assistant',
            onTap: phone.startAssistant,
            child: const SizedBox(
              width: 140,
              child: Column(
                children: [
                  SizedBox(height: 8),
                  _Round(icon: Icons.mic, color: _Palette.cardHigh, size: 96),
                  SizedBox(height: 12),
                  Text('Assistant'),
                  SizedBox(height: 8),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }
}

// === overlays ===

class _AssistantBar extends StatelessWidget {
  final SimulatedPhone phone;

  const _AssistantBar({required this.phone});

  @override
  Widget build(BuildContext context) {
    return const _Card(
      color: _Palette.cardHigh,
      child: Row(
        children: [
          Icon(Icons.mic, size: 36, color: _Palette.accent),
          SizedBox(width: 16),
          Expanded(
            child: Text(
              'Listening... (simulated, nothing is recorded)',
              style: TextStyle(fontSize: 22),
            ),
          ),
        ],
      ),
    );
  }
}

/// Where fingers are and where taps just landed, so a mis-mapped touch shows up.
class _Touches extends StatelessWidget {
  final SimulatedPhone phone;

  const _Touches({required this.phone});

  @override
  Widget build(BuildContext context) {
    return Stack(
      children: [
        for (final finger in phone.fingers.values)
          Positioned(
            left: finger.dx - 24,
            top: finger.dy - 24,
            child: Container(
              width: 48,
              height: 48,
              decoration: BoxDecoration(
                shape: BoxShape.circle,
                color: const Color(0x55FFFFFF),
                border: Border.all(color: const Color(0xCCFFFFFF), width: 2),
              ),
            ),
          ),
        for (final ripple in phone.ripples)
          Positioned(
            left: ripple.position.dx - 40,
            top: ripple.position.dy - 40,
            child: TweenAnimationBuilder<double>(
              key: ValueKey(ripple.id),
              tween: Tween(begin: 0, end: 1),
              duration: const Duration(milliseconds: 400),
              builder: (context, t, _) => Opacity(
                opacity: 1 - t,
                child: Transform.scale(
                  scale: 0.4 + 0.6 * t,
                  child: Container(
                    width: 80,
                    height: 80,
                    decoration: BoxDecoration(
                      shape: BoxShape.circle,
                      border: Border.all(color: const Color(0xFFFFFFFF), width: 3),
                    ),
                  ),
                ),
              ),
            ),
          ),
      ],
    );
  }
}

// === pieces ===

class _Card extends StatelessWidget {
  final Widget child;
  final Color color;

  const _Card({required this.child, this.color = _Palette.card});

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 16),
      decoration: BoxDecoration(
        color: color,
        borderRadius: const BorderRadius.all(Radius.circular(20)),
        boxShadow: const [BoxShadow(color: Color(0x66000000), blurRadius: 12)],
      ),
      child: child,
    );
  }
}

class _Round extends StatelessWidget {
  final IconData icon;
  final Color color;
  final Color iconColor;
  final double size;

  const _Round({
    required this.icon,
    this.color = _Palette.cardHigh,
    this.iconColor = _Palette.text,
    this.size = 56,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      width: size,
      height: size,
      decoration: BoxDecoration(color: color, shape: BoxShape.circle),
      child: Icon(icon, size: size * 0.5, color: iconColor),
    );
  }
}

class _ListRow extends StatelessWidget {
  final SimulatedPhone phone;
  final String id;
  final IconData icon;
  final String title;
  final String subtitle;
  final VoidCallback onTap;

  const _ListRow({
    required this.phone,
    required this.id,
    required this.icon,
    required this.title,
    required this.subtitle,
    required this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: _Tap(
        phone: phone,
        id: id,
        onTap: onTap,
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
          decoration: const BoxDecoration(
            color: _Palette.card,
            borderRadius: BorderRadius.all(Radius.circular(16)),
          ),
          child: Row(
            children: [
              _Round(icon: icon, size: 48),
              const SizedBox(width: 16),
              Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(title, style: const TextStyle(fontSize: 22)),
                  Text(
                    subtitle,
                    style: const TextStyle(color: _Palette.muted, fontSize: 16),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }
}
