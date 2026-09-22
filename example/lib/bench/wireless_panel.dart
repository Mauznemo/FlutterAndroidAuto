// SPDX-License-Identifier: GPL-3.0-or-later
import 'dart:async';

import 'package:android_auto/android_auto.dart';
import 'package:flutter/material.dart';

import 'panel_frame.dart';

/// Watching wireless, and switching it on and off.
///
/// None of this is needed to use wireless: `config.dart` turns it on, and a phone that
/// was paired with this machine connects on its own. The panel exists for when one does
/// not, and it answers the two questions worth asking separately. Bluetooth not
/// published is this machine's problem; a phone that linked over Bluetooth and then
/// never dialled in is a Wi-Fi problem at the phone's end.
class WirelessPanel extends StatefulWidget {
  final AndroidAutoController controller;

  /// The network to offer. Owned by the page, so what was typed survives closing the
  /// panel.
  final AndroidAutoWirelessConfig config;

  final ValueChanged<AndroidAutoWirelessConfig> onConfigChanged;
  final VoidCallback onClose;

  const WirelessPanel({
    super.key,
    required this.controller,
    required this.config,
    required this.onConfigChanged,
    required this.onClose,
  });

  @override
  State<WirelessPanel> createState() => _WirelessPanelState();
}

class _WirelessPanelState extends State<WirelessPanel> {
  late final TextEditingController _ssid = TextEditingController(
    text: widget.config.ssid,
  );
  late final TextEditingController _passphrase = TextEditingController(
    text: widget.config.passphrase,
  );
  late String _phone = widget.config.phoneAddress;
  List<AndroidAutoBluetoothDevice> _pairedPhones = const [];
  bool _showPassphrase = false;

  /// Polls the wireless summary while the panel is open. The interesting fields change
  /// without an event: an address renewed, a phone opening the Bluetooth channel. One
  /// second is far more often than any of them moves.
  Timer? _poll;

  AndroidAutoController get _controller => widget.controller;

  @override
  void initState() {
    super.initState();
    _poll = Timer.periodic(const Duration(seconds: 1), (_) => setState(() {}));
    _loadPhones();
  }

  Future<void> _loadPhones() async {
    final phones = await _controller.pairedPhones();
    if (!mounted) {
      return;
    }
    setState(() {
      _pairedPhones = phones;
      if (_phone.isEmpty) {
        final phone = phones.where((one) => one.isPhone);
        _phone = phone.isEmpty ? '' : phone.first.address;
      }
    });
    _publish();
  }

  @override
  void dispose() {
    _poll?.cancel();
    _ssid.dispose();
    _passphrase.dispose();
    super.dispose();
  }

  /// Everything the fields say, as the config the page keeps. Applied to the controller
  /// only when wireless is switched on, so editing never disturbs a phone mid session.
  AndroidAutoWirelessConfig _publish() {
    final base = widget.config;
    final config = AndroidAutoWirelessConfig(
      ssid: _ssid.text,
      passphrase: _passphrase.text,
      phoneAddress: _phone,
      bssid: base.bssid,
      interfaceName: base.interfaceName,
      ipAddress: base.ipAddress,
      port: base.port,
      security: base.security,
      accessPoint: base.accessPoint,
    );
    widget.onConfigChanged(config);
    return config;
  }

  Future<void> _toggle() async {
    if (_controller.wirelessActive) {
      await _controller.stopWireless();
      return;
    }
    // The session has to exist first: wireless hangs off it, and the io threads it
    // runs the acceptor on are started by start().
    if (_controller.state == AndroidAutoConnectionState.idle) {
      await _controller.start();
    }
    _controller.setWirelessConfig(_publish());
    await _controller.startWireless();
  }

  @override
  Widget build(BuildContext context) {
    final status = _controller.wirelessStatus;
    return PanelFrame(
      title: 'Wireless',
      icon: _controller.wirelessActive ? Icons.wifi : Icons.wifi_off,
      onClose: widget.onClose,
      children: [
        Row(
          children: [
            Expanded(
              child: Text(
                status == null
                    ? 'Not offering wireless'
                    : '${status.ssid} on ${status.interfaceName}\n'
                          '${status.ipAddress}:${status.port}, '
                          '${status.hosting ? "hosting" : "joined"}',
                style: const TextStyle(fontSize: 12),
              ),
            ),
            Switch(
              value: _controller.wirelessActive,
              onChanged: (_) => _toggle(),
            ),
          ],
        ),
        if (status != null)
          PanelNote(
            'Bluetooth ${status.bluetoothReady ? "published" : "down"}, '
            'phone ${status.phoneLinked ? "linked" : "not linked"}',
            color: status.phoneLinked ? Colors.greenAccent : Colors.white70,
          ),
        const PanelSection('Network to offer'),
        TextField(
          controller: _ssid,
          onChanged: (_) => _publish(),
          decoration: const InputDecoration(
            labelText: 'SSID',
            helperText: 'Empty reads it off the wireless interface',
            isDense: true,
          ),
          style: const TextStyle(fontSize: 13),
        ),
        const SizedBox(height: 6),
        TextField(
          controller: _passphrase,
          onChanged: (_) => _publish(),
          obscureText: !_showPassphrase,
          decoration: InputDecoration(
            labelText: 'Wi-Fi passphrase',
            helperText: 'The one thing nothing here can read off the machine',
            isDense: true,
            suffixIcon: IconButton(
              tooltip: _showPassphrase ? 'Hide' : 'Show',
              onPressed: () => setState(() => _showPassphrase = !_showPassphrase),
              icon: Icon(
                _showPassphrase ? Icons.visibility_off : Icons.visibility,
                size: 18,
              ),
            ),
          ),
          style: const TextStyle(fontSize: 13),
        ),
        const PanelSection('Paired phones'),
        if (_pairedPhones.isEmpty)
          const PanelNote('None. Pair the phone the way you would for music first.'),
        RadioGroup<String>(
          groupValue: _phone,
          onChanged: (value) {
            setState(() => _phone = value ?? '');
            _publish();
          },
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              for (final phone in _pairedPhones)
                RadioListTile<String>(
                  dense: true,
                  contentPadding: EdgeInsets.zero,
                  value: phone.address,
                  title: Text(
                    '${phone.name}${phone.connected ? " (connected)" : ""}',
                    style: const TextStyle(fontSize: 12),
                  ),
                  subtitle: PanelNote(phone.address),
                ),
            ],
          ),
        ),
      ],
    );
  }
}
