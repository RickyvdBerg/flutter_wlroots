# FFI Refactoring Plan

## Context & Motivation

This document outlines a plan to replace Platform Channel communication with Dart FFI for performance-critical input paths between Flutter and the C compositor.

### Current Architecture

```
Hardware Input → wlroots → C compositor → Platform Channel → Dart → Widget hit-test → Platform Channel → C → wlroots seat
```

**Latency:** Platform channels add ~1-2ms round-trip overhead per event.

### Issues That Led to This Plan

1. **Scroll events don't reach Flutter widgets**: `onPointerSignal` doesn't reliably receive scroll events sent via the embedder API. Root cause unclear - possibly phase state mismatch, Flutter 3.3+ trackpad handling changes, or widget hit-test timing issues.

2. **Hybrid workarounds required**: Currently scroll must be sent both to Flutter AND directly to wlroots seat, bypassing Flutter's hit-testing for the actual Wayland delivery.

3. **Input latency**: For gaming/low-latency scenarios, 1-2ms per event is noticeable. Direct input mode exists as a workaround but breaks Flutter-first architecture.

## Proposed FFI Architecture

### Phase 1: Dart→C FFI (Input Dispatch)

Replace platform channel calls for input forwarding with direct FFI calls.

**Current (Platform Channel):**
```dart
// surface.dart
await Compositor.compositor.platform.channel.invokeMethod(
  "surface_pointer_event",
  data,
);
```

**Proposed (FFI):**
```dart
// ffi_bindings.dart
final void Function(int handle, double x, double y, int buttons, int eventType)
    nativeSurfacePointerEvent = dylib.lookupFunction<
        Void Function(Int32, Double, Double, Int32, Int32),
        void Function(int, double, double, int, int)
    >('fwr_surface_pointer_event_ffi');

// surface.dart
nativeSurfacePointerEvent(surface.handle, x, y, buttons, eventType);
```

**Benefits:**
- Latency: ~1-2ms → microseconds
- Synchronous execution (no async overhead)
- Type-safe bindings via `ffigen`

### Phase 2: C→Dart FFI (Event Callbacks)

This is more complex - the embedder API limitation remains.

**Options:**

1. **NativePort + Isolate**: C sends to Dart isolate via `Dart_PostCObject`, main isolate receives via `ReceivePort`. Still async but lower overhead than platform channel.

2. **Shared memory ring buffer**: C writes events to shared buffer, Dart polls or gets notified. Complex but lowest latency.

3. **Keep embedder for C→Dart**: Use FFI only for Dart→C direction where we control the call site.

**Recommendation:** Start with option 3 (FFI for Dart→C only), evaluate option 1 if needed.

### Phase 3: Direct Seat Notification from Dart

If FFI latency is low enough, Flutter-first becomes viable:

```
Hardware → wlroots → Embedder → Flutter widget hit-test → FFI → wlroots seat
```

No hybrid workarounds needed. Flutter owns all routing decisions.

## Implementation Steps

### Step 1: Create FFI Bindings

1. Add `ffi` and `ffigen` dependencies to compositor_dart
2. Create C header with FFI-exported functions:
   ```c
   // ffi_exports.h
   FFI_EXPORT void fwr_surface_pointer_event_ffi(
       int32_t handle, double x, double y, int32_t buttons, int32_t event_type);
   FFI_EXPORT void fwr_surface_send_key_ffi(
       int32_t handle, int32_t keycode, int32_t state);
   FFI_EXPORT void fwr_popup_pointer_event_ffi(
       int32_t handle, double x, double y, int32_t buttons, int32_t event_type);
   ```
3. Generate Dart bindings with ffigen
4. Load library in Dart: `DynamicLibrary.open('libflutter_wlroots.so')`

### Step 2: Parallel Implementation

1. Keep platform channel code working
2. Add FFI path alongside
3. Add toggle to switch between them for A/B testing
4. Measure latency difference

### Step 3: Migration

1. Validate FFI path works correctly
2. Remove platform channel input methods
3. Keep platform channel for non-latency-critical calls (surface_map, etc.)

## Files to Modify

| File | Changes |
|------|---------|
| `compositor_dart/pubspec.yaml` | Add ffi, ffigen dependencies |
| `compositor_dart/ffigen.yaml` | FFI generator config |
| `include/ffi_exports.h` | New - FFI function declarations |
| `src/ffi_exports.c` | New - FFI function implementations |
| `meson.build` | Export symbols for FFI |
| `compositor_dart/lib/ffi_bindings.dart` | Generated + manual bindings |
| `compositor_dart/lib/surface.dart` | Use FFI instead of platform channel |
| `compositor_dart/lib/platform.dart` | Add FFI initialization |

## Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| Symbol loading fails on some systems | Fallback to platform channel |
| Thread safety issues | All FFI calls from main isolate only |
| Memory management | Use arena allocators, careful lifetime tracking |
| Breaking changes in Dart FFI | Pin ffi package version |

## Success Criteria

1. Input latency reduced from ~1-2ms to <100μs
2. Scroll works through Flutter-first path (no hybrid)
3. No regressions in functionality
4. Clean fallback to platform channel if FFI unavailable

## Future Considerations

- **dart:ffi improvements**: Dart team working on better callback support
- **NativeCallable**: Dart 3.1+ has `NativeCallable.listener` for C→Dart callbacks
- **Isolate FFI**: Future Dart versions may allow FFI from isolates

## References

- [Dart FFI documentation](https://dart.dev/guides/libraries/c-interop)
- [ffigen package](https://pub.dev/packages/ffigen)
- [Flutter embedder API](https://github.com/flutter/engine/blob/main/shell/platform/embedder/embedder.h)
- Flutter 3.3+ trackpad gesture changes affecting scroll handling
