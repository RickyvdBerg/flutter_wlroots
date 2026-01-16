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

// Cached xcursor texture for software rendering
static struct wlr_texture *xcursor_texture = NULL;
static char *xcursor_texture_name = NULL;

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
static void render_cursor(struct fwr_instance *instance, struct wlr_render_pass *render_pass) {
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
      if (xcursor_texture == NULL ||
          xcursor_texture_name == NULL ||
          strcmp(xcursor_texture_name, instance->current_xcursor_name) != 0) {
        // Destroy old texture
        if (xcursor_texture != NULL) {
          wlr_texture_destroy(xcursor_texture);
          xcursor_texture = NULL;
        }
        free(xcursor_texture_name);
        xcursor_texture_name = NULL;

        // Create texture from xcursor image
        xcursor_texture = wlr_texture_from_pixels(instance->renderer,
            DRM_FORMAT_ARGB8888, image->width * 4,
            image->width, image->height, image->buffer);

        if (xcursor_texture != NULL) {
          xcursor_texture_name = strdup(instance->current_xcursor_name);
        }
      }
      cursor_texture = xcursor_texture;
    }
  }

  if (cursor_texture == NULL) {
    return;
  }

  int cursor_x = (int)instance->cursor->x - hotspot_x;
  int cursor_y = (int)instance->cursor->y - hotspot_y;

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

  // Handle vsync baton for Flutter frame pacing
  intptr_t baton = atomic_exchange(&instance->vsync_baton, 0);
  if (baton != 0) {
    uint64_t current_time = instance->fl_proc_table.GetCurrentTime();
    instance->fl_proc_table.OnVsync(
      instance->engine,
      baton,
      current_time,
      current_time + 16600000
    );
    wlr_log(WLR_DEBUG, "Returning baton");
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

  // Render the Flutter scene directly (GPU textures + platform views)
  // This is the Flutter-centric path: NO CPU READBACK
  fwr_renderer_render_scene(instance, render_pass);

  // Render software cursor on top of everything
  render_cursor(instance, render_pass);

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

  if (output->instance->engine != NULL) {
    FlutterWindowMetricsEvent window_metrics = {};
    window_metrics.struct_size = sizeof(FlutterWindowMetricsEvent);
    window_metrics.width = output->wlr_output->width;
    window_metrics.height = output->wlr_output->height;
    window_metrics.pixel_ratio = output->wlr_output->scale;
    FlutterEngineSendWindowMetricsEvent(output->instance->engine, &window_metrics);
  }
}

static void output_present(struct wl_listener *listener, void *data) {
  struct fwr_output *output = wl_container_of(listener, output, present);
  struct fwr_instance *instance = output->instance;
  struct wlr_output_event_present *event = data;

  intptr_t baton = atomic_exchange(&instance->vsync_baton, 0);
  if (baton != 0) {
    uint64_t current_time = instance->fl_proc_table.GetCurrentTime();
    
    uint64_t frame_target_ns = event->refresh;
    if (frame_target_ns == 0) {
      frame_target_ns = 16600000;
    }

    instance->fl_proc_table.OnVsync(instance->engine, baton, current_time, current_time + frame_target_ns);
  }
}

void fwr_engine_vsync_callback(void *data, intptr_t baton) {
  struct fwr_instance *instance = data;
  atomic_store(&instance->vsync_baton, baton);
}

void fwr_server_new_output(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance = wl_container_of(listener, instance, new_output);
  struct wlr_output *wlr_output = data;

  if (instance->output != NULL) {
    return;
  }

  wlr_output_init_render(wlr_output, instance->allocator, instance->renderer);

	struct fwr_output *output = calloc(1, sizeof(struct fwr_output));
	output->wlr_output = wlr_output;
	output->instance = instance;
	output->scene_output = wlr_scene_output_create(instance->scene, wlr_output);

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
  output->request_state.notify = output_request_state;
  wl_signal_add(&wlr_output->events.request_state, &output->request_state);
  output->present.notify = output_present;
  wl_signal_add(&wlr_output->events.present, &output->present);
  
  instance->output = output;

  struct wlr_output_state state;
  wlr_output_state_init(&state);

  struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
  if (mode != NULL) {
    wlr_output_state_set_mode(&state, mode);
  }

  wlr_output_state_set_enabled(&state, true);

  if (!wlr_output_commit_state(wlr_output, &state)) {
    wlr_output_state_finish(&state);
    return;
  }
  wlr_output_state_finish(&state);

  wlr_log(WLR_INFO, "Setting mode when creating new output!");

  if (output->instance->engine != NULL) {
    FlutterWindowMetricsEvent window_metrics = {};
    window_metrics.struct_size = sizeof(FlutterWindowMetricsEvent);
    window_metrics.width = wlr_output->width;
    window_metrics.height = wlr_output->height;
    window_metrics.pixel_ratio = wlr_output->scale;
    FlutterEngineSendWindowMetricsEvent(instance->engine, &window_metrics);
  }

  output->layout_output = wlr_output_layout_add_auto(instance->output_layout, wlr_output);
  if (output->layout_output != NULL && output->scene_output != NULL && instance->scene_output_layout != NULL) {
    wlr_scene_output_layout_add_output(instance->scene_output_layout, output->layout_output, output->scene_output);
  }

  wlr_output_schedule_frame(wlr_output);
}
