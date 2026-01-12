#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include <wayland-server-core.h>
#include <wayland-util.h>
#include <instance.h>

#include <wlr/util/log.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/render/pass.h>

#include "renderer.h"

static void output_frame(struct wl_listener *listener, void *data) {
  struct fwr_output *output = wl_container_of(listener, output, frame);
  struct fwr_instance *instance = output->instance;
  struct wlr_output *wlr_output = output->wlr_output;

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

  struct wlr_scene_output *scene_output = output->scene_output;
  if (scene_output == NULL && instance->scene != NULL) {
    scene_output = wlr_scene_get_scene_output(instance->scene, wlr_output);
  }

  if (scene_output == NULL) {
    goto out;
  }

  fwr_renderer_update_scene_buffer(instance);

  if (!wlr_scene_output_commit(scene_output, NULL)) {
    goto out;
  }

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  wlr_scene_output_send_frame_done(scene_output, &now);

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
