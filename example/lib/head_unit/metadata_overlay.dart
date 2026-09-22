// SPDX-License-Identifier: GPL-3.0-or-later
import 'package:android_auto/android_auto.dart';
import 'package:flutter/material.dart';

/// The turn card, the call banner and the now playing bar, over the projection.
///
/// This is what the metadata channels are for: none of it is read off the phone's
/// pixels, all of it is ordinary Flutter drawn from what the phone reports. Each piece
/// appears only when there is something to say, so an idle head unit shows an empty
/// corner rather than three placeholders.
///
/// Reads the `last*` snapshots rather than listening to the streams, because the
/// controller already notifies on every update and a snapshot is what a widget built
/// after the fact needs.
class MetadataOverlay extends StatelessWidget {
  final AndroidAutoController controller;

  const MetadataOverlay({super.key, required this.controller});

  @override
  Widget build(BuildContext context) {
    final navigation = controller.lastNavigation;
    final call = controller.lastPhoneStatus?.activeCall;
    final media = controller.lastMediaInfo;
    return AnimatedSize(
      duration: const Duration(milliseconds: 200),
      alignment: Alignment.bottomLeft,
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        spacing: 10,
        children: [
          if (navigation != null && navigation.isGuiding)
            TurnCard(navigation: navigation),
          if (call != null && call.state != AndroidAutoCallState.inactive)
            CallBanner(call: call),
          if (media != null && !media.isEmpty) NowPlayingBar(media: media),
        ],
      ),
    );
  }
}

/// The frame every overlay card shares: dark enough to read over a bright map, round
/// enough not to look like part of it.
class OverlayCard extends StatelessWidget {
  final Widget child;

  const OverlayCard({super.key, required this.child});

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.fromLTRB(14, 12, 14, 12),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.78),
        borderRadius: BorderRadius.circular(14),
      ),
      child: child,
    );
  }
}

/// The next instruction, drawn from the maneuver rather than from an image.
///
/// The head unit asks for the ENUM instrument cluster type, so the phone says "normal
/// left" and this picks the arrow. A phone old enough to send a rendered image instead
/// is honoured too, which is what [AndroidAutoNavigation.maneuverImage] is.
class TurnCard extends StatelessWidget {
  final AndroidAutoNavigation navigation;

  const TurnCard({super.key, required this.navigation});

  @override
  Widget build(BuildContext context) {
    final destination = navigation.destination;
    const secondary = TextStyle(fontSize: 11, color: Colors.white54);
    return OverlayCard(
      child: Row(
        spacing: 14,
        children: [
          if (navigation.maneuverImage != null)
            Image.memory(navigation.maneuverImage!, width: 44, height: 44)
          else
            Icon(maneuverIcon(navigation.maneuver), size: 44),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: [
                Text(
                  navigation.stepDistance.isEmpty
                      ? (navigation.maneuver?.name ?? 'Guiding')
                      : navigation.stepDistance.display,
                  style: const TextStyle(fontSize: 22, fontWeight: FontWeight.bold),
                ),
                Text(
                  navigation.road ?? navigation.cue.firstOrNull ?? '',
                  overflow: TextOverflow.ellipsis,
                ),
                if (navigation.currentRoad != null)
                  Text('on ${navigation.currentRoad}', style: secondary),
                if (destination != null)
                  Text(
                    [
                      if (destination.etaText != null) destination.etaText,
                      if (!destination.distance.isEmpty) destination.distance.display,
                      if (destination.address != null) destination.address,
                    ].join('  '),
                    style: secondary,
                    overflow: TextOverflow.ellipsis,
                  ),
              ],
            ),
          ),
          if (navigation.status == AndroidAutoNavigationStatus.rerouting)
            const SizedBox(
              width: 16,
              height: 16,
              child: CircularProgressIndicator(strokeWidth: 2),
            ),
        ],
      ),
    );
  }
}

/// A call in progress. The audio itself goes over Bluetooth, not through this plugin.
class CallBanner extends StatelessWidget {
  final AndroidAutoCall call;

  const CallBanner({super.key, required this.call});

  @override
  Widget build(BuildContext context) {
    return OverlayCard(
      child: Row(
        spacing: 12,
        children: [
          if (call.thumbnail != null)
            ClipOval(child: Image.memory(call.thumbnail!, width: 36, height: 36))
          else
            const Icon(Icons.phone_in_talk, size: 28, color: Colors.greenAccent),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: [
                Text(
                  call.displayName,
                  style: const TextStyle(fontWeight: FontWeight.bold),
                  overflow: TextOverflow.ellipsis,
                ),
                Text(
                  '${call.state.name}  ${formatDuration(call.duration)}',
                  style: const TextStyle(fontSize: 11, color: Colors.white54),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

/// The now playing bar, drawn from the media playback channel rather than the video.
class NowPlayingBar extends StatelessWidget {
  final AndroidAutoMediaInfo media;

  const NowPlayingBar({super.key, required this.media});

  @override
  Widget build(BuildContext context) {
    final duration = media.duration;
    final position = media.position ?? Duration.zero;
    return OverlayCard(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            spacing: 12,
            children: [
              if (media.albumArt != null)
                ClipRRect(
                  borderRadius: BorderRadius.circular(6),
                  child: Image.memory(media.albumArt!, width: 52, height: 52),
                )
              else
                const Icon(Icons.album, size: 40, color: Colors.white24),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Text(
                      media.song ?? 'Unknown track',
                      style: const TextStyle(fontSize: 16, fontWeight: FontWeight.bold),
                      overflow: TextOverflow.ellipsis,
                    ),
                    Text(
                      [
                        if (media.artist != null) media.artist,
                        if (media.album != null) media.album,
                      ].join('  -  '),
                      style: const TextStyle(color: Colors.white70),
                      overflow: TextOverflow.ellipsis,
                    ),
                    if (media.source != null)
                      Text(
                        media.source!,
                        style: const TextStyle(fontSize: 11, color: Colors.white38),
                      ),
                  ],
                ),
              ),
              Icon(
                media.isPlaying ? Icons.play_arrow : Icons.pause,
                color: media.isPlaying ? Colors.greenAccent : Colors.white54,
              ),
            ],
          ),
          if (duration != null && duration > Duration.zero) ...[
            const SizedBox(height: 8),
            LinearProgressIndicator(
              value: (position.inMilliseconds / duration.inMilliseconds).clamp(
                0.0,
                1.0,
              ),
              minHeight: 4,
              borderRadius: BorderRadius.circular(2),
              backgroundColor: Colors.white12,
            ),
            const SizedBox(height: 4),
            Text(
              '${formatDuration(position)} / ${formatDuration(duration)}',
              style: const TextStyle(fontSize: 11, color: Colors.white38),
            ),
          ],
        ],
      ),
    );
  }
}

/// `m:ss`, which is how a car shows a track position or a call timer.
String formatDuration(Duration duration) {
  final minutes = duration.inMinutes;
  final seconds = duration.inSeconds % 60;
  return '$minutes:${seconds.toString().padLeft(2, "0")}';
}

/// One arrow per maneuver family. Material has nothing for a sharp left or a roundabout
/// exit, so the forty three values collapse onto the dozen icons that do exist. A real
/// head unit would ship its own artwork.
IconData maneuverIcon(AndroidAutoManeuver? maneuver) {
  if (maneuver == null) {
    return Icons.navigation;
  }
  if (maneuver.isDestination) {
    return Icons.place;
  }
  if (maneuver.isRoundabout) {
    return Icons.roundabout_left;
  }
  switch (maneuver) {
    case AndroidAutoManeuver.straight:
    case AndroidAutoManeuver.depart:
    case AndroidAutoManeuver.nameChange:
      return Icons.straight;
    case AndroidAutoManeuver.uTurnLeft:
    case AndroidAutoManeuver.uTurnRight:
    case AndroidAutoManeuver.onRampUTurnLeft:
    case AndroidAutoManeuver.onRampUTurnRight:
      return Icons.u_turn_left;
    case AndroidAutoManeuver.ferryBoat:
      return Icons.directions_boat;
    case AndroidAutoManeuver.ferryTrain:
      return Icons.train;
    default:
      if (maneuver.turnsLeft) {
        return Icons.turn_left;
      }
      if (maneuver.turnsRight) {
        return Icons.turn_right;
      }
      return Icons.navigation;
  }
}
