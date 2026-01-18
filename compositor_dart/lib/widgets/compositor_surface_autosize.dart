import 'package:compositor_dart/compositor_dart.dart';
import 'package:flutter/widgets.dart';

/// A compatibility wrapper widget that previously sent size updates to the compositor.
///
/// Size updates are now handled by WindowManager._syncSizeToCompositor(),
/// which is the single source of truth for resize requests. This widget
/// is kept for API compatibility but simply passes through its child.
class CompositorSurfaceAutosizeWidget extends StatelessWidget {
  final Widget child;

  /// Surface parameter is kept for API compatibility but is no longer used.
  final Surface surface;

  const CompositorSurfaceAutosizeWidget({
    super.key,
    required this.child,
    required this.surface,
  });

  @override
  Widget build(BuildContext context) {
    return child;
  }
}
