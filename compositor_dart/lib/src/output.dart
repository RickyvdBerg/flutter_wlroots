/// Display output (monitor) representation for multi-monitor support.
library;

/// Represents an available display mode (resolution + refresh rate).
class DisplayMode {
  final int width;
  final int height;
  final int refresh; // mHz (e.g., 144000 for 144Hz)

  const DisplayMode({
    required this.width,
    required this.height,
    required this.refresh,
  });

  /// Refresh rate in Hz (e.g., 144.0 for 144Hz)
  double get refreshHz => refresh / 1000.0;

  @override
  String toString() => '${width}x$height @ ${refreshHz.toStringAsFixed(0)}Hz';

  @override
  bool operator ==(Object other) =>
      other is DisplayMode &&
      other.width == width &&
      other.height == height &&
      other.refresh == refresh;

  @override
  int get hashCode => Object.hash(width, height, refresh);
}

/// Represents a physical display output (monitor).
class DisplayOutput {
  /// Unique output ID from the compositor.
  final int id;

  /// Output name (e.g., "HDMI-A-1", "DP-2", "eDP-1").
  final String name;

  /// Manufacturer name.
  final String make;

  /// Model name.
  final String model;

  /// Position in the unified coordinate space.
  int x;
  int y;

  /// Current resolution.
  int width;
  int height;

  /// Current refresh rate in mHz (e.g., 144000 for 144Hz).
  int refreshRate;

  /// Display scale factor.
  double scale;

  /// Transform (rotation/flip): 0=normal, 1=90, 2=180, 3=270, 4-7=flipped variants.
  int transform;

  /// Available display modes.
  List<DisplayMode> availableModes;

  /// Whether this is the primary display.
  bool isPrimary;

  DisplayOutput({
    required this.id,
    required this.name,
    this.make = '',
    this.model = '',
    this.x = 0,
    this.y = 0,
    this.width = 0,
    this.height = 0,
    this.refreshRate = 60000,
    this.scale = 1.0,
    this.transform = 0,
    this.availableModes = const [],
    this.isPrimary = false,
  });

  /// Refresh rate in Hz (e.g., 144.0 for 144Hz).
  double get refreshHz => refreshRate / 1000.0;

  /// Display bounds as a rectangle.
  ({int x, int y, int width, int height}) get bounds => (
        x: x,
        y: y,
        width: width,
        height: height,
      );

  /// Check if a point is within this display's bounds.
  bool containsPoint(double px, double py) {
    return px >= x && px < x + width && py >= y && py < y + height;
  }

  /// Update from platform channel message arguments.
  void updateFrom(Map<dynamic, dynamic> args) {
    if (args.containsKey('x')) x = args['x'] as int;
    if (args.containsKey('y')) y = args['y'] as int;
    if (args.containsKey('width')) width = args['width'] as int;
    if (args.containsKey('height')) height = args['height'] as int;
    if (args.containsKey('refresh')) refreshRate = args['refresh'] as int;
    if (args.containsKey('scale')) scale = (args['scale'] as num).toDouble();
    if (args.containsKey('transform')) transform = args['transform'] as int;

    if (args.containsKey('modes')) {
      final modesList = args['modes'] as List<dynamic>;
      availableModes = modesList.map((m) {
        final mode = m as Map<dynamic, dynamic>;
        return DisplayMode(
          width: mode['width'] as int,
          height: mode['height'] as int,
          refresh: mode['refresh'] as int,
        );
      }).toList();
    }
  }

  /// Create from platform channel message arguments.
  factory DisplayOutput.fromArgs(Map<dynamic, dynamic> args) {
    final output = DisplayOutput(
      id: args['id'] as int,
      name: args['name'] as String? ?? '',
      make: args['make'] as String? ?? '',
      model: args['model'] as String? ?? '',
    );
    output.updateFrom(args);
    return output;
  }

  @override
  String toString() =>
      'DisplayOutput($name, ${width}x$height @ ${refreshHz.toStringAsFixed(0)}Hz, pos=($x,$y))';
}
