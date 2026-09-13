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
    config: const AndroidAutoConfig(width: 1280, height: 720, fps: 30),
  );

  int _tapCount = 0;
  bool _patternRunning = false;
  String _lastInput = 'none';

  @override
  void initState() {
    super.initState();
    _controller.addListener(_onControllerChanged);
  }

  void _onControllerChanged() => setState(() {});

  @override
  void dispose() {
    _controller
      ..removeListener(_onControllerChanged)
      ..dispose();
    super.dispose();
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
