import 'dart:async';

import 'package:android_auto/android_auto.dart';
import 'package:flutter/material.dart';

void main() => runApp(const TestBenchApp());

/// Test bench for the android_auto plugin.
///
/// Deliberately shaped like the real thing from day one: the projection fills the
/// window and every piece of chrome is an ordinary Flutter widget drawn on top of it.
/// If overlaying ever stops working, this app breaks immediately rather than at the
/// end of the project.
class TestBenchApp extends StatelessWidget {
  const TestBenchApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Android Auto test bench',
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark(useMaterial3: true),
      home: const TestBenchPage(),
    );
  }
}

class TestBenchPage extends StatefulWidget {
  const TestBenchPage({super.key});

  @override
  State<TestBenchPage> createState() => _TestBenchPageState();
}

class _TestBenchPageState extends State<TestBenchPage> {
  final AndroidAutoController _controller = AndroidAutoController(
    config: const AndroidAutoConfig(
      width: 1280,
      height: 720,
      fps: 30,
      // Location is in here as well as the two defaults, because a test bench that
      // cannot exercise the GPS sensor cannot tell whether it works. It comes with the
      // obligation attached: the phone stops using its own receiver the moment it sees
      // this, so _gpsTimer below feeds a fix every second from the moment the session
      // starts. Take location out of this set in a real head unit that has no receiver.
      sensors: {
        AndroidAutoSensor.nightMode,
        AndroidAutoSensor.drivingStatus,
        AndroidAutoSensor.location,
      },
      // All five, which is more than the default. A test bench that cannot exercise the
      // media browser or the notifications cannot tell whether they work, and unlike a
      // sensor none of these is a promise: the phone pushes what it has.
      metadata: {
        AndroidAutoMetadata.navigation,
        AndroidAutoMetadata.media,
        AndroidAutoMetadata.phone,
        AndroidAutoMetadata.notification,
        AndroidAutoMetadata.browse,
      },
    ),
  );

  int _tapCount = 0;
  bool _patternRunning = false;
  String _lastInput = 'none';
  bool _audioPanelOpen = false;
  List<AndroidAutoAudioDevice> _audioDevices = const [];
  List<AndroidAutoAudioDevice> _microphoneDevices = const [];
  StreamSubscription<AndroidAutoAudioBuffer>? _pcmSubscription;
  int _pcmBytes = 0;
  double _pcmPeak = 0;
  bool _sensorPanelOpen = false;
  bool _metadataPanelOpen = false;
  AndroidAutoNotification? _lastNotification;
  AndroidAutoBrowseNode? _browseNode;
  /// Set when a browse request was refused, which is what a phone that never opened the
  /// browser channel looks like from here. Worth saying out loud: the alternative is a
  /// list that stays empty with no reason given.
  String? _browseRefusal;
  /// Where the browser has been, so there is a way back out of a library. The protocol
  /// has no parent pointer: a node knows its own path and nothing above it.
  final List<String> _browseTrail = [];
  final List<StreamSubscription<void>> _metadataSubscriptions = [];
  bool _feedGps = true;
  Timer? _gpsTimer;
  final TextEditingController _latitude = TextEditingController(text: '52.520008');
  final TextEditingController _longitude = TextEditingController(text: '13.404954');

  @override
  void initState() {
    super.initState();
    _controller.addListener(_onControllerChanged);
    // Once a second, which is what a GPS receiver produces and what the phone expects.
    // Started here rather than on connect because the value is remembered across
    // sessions: setting it before there is a phone is the case this exercises.
    _gpsTimer = Timer.periodic(const Duration(seconds: 1), (_) => _sendFix());
    _sendFix();
    // The controller repaints this page for navigation, media and telephony already.
    // These two are events rather than states, so nothing keeps them unless the app
    // does.
    _metadataSubscriptions.addAll([
      _controller.notifications.listen(
        (notification) => setState(() => _lastNotification = notification),
      ),
      _controller.browseResults.listen(
        (node) => setState(() => _browseNode = node),
      ),
    ]);
  }

  void _onControllerChanged() => setState(() {});

  @override
  void dispose() {
    _gpsTimer?.cancel();
    _latitude.dispose();
    _longitude.dispose();
    _pcmSubscription?.cancel();
    for (final subscription in _metadataSubscriptions) {
      subscription.cancel();
    }
    _controller
      ..removeListener(_onControllerChanged)
      ..dispose();
    super.dispose();
  }

  /// Pushes the position in the two fields, if the switch is on and they parse.
  ///
  /// A fixed point rather than a simulated drive. Feeding a phone a route it is not on
  /// makes Maps recalculate all the way through a test, which is noise rather than
  /// evidence: what M8 has to show is that a fix reaches the phone at all.
  void _sendFix() {
    if (!_feedGps) {
      return;
    }
    final latitude = double.tryParse(_latitude.text);
    final longitude = double.tryParse(_longitude.text);
    if (latitude == null || longitude == null) {
      return;
    }
    _controller.setLocation(
      AndroidAutoLocation(
        latitude: latitude,
        longitude: longitude,
        accuracyMetres: 5,
        speedMps: 0,
      ),
    );
  }

  /// Subscribes to the raw PCM the phone is sending, for apps that mix it themselves.
  ///
  /// Nothing is copied out of the core while nobody is listening, so this switch is
  /// what turns the tap on. Pair it with Play here off to hear the difference between
  /// the plugin playing the audio and the app being handed it.
  void _togglePcmTap(bool on) {
    if (!on) {
      _pcmSubscription?.cancel();
      setState(() {
        _pcmSubscription = null;
        _pcmBytes = 0;
        _pcmPeak = 0;
      });
      return;
    }
    setState(() {
      _pcmSubscription = _controller.audioBuffers.listen((buffer) {
        // Peak of the buffer, so the meter says something arrived rather than only
        // that bytes did. Signed 16 bit little endian, which is all the protocol sends.
        final samples = buffer.samples.buffer.asInt16List(
          buffer.samples.offsetInBytes,
          buffer.samples.lengthInBytes ~/ 2,
        );
        var peak = 0;
        for (final sample in samples) {
          final magnitude = sample.abs();
          if (magnitude > peak) {
            peak = magnitude;
          }
        }
        setState(() {
          _pcmBytes += buffer.samples.lengthInBytes;
          _pcmPeak = peak / 32768;
        });
      });
    });
  }

  Future<void> _togglePattern() async {
    if (_patternRunning) {
      await _controller.stopTestPattern();
    } else {
      if (_controller.state == AndroidAutoConnectionState.idle) {
        await _controller.start();
      }
      await _controller.startTestPattern();
    }
    setState(() => _patternRunning = !_patternRunning);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      body: Stack(
        fit: StackFit.expand,
        children: [
          AndroidAutoView(
            controller: _controller,
            placeholder: const _ProjectionPlaceholder(),
          ),
          Positioned(top: 0, left: 0, right: 0, child: _statusBar()),
          if (_audioPanelOpen)
            Positioned(top: 60, right: 20, width: 360, child: _audioPanel()),
          if (_sensorPanelOpen)
            Positioned(top: 60, left: 20, width: 380, child: _sensorPanel()),
          if (_metadataPanelOpen)
            Positioned(top: 60, left: 420, width: 440, child: _metadataPanel()),
          // The point of M9, drawn as ordinary Flutter widgets over the projection
          // rather than read off the phone's own pixels.
          Positioned(left: 20, bottom: 160, width: 440, child: _metadataOverlay()),
          Positioned(bottom: 24, left: 0, right: 0, child: _controls()),
        ],
      ),
    );
  }

  Widget _statusBar() {
    final textureId = _controller.textureId;
    final video = _controller.videoInfo;
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 12),
      color: Colors.black.withValues(alpha: 0.55),
      child: Row(
        children: [
          Icon(
            _controller.state == AndroidAutoConnectionState.connected
                ? Icons.directions_car
                : Icons.usb_off,
            size: 20,
          ),
          const SizedBox(width: 10),
          Text('State: ${_controller.state.name}'),
          const SizedBox(width: 24),
          Text('Texture: ${textureId ?? "none"}'),
          const SizedBox(width: 24),
          Text(
            video == null
                ? 'Video: none'
                : 'Video: ${video.width}x${video.height} (${video.decoder})',
          ),
          const SizedBox(width: 24),
          Text('Last input: $_lastInput'),
          const SizedBox(width: 24),
          Text('Audio: ${_controller.audioBackend}$_underrunSuffix'),
          const SizedBox(width: 24),
          Icon(
            _controller.nightMode ? Icons.dark_mode : Icons.light_mode,
            size: 18,
            color: Colors.white54,
          ),
          const SizedBox(width: 6),
          Text(
            _controller.drivingRestrictions.isEmpty ? 'Parked' : 'Moving',
            style: TextStyle(
              color: _controller.drivingRestrictions.isEmpty
                  ? Colors.white
                  : Colors.orangeAccent,
            ),
          ),
          const SizedBox(width: 24),
          _MicIndicator(controller: _controller),
          const Spacer(),
          Text('Overlay taps: $_tapCount'),
        ],
      ),
    );
  }

  /// The hardware buttons a real head unit has on its dashboard or steering wheel.
  ///
  /// Exists to exercise the key half of the input channel, which touch alone never
  /// reaches: a phone routes these itself rather than drawing them, so the only way to
  /// tell they arrived is to watch what the projection does.
  Widget _keypad() {
    Widget key(IconData icon, String tooltip, VoidCallback onPressed) {
      return IconButton(
        tooltip: tooltip,
        onPressed: onPressed,
        icon: Icon(icon, size: 20),
        style: IconButton.styleFrom(
          backgroundColor: Colors.black.withValues(alpha: 0.55),
        ),
      );
    }

    return Row(
      mainAxisAlignment: MainAxisAlignment.center,
      spacing: 8,
      children: [
        key(Icons.arrow_back, 'Back', () => _press(AndroidAutoKey.back)),
        key(Icons.home, 'Home', () => _press(AndroidAutoKey.home)),
        key(Icons.skip_previous, 'Previous', () => _press(AndroidAutoKey.previous)),
        key(Icons.play_arrow, 'Play/pause', () => _press(AndroidAutoKey.playPause)),
        key(Icons.skip_next, 'Next', () => _press(AndroidAutoKey.next)),
        key(Icons.mic, 'Assistant', () => _press(AndroidAutoKey.microphone)),
        const SizedBox(width: 16),
        key(Icons.rotate_left, 'Rotary anticlockwise', () => _rotate(-1)),
        key(Icons.adjust, 'Rotary push', () => _press(AndroidAutoKey.enter)),
        key(Icons.rotate_right, 'Rotary clockwise', () => _rotate(1)),
      ],
    );
  }

  /// Per stream volume, mute and the output device.
  ///
  /// The three streams are separate because Android Auto sends them separately and
  /// leaves the mixing to the head unit. Media ducking under speech is automatic, so
  /// the thing to watch here is the speech slider: turning it up and letting a
  /// navigation prompt play should visibly pull the media level down and let it back up.
  Widget _audioPanel() {
    Widget stream(String label, AndroidAutoAudioStream which) {
      final muted = _controller.muted(which);
      return Row(
        children: [
          SizedBox(width: 62, child: Text(label)),
          IconButton(
            tooltip: muted ? 'Unmute' : 'Mute',
            onPressed: () => setState(() => _controller.setMuted(which, !muted)),
            icon: Icon(muted ? Icons.volume_off : Icons.volume_up, size: 18),
          ),
          Expanded(
            child: Slider(
              value: _controller.volume(which),
              onChanged: (value) =>
                  setState(() => _controller.setVolume(which, value)),
            ),
          ),
          SizedBox(
            width: 38,
            child: Text(
              '${(_controller.volume(which) * 100).round()}%',
              textAlign: TextAlign.right,
              style: const TextStyle(fontSize: 12),
            ),
          ),
        ],
      );
    }

    return Container(
      padding: const EdgeInsets.fromLTRB(16, 12, 16, 12),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.75),
        borderRadius: BorderRadius.circular(10),
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text('Audio', style: TextStyle(fontWeight: FontWeight.bold)),
          const SizedBox(height: 4),
          stream('Media', AndroidAutoAudioStream.media),
          stream('System', AndroidAutoAudioStream.system),
          stream('Speech', AndroidAutoAudioStream.speech),
          const SizedBox(height: 4),
          Row(
            children: [
              const Text('Output', style: TextStyle(fontSize: 12)),
              const SizedBox(width: 10),
              Expanded(
                child: DropdownButton<String>(
                  isExpanded: true,
                  value: _controller.audioDevice,
                  style: const TextStyle(fontSize: 12, color: Colors.white),
                  items: [
                    const DropdownMenuItem(value: '', child: Text('System default')),
                    for (final device in _audioDevices)
                      DropdownMenuItem(
                        value: device.name,
                        child: Text(
                          device.description,
                          overflow: TextOverflow.ellipsis,
                        ),
                      ),
                  ],
                  onChanged: (value) =>
                      setState(() => _controller.setAudioDevice(value)),
                ),
              ),
            ],
          ),
          Row(
            children: [
              const Text('Mic', style: TextStyle(fontSize: 12)),
              const SizedBox(width: 10),
              Expanded(
                child: DropdownButton<String>(
                  isExpanded: true,
                  value: _controller.microphoneDevice,
                  style: const TextStyle(fontSize: 12, color: Colors.white),
                  items: [
                    const DropdownMenuItem(value: '', child: Text('System default')),
                    for (final device in _microphoneDevices)
                      DropdownMenuItem(
                        value: device.name,
                        child: Text(
                          device.description,
                          overflow: TextOverflow.ellipsis,
                        ),
                      ),
                  ],
                  onChanged: (value) =>
                      setState(() => _controller.setMicrophoneDevice(value)),
                ),
              ),
            ],
          ),
          SwitchListTile(
            dense: true,
            contentPadding: EdgeInsets.zero,
            title: const Text('Play here', style: TextStyle(fontSize: 12)),
            subtitle: const Text(
              'Off hands the PCM to the app and plays nothing',
              style: TextStyle(fontSize: 11),
            ),
            value: _controller.audioOutputEnabled,
            onChanged: (value) =>
                setState(() => _controller.setAudioOutputEnabled(value)),
          ),
          SwitchListTile(
            dense: true,
            contentPadding: EdgeInsets.zero,
            title: const Text('Tap raw PCM', style: TextStyle(fontSize: 12)),
            subtitle: Text(
              _pcmSubscription == null
                  ? 'Nothing is copied out while nobody listens'
                  : '${(_pcmBytes / 1024).round()} KB, peak '
                        '${(_pcmPeak * 100).round()}%',
              style: const TextStyle(fontSize: 11),
            ),
            value: _pcmSubscription != null,
            onChanged: _togglePcmTap,
          ),
          Text(
            'Latency ${_controller.audioLatency(AndroidAutoAudioStream.media).inMilliseconds} ms, '
            'underruns $_totalUnderruns, dropped $_totalDropped',
            style: const TextStyle(fontSize: 11, color: Colors.white54),
          ),
          Text(
            'Mic ${_controller.microphoneBackend}, '
            '${(_controller.microphoneBytes / 1024).round()} KB captured',
            style: const TextStyle(fontSize: 11, color: Colors.white54),
          ),
        ],
      ),
    );
  }

  /// What the head unit is telling the phone about the car.
  ///
  /// The two switches at the top are the ones with a visible effect: night mode flips
  /// the phone's own theme within a second, and moving locks parts of its interface.
  /// Watch the subscription line at the bottom before concluding anything, because a
  /// value set for a sensor the phone did not subscribe to goes nowhere by design.
  Widget _sensorPanel() {
    final subscribed = _controller.sensorSubscriptions;
    return Container(
      padding: const EdgeInsets.fromLTRB(16, 12, 16, 12),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.75),
        borderRadius: BorderRadius.circular(10),
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text('Sensors', style: TextStyle(fontWeight: FontWeight.bold)),
          SwitchListTile(
            dense: true,
            contentPadding: EdgeInsets.zero,
            title: const Text('Night mode', style: TextStyle(fontSize: 12)),
            subtitle: const Text(
              "Flips the phone's own light and dark theme",
              style: TextStyle(fontSize: 11),
            ),
            value: _controller.nightMode,
            onChanged: (value) => setState(() => _controller.setNightMode(value)),
          ),
          SwitchListTile(
            dense: true,
            contentPadding: EdgeInsets.zero,
            title: const Text('Moving', style: TextStyle(fontSize: 12)),
            subtitle: const Text(
              'Locks the keyboard, settings and video on the phone',
              style: TextStyle(fontSize: 11),
            ),
            value: _controller.drivingRestrictions.isNotEmpty,
            onChanged: (value) => setState(() => _controller.setParked(!value)),
          ),
          SwitchListTile(
            dense: true,
            contentPadding: EdgeInsets.zero,
            title: const Text('Feed GPS', style: TextStyle(fontSize: 12)),
            subtitle: const Text(
              'A fix a second. The phone stops using its own receiver',
              style: TextStyle(fontSize: 11),
            ),
            value: _feedGps,
            onChanged: (value) {
              setState(() => _feedGps = value);
              _sendFix();
            },
          ),
          Row(
            spacing: 10,
            children: [
              Expanded(
                child: TextField(
                  controller: _latitude,
                  style: const TextStyle(fontSize: 12),
                  decoration: const InputDecoration(
                    labelText: 'Latitude',
                    isDense: true,
                  ),
                  onSubmitted: (_) => _sendFix(),
                ),
              ),
              Expanded(
                child: TextField(
                  controller: _longitude,
                  style: const TextStyle(fontSize: 12),
                  decoration: const InputDecoration(
                    labelText: 'Longitude',
                    isDense: true,
                  ),
                  onSubmitted: (_) => _sendFix(),
                ),
              ),
            ],
          ),
          const SizedBox(height: 8),
          Text(
            'Advertised: '
            '${_controller.config.sensors.map((s) => s.name).join(", ")}',
            style: const TextStyle(fontSize: 11, color: Colors.white54),
          ),
          Text(
            'Subscribed: '
            '${subscribed.isEmpty ? "none" : subscribed.map((s) => s.name).join(", ")}',
            style: const TextStyle(fontSize: 11, color: Colors.white54),
          ),
          Text(
            '${_controller.sensorBatches} readings sent',
            style: const TextStyle(fontSize: 11, color: Colors.white54),
          ),
        ],
      ),
    );
  }


  /// The turn card, the call banner and the now playing bar, over the projection.
  ///
  /// This is what M9 is for: none of it is read off the phone's pixels, all of it is
  /// ordinary Flutter drawn from the metadata channels. Each piece appears only when
  /// there is something to say, so an idle head unit shows an empty corner rather than
  /// three placeholders.
  Widget _metadataOverlay() {
    final navigation = _controller.lastNavigation;
    final call = _controller.lastPhoneStatus?.activeCall;
    final media = _controller.lastMediaInfo;
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      spacing: 10,
      children: [
        if (navigation != null && navigation.isGuiding) _turnCard(navigation),
        if (call != null && call.state != AndroidAutoCallState.inactive)
          _callBanner(call),
        if (media != null && !media.isEmpty) _nowPlayingBar(media),
      ],
    );
  }

  Widget _overlayCard({required Widget child}) => Container(
    padding: const EdgeInsets.fromLTRB(14, 12, 14, 12),
    decoration: BoxDecoration(
      color: Colors.black.withValues(alpha: 0.72),
      borderRadius: BorderRadius.circular(12),
    ),
    child: child,
  );

  /// The next instruction, drawn from the maneuver rather than from an image.
  ///
  /// The head unit asks for the ENUM instrument cluster type, so the phone says "normal
  /// left" and this picks the arrow. A phone old enough to send a rendered image
  /// instead is honoured too, which is what [AndroidAutoNavigation.maneuverImage] is.
  Widget _turnCard(AndroidAutoNavigation navigation) {
    final destination = navigation.destination;
    return _overlayCard(
      child: Row(
        spacing: 14,
        children: [
          if (navigation.maneuverImage != null)
            Image.memory(navigation.maneuverImage!, width: 44, height: 44)
          else
            Icon(_maneuverIcon(navigation.maneuver), size: 44),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: [
                Text(
                  navigation.stepDistance.isEmpty
                      ? (navigation.maneuver?.name ?? 'Guiding')
                      : navigation.stepDistance.display,
                  style: const TextStyle(
                    fontSize: 22,
                    fontWeight: FontWeight.bold,
                  ),
                ),
                Text(
                  navigation.road ?? navigation.cue.firstOrNull ?? '',
                  overflow: TextOverflow.ellipsis,
                ),
                if (navigation.currentRoad != null)
                  Text(
                    'on ${navigation.currentRoad}',
                    style: const TextStyle(fontSize: 11, color: Colors.white54),
                  ),
                if (destination != null)
                  Text(
                    [
                      if (destination.etaText != null) destination.etaText,
                      if (!destination.distance.isEmpty) destination.distance.display,
                      if (destination.address != null) destination.address,
                    ].join('  '),
                    style: const TextStyle(fontSize: 11, color: Colors.white54),
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

  Widget _callBanner(AndroidAutoCall call) => _overlayCard(
    child: Row(
      spacing: 12,
      children: [
        if (call.thumbnail != null)
          ClipOval(
            child: Image.memory(call.thumbnail!, width: 36, height: 36),
          )
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
                '${call.state.name}  ${_hms(call.duration)}',
                style: const TextStyle(fontSize: 11, color: Colors.white54),
              ),
            ],
          ),
        ),
      ],
    ),
  );

  /// The now playing bar. The deliverable M9 is measured by.
  Widget _nowPlayingBar(AndroidAutoMediaInfo media) {
    final duration = media.duration;
    final position = media.position;
    return _overlayCard(
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
                      style: const TextStyle(
                        fontSize: 16,
                        fontWeight: FontWeight.bold,
                      ),
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
                        style: const TextStyle(
                          fontSize: 11,
                          color: Colors.white38,
                        ),
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
              value: ((position ?? Duration.zero).inMilliseconds /
                      duration.inMilliseconds)
                  .clamp(0.0, 1.0),
              minHeight: 4,
              backgroundColor: Colors.white12,
            ),
            const SizedBox(height: 4),
            Text(
              '${_hms(position ?? Duration.zero)} / ${_hms(duration)}',
              style: const TextStyle(fontSize: 11, color: Colors.white38),
            ),
          ],
        ],
      ),
    );
  }

  /// What arrived on each metadata channel, and the media browser.
  ///
  /// The counters at the top are the first thing to read when a card above is empty:
  /// a channel the phone never opened and a channel that opened and said nothing are
  /// different problems, and only these two lines tell them apart.
  Widget _metadataPanel() {
    final opened = _controller.metadataChannels;
    return Container(
      padding: const EdgeInsets.fromLTRB(16, 12, 16, 12),
      decoration: BoxDecoration(
        color: Colors.black.withValues(alpha: 0.78),
        borderRadius: BorderRadius.circular(10),
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text('Metadata', style: TextStyle(fontWeight: FontWeight.bold)),
          const SizedBox(height: 4),
          for (final kind in AndroidAutoMetadata.values)
            Text(
              '${kind.name}: ${opened.contains(kind) ? "open" : "not opened"}, '
              '${_controller.metadataUpdates(kind)} updates',
              style: TextStyle(
                fontSize: 11,
                color: opened.contains(kind) ? Colors.white70 : Colors.white38,
              ),
            ),
          const Divider(height: 18),
          Text(
            'Notification: ${_lastNotification?.text ?? "none"}',
            style: const TextStyle(fontSize: 11, color: Colors.white54),
            maxLines: 2,
            overflow: TextOverflow.ellipsis,
          ),
          const Divider(height: 18),
          Row(
            children: [
              const Text('Library', style: TextStyle(fontSize: 12)),
              const Spacer(),
              if (_browseTrail.isNotEmpty)
                TextButton(
                  onPressed: _browseBack,
                  child: const Text('Back', style: TextStyle(fontSize: 12)),
                ),
              TextButton(
                onPressed: () {
                  _browseTrail.clear();
                  _requestBrowse('');
                },
                child: const Text('Root', style: TextStyle(fontSize: 12)),
              ),
            ],
          ),
          SizedBox(height: 180, child: _browseList()),
        ],
      ),
    );
  }

  Widget _browseList() {
    final refusal = _browseRefusal;
    if (refusal != null) {
      return Center(
        child: Text(
          refusal,
          style: const TextStyle(fontSize: 11, color: Colors.orangeAccent),
          textAlign: TextAlign.center,
        ),
      );
    }
    final node = _browseNode;
    if (node == null) {
      return const Center(
        child: Text(
          'Press Root to ask the phone for its media library',
          style: TextStyle(fontSize: 11, color: Colors.white38),
          textAlign: TextAlign.center,
        ),
      );
    }
    final entries = <_BrowseEntry>[
      for (final source in node.sources)
        _BrowseEntry(source.path, source.name ?? source.path, Icons.apps, true),
      for (final list in node.lists)
        _BrowseEntry(list.path, list.name ?? list.path, Icons.queue_music, true),
      for (final song in node.songs)
        _BrowseEntry(song.path, song.name, Icons.music_note, false),
      if (node.song != null)
        _BrowseEntry(node.song!.path, node.song!.name, Icons.music_note, false),
    ];
    if (entries.isEmpty) {
      return Center(
        child: Text(
          'Nothing under ${node.path.isEmpty ? "the root" : node.path}',
          style: const TextStyle(fontSize: 11, color: Colors.white38),
          textAlign: TextAlign.center,
        ),
      );
    }
    return ListView.builder(
      itemCount: entries.length,
      itemBuilder: (context, index) {
        final entry = entries[index];
        return ListTile(
          dense: true,
          visualDensity: VisualDensity.compact,
          contentPadding: EdgeInsets.zero,
          leading: Icon(entry.icon, size: 18),
          title: Text(entry.name, style: const TextStyle(fontSize: 12)),
          onTap: () {
            if (entry.isContainer) {
              _browseTrail.add(node.path);
              _requestBrowse(entry.path);
            } else {
              // A song is played rather than opened. The protocol calls it a browser
              // input, which is the head unit reporting that someone pressed enter on
              // this path.
              _controller.browseSelect(entry.path);
            }
          },
        );
      },
    );
  }

  void _browseBack() {
    if (_browseTrail.isEmpty) {
      return;
    }
    _requestBrowse(_browseTrail.removeLast());
  }

  /// Asks for a node, and says so when the answer is no.
  ///
  /// A refusal means the phone never opened the browser channel, which is what this
  /// Pixel does. Showing it beats an empty list that never fills in.
  void _requestBrowse(String path) {
    final sent = _controller.browse(path: path);
    setState(() {
      _browseRefusal = sent
          ? null
          : 'The phone has not opened the media browser channel, so there is nothing '
                'to ask.';
    });
  }

  void _toggleMetadataPanel() {
    setState(() => _metadataPanelOpen = !_metadataPanelOpen);
  }

  static String _hms(Duration duration) {
    final minutes = duration.inMinutes;
    final seconds = duration.inSeconds % 60;
    return '$minutes:${seconds.toString().padLeft(2, "0")}';
  }

  /// One arrow per maneuver family. Material has nothing for a sharp left or a
  /// roundabout exit, so the forty three values collapse onto the dozen icons that do
  /// exist. A real head unit would ship its own artwork.
  static IconData _maneuverIcon(AndroidAutoManeuver? maneuver) {
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

  int get _totalUnderruns => AndroidAutoAudioStream.values
      .map(_controller.audioUnderruns)
      .fold(0, (a, b) => a + b);

  int get _totalDropped => AndroidAutoAudioStream.values
      .map(_controller.audioDropped)
      .fold(0, (a, b) => a + b);

  String get _underrunSuffix =>
      _totalUnderruns == 0 ? '' : ' ($_totalUnderruns underruns)';

  Future<void> _toggleAudioPanel() async {
    final outputs = _audioPanelOpen ? _audioDevices : await _controller.audioDevices();
    final inputs =
        _audioPanelOpen ? _microphoneDevices : await _controller.microphoneDevices();
    if (!mounted) {
      return;
    }
    setState(() {
      _audioDevices = outputs;
      _microphoneDevices = inputs;
      _audioPanelOpen = !_audioPanelOpen;
    });
  }

  void _press(AndroidAutoKey key) {
    _controller.pressKey(key);
    setState(() => _lastInput = 'key ${key.name}');
  }

  void _rotate(int steps) {
    _controller.sendRotary(steps);
    setState(() => _lastInput = 'rotary $steps');
  }

  Widget _controls() {
    final message = _controller.message;
    return Column(
      children: [
        if (message != null)
          Container(
            margin: const EdgeInsets.symmetric(horizontal: 60),
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
            decoration: BoxDecoration(
              color: Colors.black.withValues(alpha: 0.6),
              borderRadius: BorderRadius.circular(8),
            ),
            child: Text(
              message,
              textAlign: TextAlign.center,
              style: const TextStyle(color: Colors.orangeAccent),
            ),
          ),
        const SizedBox(height: 12),
        _keypad(),
        const SizedBox(height: 12),
        Row(
          mainAxisAlignment: MainAxisAlignment.center,
          spacing: 16,
          children: [
            FilledButton.icon(
              onPressed: _controller.start,
              icon: const Icon(Icons.play_arrow),
              label: const Text('Start head unit'),
            ),
            FilledButton.tonalIcon(
              onPressed: _togglePattern,
              icon: Icon(_patternRunning ? Icons.pause : Icons.gradient),
              label: Text(_patternRunning ? 'Stop pattern' : 'Test pattern'),
            ),
            FilledButton.tonalIcon(
              onPressed: _toggleAudioPanel,
              icon: const Icon(Icons.tune),
              label: const Text('Audio'),
            ),
            FilledButton.tonalIcon(
              onPressed: () =>
                  setState(() => _sensorPanelOpen = !_sensorPanelOpen),
              icon: const Icon(Icons.sensors),
              label: const Text('Sensors'),
            ),
            FilledButton.tonalIcon(
              onPressed: _toggleMetadataPanel,
              icon: const Icon(Icons.info_outline),
              label: const Text('Metadata'),
            ),
            OutlinedButton.icon(
              onPressed: () async {
                await _controller.stop();
                setState(() => _patternRunning = false);
              },
              icon: const Icon(Icons.stop),
              label: const Text('Stop'),
            ),
            // Proves that widgets drawn over the projection still receive input, and
            // gives the agent a click target when testing GUI automation.
            OutlinedButton(
              onPressed: () => setState(() => _tapCount++),
              child: const Text('Overlay hit test'),
            ),
          ],
        ),
      ],
    );
  }
}

/// One row of the media browser, flattened out of whichever collection the node had.
class _BrowseEntry {
  final String path;
  final String name;
  final IconData icon;

  /// Whether tapping it opens something or plays something.
  final bool isContainer;

  const _BrowseEntry(this.path, this.name, this.icon, this.isContainer);
}

/// Says whether the car is listening, and how loudly.
///
/// Its own widget with its own timer on purpose. The microphone flag only changes when
/// the phone opens or closes it, but the level changes with every 32 ms buffer, and
/// rebuilding the page around the projection at that rate to move a meter would be
/// absurd. Polling is the right shape here: the native side keeps the value, this asks
/// for it five times a second and repaints only when it has moved.
class _MicIndicator extends StatefulWidget {
  final AndroidAutoController controller;

  const _MicIndicator({required this.controller});

  @override
  State<_MicIndicator> createState() => _MicIndicatorState();
}

class _MicIndicatorState extends State<_MicIndicator> {
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
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Icon(
          _active ? Icons.mic : Icons.mic_off,
          size: 18,
          color: _active ? Colors.redAccent : Colors.white38,
        ),
        const SizedBox(width: 6),
        // Sized whether or not it is active, so the status bar does not jump when the
        // Assistant is invoked.
        SizedBox(
          width: 60,
          child: LinearProgressIndicator(
            value: _active ? _level.clamp(0.0, 1.0) : 0,
            minHeight: 6,
            backgroundColor: Colors.white12,
            color: Colors.redAccent,
          ),
        ),
      ],
    );
  }
}

/// Stands in for the projected video until a session is running.
class _ProjectionPlaceholder extends StatelessWidget {
  const _ProjectionPlaceholder();

  @override
  Widget build(BuildContext context) {
    return Container(
      decoration: const BoxDecoration(
        gradient: LinearGradient(
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
          colors: [Color(0xFF15202B), Color(0xFF0B1016)],
        ),
      ),
      alignment: Alignment.center,
      child: const Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(Icons.cast_connected, size: 64, color: Colors.white24),
          SizedBox(height: 16),
          Text(
            'No video',
            style: TextStyle(fontSize: 22, color: Colors.white38),
          ),
          SizedBox(height: 6),
          Text(
            'Press Test pattern to drive the texture without a phone',
            style: TextStyle(color: Colors.white24),
          ),
        ],
      ),
    );
  }
}
