// SPDX-License-Identifier: GPL-3.0-or-later
import 'dart:io';

import 'package:android_auto/android_auto.dart';

/// What this head unit tells the phone it is.
///
/// Read once, at service discovery, so everything in here is a decision made before a
/// phone is ever connected. Not const: the Wi-Fi passphrase is read from the
/// environment at startup.
AndroidAutoConfig headUnitConfig() => AndroidAutoConfig(
  // No width or height: the frame size follows the view, which is the default, and the
  // phone lays itself out for the view's shape. Name one to pin it, 1920x1080 say.
  fps: 30,
  // Location is in here as well as the two defaults, because a test bench that cannot
  // exercise the GPS sensor cannot tell whether it works. It comes with the obligation
  // attached: the phone stops using its own receiver the moment it sees this, so
  // `LocationFeed` feeds a fix every second from the moment the app starts. Take
  // location out of this set in a real head unit that has no receiver.
  sensors: const {
    AndroidAutoSensor.nightMode,
    AndroidAutoSensor.drivingStatus,
    AndroidAutoSensor.location,
  },
  // All five, which is more than the default. A test bench that cannot exercise the
  // media browser or the notifications cannot tell whether they work, and unlike a
  // sensor none of these is a promise: the phone pushes what it has.
  metadata: const {
    AndroidAutoMetadata.navigation,
    AndroidAutoMetadata.media,
    AndroidAutoMetadata.phone,
    AndroidAutoMetadata.notification,
    AndroidAutoMetadata.browse,
  },
  // Both, which is what a head unit with a cable and a radio is. This is the whole of
  // turning wireless on: a set and a passphrase. Nothing else in this app needs to know
  // that wireless exists, which is the point; the Wireless panel only exists to watch it.
  //
  // A real head unit picks its own set. Leaving wireless out means the Bluetooth
  // service is never published, and a phone paired while it is out never learns this
  // machine can project, so it never asks.
  transports: const {AndroidAutoTransport.usb, AndroidAutoTransport.wireless},
  // The one thing that cannot be read off the machine. Everything else, the SSID, the
  // access point's MAC and the address to dial, comes from the interface.
  //
  // Read from the environment rather than written here, and the fallback is not a
  // passphrase but a reminder. A real head unit gets this from whatever brought its
  // network up; a plausible looking literal in an example is a literal that ends up in
  // somebody's product.
  wireless: AndroidAutoWirelessConfig(
    passphrase:
        Platform.environment['AA_WIRELESS_PASSPHRASE'] ??
        'set-AA_WIRELESS_PASSPHRASE',
  ),
);
