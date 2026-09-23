// SPDX-License-Identifier: GPL-3.0-or-later
/// Embed an Android Auto head unit inside a Flutter app.
///
/// The projected phone screen is rendered into a Flutter [Texture], so ordinary
/// widgets can be composited on top of it. How the pieces fit together is written up in
/// [docs/architecture.md](https://github.com/Mauznemo/FlutterAndroidAuto/blob/main/docs/architecture.md).
library;

export 'package:android_auto_platform_interface/android_auto_platform_interface.dart'
    show
        AndroidAutoAccessPointType,
        AndroidAutoAudioBuffer,
        AndroidAutoAudioDevice,
        AndroidAutoAudioStream,
        AndroidAutoBluetoothDevice,
        AndroidAutoBrowseList,
        AndroidAutoBrowseListType,
        AndroidAutoBrowseNode,
        AndroidAutoBrowseNodeKind,
        AndroidAutoBrowseSong,
        AndroidAutoBrowseSource,
        AndroidAutoCall,
        AndroidAutoCallState,
        AndroidAutoConfig,
        AndroidAutoConnectionState,
        AndroidAutoDestination,
        AndroidAutoDistance,
        AndroidAutoDistanceUnit,
        AndroidAutoDrivingRestriction,
        AndroidAutoEvent,
        AndroidAutoKey,
        AndroidAutoLane,
        AndroidAutoLaneDirection,
        AndroidAutoLaneShape,
        AndroidAutoLocation,
        AndroidAutoManeuver,
        AndroidAutoMediaInfo,
        AndroidAutoMetadata,
        AndroidAutoNavigation,
        AndroidAutoNavigationStatus,
        AndroidAutoNotification,
        AndroidAutoPhoneStatus,
        AndroidAutoPlaybackState,
        AndroidAutoSensor,
        AndroidAutoTouchAction,
        AndroidAutoTouchPoint,
        AndroidAutoTransport,
        AndroidAutoVideoInfo,
        AndroidAutoWifiSecurity,
        AndroidAutoWirelessConfig,
        AndroidAutoWirelessStatus;

export 'src/android_auto_controller.dart';
export 'src/android_auto_view.dart';
export 'src/simulator/android_auto_simulator.dart';
