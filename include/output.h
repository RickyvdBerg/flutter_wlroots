#pragma once

#include <stdbool.h>
#include <wayland-util.h>
#include <wayland-server-core.h>

// Forward declarations
struct fwr_instance;
struct fwr_output;

void fwr_server_new_output(struct wl_listener *listener, void *data);
void fwr_engine_vsync_callback(void *data, intptr_t baton);

// Multi-monitor support: determine which output a box is on (by center point)
struct fwr_output *fwr_output_for_box(struct fwr_instance *instance,
                                       int x, int y, int width, int height);

// Send all existing outputs to Flutter (called after engine initialization)
void fwr_send_all_outputs(struct fwr_instance *instance);

// Vsync driver selection
void fwr_select_highest_refresh_output(struct fwr_instance *instance);
void fwr_set_vsync_output(struct fwr_instance *instance, uint32_t output_id);
void fwr_set_vsync_rate_limit(struct fwr_instance *instance, int max_hz);

// Output configuration
bool fwr_set_output_mode(struct fwr_instance *instance, uint32_t output_id,
                         int width, int height, int refresh);
bool fwr_set_output_position(struct fwr_instance *instance, uint32_t output_id,
                             int x, int y);
bool fwr_set_output_scale(struct fwr_instance *instance, uint32_t output_id,
                          double scale);