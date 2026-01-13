#include <assert.h>
#include <stdlib.h>

#include <wayland-server-core.h>
#include <wayland-util.h>

#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>

#include "surface.h"
#include "messages.h"

static void cb(const uint8_t *data, size_t size, void *user_data) {
  wlr_log(WLR_INFO, "callback");
}

static void send_surface_title(struct fwr_view *view) {
  struct fwr_instance *instance = view->instance;
  const char *title = view->toplevel->title;
  const char *app_id = view->toplevel->app_id;

  struct message_builder msg = message_builder_new();
  struct message_builder_segment msg_seg = message_builder_segment(&msg);
  message_builder_segment_push_string(&msg_seg, "surface_title");
  message_builder_segment_finish(&msg_seg);

  msg_seg = message_builder_segment(&msg);
  struct message_builder_segment arg_seg =
      message_builder_segment_push_map(&msg_seg, 3);
  message_builder_segment_push_string(&arg_seg, "handle");
  message_builder_segment_push_int64(&arg_seg, view->handle);
  message_builder_segment_push_string(&arg_seg, "title");
  message_builder_segment_push_string(&arg_seg, title == NULL ? "" : title);
  message_builder_segment_push_string(&arg_seg, "app_id");
  message_builder_segment_push_string(&arg_seg, app_id == NULL ? "" : app_id);
  message_builder_segment_finish(&arg_seg);

  message_builder_segment_finish(&msg_seg);
  uint8_t *msg_buf;
  size_t msg_buf_len;
  message_builder_finish(&msg, &msg_buf, &msg_buf_len);

  FlutterPlatformMessageResponseHandle *response_handle;
  instance->fl_proc_table.PlatformMessageCreateResponseHandle(
      instance->engine, cb, NULL, &response_handle);

  FlutterPlatformMessage platform_message = {};
  platform_message.struct_size = sizeof(FlutterPlatformMessage);
  platform_message.channel = "wlroots";
  platform_message.message = msg_buf;
  platform_message.message_size = msg_buf_len;
  platform_message.response_handle = response_handle;
  instance->fl_proc_table.SendPlatformMessage(instance->engine,
                                              &platform_message);

  free(msg_buf);

  instance->fl_proc_table.PlatformMessageReleaseResponseHandle(instance->engine,
                                                               response_handle);
}

void fwr_focus_view(struct fwr_view *view) {
	if (view == NULL) {
		return;
	}
	struct fwr_instance *instance = view->instance;
	struct wlr_seat *seat = instance->seat;
	struct wlr_surface *surface = view->xdg_surface->surface;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	if (prev_surface == surface) {
		return;
	}
	if (prev_surface) {
		struct wlr_xdg_surface *previous = wlr_xdg_surface_try_from_wlr_surface(
					seat->keyboard_state.focused_surface);
		if (previous && previous->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
			wlr_xdg_toplevel_set_activated(previous->toplevel, false);
			struct fwr_view *prev_view = previous->data;
			if (prev_view != NULL) {
				prev_view->activated = false;
			}
		}
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		wlr_xdg_toplevel_set_activated(view->toplevel, true);
		view->activated = true;
	}
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	} else {
		struct wlr_keyboard_modifiers modifiers = {0};
		wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, &modifiers);
	}
	instance->current_focused_view = view->handle;
}

static void view_update_scene(struct fwr_view *view) {
  if (view->scene_tree == NULL || view->scene_xdg_tree == NULL) {
    return;
  }

  wlr_scene_node_set_position(&view->scene_xdg_tree->node, 0, 0);
}

static void view_create_scene(struct fwr_view *view) {
  if (view->scene_tree != NULL) {
    return;
  }

  struct fwr_instance *instance = view->instance;
  if (instance->scene == NULL) {
    return;
  }

  view->scene_tree = wlr_scene_tree_create(&instance->scene->tree);
  wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);

  view->scene_xdg_tree = wlr_scene_xdg_surface_create(view->scene_tree, view->xdg_surface);

  view_update_scene(view);
}

static void view_destroy_scene(struct fwr_view *view) {
  if (view->scene_tree != NULL) {
    wlr_scene_node_destroy(&view->scene_tree->node);
  }

  view->scene_tree = NULL;
  view->scene_xdg_tree = NULL;
  view->scene_frame = NULL;
  view->scene_titlebar = NULL;
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
  struct fwr_view *view = wl_container_of(listener, view, map);
  struct fwr_instance *instance = view->instance;

  view_create_scene(view);
  if (view->scene_tree != NULL) {
    wlr_scene_node_set_enabled(&view->scene_tree->node, true);
  }

  struct wlr_box geo;
  wlr_xdg_surface_get_geometry(view->xdg_surface, &geo);
  view->width = geo.width;
  view->height = geo.height;

  view->texture_id = (int64_t)view->handle;
  FlutterEngineResult result = instance->fl_proc_table.RegisterExternalTexture(
      instance->engine, view->texture_id);
  if (result == kSuccess) {
    view->texture_registered = true;
    wlr_log(WLR_INFO, "Registered external texture %ld for view %d", view->texture_id, view->handle);
  } else {
    wlr_log(WLR_ERROR, "Failed to register external texture for view %d", view->handle);
    view->texture_registered = false;
  }

  int32_t pid;
  uint32_t uid, gid;
  wl_client_get_credentials(view->xdg_surface->client->client, &pid, &uid, &gid);

  struct message_builder msg = message_builder_new();
  struct message_builder_segment msg_seg = message_builder_segment(&msg);
  message_builder_segment_push_string(&msg_seg, "surface_map");
  message_builder_segment_finish(&msg_seg);

  msg_seg = message_builder_segment(&msg);
  struct message_builder_segment arg_seg =
      message_builder_segment_push_map(&msg_seg, 13);
  message_builder_segment_push_string(&arg_seg, "handle");
  wlr_log(WLR_INFO, "viewhandle %d", view->handle);
  message_builder_segment_push_int64(&arg_seg, view->handle);
  message_builder_segment_push_string(&arg_seg, "texture_id");
  message_builder_segment_push_int64(&arg_seg, view->texture_id);
  message_builder_segment_push_string(&arg_seg, "x");
  message_builder_segment_push_int64(&arg_seg, view->x);
  message_builder_segment_push_string(&arg_seg, "y");
  message_builder_segment_push_int64(&arg_seg, view->y);
  message_builder_segment_push_string(&arg_seg, "width");
  message_builder_segment_push_int64(&arg_seg, view->width);
  message_builder_segment_push_string(&arg_seg, "height");
  message_builder_segment_push_int64(&arg_seg, view->height);
  message_builder_segment_push_string(&arg_seg, "client_pid");
  message_builder_segment_push_int64(&arg_seg, pid);
  message_builder_segment_push_string(&arg_seg, "client_uid");
  message_builder_segment_push_int64(&arg_seg, uid);
  message_builder_segment_push_string(&arg_seg, "client_gid");
  message_builder_segment_push_int64(&arg_seg, gid);
  message_builder_segment_push_string(&arg_seg, "title");
  message_builder_segment_push_string(
      &arg_seg,
      view->toplevel->title == NULL ? "" : view->toplevel->title);
  message_builder_segment_push_string(&arg_seg, "app_id");
  message_builder_segment_push_string(
      &arg_seg,
      view->toplevel->app_id == NULL ? "" : view->toplevel->app_id);
  message_builder_segment_push_string(&arg_seg, "maximized");
  message_builder_segment_push_int64(&arg_seg, view->maximized ? 1 : 0);
  message_builder_segment_push_string(&arg_seg, "activated");
  message_builder_segment_push_int64(&arg_seg, view->activated ? 1 : 0);
  message_builder_segment_finish(&arg_seg);

  message_builder_segment_finish(&msg_seg);
  uint8_t *msg_buf;
  size_t msg_buf_len;
  message_builder_finish(&msg, &msg_buf, &msg_buf_len);

  FlutterPlatformMessageResponseHandle *response_handle;
  instance->fl_proc_table.PlatformMessageCreateResponseHandle(
      instance->engine, cb, NULL, &response_handle);

  FlutterPlatformMessage platform_message = {};
  platform_message.struct_size = sizeof(FlutterPlatformMessage);
  platform_message.channel = "wlroots";
  platform_message.message = msg_buf;
  platform_message.message_size = msg_buf_len;
  platform_message.response_handle = response_handle;
  instance->fl_proc_table.SendPlatformMessage(instance->engine,
                                              &platform_message);

  free(msg_buf);

  instance->fl_proc_table.PlatformMessageReleaseResponseHandle(instance->engine,
                                                               response_handle);

  fwr_focus_view(view);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
  struct fwr_view *view = wl_container_of(listener, view, unmap);
  struct fwr_instance *instance = view->instance;

  if (view->scene_tree != NULL) {
    wlr_scene_node_set_enabled(&view->scene_tree->node, false);
  }

  struct message_builder msg = message_builder_new();
  struct message_builder_segment msg_seg = message_builder_segment(&msg);
  message_builder_segment_push_string(&msg_seg, "surface_unmap");
  message_builder_segment_finish(&msg_seg);

  msg_seg = message_builder_segment(&msg);
  struct message_builder_segment arg_seg =
      message_builder_segment_push_map(&msg_seg, 1);
  message_builder_segment_push_string(&arg_seg, "handle");
  message_builder_segment_push_int64(&arg_seg, view->handle);
  message_builder_segment_finish(&arg_seg);

  message_builder_segment_finish(&msg_seg);
  uint8_t *msg_buf;
  size_t msg_buf_len;
  message_builder_finish(&msg, &msg_buf, &msg_buf_len);

  FlutterPlatformMessageResponseHandle *response_handle;
  instance->fl_proc_table.PlatformMessageCreateResponseHandle(
      instance->engine, cb, NULL, &response_handle);

  FlutterPlatformMessage platform_message = {};
  platform_message.struct_size = sizeof(FlutterPlatformMessage);
  platform_message.channel = "wlroots";
  platform_message.message = msg_buf;
  platform_message.message_size = msg_buf_len;
  platform_message.response_handle = response_handle;
  instance->fl_proc_table.SendPlatformMessage(instance->engine,
                                              &platform_message);

  free(msg_buf);
}

static void xdg_toplevel_set_title(struct wl_listener *listener, void *data) {
  struct fwr_view *view = wl_container_of(listener, view, set_title);
  send_surface_title(view);
}

static void xdg_toplevel_set_app_id(struct wl_listener *listener, void *data) {
  struct fwr_view *view = wl_container_of(listener, view, set_app_id);
  send_surface_title(view);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
  struct fwr_view *view = wl_container_of(listener, view, commit);
  struct fwr_instance *instance = view->instance;
  
  struct wlr_box geo;
  wlr_xdg_surface_get_geometry(view->xdg_surface, &geo);
  view->width = geo.width;
  view->height = geo.height;
  
  view_update_scene(view);
  
  if (view->texture_registered) {
    instance->fl_proc_table.MarkExternalTextureFrameAvailable(
        instance->engine, view->texture_id);
  }
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
  struct fwr_view *view = wl_container_of(listener, view, destroy);
  struct fwr_instance *instance = view->instance;

  if (view->texture_registered) {
    instance->fl_proc_table.UnregisterExternalTexture(instance->engine, view->texture_id);
    view->texture_registered = false;
  }

  struct gl_fns *fns = &instance->fwr_renderer.fns;
  if (view->cached_tex != 0) {
    fns->glDeleteTextures(1, &view->cached_tex);
  }
  if (view->cached_fbo != 0) {
    fns->glDeleteFramebuffers(1, &view->cached_fbo);
  }

  view_destroy_scene(view);

  handle_map_remove(instance->views, view->handle);

  wl_list_remove(&view->map.link);
  wl_list_remove(&view->unmap.link);
  wl_list_remove(&view->destroy.link);
  wl_list_remove(&view->commit.link);
  wl_list_remove(&view->set_title.link);
  wl_list_remove(&view->set_app_id.link);
  wl_list_remove(&view->link);

  free(view);
}

void fwr_new_xdg_toplevel(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance =
      wl_container_of(listener, instance, new_xdg_toplevel);
  struct wlr_xdg_toplevel *toplevel = data;
  struct wlr_xdg_surface *xdg_surface = toplevel->base;

  if (xdg_surface == NULL) {
    return;
  }

  struct fwr_view *view = calloc(1, sizeof(struct fwr_view));

  view->instance = instance;
  view->xdg_surface = xdg_surface;
  view->toplevel = toplevel;

  wl_list_insert(&instance->views_list, &view->link);

  view->map.notify = xdg_toplevel_map;
  wl_signal_add(&xdg_surface->surface->events.map, &view->map);
  view->unmap.notify = xdg_toplevel_unmap;
  wl_signal_add(&xdg_surface->surface->events.unmap, &view->unmap);
  view->destroy.notify = xdg_toplevel_destroy;
  wl_signal_add(&toplevel->events.destroy, &view->destroy);
  view->commit.notify = xdg_toplevel_commit;
  wl_signal_add(&xdg_surface->surface->events.commit, &view->commit);
  view->set_title.notify = xdg_toplevel_set_title;
  wl_signal_add(&toplevel->events.set_title, &view->set_title);
  view->set_app_id.notify = xdg_toplevel_set_app_id;
  wl_signal_add(&toplevel->events.set_app_id, &view->set_app_id);

  uint32_t view_handle = handle_map_add(instance->views, (void *)view);
  view->handle = view_handle;

  int offset = (int)(view_handle % 10) * 32;
  view->x = 50 + offset;
  view->y = 50 + offset;
  view->width = 0;
  view->height = 0;
  view->maximized = false;
  view->fullscreen = false;
  view->activated = false;

  xdg_surface->data = view;
}

void fwr_handle_surface_toplevel_set_size(
    struct fwr_instance *instance,
    const FlutterPlatformMessageResponseHandle *handle,
    struct dart_value *args) {
  struct surface_toplevel_set_size_message message;
  if (!decode_surface_toplevel_set_size_message(args, &message)) {
    goto error;
  }

  struct fwr_view *view;
  if (!handle_map_get(instance->views, message.surface_handle, (void**) &view)) {
    // This implies a race condition of surface removal.
    // We return success here.
    goto success;
  }

  if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
    wlr_xdg_toplevel_set_size(view->toplevel, message.size_x, message.size_y);
  }

success:
  instance->fl_proc_table.SendPlatformMessageResponse(instance->engine, handle, method_call_null_success, sizeof(method_call_null_success));
  return;

error:
  wlr_log(WLR_ERROR, "Invalid toplevel set size message");
  // Send failure
  instance->fl_proc_table.SendPlatformMessageResponse(instance->engine, handle, NULL, 0);
}

void fwr_handle_surface_toplevel_set_maximized(
    struct fwr_instance *instance,
    const FlutterPlatformMessageResponseHandle *handle,
    struct dart_value *args) {
  struct surface_toplevel_set_maximized_message message;
  if (!decode_surface_toplevel_set_maximized_message(args, &message)) {
    goto error;
  }

  struct fwr_view *view;
  if (!handle_map_get(instance->views, message.surface_handle, (void**)&view)) {
    goto success;
  }

  if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
    view->maximized = (message.maximized != 0);
    wlr_xdg_toplevel_set_maximized(view->toplevel, message.maximized != 0);
  }

success:
  instance->fl_proc_table.SendPlatformMessageResponse(instance->engine, handle, method_call_null_success, sizeof(method_call_null_success));
  return;

error:
  wlr_log(WLR_ERROR, "Invalid toplevel set maximized message");
  instance->fl_proc_table.SendPlatformMessageResponse(instance->engine, handle, NULL, 0);
}

void fwr_handle_surface_toplevel_close(
    struct fwr_instance *instance,
    const FlutterPlatformMessageResponseHandle *handle,
    struct dart_value *args) {
  struct surface_toplevel_close_message message;
  if (!decode_surface_toplevel_close_message(args, &message)) {
    goto error;
  }

  struct fwr_view *view;
  if (!handle_map_get(instance->views, message.surface_handle, (void**)&view)) {
    goto success;
  }

  if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
    wlr_xdg_toplevel_send_close(view->toplevel);
  }

success:
  instance->fl_proc_table.SendPlatformMessageResponse(instance->engine, handle, method_call_null_success, sizeof(method_call_null_success));
  return;

error:
  wlr_log(WLR_ERROR, "Invalid toplevel close message");
  instance->fl_proc_table.SendPlatformMessageResponse(instance->engine, handle, NULL, 0);
}

void fwr_handle_surface_focus(
    struct fwr_instance *instance,
    const FlutterPlatformMessageResponseHandle *handle,
    struct dart_value *args) {
  struct surface_toplevel_close_message message;
  if (!decode_surface_toplevel_close_message(args, &message)) {
    goto error;
  }

  struct fwr_view *view;
  if (!handle_map_get(instance->views, message.surface_handle, (void**)&view)) {
    goto success;
  }

  fwr_focus_view(view);
  if (view->scene_tree != NULL) {
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
  }

success:
  instance->fl_proc_table.SendPlatformMessageResponse(instance->engine, handle, method_call_null_success, sizeof(method_call_null_success));
  return;

error:
  wlr_log(WLR_ERROR, "Invalid surface focus message");
  instance->fl_proc_table.SendPlatformMessageResponse(instance->engine, handle, NULL, 0);
}

void fwr_handle_surface_set_position(
    struct fwr_instance *instance,
    const FlutterPlatformMessageResponseHandle *handle,
    struct dart_value *args) {
  struct surface_set_position_message message;
  if (!decode_surface_set_position_message(args, &message)) {
    goto error;
  }

  struct fwr_view *view;
  if (!handle_map_get(instance->views, message.surface_handle, (void**)&view)) {
    goto success;
  }

  // Update position (Dart is source of truth for window positioning)
  view->x = (int)message.x;
  view->y = (int)message.y;

  // Update scene tree position for input hit-testing
  if (view->scene_tree != NULL) {
    wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
  }

success:
  instance->fl_proc_table.SendPlatformMessageResponse(instance->engine, handle, method_call_null_success, sizeof(method_call_null_success));
  return;

error:
  wlr_log(WLR_ERROR, "Invalid surface set position message");
  instance->fl_proc_table.SendPlatformMessageResponse(instance->engine, handle, NULL, 0);
}
