// SPDX-License-Identifier: GPL-3.0-or-later
import 'dart:io';

import 'package:android_auto/android_auto.dart';
import 'package:flutter/material.dart';

import 'head_unit_page.dart';

void main() {
  // macOS and Windows get the simulator without asking. This puts it on Linux too, for
  // working on the app with no phone to hand. An example app knob, not a plugin one.
  if (Platform.environment['AA_SIMULATE'] == '1') {
    AndroidAutoSimulator.registerWith();
  }
  runApp(const HeadUnitApp());
}

/// The reference integration for the android_auto plugin.
///
/// Read it in this order:
///
/// 1. `config.dart`, what the head unit tells the phone it is.
/// 2. `head_unit_page.dart`, one controller, one view, and everything else stacked on
///    top of the projection as ordinary Flutter widgets.
/// 3. `head_unit/`, the pieces a real head unit would ship: the status bar, the
///    hardware keys, and the turn card and now playing bar drawn from metadata.
/// 4. `bench/`, the test bench panels. A product would not show these to a driver,
///    but every call in them is the one a product would make.
class HeadUnitApp extends StatelessWidget {
  const HeadUnitApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Android Auto head unit',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        useMaterial3: true,
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF4F8CFF),
          brightness: Brightness.dark,
        ),
      ),
      home: const HeadUnitPage(),
    );
  }
}
