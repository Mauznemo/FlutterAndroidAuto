// SPDX-License-Identifier: GPL-3.0-or-later
import 'dart:async';

import 'package:android_auto/android_auto.dart';
import 'package:flutter/material.dart';

/// The head unit's own view of itself, in a bar above the projection.
///
/// Connection state first, because it is the one thing a driver needs, then what the
/// session is doing, then what the car is reporting. The latest message from the
/// plugin sits in the middle: decoder statistics while all is well, the reason when it
/// is not.
class StatusBar extends StatelessWidget {
  final AndroidAutoController controller;

  /// The last key or rotary event the head unit sent, for watching the input channel.
  final String lastInput;

  const StatusBar({super.key, required this.controller, required this.lastInput});

  @override
  Widget build(BuildContext context) {
    final state = controller.state;
    final video = controller.videoInfo;
    final message = controller.message;
    final underruns = AndroidAutoAudioStream.values
        .map(controller.audioUnderruns)
        .fold(0, (a, b) => a + b);
    final moving = controller.drivingRestrictions.isNotEmpty;
    const dim = TextStyle(fontSize: 13, color: Colors.white70);

    return Container(
      height: 40,
      padding: const EdgeInsets.symmetric(horizontal: 16),
      color: Colors.black.withValues(alpha: 0.6),
      child: Row(
        spacing: 18,
        children: [
          _ConnectionChip(state: state),
          Tooltip(
            message: 'Flutter texture ${controller.textureId ?? "not registered"}',
            child: Text(
              video == null
                  ? 'No video'
                  : '${video.width}x${video.height} ${video.decoder}',
              style: dim,
            ),
          ),
          Expanded(
            child: Text(
              message ?? '',
              overflow: TextOverflow.ellipsis,
              style: TextStyle(
                fontSize: 12,
                color: state == AndroidAutoConnectionState.error
                    ? Colors.orangeAccent
                    : Colors.white38,
              ),
            ),
          ),
          Text('Input: $lastInput', style: dim),
          Text(
            'Audio: ${controller.audioBackend}'
            '${underruns == 0 ? "" : ", $underruns underruns"}',
            style: dim,
          ),
          Row(
            mainAxisSize: MainAxisSize.min,
            spacing: 6,
            children: [
              Icon(
                controller.nightMode ? Icons.dark_mode : Icons.light_mode,
                size: 16,
                color: Colors.white54,
              ),
              Text(
                moving ? 'Moving' : 'Parked',
                style: dim.copyWith(
                  color: moving ? Colors.orangeAccent : Colors.white70,
                ),
              ),
            ],
          ),
          MicIndicator(controller: controller),
        ],
      ),
    );
  }
}

class _ConnectionChip extends StatelessWidget {
  final AndroidAutoConnectionState state;

  const _ConnectionChip({required this.state});

  @override
  Widget build(BuildContext context) {
    final (label, color) = switch (state) {
      AndroidAutoConnectionState.idle => ('Idle', Colors.white38),
      AndroidAutoConnectionState.searching => ('Waiting for a phone', Colors.amber),
      AndroidAutoConnectionState.handshaking => ('Connecting', Colors.amber),
      AndroidAutoConnectionState.connected => ('Connected', Colors.greenAccent),
      AndroidAutoConnectionState.error => ('Error', Colors.redAccent),
    };
    return Row(
      mainAxisSize: MainAxisSize.min,
      spacing: 8,
      children: [
        Icon(Icons.circle, size: 10, color: color),
        Text(
          label,
          style: const TextStyle(fontSize: 13, fontWeight: FontWeight.w600),
        ),
      ],
    );
  }
}

/// Says whether the car is listening, and how loudly.
///
/// Its own widget with its own timer on purpose. The microphone flag only changes when
/// the phone opens or closes it, but the level changes with every 32 ms buffer, and
/// rebuilding the page around the projection at that rate to move a meter would be
/// absurd. Polling is the right shape here: the native side keeps the value, this asks
/// for it five times a second and repaints only when it has moved.
class MicIndicator extends StatefulWidget {
  final AndroidAutoController controller;

  const MicIndicator({super.key, required this.controller});

  @override
  State<MicIndicator> createState() => _MicIndicatorState();
}

class _MicIndicatorState extends State<MicIndicator> {
  Timer? _timer;
  bool _active = false;
  double _level = 0;

  @override
  void initState() {
    super.initState();
    _timer = Timer.periodic(const Duration(milliseconds: 200), (_) => _poll());
  }

  void _poll() {
    final active = widget.controller.microphoneActive;
    final level = widget.controller.microphoneLevel;
    // Only when something visible changed. The level is quantised so that room noise
    // does not repaint this twice a second forever.
    if (active != _active || (level * 20).round() != (_level * 20).round()) {
      setState(() {
        _active = active;
        _level = level;
      });
    }
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Tooltip(
      message: _active
          ? 'The phone has the microphone open'
          : 'Nothing is being captured',
      child: Row(
        mainAxisSize: MainAxisSize.min,
        spacing: 6,
        children: [
          Icon(
            _active ? Icons.mic : Icons.mic_off,
            size: 16,
            color: _active ? Colors.redAccent : Colors.white38,
          ),
          // Sized whether or not it is active, so the status bar does not jump when
          // the Assistant is invoked.
          SizedBox(
            width: 48,
            child: LinearProgressIndicator(
              value: _active ? _level.clamp(0.0, 1.0) : 0,
              minHeight: 5,
              borderRadius: BorderRadius.circular(3),
              backgroundColor: Colors.white12,
              color: Colors.redAccent,
            ),
          ),
        ],
      ),
    );
  }
}
