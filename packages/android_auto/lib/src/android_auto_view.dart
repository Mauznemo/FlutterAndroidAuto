// SPDX-License-Identifier: GPL-3.0-or-later
import 'package:android_auto_platform_interface/android_auto_platform_interface.dart';
import 'package:flutter/widgets.dart';

import 'android_auto_controller.dart';

/// Renders the projected phone screen and forwards touches to it.
///
/// This is the whole point of the package: the projection is a [Texture] in the widget
/// tree, not a separate window, so a host app can put anything it likes on top of it
/// with ordinary Flutter widgets.
///
/// ```dart
/// Stack(
///   children: [
///     AndroidAutoView(controller: controller),
///     MyStatusBar(),
///   ],
/// )
/// ```
///
/// Anything drawn over the view wins the hit test, so an overlay button takes its own
/// taps and the phone never sees them. Touches that land on the projection itself are
/// mapped into projected video pixels and sent on, which is what makes the phone's UI
/// usable rather than just visible.
///
/// The view tells the controller its size on every layout, and with
/// [AndroidAutoConfig.matchViewAspectRatio] on (the default) the phone lays its
/// interface out in the view's shape. So the projection can sit under a status bar,
/// beside a panel or in any other space the host app has left, and fill it. Put
/// widgets *beside* the view to take space from the phone, and *over* it to cover part
/// of what the phone draws.
class AndroidAutoView extends StatefulWidget {
  /// The session to render.
  final AndroidAutoController controller;

  /// Shown while there is no video stream yet.
  final Widget? placeholder;

  /// How the projected surface is fitted into the available space.
  ///
  /// Touch coordinates are mapped through the same fit, so changing this stays
  /// consistent between what is drawn and where taps land.
  final BoxFit fit;

  /// Whether touches on the projection are forwarded to the phone.
  ///
  /// Turn it off for a head unit that drives input some other way, for instance from a
  /// real touchscreen wired straight into [AndroidAutoController.sendTouch].
  final bool enableTouch;

  /// Creates a view for [controller].
  const AndroidAutoView({
    super.key,
    required this.controller,
    this.placeholder,
    this.fit = BoxFit.contain,
    this.enableTouch = true,
  });

  @override
  State<AndroidAutoView> createState() => _AndroidAutoViewState();
}

class _AndroidAutoViewState extends State<AndroidAutoView> {
  /// Every finger currently down, in the order it arrived, which is the order the
  /// phone is given them in. Keyed by Flutter's pointer id.
  final Map<int, AndroidAutoTouchPoint> _pointers = {};

  /// Flutter pointer id to the small index the protocol wants.
  ///
  /// Flutter's ids are an ever growing counter across the whole app, and the phone
  /// expects Android's pointer numbering: 0, 1, 2, reused as fingers come and go.
  final Map<int, int> _slots = {};

  /// The projection's rectangle inside this widget, in local coordinates, as of the
  /// last layout. Touch mapping needs it and paint already computed it.
  Rect _projection = Rect.zero;

  /// The video size that [_projection] was computed against.
  Size _source = Size.zero;

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        // Before there is a texture as well as after: the phone is told the shape
        // when it connects, which is before its first frame. Physical pixels, because
        // the frame size is chosen to cover them.
        final size = constraints.biggest;
        if (size.isFinite && !size.isEmpty) {
          widget.controller.setViewSize(size * View.of(context).devicePixelRatio);
        }
        return _buildProjection(constraints);
      },
    );
  }

  Widget _buildProjection(BoxConstraints constraints) {
    return ListenableBuilder(
      listenable: widget.controller,
      builder: (context, _) {
        final textureId = widget.controller.textureId;
        if (textureId == null) {
          // The projection went away mid gesture, so the fingers it was tracking are
          // not coming back up. Forget them, or the next tap reports itself as a
          // second finger on a screen nothing is touching.
          _pointers.clear();
          _slots.clear();
          return widget.placeholder ?? const SizedBox.expand();
        }
        // The phone's actual size when it is known, the requested one until then. They
        // usually agree, and when they do not it is the phone that is right.
        final info = widget.controller.videoInfo;
        final source = Size(
          (info?.width ?? widget.controller.config.width ?? 1280).toDouble(),
          (info?.height ?? widget.controller.config.height ?? 720).toDouble(),
        );
        _source = source;
        _projection = _fitProjection(source, constraints.biggest, widget.fit);
        // In physical pixels, because that is what decides whether the texture is
        // being shrunk: a 1280 pixel wide video in a 640 logical pixel view on a
        // screen at 2x is drawn one to one.
        final pixelRatio = View.of(context).devicePixelRatio;
        widget.controller.setDisplaySize(_projection.size * pixelRatio);
        final view = FittedBox(
          fit: widget.fit,
          child: SizedBox(
            width: source.width,
            height: source.height,
            child: Texture(textureId: textureId),
          ),
        );
        if (!widget.enableTouch) {
          return view;
        }
        return Listener(
          // The projection swallows what lands on it rather than letting it fall
          // through to whatever is behind, which is what a real head unit screen
          // does. Widgets drawn *over* the view are unaffected: they are later in the
          // stack and win the hit test before this ever sees the pointer.
          behavior: HitTestBehavior.opaque,
          onPointerDown: _onPointerDown,
          onPointerMove: _onPointerMove,
          onPointerUp: _onPointerUp,
          onPointerCancel: _onPointerCancel,
          child: view,
        );
      },
    );
  }

  /// Where the projection is drawn inside a box of [box], for [fit].
  ///
  /// The same arithmetic [FittedBox] does, repeated here because it keeps the result
  /// rather than throwing it away: mapping a touch back onto the video needs it.
  static Rect _fitProjection(Size source, Size box, BoxFit fit) {
    if (source.isEmpty) {
      return Rect.zero;
    }
    // An unbounded constraint means FittedBox sized itself to the child instead of
    // scaling it, so the projection is exactly the source. Substituting the source
    // dimension here reproduces that rather than giving up on touch.
    final resolved = Size(
      box.width.isFinite ? box.width : source.width,
      box.height.isFinite ? box.height : source.height,
    );
    if (resolved.isEmpty) {
      return Rect.zero;
    }
    final sizes = applyBoxFit(fit, source, resolved);
    // Alignment.center, because that is FittedBox's default and this has to agree with
    // what was actually painted.
    return Alignment.center.inscribe(sizes.destination, Offset.zero & resolved);
  }

  /// Maps a position in this widget onto projected video pixels.
  ///
  /// Returns null when the projection has no size yet. Positions outside the
  /// projection are clamped rather than rejected, so a drag that wanders onto the
  /// letterbox bars keeps tracking the edge instead of stopping dead.
  AndroidAutoTouchPoint? _toProjected(int slot, Offset local) {
    if (_projection.isEmpty || _source.isEmpty) {
      return null;
    }
    final x = (local.dx - _projection.left) * _source.width / _projection.width;
    final y = (local.dy - _projection.top) * _source.height / _projection.height;
    return AndroidAutoTouchPoint(
      id: slot,
      x: x.clamp(0.0, _source.width - 1).round(),
      y: y.clamp(0.0, _source.height - 1).round(),
    );
  }

  /// The lowest pointer index not already in use, which is how Android numbers them.
  int _claimSlot(int pointer) {
    final taken = _slots.values.toSet();
    var slot = 0;
    while (taken.contains(slot)) {
      slot++;
    }
    _slots[pointer] = slot;
    return slot;
  }

  void _onPointerDown(PointerDownEvent event) {
    // A touch starting on the letterbox bars is not a touch on the phone's screen, so
    // it is dropped outright rather than clamped onto the nearest edge. Clamping is
    // right for a drag that wanders off, wrong for a tap that was never on.
    if (!_projection.contains(event.localPosition)) {
      return;
    }
    final slot = _claimSlot(event.pointer);
    final point = _toProjected(slot, event.localPosition);
    if (point == null) {
      _slots.remove(event.pointer);
      return;
    }
    final first = _pointers.isEmpty;
    _pointers[event.pointer] = point;
    _send(
      first ? AndroidAutoTouchAction.down : AndroidAutoTouchAction.pointerDown,
      event.pointer,
    );
  }

  void _onPointerMove(PointerMoveEvent event) {
    final slot = _slots[event.pointer];
    if (slot == null) {
      return;
    }
    final point = _toProjected(slot, event.localPosition);
    if (point == null) {
      return;
    }
    _pointers[event.pointer] = point;
    _send(AndroidAutoTouchAction.move, event.pointer);
  }

  void _onPointerUp(PointerUpEvent event) => _release(event.pointer);

  // Cancel goes out as an ordinary up. The protocol has no cancel action, and a phone
  // left holding a finger that never lifts stays stuck in whatever gesture it thought
  // was in progress.
  void _onPointerCancel(PointerCancelEvent event) => _release(event.pointer);

  void _release(int pointer) {
    if (!_pointers.containsKey(pointer)) {
      return;
    }
    final last = _pointers.length == 1;
    // The finger going up is still in the report: Android includes it, and
    // action_index is what says which one it was.
    _send(last ? AndroidAutoTouchAction.up : AndroidAutoTouchAction.pointerUp, pointer);
    _pointers.remove(pointer);
    _slots.remove(pointer);
  }

  /// Sends the current pointer set, with [pointer] named as the one this is about.
  void _send(AndroidAutoTouchAction action, int pointer) {
    final order = _pointers.keys.toList();
    final points = [for (final key in order) _pointers[key]!];
    if (points.isEmpty) {
      return;
    }
    widget.controller.sendTouch(
      action,
      points,
      actionIndex: order.indexOf(pointer).clamp(0, points.length - 1),
    );
  }
}
