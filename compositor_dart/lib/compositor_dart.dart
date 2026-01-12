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

class Surface {
  final int handle;

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

  Surface({
    required this.handle,
    required this.pid,
    required this.gid,
    required this.uid,
    required this.compositor,
    this.title,
    this.appId,
    this.width = 0,
    this.height = 0,
    this.maximized = false,
    this.activated = false,
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

  // Emits an event when a surface has been added and is ready to be presented on the screen.
  StreamController<Surface> surfaceMapped = StreamController.broadcast();
  StreamController<Surface> surfaceUnmapped = StreamController.broadcast();
  StreamController<Surface> surfaceUpdated = StreamController.broadcast();
  StreamController<SurfacePositionEvent> surfacePositionChanged = StreamController.broadcast();
  StreamController<SurfaceGrabEndEvent> surfaceGrabEnded = StreamController.broadcast();

  int? keyToXkb(int physicalKey) => physicalToXkbMap[physicalKey];

  Compositor() {
    platform.addHandler("surface_map", (call) async {
      Surface surface = Surface(
        handle: call.arguments["handle"],
        pid: call.arguments["client_pid"],
        gid: call.arguments["client_gid"],
        uid: call.arguments["client_uid"],
        compositor: this,
        title: call.arguments["title"],
        appId: call.arguments["app_id"],
        width: call.arguments["width"] ?? 0,
        height: call.arguments["height"] ?? 0,
        maximized: (call.arguments["maximized"] ?? 0) != 0,
        activated: (call.arguments["activated"] ?? 0) != 0,
      );
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

    platform.addHandler("flutter/keyevent", (call) async {});
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
