#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "flutter_embedder.h"
#include "shaders.h"
#include "standard_message_codec.h"

#define FWR_MULTITOUCH_MAX 10

enum fwr_grab_type {
    FWR_GRAB_NONE = 0,
    FWR_GRAB_MOVE,
    FWR_GRAB_RESIZE,
};

struct fwr_grab_state {
    enum fwr_grab_type type;
    uint32_t view_handle;
    double start_cursor_x;
    double start_cursor_y;
    int start_view_x;
    int start_view_y;
    int start_view_width;
    int start_view_height;
    uint32_t resize_edges;
};

struct fwr_input_state {
    uint32_t mouse_button_mask;
    uint32_t fl_mouse_button_mask;

    // Accumulated state for cursor before frame.
    uint32_t acc_mouse_button_mask;
    double acc_scroll_delta_x;
    double acc_scroll_delta_y;

    // Touch state
    bool simulating_pointer_from_touch;
    int touch_pointer_simulation_id;
    int64_t touch_ids[FWR_MULTITOUCH_MAX];

    // Flutter cursor position (from platform channel events)
    // Used for grab operations when running nested
    double flutter_cursor_x;
    double flutter_cursor_y;

    struct fwr_grab_state grab;
};

struct fwr_input_touch_point_state {
    double x;
    double y;
};

struct fwr_input_device_state {
    struct fwr_input_touch_point_state touch_points[10];
};

void fwr_input_init(struct fwr_instance *instance);

void fwr_handle_surface_pointer_event_message(struct fwr_instance *instance, const FlutterPlatformMessageResponseHandle *handle, struct dart_value *args);

void fwr_handle_surface_keyboard_key_message(struct fwr_instance *instance, const FlutterPlatformMessageResponseHandle *handle, struct dart_value *args);

void fwr_handle_surface_begin_move(struct fwr_instance *instance, const FlutterPlatformMessageResponseHandle *handle, struct dart_value *args);

void fwr_handle_surface_begin_resize(struct fwr_instance *instance, const FlutterPlatformMessageResponseHandle *handle, struct dart_value *args);

void fwr_handle_popup_pointer_event_message(struct fwr_instance *instance, const FlutterPlatformMessageResponseHandle *handle, struct dart_value *args);

// Clear button state tracking for a surface (call when surface is destroyed)
void fwr_clear_surface_buttons(uint32_t surface_handle);