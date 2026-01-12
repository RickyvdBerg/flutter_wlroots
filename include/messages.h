#include <stdint.h>
#include <stdbool.h>

#include "standard_message_codec.h"

struct surface_pointer_event_message {
    uint32_t surface_handle;
    int64_t buttons;
    int64_t embedder_id;
    uint8_t device_kind;
    double local_pos_x;
    double local_pos_y;
    int64_t platform_id;
    int64_t pointer;
    double widget_size_x;
    double widget_size_y;
    double scroll_delta_x;
    double scroll_delta_y;
    uint8_t event_type;
    int64_t timestamp;
};

bool decode_surface_pointer_event_message(struct dart_value *value, struct surface_pointer_event_message *out);

struct surface_toplevel_set_size_message {
    uint32_t surface_handle;
    int64_t size_x;
    int64_t size_y;
};

bool decode_surface_toplevel_set_size_message(struct dart_value *value, struct surface_toplevel_set_size_message *out);

struct surface_keyboard_key_message {
    uint32_t surface_handle;
    uint64_t keycode;
    uint8_t event_type; // 0 - released, 1 - pressed
    int64_t timestamp;
};

bool decode_surface_keyboard_key_message(struct dart_value *value, struct surface_keyboard_key_message *out);

struct surface_toplevel_set_maximized_message {
    uint32_t surface_handle;
    int64_t maximized;
};

bool decode_surface_toplevel_set_maximized_message(struct dart_value *value, struct surface_toplevel_set_maximized_message *out);

struct surface_toplevel_close_message {
    uint32_t surface_handle;
};

bool decode_surface_toplevel_close_message(struct dart_value *value, struct surface_toplevel_close_message *out);

struct surface_begin_move_message {
    uint32_t surface_handle;
};

bool decode_surface_begin_move_message(struct dart_value *value, struct surface_begin_move_message *out);

struct surface_begin_resize_message {
    uint32_t surface_handle;
    int64_t edges;
};

bool decode_surface_begin_resize_message(struct dart_value *value, struct surface_begin_resize_message *out);

struct surface_set_position_message {
    uint32_t surface_handle;
    int64_t x;
    int64_t y;
};

bool decode_surface_set_position_message(struct dart_value *value, struct surface_set_position_message *out);
