// SPDX-License-Identifier: GPL-3.0-or-later
import 'dart:async';

import 'package:android_auto/android_auto.dart';
import 'package:flutter/material.dart';

import 'panel_frame.dart';

/// What arrived on each metadata channel, and the media browser.
///
/// The counters at the top are the first thing to read when a card on the overlay is
/// empty: a channel the phone never opened and a channel that opened and said nothing
/// are different problems, and only these lines tell them apart.
class MetadataPanel extends StatefulWidget {
  final AndroidAutoController controller;

  /// Kept by the page, because notifications are events and one that arrives while
  /// this panel is closed would otherwise be lost.
  final AndroidAutoNotification? lastNotification;

  final VoidCallback onClose;

  const MetadataPanel({
    super.key,
    required this.controller,
    required this.lastNotification,
    required this.onClose,
  });

  @override
  State<MetadataPanel> createState() => _MetadataPanelState();
}

class _MetadataPanelState extends State<MetadataPanel> {
  StreamSubscription<AndroidAutoBrowseNode>? _results;
  AndroidAutoBrowseNode? _node;

  /// Set when a browse request was refused, which is what a phone that never opened
  /// the browser channel looks like from here. Worth saying out loud: the alternative
  /// is a list that stays empty with no reason given.
  String? _refusal;

  /// Where the browser has been, so there is a way back out of a library. The protocol
  /// has no parent pointer: a node knows its own path and nothing above it.
  final List<String> _trail = [];

  AndroidAutoController get _controller => widget.controller;

  @override
  void initState() {
    super.initState();
    _results = _controller.browseResults.listen(
      (node) => setState(() => _node = node),
    );
  }

  @override
  void dispose() {
    _results?.cancel();
    super.dispose();
  }

  /// Asks for a node, and says so when the answer is no.
  ///
  /// A refusal means the phone never opened the browser channel, which is what the
  /// one phone tested does. Showing it beats an empty list that never fills in.
  void _request(String path) {
    final sent = _controller.browse(path: path);
    setState(() {
      _refusal = sent
          ? null
          : 'The phone has not opened the media browser channel, so there is '
                'nothing to ask.';
    });
  }

  void _back() {
    if (_trail.isNotEmpty) {
      _request(_trail.removeLast());
    }
  }

  @override
  Widget build(BuildContext context) {
    final opened = _controller.metadataChannels;
    return PanelFrame(
      title: 'Metadata',
      icon: Icons.info_outline,
      onClose: widget.onClose,
      children: [
        const PanelSection('Channels'),
        for (final kind in AndroidAutoMetadata.values)
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 1),
            child: Row(
              spacing: 8,
              children: [
                Icon(
                  Icons.circle,
                  size: 8,
                  color: opened.contains(kind)
                      ? Colors.greenAccent
                      : Colors.white24,
                ),
                SizedBox(
                  width: 90,
                  child: Text(kind.name, style: const TextStyle(fontSize: 12)),
                ),
                PanelNote(
                  opened.contains(kind)
                      ? '${_controller.metadataUpdates(kind)} updates'
                      : 'not opened',
                ),
              ],
            ),
          ),
        const PanelSection('Last notification'),
        PanelNote(widget.lastNotification?.text ?? 'none'),
        Row(
          children: [
            const Expanded(child: PanelSection('Media library')),
            if (_trail.isNotEmpty)
              TextButton(onPressed: _back, child: const Text('Back')),
            TextButton(
              onPressed: () {
                _trail.clear();
                _request('');
              },
              child: const Text('Root'),
            ),
          ],
        ),
        _browseList(),
      ],
    );
  }

  Widget _browseList() {
    final refusal = _refusal;
    if (refusal != null) {
      return PanelNote(refusal, color: Colors.orangeAccent);
    }
    final node = _node;
    if (node == null) {
      return const PanelNote(
        'Press Root to ask the phone for its media library',
        color: Colors.white38,
      );
    }
    final entries = <_BrowseEntry>[
      for (final source in node.sources)
        _BrowseEntry(source.path, source.name ?? source.path, Icons.apps, true),
      for (final list in node.lists)
        _BrowseEntry(
          list.path,
          list.name ?? list.path,
          Icons.queue_music,
          true,
        ),
      for (final song in node.songs)
        _BrowseEntry(song.path, song.name, Icons.music_note, false),
      if (node.song != null)
        _BrowseEntry(node.song!.path, node.song!.name, Icons.music_note, false),
    ];
    if (entries.isEmpty) {
      return PanelNote(
        'Nothing under ${node.path.isEmpty ? "the root" : node.path}',
        color: Colors.white38,
      );
    }
    return SizedBox(
      height: 180,
      child: ListView.builder(
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
                _trail.add(node.path);
                _request(entry.path);
              } else {
                // A song is played rather than opened. The protocol calls it a browser
                // input, which is the head unit reporting that someone pressed enter on
                // this path.
                _controller.browseSelect(entry.path);
              }
            },
          );
        },
      ),
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
