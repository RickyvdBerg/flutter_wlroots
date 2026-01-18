import 'dart:async';

import 'package:compositor_dart/compositor_dart.dart';
export 'package:compositor_dart/compositor_dart.dart' show Surface, Subsurface;
import 'package:compositor_dart/constants.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';

typedef _OnWidgetSizeChange = void Function(Size? size);

class _MeasureSizeRenderObject extends RenderProxyBox {
  Size? oldSize;
  final _OnWidgetSizeChange onChange;

  _MeasureSizeRenderObject(this.onChange);

  @override
  void performLayout() {
    super.performLayout();

    Size? newSize = child?.size;
    if (oldSize == newSize) return;

    oldSize = newSize;
    WidgetsBinding.instance?.addPostFrameCallback((_) {
      onChange(newSize);
    });
  }
}

class _MeasureSize extends SingleChildRenderObjectWidget {
  final _OnWidgetSizeChange onChange;

  const _MeasureSize({
    Key? key,
    required this.onChange,
    required Widget child,
  }) : super(key: key, child: child);

  @override
  RenderObject createRenderObject(BuildContext context) {
    return _MeasureSizeRenderObject(onChange);
  }
}

/// Shared mixin for encoding pointer events to the platform channel format.
/// Used by both surface and popup event dispatchers.
mixin _PointerEventEncoder {
  /// Encode a PointerEvent into the list format expected by the C side.
  /// Returns the encoded data list ready for platform channel invocation.
  List encodePointerEvent({
    required int handle,
    required PointerEvent event,
    required Size widgetSize,
    Offset Function(Offset)? coordTransform,
  }) {
    final int deviceKind;
    switch (event.kind) {
      case PointerDeviceKind.mouse:
        deviceKind = pointerKindMouse;
        break;
      case PointerDeviceKind.touch:
        deviceKind = pointerKindTouch;
        break;
      default:
        deviceKind = pointerKindUnknown;
        break;
    }

    final int eventType;
    Offset scrollAmount = Offset.zero;
    if (event is PointerDownEvent) {
      eventType = pointerDownEvent;
    } else if (event is PointerUpEvent) {
      eventType = pointerUpEvent;
    } else if (event is PointerHoverEvent) {
      eventType = pointerHoverEvent;
    } else if (event is PointerMoveEvent) {
      eventType = pointerMoveEvent;
    } else if (event is PointerEnterEvent) {
      eventType = pointerEnterEvent;
    } else if (event is PointerExitEvent) {
      eventType = pointerExitEvent;
    } else if (event is PointerScrollEvent) {
      eventType = pointerScrollEvent;
      scrollAmount = event.scrollDelta;
    } else {
      eventType = pointerUnknownEvent;
    }

    // Apply coordinate transformation if provided, otherwise pass through
    final localPos = coordTransform != null
        ? coordTransform(event.localPosition)
        : event.localPosition;

    return [
      handle,
      event.buttons,
      event.delta.dx,
      event.delta.dy,
      event.device,
      event.distance,
      event.down,
      event.embedderId,
      deviceKind,
      event.localDelta.dx,
      event.localDelta.dy,
      localPos.dx,
      localPos.dy,
      event.obscured,
      event.orientation,
      event.platformData,
      event.pointer,
      event.position.dx,
      event.position.dy,
      event.pressure,
      event.radiusMajor,
      event.radiusMinor,
      event.size,
      event.synthesized,
      event.tilt,
      event.timeStamp.inMicroseconds,
      eventType,
      widgetSize.width,
      widgetSize.height,
      scrollAmount.dx,
      scrollAmount.dy,
    ];
  }
}

class SurfaceView extends StatefulWidget {
  final Surface surface;

  const SurfaceView({Key? key, required this.surface}) : super(key: key);

  @override
  State<StatefulWidget> createState() {
    return _SurfaceViewState();
  }
}

class _SurfaceViewState extends State<SurfaceView> {
  late _CompositorPlatformViewController controller;
  StreamSubscription<Surface>? _updateSubscription;
  List<Subsurface> _subsurfaces = [];

  @override
  void initState() {
    super.initState();
    controller = _CompositorPlatformViewController(surface: widget.surface);
    _subsurfaces = List.from(widget.surface.subsurfaces);
    _updateSubscription = Compositor.compositor.surfaceUpdated.stream.listen((surface) {
      if (surface.handle == widget.surface.handle) {
        setState(() {
          _subsurfaces = List.from(widget.surface.subsurfaces);
        });
      }
    });
  }

  @override
  void didUpdateWidget(SurfaceView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.surface != widget.surface) {
      controller.dispose();
      controller = _CompositorPlatformViewController(surface: widget.surface);
      _subsurfaces = List.from(widget.surface.subsurfaces);
    }
  }

  @override
  void dispose() {
    _updateSubscription?.cancel();
    super.dispose();
  }

  void _onPointerDown(PointerDownEvent event) {
    // NOTE: Focus is now handled by WindowManager via WindowFrame.
    // This avoids race conditions between C-side focus and Dart-side state.
    // The WindowFrame wraps all content with a Listener that calls
    // manager.activate() on pointer down.
    controller.dispatchPointerEvent(event);
  }

  void _onPointerSignal(PointerSignalEvent event) {
    controller.dispatchPointerEvent(event);
  }

  @override
  Widget build(BuildContext context) {
    return Listener(
      onPointerDown: _onPointerDown,
      onPointerMove: controller.dispatchPointerEvent,
      onPointerUp: controller.dispatchPointerEvent,
      onPointerHover: controller.dispatchPointerEvent,
      onPointerCancel: controller.dispatchPointerEvent,
      onPointerSignal: _onPointerSignal,
      behavior: HitTestBehavior.opaque,
      child: Focus(
        onKeyEvent: (node, event) {
          final KeyStatus status;

          if (event is KeyDownEvent) {
            status = KeyStatus.pressed;
          } else {
            status = KeyStatus.released;
          }

          int? keycode = physicalToXkbMap[event.physicalKey.usbHidUsage];

          if (keycode != null) {
            controller.surface.compositor.platform.surfaceSendKey(
              widget.surface,
              keycode,
              status,
              event.timeStamp,
            );

            return KeyEventResult.handled;
          }

          return KeyEventResult.ignored;
        },
        child: _MeasureSize(
          onChange: (size) {
            if (size != null) {
              controller.setSize(size);
            }
          },
          child: _buildSurfaceTree(),
        ),
      ),
    );
  }

  /// Builds the surface tree with toplevel + all subsurfaces as a Stack.
  /// The Texture widget expands to fill its container and Flutter stretches
  /// the GL texture to fill that rect. This provides smooth resize - content
  /// stretches until the client commits a new buffer at the correct size.
  Widget _buildSurfaceTree() {
    final surface = widget.surface;

    // SizedBox.expand makes the texture fill its parent container.
    // Flutter's texture rendering stretches the GL texture to fill this rect.
    // filterQuality.medium provides smooth bilinear interpolation during scaling.
    Widget mainTexture = SizedBox.expand(
      child: Texture(
        textureId: surface.textureId,
        filterQuality: FilterQuality.medium,
      ),
    );

    if (_subsurfaces.isEmpty) {
      return mainTexture;
    }

    // Render toplevel + subsurfaces as a Stack
    return Stack(
      clipBehavior: Clip.none,
      children: [
        mainTexture,
        ..._subsurfaces.map((sub) => Positioned(
          left: sub.x.toDouble(),
          top: sub.y.toDouble(),
          width: sub.width > 0 ? sub.width.toDouble() : null,
          height: sub.height > 0 ? sub.height.toDouble() : null,
          child: Texture(textureId: sub.textureId),
        )),
      ],
    );
  }
}

/// Widget for rendering popup surfaces (menus, dropdowns, tooltips).
/// Handles input through Flutter and forwards to wlroots via platform channel.
class PopupView extends StatefulWidget {
  final Popup popup;

  const PopupView({Key? key, required this.popup}) : super(key: key);

  @override
  State<PopupView> createState() => _PopupViewState();
}

class _PopupViewState extends State<PopupView> {
  late _PopupPlatformViewController _controller;

  @override
  void initState() {
    super.initState();
    _controller = _PopupPlatformViewController(popup: widget.popup);
  }

  @override
  void didUpdateWidget(PopupView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.popup != widget.popup) {
      _controller = _PopupPlatformViewController(popup: widget.popup);
    }
  }

  void _onPointerSignal(PointerSignalEvent event) {
    _controller.dispatchPointerEvent(event);
  }

  @override
  Widget build(BuildContext context) {
    // Use Listener to capture all pointer events and forward to wlroots
    // This maintains Flutter-first architecture while enabling popup input
    return Listener(
      onPointerDown: _controller.dispatchPointerEvent,
      onPointerMove: _controller.dispatchPointerEvent,
      onPointerUp: _controller.dispatchPointerEvent,
      onPointerHover: _controller.dispatchPointerEvent,
      onPointerCancel: _controller.dispatchPointerEvent,
      onPointerSignal: _onPointerSignal,
      behavior: HitTestBehavior.opaque,
      child: Texture(
        textureId: widget.popup.textureId,
        // Use medium filter quality for better popup rendering when scaled
        filterQuality: FilterQuality.medium,
      ),
    );
  }
}

class _CompositorPlatformViewController extends PlatformViewController with _PointerEventEncoder {
  Surface surface;
  Size size = const Size(100, 100);

  _CompositorPlatformViewController({required this.surface});

  void setSize(Size size) {
    // Only track size for input coordinate calculations.
    // NOTE: Size updates to compositor are now handled by WindowManager.
    // WindowManager._syncSizeToCompositor() is the single source of truth for resize.
    this.size = size;
  }

  @override
  Future<void> clearFocus() => Compositor.compositor.platform.clearFocus(surface);

  @override
  Future<void> dispatchPointerEvent(PointerEvent event) async {
    final data = encodePointerEvent(
      handle: surface.handle,
      event: event,
      widgetSize: size,
    );

    await Compositor.compositor.platform.channel.invokeMethod(
      "surface_pointer_event",
      data,
    );
  }

  @override
  Future<void> dispose() async {
    // TODO: implement dispose
  }

  @override
  int get viewId => surface.handle;
}

/// Controller for dispatching pointer events from PopupView to wlroots.
/// Follows the same pattern as _CompositorPlatformViewController but uses
/// popup_pointer_event channel method.
class _PopupPlatformViewController with _PointerEventEncoder {
  final Popup popup;

  _PopupPlatformViewController({required this.popup});

  /// Get the popup size for coordinate calculations
  Size get size => Size(
    popup.width > 0 ? popup.width.toDouble() : 100,
    popup.height > 0 ? popup.height.toDouble() : 100,
  );

  Future<void> dispatchPointerEvent(PointerEvent event) async {
    final data = encodePointerEvent(
      handle: popup.handle,
      event: event,
      widgetSize: size,
    );

    await Compositor.compositor.platform.channel.invokeMethod(
      "popup_pointer_event",
      data,
    );
  }
}
