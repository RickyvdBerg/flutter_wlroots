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
    // Focus the surface when clicked
    Compositor.compositor.platform.surfaceFocus(widget.surface);
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
      onPointerSignal: controller.dispatchPointerEvent,
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
  Widget _buildSurfaceTree() {
    final surface = widget.surface;

    Widget mainTexture = Texture(textureId: surface.textureId);

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

/// Widget for rendering popup surfaces (menus, dropdowns)
/// Input is handled directly by wlroots via cursor hit testing, not Flutter
class PopupView extends StatelessWidget {
  final Popup popup;

  const PopupView({Key? key, required this.popup}) : super(key: key);

  @override
  Widget build(BuildContext context) {
    // IgnorePointer ensures Flutter doesn't intercept pointer events
    // wlroots handles popup input directly via cursor motion/button handlers
    return IgnorePointer(
      child: Texture(textureId: popup.textureId),
    );
  }
}

class _CompositorPlatformViewController extends PlatformViewController {
  Surface surface;
  Size size = const Size(100, 100);

  _CompositorPlatformViewController({required this.surface});

  void setSize(Size size) {
    this.size = size;
    Compositor.compositor.platform.surfaceToplevelSetSize(surface, size.width.round(), size.height.round());
  }

  /// Transform widget-relative coordinates to surface coordinates.
  /// Simple pass-through - let wlroots handle coordinate mapping.
  Offset _toSurfaceCoords(Offset widgetPos) {
    return widgetPos;
  }

  @override
  Future<void> clearFocus() => Compositor.compositor.platform.clearFocus(surface);

  @override
  Future<void> dispatchPointerEvent(PointerEvent event) async {
    //print("${event.toString()}");

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

    // Simple coordinate pass-through
    final surfacePos = _toSurfaceCoords(event.localPosition);

    List data = [
      surface.handle,
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
      surfacePos.dx,
      surfacePos.dy,
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
      size.width,
      size.height,
      scrollAmount.dx,
      scrollAmount.dy,
    ];

    //print("pointerevent $data");

    await Compositor.compositor.platform.channel.invokeMethod(
      "surface_pointer_event",
      data,
    );
  }

  @override
  Future<void> dispose() async {
    // TODO: implement dispose
    //throw UnimplementedError();
  }

  @override
  int get viewId => surface.handle;
}
