#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <drm_fourcc.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <instance.h>

#include <wlr/util/log.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/render/pass.h>
#include <wlr/backend.h>
#include <wlr/backend/wayland.h>
#include <wlr/backend/x11.h>
#include <wlr/backend/multi.h>

#include "renderer.h"
#include "handle_map.h"
#include "messages.h"

// Platform channel callback (no-op, messages are fire-and-forget)
static void output_platform_cb(const uint8_t *data, size_t size, void *user_data) {
  // No response expected
}

// Get output refresh rate in mHz
static int get_output_refresh(struct wlr_output *output) {
  if (output->current_mode != NULL) {
    return output->current_mode->refresh;
  }
  return 60000;  // Default to 60Hz
}

// Select the vsync output based on highest refresh rate
void fwr_select_highest_refresh_output(struct fwr_instance *instance) {
  struct fwr_output *best = NULL;
  int best_refresh = 0;

  struct fwr_output *output;
  wl_list_for_each(output, &instance->outputs, link) {
    int refresh = get_output_refresh(output->wlr_output);
    if (refresh > best_refresh) {
      best_refresh = refresh;
      best = output;
    }
  }

  instance->vsync_output = best;
  if (best != NULL) {
    wlr_log(WLR_INFO, "Selected vsync output: %s (%d mHz)",
            best->wlr_output->name, best_refresh);
  } else {
    wlr_log(WLR_INFO, "No outputs available for vsync - Flutter frame pacing may be affected");
  }
}

// Set a specific output as the vsync driver (0 = auto/highest)
void fwr_set_vsync_output(struct fwr_instance *instance, uint32_t output_id) {
  if (output_id == 0) {
    fwr_select_highest_refresh_output(instance);
    return;
  }

  struct fwr_output *output;
  wl_list_for_each(output, &instance->outputs, link) {
    if (output->id == output_id) {
      instance->vsync_output = output;
      wlr_log(WLR_INFO, "Set vsync output to: %s (id=%d)",
              output->wlr_output->name, output_id);
      return;
    }
  }

  wlr_log(WLR_ERROR, "Vsync output id %d not found, using auto", output_id);
  fwr_select_highest_refresh_output(instance);
}

// Set vsync rate limit (0 = unlimited, >0 = max Hz for power saving)
void fwr_set_vsync_rate_limit(struct fwr_instance *instance, int max_hz) {
  instance->vsync_rate_limit = max_hz;
  wlr_log(WLR_INFO, "Vsync rate limit set to: %d Hz (0 = unlimited)", max_hz);
}

// Find output by ID
static struct fwr_output *find_output_by_id(struct fwr_instance *instance, uint32_t output_id) {
  struct fwr_output *output;
  wl_list_for_each(output, &instance->outputs, link) {
    if (output->id == output_id) {
      return output;
    }
  }
  return NULL;
}

// Multi-monitor support: determine which output a box is on (by center point)
struct fwr_output *fwr_output_for_box(struct fwr_instance *instance,
                                       int x, int y, int width, int height) {
  // Use center point to determine output
  double cx = (double)x + (double)width / 2.0;
  double cy = (double)y + (double)height / 2.0;

  struct wlr_output *wlr_out = wlr_output_layout_output_at(
      instance->output_layout, cx, cy);

  if (wlr_out == NULL) {
    // Fallback to first output
    return fwr_get_first_output(instance);
  }

  // Find our fwr_output wrapper
  struct fwr_output *output;
  wl_list_for_each(output, &instance->outputs, link) {
    if (output->wlr_output == wlr_out) {
      return output;
    }
  }

  // Shouldn't happen, but fallback
  return fwr_get_first_output(instance);
}

// Set output mode (resolution + refresh rate)
bool fwr_set_output_mode(struct fwr_instance *instance, uint32_t output_id,
                         int width, int height, int refresh) {
  struct fwr_output *fwr_out = find_output_by_id(instance, output_id);
  if (fwr_out == NULL) {
    wlr_log(WLR_ERROR, "Output id %d not found", output_id);
    return false;
  }

  struct wlr_output *wlr_out = fwr_out->wlr_output;

  // Find matching mode
  struct wlr_output_mode *mode;
  struct wlr_output_mode *best_mode = NULL;
  wl_list_for_each(mode, &wlr_out->modes, link) {
    if (mode->width == width && mode->height == height) {
      if (refresh == 0 || mode->refresh == refresh) {
        best_mode = mode;
        if (refresh != 0) break;  // Exact match found
      }
    }
  }

  if (best_mode == NULL) {
    wlr_log(WLR_ERROR, "Mode %dx%d@%d not found for output %s",
            width, height, refresh, wlr_out->name);
    return false;
  }

  struct wlr_output_state state;
  wlr_output_state_init(&state);
  wlr_output_state_set_mode(&state, best_mode);

  if (!wlr_output_commit_state(wlr_out, &state)) {
    wlr_log(WLR_ERROR, "Failed to set mode for output %s", wlr_out->name);
    wlr_output_state_finish(&state);
    return false;
  }

  wlr_output_state_finish(&state);
  wlr_log(WLR_INFO, "Set output %s mode to %dx%d@%d",
          wlr_out->name, best_mode->width, best_mode->height, best_mode->refresh);
  return true;
}

// Set output position in layout
bool fwr_set_output_position(struct fwr_instance *instance, uint32_t output_id,
                             int x, int y) {
  struct fwr_output *fwr_out = find_output_by_id(instance, output_id);
  if (fwr_out == NULL) {
    wlr_log(WLR_ERROR, "Output id %d not found", output_id);
    return false;
  }

  wlr_output_layout_add(instance->output_layout, fwr_out->wlr_output, x, y);
  wlr_log(WLR_INFO, "Set output %s position to (%d, %d)",
          fwr_out->wlr_output->name, x, y);
  return true;
}

// Set output scale factor
bool fwr_set_output_scale(struct fwr_instance *instance, uint32_t output_id,
                          double scale) {
  struct fwr_output *fwr_out = find_output_by_id(instance, output_id);
  if (fwr_out == NULL) {
    wlr_log(WLR_ERROR, "Output id %d not found", output_id);
    return false;
  }

  struct wlr_output *wlr_out = fwr_out->wlr_output;

  struct wlr_output_state state;
  wlr_output_state_init(&state);
  wlr_output_state_set_scale(&state, scale);

  if (!wlr_output_commit_state(wlr_out, &state)) {
    wlr_log(WLR_ERROR, "Failed to set scale for output %s", wlr_out->name);
    wlr_output_state_finish(&state);
    return false;
  }

  wlr_output_state_finish(&state);
  wlr_log(WLR_INFO, "Set output %s scale to %.2f", wlr_out->name, scale);
  return true;
}

// Send output_added platform channel message
static void send_output_added(struct fwr_instance *instance, struct fwr_output *output) {
  if (instance->engine == NULL) return;

  struct wlr_output *wlr_out = output->wlr_output;
  struct wlr_box box;
  wlr_output_layout_get_box(instance->output_layout, wlr_out, &box);

  // Count available modes
  int mode_count = 0;
  struct wlr_output_mode *mode;
  wl_list_for_each(mode, &wlr_out->modes, link) {
    mode_count++;
  }

  struct message_builder msg = message_builder_new();
  struct message_builder_segment msg_seg = message_builder_segment(&msg);
  message_builder_segment_push_string(&msg_seg, "output_added");
  message_builder_segment_finish(&msg_seg);

  msg_seg = message_builder_segment(&msg);
  struct message_builder_segment arg_seg =
      message_builder_segment_push_map(&msg_seg, 12);

  message_builder_segment_push_string(&arg_seg, "id");
  message_builder_segment_push_int64(&arg_seg, output->id);

  message_builder_segment_push_string(&arg_seg, "name");
  message_builder_segment_push_string(&arg_seg, wlr_out->name ? wlr_out->name : "");

  message_builder_segment_push_string(&arg_seg, "make");
  message_builder_segment_push_string(&arg_seg, wlr_out->make ? wlr_out->make : "");

  message_builder_segment_push_string(&arg_seg, "model");
  message_builder_segment_push_string(&arg_seg, wlr_out->model ? wlr_out->model : "");

  message_builder_segment_push_string(&arg_seg, "x");
  message_builder_segment_push_int64(&arg_seg, box.x);

  message_builder_segment_push_string(&arg_seg, "y");
  message_builder_segment_push_int64(&arg_seg, box.y);

  message_builder_segment_push_string(&arg_seg, "width");
  message_builder_segment_push_int64(&arg_seg, wlr_out->width);

  message_builder_segment_push_string(&arg_seg, "height");
  message_builder_segment_push_int64(&arg_seg, wlr_out->height);

  message_builder_segment_push_string(&arg_seg, "refresh");
  message_builder_segment_push_int64(&arg_seg, get_output_refresh(wlr_out));

  message_builder_segment_push_string(&arg_seg, "scale");
  message_builder_segment_push_float64(&arg_seg, wlr_out->scale);

  message_builder_segment_push_string(&arg_seg, "transform");
  message_builder_segment_push_int64(&arg_seg, wlr_out->transform);

  // Build modes list
  message_builder_segment_push_string(&arg_seg, "modes");
  struct message_builder_segment modes_seg =
      message_builder_segment_push_list(&arg_seg, mode_count);
  wl_list_for_each(mode, &wlr_out->modes, link) {
    struct message_builder_segment mode_seg =
        message_builder_segment_push_map(&modes_seg, 3);
    message_builder_segment_push_string(&mode_seg, "width");
    message_builder_segment_push_int64(&mode_seg, mode->width);
    message_builder_segment_push_string(&mode_seg, "height");
    message_builder_segment_push_int64(&mode_seg, mode->height);
    message_builder_segment_push_string(&mode_seg, "refresh");
    message_builder_segment_push_int64(&mode_seg, mode->refresh);
    message_builder_segment_finish(&mode_seg);
  }
  message_builder_segment_finish(&modes_seg);

  message_builder_segment_finish(&arg_seg);
  message_builder_segment_finish(&msg_seg);

  uint8_t *msg_buf;
  size_t msg_buf_len;
  message_builder_finish(&msg, &msg_buf, &msg_buf_len);

  FlutterPlatformMessageResponseHandle *response_handle;
  instance->fl_proc_table.PlatformMessageCreateResponseHandle(
      instance->engine, output_platform_cb, NULL, &response_handle);

  FlutterPlatformMessage platform_message = {};
  platform_message.struct_size = sizeof(FlutterPlatformMessage);
  platform_message.channel = "wlroots";
  platform_message.message = msg_buf;
  platform_message.message_size = msg_buf_len;
  platform_message.response_handle = response_handle;
  instance->fl_proc_table.SendPlatformMessage(instance->engine, &platform_message);

  free(msg_buf);
  instance->fl_proc_table.PlatformMessageReleaseResponseHandle(instance->engine,
                                                               response_handle);

  wlr_log(WLR_INFO, "Sent output_added for %s (id=%d, %dx%d @ %d mHz)",
          wlr_out->name, output->id, wlr_out->width, wlr_out->height,
          get_output_refresh(wlr_out));
}

// Send output_added messages for all existing outputs.
// Called after Flutter engine is ready to handle messages that were dropped
// during early initialization (outputs are detected before engine starts).
void fwr_send_all_outputs(struct fwr_instance *instance) {
  if (instance->engine == NULL) return;

  int count = 0;
  struct fwr_output *output;
  wl_list_for_each(output, &instance->outputs, link) {
    send_output_added(instance, output);
    count++;
  }
  wlr_log(WLR_INFO, "Sent %d existing outputs to Flutter", count);
}

// Send output_removed platform channel message
static void send_output_removed(struct fwr_instance *instance, uint32_t output_id) {
  if (instance->engine == NULL) return;

  struct message_builder msg = message_builder_new();
  struct message_builder_segment msg_seg = message_builder_segment(&msg);
  message_builder_segment_push_string(&msg_seg, "output_removed");
  message_builder_segment_finish(&msg_seg);

  msg_seg = message_builder_segment(&msg);
  struct message_builder_segment arg_seg =
      message_builder_segment_push_map(&msg_seg, 1);
  message_builder_segment_push_string(&arg_seg, "id");
  message_builder_segment_push_int64(&arg_seg, output_id);
  message_builder_segment_finish(&arg_seg);
  message_builder_segment_finish(&msg_seg);

  uint8_t *msg_buf;
  size_t msg_buf_len;
  message_builder_finish(&msg, &msg_buf, &msg_buf_len);

  FlutterPlatformMessageResponseHandle *response_handle;
  instance->fl_proc_table.PlatformMessageCreateResponseHandle(
      instance->engine, output_platform_cb, NULL, &response_handle);

  FlutterPlatformMessage platform_message = {};
  platform_message.struct_size = sizeof(FlutterPlatformMessage);
  platform_message.channel = "wlroots";
  platform_message.message = msg_buf;
  platform_message.message_size = msg_buf_len;
  platform_message.response_handle = response_handle;
  instance->fl_proc_table.SendPlatformMessage(instance->engine, &platform_message);

  free(msg_buf);
  instance->fl_proc_table.PlatformMessageReleaseResponseHandle(instance->engine,
                                                               response_handle);

  wlr_log(WLR_INFO, "Sent output_removed for id=%d", output_id);
}

// Check if a backend is nested (running inside another compositor)
static bool is_nested_backend(struct wlr_backend *backend) {
  if (backend == NULL) {
    return false;
  }
  if (wlr_backend_is_wl(backend) || wlr_backend_is_x11(backend)) {
    return true;
  }
  // For multi-backend, check each child
  if (wlr_backend_is_multi(backend)) {
    // Multi-backend created by autocreate will have nested if WAYLAND_DISPLAY is set
    // The multi backend combines multiple backends; if any is WL/X11, we're nested
    // However, there's no easy way to iterate children, so check environment
    return getenv("WAYLAND_DISPLAY") != NULL || getenv("DISPLAY") != NULL;
  }
  return false;
}

// Render software cursor on the output
static void render_cursor(struct fwr_instance *instance, struct wlr_render_pass *render_pass,
                          struct fwr_output_viewport *viewport) {
  if (instance->cursor == NULL || instance->renderer == NULL) {
    return;
  }

  // Skip software cursor rendering in nested mode (host handles cursor)
  if (is_nested_backend(instance->backend)) {
    return;
  }

  struct wlr_texture *cursor_texture = NULL;
  int hotspot_x = 0;
  int hotspot_y = 0;

  // Try client-provided cursor surface first
  if (instance->client_cursor_surface != NULL &&
      instance->client_cursor_surface->mapped) {
    cursor_texture = wlr_surface_get_texture(instance->client_cursor_surface);
    hotspot_x = instance->client_cursor_hotspot_x;
    hotspot_y = instance->client_cursor_hotspot_y;
  }
  // Fall back to xcursor
  else if (instance->current_xcursor_name != NULL && instance->cursor_mgr != NULL) {
    struct wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(
        instance->cursor_mgr, instance->current_xcursor_name, 1);
    if (xcursor != NULL && xcursor->image_count > 0) {
      struct wlr_xcursor_image *image = xcursor->images[0];
      hotspot_x = image->hotspot_x;
      hotspot_y = image->hotspot_y;

      // Create or reuse cached texture for this xcursor
      if (instance->xcursor_texture == NULL ||
          instance->xcursor_texture_name == NULL ||
          strcmp(instance->xcursor_texture_name, instance->current_xcursor_name) != 0) {
        // Destroy old texture
        if (instance->xcursor_texture != NULL) {
          wlr_texture_destroy(instance->xcursor_texture);
          instance->xcursor_texture = NULL;
        }
        free(instance->xcursor_texture_name);
        instance->xcursor_texture_name = NULL;

        // Create texture from xcursor image
        instance->xcursor_texture = wlr_texture_from_pixels(instance->renderer,
            DRM_FORMAT_ARGB8888, image->width * 4,
            image->width, image->height, image->buffer);

        if (instance->xcursor_texture != NULL) {
          instance->xcursor_texture_name = strdup(instance->current_xcursor_name);
        }
      }
      cursor_texture = instance->xcursor_texture;
    }
  }

  if (cursor_texture == NULL) {
    return;
  }

  // Apply viewport offset for multi-monitor: cursor position is in global layout coords
  int cursor_x = (int)instance->cursor->x - hotspot_x - viewport->x;
  int cursor_y = (int)instance->cursor->y - hotspot_y - viewport->y;

  struct wlr_box cursor_box = {
    .x = cursor_x,
    .y = cursor_y,
    .width = cursor_texture->width,
    .height = cursor_texture->height,
  };

  wlr_render_pass_add_texture(render_pass, &(struct wlr_render_texture_options){
    .texture = cursor_texture,
    .dst_box = cursor_box,
  });
}

static void output_frame(struct wl_listener *listener, void *data) {
  struct fwr_output *output = wl_container_of(listener, output, frame);
  struct fwr_instance *instance = output->instance;
  struct wlr_output *wlr_output = output->wlr_output;

  // Handle vsync baton for Flutter frame pacing - only for vsync_output
  if (output == instance->vsync_output) {
    intptr_t baton = atomic_exchange(&instance->vsync_baton, 0);
    if (baton != 0) {
      uint64_t current_time = instance->fl_proc_table.GetCurrentTime();

      // Use this output's refresh rate for frame timing
      uint64_t refresh_ns = 16600000;  // Default 60Hz
      if (wlr_output->current_mode != NULL && wlr_output->current_mode->refresh > 0) {
        refresh_ns = 1000000000000ULL / wlr_output->current_mode->refresh;
      }

      // Apply rate limit if set (for power saving)
      if (instance->vsync_rate_limit > 0) {
        uint64_t limited_ns = 1000000000ULL / instance->vsync_rate_limit;
        if (limited_ns > refresh_ns) {
          refresh_ns = limited_ns;
        }
      }

      instance->fl_proc_table.OnVsync(
        instance->engine,
        baton,
        current_time,
        current_time + refresh_ns
      );
      wlr_log(WLR_DEBUG, "Returning baton (output: %s)", wlr_output->name);
    }
  }

  // Begin render pass - Flutter-centric compositing
  // Flutter renders everything: decorations + Wayland surfaces via platform views
  struct wlr_output_state output_state;
  wlr_output_state_init(&output_state);

  struct wlr_render_pass *render_pass = wlr_output_begin_render_pass(
      wlr_output, &output_state, NULL, NULL);
  
  if (render_pass == NULL) {
    wlr_output_state_finish(&output_state);
    goto out;
  }

  // Clear to dark background (Flutter will draw over this)
  wlr_render_pass_add_rect(render_pass, &(struct wlr_render_rect_options){
    .box = { .x = 0, .y = 0, .width = wlr_output->width, .height = wlr_output->height },
    .color = { 0.1f, 0.1f, 0.1f, 1.0f },
  });

  // Update scene node positions for input hit-testing
  // (Scene nodes are still used for hit-testing even though we render directly)
  fwr_renderer_update_scene_positions(instance);

  // Get this output's position in the layout for viewport offset
  struct wlr_box output_box;
  wlr_output_layout_get_box(instance->output_layout, wlr_output, &output_box);
  struct fwr_output_viewport viewport = {
    .x = output_box.x,
    .y = output_box.y,
    .width = wlr_output->width,
    .height = wlr_output->height,
  };

  // Render the Flutter scene directly (GPU textures + platform views)
  // This is the Flutter-centric path: NO CPU READBACK
  // Each output renders its portion of the unified coordinate space
  fwr_renderer_render_scene(instance, render_pass, &viewport);

  // Render software cursor on top of everything
  render_cursor(instance, render_pass, &viewport);

  // Submit render pass
  if (!wlr_render_pass_submit(render_pass)) {
    wlr_output_state_finish(&output_state);
    goto out;
  }

  // Commit the output
  wlr_output_commit_state(wlr_output, &output_state);
  wlr_output_state_finish(&output_state);

  // Send frame done to Wayland surfaces
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  
  // Notify surfaces that the frame is done
  struct fwr_view *view;
  wl_list_for_each(view, &instance->views_list, link) {
    if (view->xdg_surface != NULL && view->xdg_surface->surface != NULL) {
      wlr_surface_send_frame_done(view->xdg_surface->surface, &now);
    }
  }

  // Send frame done to popup surfaces too
  struct handle_map_iter *iter = handle_map_iter_new(instance->popups);
  uint32_t handle;
  void *value;
  while (handle_map_iter_next(iter, &handle, &value)) {
    struct fwr_popup *popup = (struct fwr_popup *)value;
    if (popup != NULL && popup->xdg_surface != NULL &&
        popup->xdg_surface->surface != NULL &&
        popup->xdg_surface->surface->mapped) {
      wlr_surface_send_frame_done(popup->xdg_surface->surface, &now);
    }
  }
  handle_map_iter_destroy(iter);

out:
  wlr_output_schedule_frame(wlr_output);
}

static void output_request_state(struct wl_listener *listener, void *data) {
  struct fwr_output *output = wl_container_of(listener, output, request_state);
  struct wlr_output_event_request_state *event = data;

  wlr_output_commit_state(output->wlr_output, event->state);

  // Update Flutter window metrics with TOTAL layout bounds, not just this output
  // Using single-output dimensions here was causing severe rendering bugs in multi-monitor setups
  struct fwr_instance *instance = output->instance;
  if (instance->engine != NULL) {
    struct wlr_box total_box = {0};
    wlr_output_layout_get_box(instance->output_layout, NULL, &total_box);

    if (total_box.width > 0 && total_box.height > 0) {
      FlutterWindowMetricsEvent window_metrics = {};
      window_metrics.struct_size = sizeof(FlutterWindowMetricsEvent);
      window_metrics.width = total_box.width;
      window_metrics.height = total_box.height;
      window_metrics.pixel_ratio = 1.0;  // Use 1.0 for multi-output; per-output scaling is handled separately
      wlr_log(WLR_INFO, "Output %s state changed, updated Flutter metrics: %dx%d",
              output->wlr_output->name, total_box.width, total_box.height);
      FlutterEngineSendWindowMetricsEvent(instance->engine, &window_metrics);
    }
  }
}

static void output_present(struct wl_listener *listener, void *data) {
  struct fwr_output *output = wl_container_of(listener, output, present);
  struct fwr_instance *instance = output->instance;
  struct wlr_output_event_present *event = data;

  // Only handle vsync baton for the designated vsync output
  if (output != instance->vsync_output) {
    return;
  }

  intptr_t baton = atomic_exchange(&instance->vsync_baton, 0);
  if (baton != 0) {
    uint64_t current_time = instance->fl_proc_table.GetCurrentTime();

    uint64_t frame_target_ns = event->refresh;
    if (frame_target_ns == 0) {
      frame_target_ns = 16600000;
    }

    // Apply rate limit if set (for power saving)
    if (instance->vsync_rate_limit > 0) {
      uint64_t limited_ns = 1000000000ULL / instance->vsync_rate_limit;
      if (limited_ns > frame_target_ns) {
        frame_target_ns = limited_ns;
      }
    }

    instance->fl_proc_table.OnVsync(instance->engine, baton, current_time, current_time + frame_target_ns);
  }
}

void fwr_engine_vsync_callback(void *data, intptr_t baton) {
  struct fwr_instance *instance = data;
  atomic_store(&instance->vsync_baton, baton);
}

// Handle output disconnect/hotplug removal
static void output_destroy(struct wl_listener *listener, void *data) {
  struct fwr_output *output = wl_container_of(listener, output, destroy);
  struct fwr_instance *instance = output->instance;
  uint32_t output_id = output->id;
  bool was_vsync_output = (output == instance->vsync_output);

  wlr_log(WLR_INFO, "Output %s (id=%d) disconnected", output->wlr_output->name, output_id);

  // Remove listeners
  wl_list_remove(&output->frame.link);
  wl_list_remove(&output->request_state.link);
  wl_list_remove(&output->present.link);
  wl_list_remove(&output->destroy.link);

  // Remove from linked list
  wl_list_remove(&output->link);

  // Destroy scene output (this also removes it from scene_output_layout)
  if (output->scene_output != NULL) {
    wlr_scene_output_destroy(output->scene_output);
    output->scene_output = NULL;
  }

  // Clear vsync_output if this was it
  if (was_vsync_output) {
    instance->vsync_output = NULL;
  }

  // Send platform channel message
  send_output_removed(instance, output_id);

  free(output);

  // If this was the vsync output, select a new one
  if (was_vsync_output) {
    fwr_select_highest_refresh_output(instance);
  }

  // Update Flutter window metrics to reflect remaining outputs
  // Calculate total bounds across all remaining outputs
  struct wlr_box total_box = {0};
  wlr_output_layout_get_box(instance->output_layout, NULL, &total_box);

  if (instance->engine != NULL && total_box.width > 0 && total_box.height > 0) {
    FlutterWindowMetricsEvent window_metrics = {};
    window_metrics.struct_size = sizeof(FlutterWindowMetricsEvent);
    window_metrics.width = total_box.width;
    window_metrics.height = total_box.height;
    window_metrics.pixel_ratio = 1.0;  // Use default, apps handle per-output scaling
    FlutterEngineSendWindowMetricsEvent(instance->engine, &window_metrics);
  }
}

void fwr_server_new_output(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance = wl_container_of(listener, instance, new_output);
  struct wlr_output *wlr_output = data;

  wlr_log(WLR_INFO, "New output detected: %s (%s %s)",
          wlr_output->name,
          wlr_output->make ? wlr_output->make : "unknown",
          wlr_output->model ? wlr_output->model : "unknown");

  wlr_output_init_render(wlr_output, instance->allocator, instance->renderer);

  struct fwr_output *output = calloc(1, sizeof(struct fwr_output));
  output->id = ++instance->next_output_id;  // Assign unique ID
  output->wlr_output = wlr_output;
  output->instance = instance;
  output->scene_output = wlr_scene_output_create(instance->scene, wlr_output);

  // Set up listeners
  output->frame.notify = output_frame;
  wl_signal_add(&wlr_output->events.frame, &output->frame);
  output->request_state.notify = output_request_state;
  wl_signal_add(&wlr_output->events.request_state, &output->request_state);
  output->present.notify = output_present;
  wl_signal_add(&wlr_output->events.present, &output->present);
  output->destroy.notify = output_destroy;
  wl_signal_add(&wlr_output->events.destroy, &output->destroy);

  // Add to linked list
  wl_list_insert(&instance->outputs, &output->link);

  // Configure output mode
  struct wlr_output_state state;
  wlr_output_state_init(&state);

  struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
  if (mode != NULL) {
    wlr_output_state_set_mode(&state, mode);
    wlr_log(WLR_INFO, "Output %s: setting mode %dx%d @ %d mHz",
            wlr_output->name, mode->width, mode->height, mode->refresh);
  }

  // Explicitly set scale to 1.0 to ensure clients don't get confused
  // on multi-monitor setups. User can change via output settings later.
  wlr_output_state_set_scale(&state, 1.0);
  wlr_log(WLR_INFO, "Output %s: explicitly setting scale to 1.0", wlr_output->name);

  wlr_output_state_set_enabled(&state, true);

  if (!wlr_output_commit_state(wlr_output, &state)) {
    wlr_log(WLR_ERROR, "Failed to commit output state for %s", wlr_output->name);
    wlr_output_state_finish(&state);
    // Clean up on failure
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->present.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
    return;
  }
  wlr_output_state_finish(&state);

  // Add to output layout
  output->layout_output = wlr_output_layout_add_auto(instance->output_layout, wlr_output);
  if (output->layout_output != NULL && output->scene_output != NULL && instance->scene_output_layout != NULL) {
    wlr_scene_output_layout_add_output(instance->scene_output_layout, output->layout_output, output->scene_output);
  }

  // Select vsync output (highest refresh rate)
  fwr_select_highest_refresh_output(instance);

  // Send platform channel message for output_added
  send_output_added(instance, output);

  // Update Flutter window metrics with total bounds
  struct wlr_box total_box = {0};
  wlr_output_layout_get_box(instance->output_layout, NULL, &total_box);

  if (instance->engine != NULL) {
    FlutterWindowMetricsEvent window_metrics = {};
    window_metrics.struct_size = sizeof(FlutterWindowMetricsEvent);
    window_metrics.width = total_box.width;
    window_metrics.height = total_box.height;
    window_metrics.pixel_ratio = 1.0;  // Use default, apps handle per-output scaling
    FlutterEngineSendWindowMetricsEvent(instance->engine, &window_metrics);
    wlr_log(WLR_INFO, "Updated Flutter window metrics: %dx%d", total_box.width, total_box.height);
  }

  wlr_output_schedule_frame(wlr_output);
}
