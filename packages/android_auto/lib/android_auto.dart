/// Embed an Android Auto head unit inside a Flutter app.
///
/// The projected phone screen is rendered into a Flutter [Texture], so ordinary
/// widgets can be composited on top of it. See `docs/architecture.md`.
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
