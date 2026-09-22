// SPDX-License-Identifier: GPL-3.0-or-later
import 'package:android_auto/android_auto.dart';
import 'package:flutter/material.dart';

import 'panel_frame.dart';
import 'pcm_tap.dart';

/// Per stream volume, mute, the devices, and what happens to the PCM.
///
/// The three streams are separate because Android Auto sends them separately and
/// leaves the mixing to the head unit. Media ducking under speech is automatic, so the
/// thing to watch here is the speech slider: turning it up and letting a navigation
/// prompt play should audibly pull the media level down and let it back up.
class AudioPanel extends StatefulWidget {
  final AndroidAutoController controller;
  final PcmTap pcmTap;
  final VoidCallback onClose;

  const AudioPanel({
    super.key,
    required this.controller,
    required this.pcmTap,
    required this.onClose,
  });

  @override
  State<AudioPanel> createState() => _AudioPanelState();
}

class _AudioPanelState extends State<AudioPanel> {
  List<AndroidAutoAudioDevice> _outputs = const [];
  List<AndroidAutoAudioDevice> _inputs = const [];

  AndroidAutoController get _controller => widget.controller;

  @override
  void initState() {
    super.initState();
    _loadDevices();
  }

  /// Asked for once per opening. Devices come and go rarely enough that a list read
  /// when the panel opens is current enough.
  Future<void> _loadDevices() async {
    final outputs = await _controller.audioDevices();
    final inputs = await _controller.microphoneDevices();
    if (mounted) {
      setState(() {
        _outputs = outputs;
        _inputs = inputs;
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    final underruns = AndroidAutoAudioStream.values
        .map(_controller.audioUnderruns)
        .fold(0, (a, b) => a + b);
    final dropped = AndroidAutoAudioStream.values
        .map(_controller.audioDropped)
        .fold(0, (a, b) => a + b);
    final latency = _controller.audioLatency(AndroidAutoAudioStream.media);
    return PanelFrame(
      title: 'Audio',
      icon: Icons.tune,
      onClose: widget.onClose,
      children: [
        _stream('Media', AndroidAutoAudioStream.media),
        _stream('System', AndroidAutoAudioStream.system),
        _stream('Speech', AndroidAutoAudioStream.speech),
        const PanelSection('Devices'),
        _devicePicker(
          label: 'Output',
          devices: _outputs,
          value: _controller.audioDevice,
          onChanged: _controller.setAudioDevice,
        ),
        _devicePicker(
          label: 'Microphone',
          devices: _inputs,
          value: _controller.microphoneDevice,
          onChanged: _controller.setMicrophoneDevice,
        ),
        const PanelSection('Routing'),
        PanelSwitch(
          title: 'Play here',
          subtitle: 'Off hands the PCM to the app and plays nothing',
          value: _controller.audioOutputEnabled,
          onChanged: _controller.setAudioOutputEnabled,
        ),
        ListenableBuilder(
          listenable: widget.pcmTap,
          builder: (context, _) => PanelSwitch(
            title: 'Tap raw PCM',
            subtitle: widget.pcmTap.active
                ? '${(widget.pcmTap.bytes / 1024).round()} KB, peak '
                      '${(widget.pcmTap.peak * 100).round()}%'
                : 'Nothing is copied out while nobody listens',
            value: widget.pcmTap.active,
            onChanged: (value) => widget.pcmTap.active = value,
          ),
        ),
        const SizedBox(height: 6),
        PanelNote(
          'Latency ${latency.inMilliseconds} ms, underruns $underruns, '
          'dropped $dropped',
        ),
        PanelNote(
          'Mic ${_controller.microphoneBackend}, '
          '${(_controller.microphoneBytes / 1024).round()} KB captured',
        ),
      ],
    );
  }

  Widget _stream(String label, AndroidAutoAudioStream which) {
    final muted = _controller.muted(which);
    final volume = _controller.volume(which);
    return Row(
      children: [
        SizedBox(width: 58, child: Text(label, style: const TextStyle(fontSize: 13))),
        IconButton(
          tooltip: muted ? 'Unmute' : 'Mute',
          onPressed: () => _controller.setMuted(which, !muted),
          icon: Icon(muted ? Icons.volume_off : Icons.volume_up, size: 18),
        ),
        Expanded(
          child: Slider(
            value: volume,
            onChanged: (value) => _controller.setVolume(which, value),
          ),
        ),
        SizedBox(
          width: 38,
          child: Text(
            '${(volume * 100).round()}%',
            textAlign: TextAlign.right,
            style: const TextStyle(fontSize: 12),
          ),
        ),
      ],
    );
  }

  /// An empty name means the head unit's own hardware: the system default, except
  /// that a Bluetooth device is never picked for it. See
  /// [AndroidAutoController.setAudioDevice].
  Widget _devicePicker({
    required String label,
    required List<AndroidAutoAudioDevice> devices,
    required String value,
    required ValueChanged<String?> onChanged,
  }) {
    return Row(
      children: [
        SizedBox(width: 82, child: Text(label, style: const TextStyle(fontSize: 12))),
        Expanded(
          child: DropdownButton<String>(
            isExpanded: true,
            // A device picked earlier may have been unplugged since.
            value: value.isEmpty || devices.any((d) => d.name == value) ? value : '',
            style: const TextStyle(fontSize: 12, color: Colors.white),
            items: [
              const DropdownMenuItem(value: '', child: Text('Head unit default')),
              for (final device in devices)
                DropdownMenuItem(
                  value: device.name,
                  child: Text(device.description, overflow: TextOverflow.ellipsis),
                ),
            ],
            onChanged: onChanged,
          ),
        ),
      ],
    );
  }
}
