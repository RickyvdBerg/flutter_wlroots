library compositor_dart;

import 'dart:async';
import 'dart:collection';
import 'dart:io';

import 'package:compositor_dart/constants.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';
import 'package:logging/logging.dart';

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

  Popup({
    required this.handle,
    required this.textureId,
    required this.parentHandle,
    required this.compositor,
    this.x = 0,
    this.y = 0,
    this.width = 0,
    this.height = 0,
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

  Future<void> surfaceBeginMove(Surface surface) async {
    await channel.invokeListMethod("surface_begin_move", [surface.handle]);
  }

  Future<void> surfaceBeginResize(Surface surface, int edges) async {
    await channel.invokeListMethod("surface_begin_resize", [surface.handle, edges]);
  }

  Future<void> surfaceSetPosition(Surface surface, int x, int y) async {
    await channel.invokeListMethod("surface_set_position", [surface.handle, x, y]);
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

  // Emits an event when a surface has been added and is ready to be presented on the screen.
  StreamController<Surface> surfaceMapped = StreamController.broadcast();
  StreamController<Surface> surfaceUnmapped = StreamController.broadcast();

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
      );
      print("Surface mapped: handle=${surface.handle}, size=${surface.width}x${surface.height}, buffer=${surface.bufferWidth}x${surface.bufferHeight}, geoOffset=(${surface.geoX},${surface.geoY}), usesCsd=${surface.usesCsd}");
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

      Popup popup = Popup(
        handle: handle,
        textureId: textureId,
        parentHandle: parentHandle,
        compositor: this,
        x: x,
        y: y,
        width: width,
        height: height,
      );

      print("Popup mapped: handle=$handle, parent=$parentHandle, pos=($x,$y), size=${width}x$height, textureId=$textureId");
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
}
