library compositor_dart;

import 'dart:async';
import 'dart:collection';
import 'dart:io';
import 'dart:ui';

import 'package:compositor_dart/constants.dart';
import 'package:compositor_dart/src/output.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';
import 'package:logging/logging.dart';

export 'package:compositor_dart/src/output.dart';

enum KeyStatus { released, pressed }

class SurfacePositionEvent {
  final int handle;
  final int x;
  final int y;
  final int width;
  final int height;
  SurfacePositionEvent({
    required this.handle,
    required this.x,
    required this.y,
    required this.width,
    required this.height,
  });
}

class SurfaceGrabEndEvent {
  final int handle;
  final int x;
  final int y;
  final double cursorX;
  final double cursorY;
  SurfaceGrabEndEvent({
    required this.handle,
    required this.x,
    required this.y,
    required this.cursorX,
    required this.cursorY,
  });
}

class SurfaceMaximizeEvent {
  final int handle;
  final bool maximized;
  SurfaceMaximizeEvent({
    required this.handle,
    required this.maximized,
  });
}

/// Event fired when a client commits a buffer matching the requested resize size.
/// Used for synchronized resize to prevent black flickering.
class ResizeReadyEvent {
  final int handle;
  final int requestId;
  final int width;
  final int height;
  ResizeReadyEvent({
    required this.handle,
    required this.requestId,
    required this.width,
    required this.height,
  });
}

/// Represents a subsurface (child surface) within a toplevel surface.
/// Used by apps like Firefox/Zen for toolbars, content areas, etc.
class Subsurface {
  final int handle;
  final int textureId;
  final int parentHandle;

  int x;
  int y;
  int width;
  int height;

  Subsurface({
    required this.handle,
    required this.textureId,
    required this.parentHandle,
    this.x = 0,
    this.y = 0,
    this.width = 0,
    this.height = 0,
  });
}

class Surface {
  final int handle;
  final int textureId;

  final int pid;
  final int gid;
  final int uid;

  final Compositor compositor;

  String? title;
  String? appId;
  int width;
  int height;
  bool maximized;
  bool activated;

  /// Geometry offset - where visible content starts within the buffer.
  /// Used by CSD apps that include shadows in their buffer.
  int geoX;
  int geoY;

  /// Actual buffer dimensions (includes shadow area for CSD apps).
  /// For CSD apps: bufferWidth >= width + geoX, bufferHeight >= height + geoY
  int bufferWidth;
  int bufferHeight;

  /// True if this surface uses client-side decorations (CSD).
  /// CSD apps draw their own title bar and window controls.
  /// This can be updated after surface_map when decoration negotiation completes.
  bool usesCsd;

  /// Multi-monitor support: ID of the output (monitor) this surface is on.
  /// 0 means no specific output assigned.
  int outputId;

  /// Multi-monitor support: scale factor of the output this surface is on.
  /// Used for correct rendering on HiDPI displays.
  double outputScale;

  /// List of subsurfaces belonging to this surface
  final List<Subsurface> subsurfaces = [];

  Surface({
    required this.handle,
    required this.textureId,
    required this.pid,
    required this.gid,
    required this.uid,
    required this.compositor,
    this.title,
    this.appId,
    this.width = 0,
    this.height = 0,
    this.bufferWidth = 0,
    this.bufferHeight = 0,
    this.maximized = false,
    this.activated = false,
    this.geoX = 0,
    this.geoY = 0,
    this.usesCsd = false,
    this.outputId = 0,
    this.outputScale = 1.0,
  });
}

/// Popup surface (menus, dropdowns, tooltips)
class Popup {
  final int handle;
  final int textureId;
  final int parentHandle;
  final Compositor compositor;

  int x;  // Position relative to parent
  int y;
  int width;
  int height;

  /// Multi-monitor support: ID of the output (monitor) this popup is on.
  /// Inherited from parent surface.
  int outputId;

  /// Multi-monitor support: scale factor of the output this popup is on.
  /// Used for correct rendering on HiDPI displays.
  double outputScale;

  Popup({
    required this.handle,
    required this.textureId,
    required this.parentHandle,
    required this.compositor,
    this.x = 0,
    this.y = 0,
    this.width = 0,
    this.height = 0,
    this.outputId = 0,
    this.outputScale = 1.0,
  });
}

class CompositorSockets {
  CompositorSockets({required this.wayland, required this.x});
  final String wayland;
  final String x;
}

class _CompositorPlatform {
  final MethodChannel channel = const MethodChannel("wlroots");

  final HashMap<String, Future<dynamic> Function(MethodCall)> handlers = HashMap();

  _CompositorPlatform() {
    channel.setMethodCallHandler((call) async {
      Future<dynamic> Function(MethodCall)? handler = handlers[call.method];
      if (handler == null) {
        print("unhandled call: ${call.method}");
      } else {
        print("handled call ${call.method}");
        return await handler(call);
      }
    });
  }

  void addHandler(String method, Future<dynamic> Function(MethodCall) handler) {
    if (handlers.containsKey(method)) {
      throw Exception("attemped to add duplicate handler for $method");
    }
    handlers[method] = handler;
  }

  Future<void> surfaceToplevelSetSize(Surface surface, int width, int height) async {
    await channel.invokeListMethod("surface_toplevel_set_size", [surface.handle, width, height]);
  }

  Future<void> surfaceToplevelSetMaximized(Surface surface, bool maximized) async {
    await channel.invokeListMethod(
      "surface_toplevel_set_maximized",
      [surface.handle, maximized ? 1 : 0],
    );
  }

  Future<void> surfaceToplevelClose(Surface surface) async {
    await channel.invokeListMethod("surface_toplevel_close", [surface.handle]);
  }

  Future<void> surfaceFocus(Surface surface) async {
    await channel.invokeListMethod("surface_focus", [surface.handle]);
  }

  Future<void> clearFocus(Surface surface) async {
    await channel.invokeMethod("surface_clear_focus", [surface.handle]);
  }

  // NOTE: surfaceBeginMove and surfaceBeginResize have been removed.
  // Move/resize is now fully Dart-controlled via WindowManager in avio_wm.
  // Position/size updates are sent via surfaceSetPosition and surfaceToplevelSetSize.

  Future<void> surfaceSetPosition(Surface surface, int x, int y) async {
    await channel.invokeListMethod("surface_set_position", [surface.handle, x, y]);
  }

  /// Request a synchronized resize - waits for client to commit matching buffer.
  /// Returns immediately, resize_ready event is sent when client complies.
  Future<void> surfaceRequestResize(int handle, int width, int height, int requestId) async {
    await channel.invokeListMethod("surface_request_resize", [handle, width, height, requestId]);
  }

  /// Signal end of interactive resize operation.
  Future<void> surfaceEndResize(int handle) async {
    await channel.invokeListMethod("surface_end_resize", [handle]);
  }

  /// Enable direct input mode for low-latency gaming.
  /// When enabled, input events bypass Flutter and go directly to the surface.
  /// Use for fullscreen games or other latency-sensitive applications.
  Future<void> setDirectInputMode(Surface? surface, {required bool enabled}) async {
    await channel.invokeListMethod("set_direct_input_mode", [enabled, surface?.handle ?? 0]);
  }

  Future<void> surfaceSendKey(Surface surface, int keycode, KeyStatus status, Duration timestamp) async {
    await channel.invokeListMethod(
      "surface_keyboard_key",
      [
        surface.handle,
        keycode,
        status.index,
        timestamp.inMicroseconds,
      ],
    );
  }

  Future<CompositorSockets> getSocketPaths() async {
    var response = await channel.invokeMethod("get_socket_paths") as Map<dynamic, dynamic>;
    return CompositorSockets(
      wayland: response["wayland"] as String,
      x: response["x"] as String,
    );
  }
}

class Compositor {
  static final Compositor compositor = Compositor();

  static void initLogger() {
    FlutterError.onError = (FlutterErrorDetails details) {
      FlutterError.presentError(details);
      stderr.writeln(details.toString());
    };
    Logger.root.onRecord.listen((record) {
      stdout.writeln("${record.level.name}: ${record.time}: ${record.message}");
    });
  }

  static bool? _isCompositor;

  _CompositorPlatform platform = _CompositorPlatform();

  HashMap<int, Surface> surfaces = HashMap();
  HashMap<int, Subsurface> subsurfaces = HashMap();
  HashMap<int, Popup> popups = HashMap();
  HashMap<int, DisplayOutput> outputs = HashMap();

  // Emits an event when a surface has been added and is ready to be presented on the screen.
  StreamController<Surface> surfaceMapped = StreamController.broadcast();
  StreamController<Surface> surfaceUnmapped = StreamController.broadcast();

  // Output events (multi-monitor support)
  StreamController<DisplayOutput> outputAdded = StreamController.broadcast();
  StreamController<int> outputRemoved = StreamController.broadcast();
  StreamController<DisplayOutput> outputChanged = StreamController.broadcast();

  // Popup events (menus, dropdowns, tooltips)
  StreamController<Popup> popupMapped = StreamController.broadcast();
  StreamController<Popup> popupUnmapped = StreamController.broadcast();
  StreamController<Surface> surfaceUpdated = StreamController.broadcast();
  StreamController<SurfacePositionEvent> surfacePositionChanged = StreamController.broadcast();
  StreamController<SurfaceGrabEndEvent> surfaceGrabEnded = StreamController.broadcast();

  // Subsurface events
  StreamController<Subsurface> subsurfaceMapped = StreamController.broadcast();
  StreamController<Subsurface> subsurfaceUnmapped = StreamController.broadcast();
  StreamController<Subsurface> subsurfaceUpdated = StreamController.broadcast();

  // CSD app requests - when client-side decoration apps request window state changes
  StreamController<Surface> surfaceMinimizeRequested = StreamController.broadcast();
  StreamController<SurfaceMaximizeEvent> surfaceMaximizeRequested = StreamController.broadcast();

  // Synchronized resize events - fired when client commits matching buffer
  StreamController<ResizeReadyEvent> resizeReady = StreamController.broadcast();

  int? keyToXkb(int physicalKey) => physicalToXkbMap[physicalKey];

  Compositor() {
    platform.addHandler("surface_map", (call) async {
      Surface surface = Surface(
        handle: call.arguments["handle"],
        textureId: call.arguments["texture_id"] ?? call.arguments["handle"],
        pid: call.arguments["client_pid"],
        gid: call.arguments["client_gid"],
        uid: call.arguments["client_uid"],
        compositor: this,
        title: call.arguments["title"],
        appId: call.arguments["app_id"],
        width: call.arguments["width"] ?? 0,
        height: call.arguments["height"] ?? 0,
        bufferWidth: call.arguments["buffer_width"] ?? call.arguments["width"] ?? 0,
        bufferHeight: call.arguments["buffer_height"] ?? call.arguments["height"] ?? 0,
        maximized: (call.arguments["maximized"] ?? 0) != 0,
        activated: (call.arguments["activated"] ?? 0) != 0,
        geoX: call.arguments["geo_x"] ?? 0,
        geoY: call.arguments["geo_y"] ?? 0,
        usesCsd: (call.arguments["uses_csd"] ?? 0) != 0,
        outputId: call.arguments["output_id"] ?? 0,
        outputScale: (call.arguments["output_scale"] as num?)?.toDouble() ?? 1.0,
      );
      print("Surface mapped: handle=${surface.handle}, size=${surface.width}x${surface.height}, buffer=${surface.bufferWidth}x${surface.bufferHeight}, geoOffset=(${surface.geoX},${surface.geoY}), usesCsd=${surface.usesCsd}, outputId=${surface.outputId}, outputScale=${surface.outputScale}");
      surfaces[surface.handle] = surface;
      surfaceMapped.add(surface);
    });

    platform.addHandler("surface_unmap", (call) async {
      int handle = call.arguments["handle"];
      Surface surface = surfaces[handle]!;
      surfaces.remove(handle);
      surfaceUnmapped.add(surface);
    });

    platform.addHandler("surface_title", (call) async {
      int handle = call.arguments["handle"];
      Surface? surface = surfaces[handle];
      if (surface == null) return;
      surface.title = call.arguments["title"];
      surface.appId = call.arguments["app_id"];
      surfaceUpdated.add(surface);
    });

    platform.addHandler("surface_geometry", (call) async {
      int handle = call.arguments["handle"];
      Surface? surface = surfaces[handle];
      if (surface == null) return;
      surface.width = call.arguments["width"] ?? surface.width;
      surface.height = call.arguments["height"] ?? surface.height;
      surface.bufferWidth = call.arguments["buffer_width"] ?? surface.bufferWidth;
      surface.bufferHeight = call.arguments["buffer_height"] ?? surface.bufferHeight;
      surface.geoX = call.arguments["geo_x"] ?? surface.geoX;
      surface.geoY = call.arguments["geo_y"] ?? surface.geoY;
      surfaceUpdated.add(surface);
    });

    platform.addHandler("surface_decoration", (call) async {
      int handle = call.arguments["handle"];
      Surface? surface = surfaces[handle];
      if (surface == null) return;
      bool usesCsd = (call.arguments["uses_csd"] ?? 0) != 0;
      if (surface.usesCsd != usesCsd) {
        print("Decoration update: handle=$handle, usesCsd changed from ${surface.usesCsd} to $usesCsd");
        surface.usesCsd = usesCsd;
        surfaceUpdated.add(surface);
      }
    });

    platform.addHandler("surface_position", (call) async {
      int handle = call.arguments["handle"];
      int x = call.arguments["x"];
      int y = call.arguments["y"];
      int width = call.arguments["width"] ?? 0;
      int height = call.arguments["height"] ?? 0;
      surfacePositionChanged.add(SurfacePositionEvent(
        handle: handle,
        x: x,
        y: y,
        width: width,
        height: height,
      ));
    });

    platform.addHandler("surface_grab_end", (call) async {
      int handle = call.arguments["handle"];
      int x = call.arguments["x"];
      int y = call.arguments["y"];
      double cursorX = (call.arguments["cursor_x"] as num).toDouble();
      double cursorY = (call.arguments["cursor_y"] as num).toDouble();
      surfaceGrabEnded.add(SurfaceGrabEndEvent(
        handle: handle,
        x: x,
        y: y,
        cursorX: cursorX,
        cursorY: cursorY,
      ));
    });

    // Popup handling (menus, dropdowns, tooltips)
    platform.addHandler("popup_map", (call) async {
      int handle = call.arguments["handle"];
      int parentHandle = call.arguments["parent_handle"];
      int x = call.arguments["x"] ?? 0;
      int y = call.arguments["y"] ?? 0;
      int width = call.arguments["width"] ?? 0;
      int height = call.arguments["height"] ?? 0;
      int textureId = call.arguments["texture_id"] ?? (handle + 200000);
      int outputId = call.arguments["output_id"] ?? 0;
      double outputScale = (call.arguments["output_scale"] as num?)?.toDouble() ?? 1.0;

      Popup popup = Popup(
        handle: handle,
        textureId: textureId,
        parentHandle: parentHandle,
        compositor: this,
        x: x,
        y: y,
        width: width,
        height: height,
        outputId: outputId,
        outputScale: outputScale,
      );

      print("Popup mapped: handle=$handle, parent=$parentHandle, pos=($x,$y), size=${width}x$height, textureId=$textureId, outputId=$outputId, outputScale=$outputScale");
      popups[handle] = popup;
      popupMapped.add(popup);
    });

    platform.addHandler("popup_unmap", (call) async {
      int handle = call.arguments["handle"];
      Popup? popup = popups[handle];
      if (popup == null) return;

      print("Popup unmapped: handle=$handle");
      popups.remove(handle);
      popupUnmapped.add(popup);
    });

    platform.addHandler("flutter/keyevent", (call) async {});

    // Subsurface handlers
    platform.addHandler("subsurface_map", (call) async {
      int handle = call.arguments["handle"];
      int parentHandle = call.arguments["parent_handle"];
      int textureId = call.arguments["texture_id"] ?? handle;
      int x = call.arguments["x"] ?? 0;
      int y = call.arguments["y"] ?? 0;
      int width = call.arguments["width"] ?? 0;
      int height = call.arguments["height"] ?? 0;

      Subsurface subsurface = Subsurface(
        handle: handle,
        textureId: textureId,
        parentHandle: parentHandle,
        x: x,
        y: y,
        width: width,
        height: height,
      );

      subsurfaces[handle] = subsurface;

      // Add to parent surface's subsurface list
      Surface? parent = surfaces[parentHandle];
      if (parent != null) {
        parent.subsurfaces.add(subsurface);
        surfaceUpdated.add(parent);
      }

      subsurfaceMapped.add(subsurface);
    });

    platform.addHandler("subsurface_unmap", (call) async {
      int handle = call.arguments["handle"];
      Subsurface? subsurface = subsurfaces[handle];
      if (subsurface == null) return;

      subsurfaces.remove(handle);

      // Remove from parent surface's subsurface list
      Surface? parent = surfaces[subsurface.parentHandle];
      if (parent != null) {
        parent.subsurfaces.removeWhere((s) => s.handle == handle);
        surfaceUpdated.add(parent);
      }

      subsurfaceUnmapped.add(subsurface);
    });

    platform.addHandler("subsurface_position", (call) async {
      int handle = call.arguments["handle"];
      int x = call.arguments["x"] ?? 0;
      int y = call.arguments["y"] ?? 0;
      int width = call.arguments["width"] ?? 0;
      int height = call.arguments["height"] ?? 0;

      Subsurface? subsurface = subsurfaces[handle];
      if (subsurface == null) return;

      subsurface.x = x;
      subsurface.y = y;
      subsurface.width = width;
      subsurface.height = height;

      // Notify parent surface updated
      Surface? parent = surfaces[subsurface.parentHandle];
      if (parent != null) {
        surfaceUpdated.add(parent);
      }

      subsurfaceUpdated.add(subsurface);
    });

    // CSD app minimize request
    platform.addHandler("surface_minimize", (call) async {
      int handle = call.arguments["handle"];
      Surface? surface = surfaces[handle];
      if (surface == null) return;
      print("CSD app requested minimize for surface $handle");
      surfaceMinimizeRequested.add(surface);
    });

    // CSD app maximize request
    platform.addHandler("surface_request_maximize", (call) async {
      int handle = call.arguments["handle"];
      bool maximized = (call.arguments["maximized"] ?? 0) != 0;
      Surface? surface = surfaces[handle];
      if (surface == null) return;
      print("CSD app requested maximize for surface $handle, maximized=$maximized");
      surfaceMaximizeRequested.add(SurfaceMaximizeEvent(
        handle: handle,
        maximized: maximized,
      ));
    });

    // Synchronized resize: client committed buffer matching requested size
    platform.addHandler("resize_ready", (call) async {
      int handle = call.arguments["handle"];
      int requestId = call.arguments["request_id"];
      int width = call.arguments["width"];
      int height = call.arguments["height"];
      print("Resize ready: handle=$handle, requestId=$requestId, size=${width}x$height");
      resizeReady.add(ResizeReadyEvent(
        handle: handle,
        requestId: requestId,
        width: width,
        height: height,
      ));
    });

    // Output (monitor) handlers for multi-monitor support
    platform.addHandler("output_added", (call) async {
      final args = call.arguments as Map<dynamic, dynamic>;
      final output = DisplayOutput.fromArgs(args);

      // Primary is the output at position (0,0) - the leftmost/topmost monitor
      // This handles outputs being registered in any order
      if (output.x == 0 && output.y == 0) {
        // New output is at origin - make it primary, demote others
        for (final existing in outputs.values) {
          existing.isPrimary = false;
        }
        output.isPrimary = true;
      } else if (outputs.isEmpty) {
        // First output and not at origin - make primary for now
        // Will be demoted if origin output is added later
        output.isPrimary = true;
      }

      outputs[output.id] = output;
      print("Output added: $output");
      outputAdded.add(output);
    });

    platform.addHandler("output_removed", (call) async {
      final args = call.arguments as Map<dynamic, dynamic>;
      final outputId = args['id'] as int;

      final output = outputs[outputId];
      if (output == null) return;

      outputs.remove(outputId);
      print("Output removed: $outputId ($output)");
      outputRemoved.add(outputId);

      // If primary was removed, make another output primary
      if (output.isPrimary && outputs.isNotEmpty) {
        outputs.values.first.isPrimary = true;
        outputChanged.add(outputs.values.first);
      }
    });

    platform.addHandler("output_changed", (call) async {
      final args = call.arguments as Map<dynamic, dynamic>;
      final outputId = args['id'] as int;

      final output = outputs[outputId];
      if (output == null) return;

      output.updateFrom(args);
      print("Output changed: $output");
      outputChanged.add(output);
    });

    // Signal to C that Dart is ready to receive messages
    // This triggers sending of existing outputs that were detected before Dart initialized
    _signalReady();
  }

  void _signalReady() {
    // Use Future.microtask to ensure all constructor initialization is complete
    // and handlers are registered before signaling ready
    Future.microtask(() async {
      try {
        await platform.channel.invokeMethod("compositor_ready");
        print("Compositor ready signal sent to C");
      } catch (e) {
        print("Error sending compositor_ready: $e");
      }
    });
  }

  /// Returns `true` if we are currently running in the compositor embedder.
  /// If so, all functionality in this library is available.
  ///
  /// Returns `false` in all other cases. If so, no funcitonality in this
  /// library should be used.
  Future<bool> isCompositor() async {
    if (_isCompositor != null) return _isCompositor!;

    try {
      await platform.channel.invokeMethod("is_compositor");
      _isCompositor = true;
    } on MissingPluginException {
      _isCompositor = false;
    }

    return _isCompositor!;
  }

  /// Will return the paths of the compositor sockets.
  Future<CompositorSockets> getSocketPaths() => platform.getSocketPaths();

  // ============== Multi-Monitor Support ==============

  /// Get the primary display output.
  DisplayOutput? get primaryOutput =>
      outputs.values.cast<DisplayOutput?>().firstWhere(
            (o) => o?.isPrimary == true,
            orElse: () => outputs.values.isNotEmpty ? outputs.values.first : null,
          );

  /// Get total bounds across all outputs (unified coordinate space).
  Rect get totalBounds {
    if (outputs.isEmpty) {
      return Rect.zero;
    }

    int minX = outputs.values.map((o) => o.x).reduce((a, b) => a < b ? a : b);
    int minY = outputs.values.map((o) => o.y).reduce((a, b) => a < b ? a : b);
    int maxX = outputs.values.map((o) => o.x + o.width).reduce((a, b) => a > b ? a : b);
    int maxY = outputs.values.map((o) => o.y + o.height).reduce((a, b) => a > b ? a : b);

    return Rect.fromLTRB(
      minX.toDouble(),
      minY.toDouble(),
      maxX.toDouble(),
      maxY.toDouble(),
    );
  }

  /// Get the output containing a given point.
  DisplayOutput? getOutputAtPoint(double x, double y) {
    for (final output in outputs.values) {
      if (output.containsPoint(x, y)) {
        return output;
      }
    }
    return null;
  }

  /// Get the output that contains most of the given rectangle.
  DisplayOutput? getOutputForRect(Rect rect) {
    DisplayOutput? bestOutput;
    double bestOverlap = 0;

    for (final output in outputs.values) {
      final outputRect = Rect.fromLTWH(
        output.x.toDouble(),
        output.y.toDouble(),
        output.width.toDouble(),
        output.height.toDouble(),
      );

      final intersection = rect.intersect(outputRect);
      if (!intersection.isEmpty) {
        final overlap = intersection.width * intersection.height;
        if (overlap > bestOverlap) {
          bestOverlap = overlap;
          bestOutput = output;
        }
      }
    }

    return bestOutput ?? primaryOutput;
  }

  /// Set which output is primary.
  /// Returns true on success, false on failure.
  Future<bool> setPrimaryOutput(int outputId) async {
    try {
      for (final output in outputs.values) {
        output.isPrimary = output.id == outputId;
      }
      await platform.channel.invokeMethod("set_primary_output", [outputId]);
      return true;
    } on PlatformException catch (e) {
      print('Failed to set primary output: $e');
      return false;
    }
  }

  /// Set which output drives Flutter's vsync (0 = auto/highest refresh).
  /// Returns true on success, false on failure.
  Future<bool> setVsyncOutput(int outputId) async {
    try {
      await platform.channel.invokeMethod("set_vsync_output", [outputId]);
      return true;
    } on PlatformException catch (e) {
      print('Failed to set vsync output: $e');
      return false;
    }
  }

  /// Set vsync rate limit for power saving (0 = unlimited, >0 = max Hz).
  /// Returns true on success, false on failure.
  Future<bool> setVsyncRateLimit(int maxHz) async {
    try {
      await platform.channel.invokeMethod("set_vsync_rate_limit", [maxHz]);
      return true;
    } on PlatformException catch (e) {
      print('Failed to set vsync rate limit: $e');
      return false;
    }
  }

  /// Set output mode (resolution and refresh rate).
  /// Returns true on success, false on failure.
  Future<bool> setOutputMode(int outputId, DisplayMode mode) async {
    try {
      await platform.channel.invokeMethod("set_output_mode", [
        outputId,
        mode.width,
        mode.height,
        mode.refresh,
      ]);
      return true;
    } on PlatformException catch (e) {
      print('Failed to set output mode: $e');
      return false;
    }
  }

  /// Set output position in the layout.
  /// Returns true on success, false on failure.
  Future<bool> setOutputPosition(int outputId, int x, int y) async {
    try {
      await platform.channel.invokeMethod("set_output_position", [outputId, x, y]);
      return true;
    } on PlatformException catch (e) {
      print('Failed to set output position: $e');
      return false;
    }
  }

  /// Set output scale factor.
  /// Returns true on success, false on failure.
  Future<bool> setOutputScale(int outputId, double scale) async {
    try {
      await platform.channel.invokeMethod("set_output_scale", [outputId, scale]);
      return true;
    } on PlatformException catch (e) {
      print('Failed to set output scale: $e');
      return false;
    }
  }
}
