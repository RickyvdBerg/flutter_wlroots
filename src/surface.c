#include <assert.h>
#include <stdlib.h>

#include <EGL/egl.h>

#include <wayland-server-core.h>
#include <wayland-util.h>

#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/util/log.h>
#include <wlr/render/gles2.h>

#include "surface.h"
#include "messages.h"

// Forward declarations for subsurface handling
static void handle_new_subsurface(struct wl_listener *listener, void *data);
static void subsurface_handle_map(struct wl_listener *listener, void *data);
static void subsurface_handle_unmap(struct wl_listener *listener, void *data);
static void subsurface_handle_destroy(struct wl_listener *listener, void *data);
static void subsurface_handle_commit(struct wl_listener *listener, void *data);

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

// Send decoration mode update to Flutter when uses_ssd changes
void fwr_send_decoration_update(struct fwr_view *view) {
  struct fwr_instance *instance = view->instance;
  bool uses_csd = !view->uses_ssd;

  wlr_log(WLR_INFO, "Sending decoration update: handle=%d, uses_ssd=%d, uses_csd=%d",
          view->handle, view->uses_ssd, uses_csd);

  struct message_builder msg = message_builder_new();
  struct message_builder_segment msg_seg = message_builder_segment(&msg);
  message_builder_segment_push_string(&msg_seg, "surface_decoration");
  message_builder_segment_finish(&msg_seg);

  msg_seg = message_builder_segment(&msg);
  struct message_builder_segment arg_seg =
      message_builder_segment_push_map(&msg_seg, 2);
  message_builder_segment_push_string(&arg_seg, "handle");
  message_builder_segment_push_int64(&arg_seg, view->handle);
  message_builder_segment_push_string(&arg_seg, "uses_csd");
  message_builder_segment_push_int64(&arg_seg, uses_csd ? 1 : 0);
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

  wlr_log(WLR_INFO, "XDG TOPLEVEL MAP: view=%d", view->handle);

  // Create scene graph for input hit-testing (rendering is still via Flutter textures)
  view_create_scene(view);
  if (view->scene_tree != NULL) {
    wlr_scene_node_set_enabled(&view->scene_tree->node, true);
  }

  struct wlr_box geo;
  wlr_xdg_surface_get_geometry(view->xdg_surface, &geo);
  view->width = geo.width;
  view->height = geo.height;
  view->geo_x = geo.x;
  view->geo_y = geo.y;

  wlr_log(WLR_INFO, "Geometry: buffer offset=(%d,%d), visible size=%dx%d",
          view->geo_x, view->geo_y, view->width, view->height);

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

  // App uses CSD if it didn't negotiate SSD via xdg-decoration protocol
  // Note: apps that don't support the protocol at all will have uses_ssd=false
  bool uses_csd = !view->uses_ssd;
  wlr_log(WLR_INFO, "Decoration mode: uses_ssd=%d, uses_csd=%d, geo_offset=(%d,%d)",
          view->uses_ssd, uses_csd, view->geo_x, view->geo_y);

  // Get actual buffer dimensions (includes shadow area for CSD apps)
  int buffer_width = view->xdg_surface->surface->current.width;
  int buffer_height = view->xdg_surface->surface->current.height;

  msg_seg = message_builder_segment(&msg);
  struct message_builder_segment arg_seg =
      message_builder_segment_push_map(&msg_seg, 18);
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
  message_builder_segment_push_string(&arg_seg, "buffer_width");
  message_builder_segment_push_int64(&arg_seg, buffer_width);
  message_builder_segment_push_string(&arg_seg, "buffer_height");
  message_builder_segment_push_int64(&arg_seg, buffer_height);
  message_builder_segment_push_string(&arg_seg, "geo_x");
  message_builder_segment_push_int64(&arg_seg, view->geo_x);
  message_builder_segment_push_string(&arg_seg, "geo_y");
  message_builder_segment_push_int64(&arg_seg, view->geo_y);
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
  message_builder_segment_push_string(&arg_seg, "uses_csd");
  message_builder_segment_push_int64(&arg_seg, uses_csd ? 1 : 0);
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

static void send_surface_geometry(struct fwr_view *view) {
  struct fwr_instance *instance = view->instance;
  struct wlr_surface *surface = view->xdg_surface->surface;

  struct message_builder msg = message_builder_new();
  struct message_builder_segment msg_seg = message_builder_segment(&msg);
  message_builder_segment_push_string(&msg_seg, "surface_geometry");
  message_builder_segment_finish(&msg_seg);

  msg_seg = message_builder_segment(&msg);
  struct message_builder_segment arg_seg =
      message_builder_segment_push_map(&msg_seg, 7);
  message_builder_segment_push_string(&arg_seg, "handle");
  message_builder_segment_push_int64(&arg_seg, view->handle);
  message_builder_segment_push_string(&arg_seg, "width");
  message_builder_segment_push_int64(&arg_seg, view->width);
  message_builder_segment_push_string(&arg_seg, "height");
  message_builder_segment_push_int64(&arg_seg, view->height);
  message_builder_segment_push_string(&arg_seg, "buffer_width");
  message_builder_segment_push_int64(&arg_seg, surface->current.width);
  message_builder_segment_push_string(&arg_seg, "buffer_height");
  message_builder_segment_push_int64(&arg_seg, surface->current.height);
  message_builder_segment_push_string(&arg_seg, "geo_x");
  message_builder_segment_push_int64(&arg_seg, view->geo_x);
  message_builder_segment_push_string(&arg_seg, "geo_y");
  message_builder_segment_push_int64(&arg_seg, view->geo_y);
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

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
  struct fwr_view *view = wl_container_of(listener, view, commit);
  struct fwr_instance *instance = view->instance;

  struct wlr_box geo;
  wlr_xdg_surface_get_geometry(view->xdg_surface, &geo);

  // Check if geometry changed
  bool geo_changed = (view->width != geo.width || view->height != geo.height ||
                      view->geo_x != geo.x || view->geo_y != geo.y);

  view->width = geo.width;
  view->height = geo.height;
  view->geo_x = geo.x;
  view->geo_y = geo.y;

  view_update_scene(view);

  if (view->texture_registered) {
    instance->fl_proc_table.MarkExternalTextureFrameAvailable(
        instance->engine, view->texture_id);
  }

  // Notify Dart of geometry changes so it can update clipping/sizing
  if (geo_changed) {
    send_surface_geometry(view);
  }
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
  struct fwr_view *view = wl_container_of(listener, view, destroy);
  struct fwr_instance *instance = view->instance;

  if (view->texture_registered) {
    instance->fl_proc_table.UnregisterExternalTexture(instance->engine, view->texture_id);
    view->texture_registered = false;
  }

  // Delete GL resources
  fwr_cached_texture_destroy(instance, &view->cache);

  view_destroy_scene(view);

  handle_map_remove(instance->views, view->handle);

  wl_list_remove(&view->map.link);
  wl_list_remove(&view->unmap.link);
  wl_list_remove(&view->destroy.link);
  wl_list_remove(&view->commit.link);
  wl_list_remove(&view->set_title.link);
  wl_list_remove(&view->set_app_id.link);
  wl_list_remove(&view->request_move.link);
  wl_list_remove(&view->request_resize.link);
  wl_list_remove(&view->new_subsurface.link);
  wl_list_remove(&view->link);

  free(view);
}

// Handle CSD app request to start interactive move
static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
  struct fwr_view *view = wl_container_of(listener, view, request_move);
  struct fwr_instance *instance = view->instance;

  wlr_log(WLR_INFO, "CSD app requested move for view %d at cursor (%.1f, %.1f)",
          view->handle, instance->input.flutter_cursor_x, instance->input.flutter_cursor_y);

  // Start the move grab operation - use Flutter cursor position for nested compositor
  instance->input.grab.type = FWR_GRAB_MOVE;
  instance->input.grab.view_handle = view->handle;
  instance->input.grab.start_cursor_x = instance->input.flutter_cursor_x;
  instance->input.grab.start_cursor_y = instance->input.flutter_cursor_y;
  instance->input.grab.start_view_x = view->x;
  instance->input.grab.start_view_y = view->y;

  fwr_focus_view(view);
  if (view->scene_tree != NULL) {
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
  }
}

// Handle CSD app request to start interactive resize
static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
  struct fwr_view *view = wl_container_of(listener, view, request_resize);
  struct wlr_xdg_toplevel_resize_event *event = data;
  struct fwr_instance *instance = view->instance;

  wlr_log(WLR_INFO, "CSD app requested resize for view %d, edges=%d at cursor (%.1f, %.1f)",
          view->handle, event->edges, instance->input.flutter_cursor_x, instance->input.flutter_cursor_y);

  // Start the resize grab operation - use Flutter cursor position for nested compositor
  instance->input.grab.type = FWR_GRAB_RESIZE;
  instance->input.grab.view_handle = view->handle;
  instance->input.grab.start_cursor_x = instance->input.flutter_cursor_x;
  instance->input.grab.start_cursor_y = instance->input.flutter_cursor_y;
  instance->input.grab.start_view_x = view->x;
  instance->input.grab.start_view_y = view->y;
  instance->input.grab.start_view_width = view->width;
  instance->input.grab.start_view_height = view->height;
  instance->input.grab.resize_edges = event->edges;

  fwr_focus_view(view);
  if (view->scene_tree != NULL) {
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
  }
}

// Send minimize request to Dart
static void send_surface_minimize(struct fwr_instance *instance, struct fwr_view *view) {
  struct message_builder msg = message_builder_new();
  struct message_builder_segment msg_seg = message_builder_segment(&msg);
  message_builder_segment_push_string(&msg_seg, "surface_minimize");
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

  instance->fl_proc_table.PlatformMessageReleaseResponseHandle(instance->engine,
                                                               response_handle);
}

// Handle CSD app request to minimize
static void xdg_toplevel_request_minimize(struct wl_listener *listener, void *data) {
  struct fwr_view *view = wl_container_of(listener, view, request_minimize);
  struct fwr_instance *instance = view->instance;

  wlr_log(WLR_INFO, "CSD app requested minimize for view %d", view->handle);
  send_surface_minimize(instance, view);
}

// Send maximize request to Dart
static void send_surface_maximize_request(struct fwr_instance *instance, struct fwr_view *view, bool maximized) {
  struct message_builder msg = message_builder_new();
  struct message_builder_segment msg_seg = message_builder_segment(&msg);
  message_builder_segment_push_string(&msg_seg, "surface_request_maximize");
  message_builder_segment_finish(&msg_seg);

  msg_seg = message_builder_segment(&msg);
  struct message_builder_segment arg_seg =
      message_builder_segment_push_map(&msg_seg, 2);
  message_builder_segment_push_string(&arg_seg, "handle");
  message_builder_segment_push_int64(&arg_seg, view->handle);
  message_builder_segment_push_string(&arg_seg, "maximized");
  message_builder_segment_push_int64(&arg_seg, maximized ? 1 : 0);
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

// Handle CSD app request to maximize
static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
  struct fwr_view *view = wl_container_of(listener, view, request_maximize);
  struct fwr_instance *instance = view->instance;

  // Toggle maximize state
  bool want_maximized = !view->maximized;
  wlr_log(WLR_INFO, "CSD app requested maximize for view %d, want_maximized=%d", view->handle, want_maximized);
  send_surface_maximize_request(instance, view, want_maximized);
}

void fwr_new_xdg_toplevel(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance =
      wl_container_of(listener, instance, new_xdg_toplevel);
  struct wlr_xdg_toplevel *toplevel = data;
  struct wlr_xdg_surface *xdg_surface = toplevel->base;

  wlr_log(WLR_INFO, "NEW XDG TOPLEVEL: %p, surface mapped=%d",
          toplevel, xdg_surface->surface->mapped);

  if (xdg_surface == NULL) {
    return;
  }

  struct fwr_view *view = calloc(1, sizeof(struct fwr_view));

  view->instance = instance;
  view->xdg_surface = xdg_surface;
  view->toplevel = toplevel;

  wl_list_insert(&instance->views_list, &view->link);

  // Initialize subsurface list
  wl_list_init(&view->subsurfaces);

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

  // Handle CSD app move/resize requests (when user drags their own titlebar)
  view->request_move.notify = xdg_toplevel_request_move;
  wl_signal_add(&toplevel->events.request_move, &view->request_move);
  view->request_resize.notify = xdg_toplevel_request_resize;
  wl_signal_add(&toplevel->events.request_resize, &view->request_resize);

  // Handle CSD app minimize/maximize requests
  view->request_minimize.notify = xdg_toplevel_request_minimize;
  wl_signal_add(&toplevel->events.request_minimize, &view->request_minimize);
  view->request_maximize.notify = xdg_toplevel_request_maximize;
  wl_signal_add(&toplevel->events.request_maximize, &view->request_maximize);

  // Listen for new subsurfaces
  view->new_subsurface.notify = handle_new_subsurface;
  wl_signal_add(&xdg_surface->surface->events.new_subsurface, &view->new_subsurface);

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

  // Decoration tracking - will be set by handle_new_toplevel_decoration if client supports xdg-decoration
  view->decoration = NULL;
  view->uses_ssd = false;

  xdg_surface->data = view;

  // Send initial configure event so client can commit and map
  // Size 0,0 means "choose your own size"
  wlr_xdg_toplevel_set_size(toplevel, 0, 0);
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
  // Note: position from Dart includes the decoration/titlebar
  view->x = (int)message.x;
  view->y = (int)message.y;

  // Update scene tree position for input hit-testing
  // For SSD windows, the surface content is offset by the titlebar height
  // Note: wlr_scene_xdg_surface handles geometry offset internally
  if (view->scene_tree != NULL) {
    int titlebar_offset = view->uses_ssd ? 38 : 0;
    wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y + titlebar_offset);
  }

success:
  instance->fl_proc_table.SendPlatformMessageResponse(instance->engine, handle, method_call_null_success, sizeof(method_call_null_success));
  return;

error:
  wlr_log(WLR_ERROR, "Invalid surface set position message");
  instance->fl_proc_table.SendPlatformMessageResponse(instance->engine, handle, NULL, 0);
}

// ============================================================================
// Subsurface Handling
// ============================================================================

static void send_subsurface_map(struct fwr_subsurface *sub) {
  struct fwr_instance *instance = sub->parent_view->instance;

  struct message_builder msg = message_builder_new();
  struct message_builder_segment msg_seg = message_builder_segment(&msg);
  message_builder_segment_push_string(&msg_seg, "subsurface_map");
  message_builder_segment_finish(&msg_seg);

  msg_seg = message_builder_segment(&msg);
  struct message_builder_segment arg_seg =
      message_builder_segment_push_map(&msg_seg, 7);
  message_builder_segment_push_string(&arg_seg, "handle");
  message_builder_segment_push_int64(&arg_seg, sub->handle);
  message_builder_segment_push_string(&arg_seg, "parent_handle");
  message_builder_segment_push_int64(&arg_seg, sub->parent_view->handle);
  message_builder_segment_push_string(&arg_seg, "texture_id");
  message_builder_segment_push_int64(&arg_seg, sub->texture_id);
  message_builder_segment_push_string(&arg_seg, "x");
  message_builder_segment_push_int64(&arg_seg, sub->x);
  message_builder_segment_push_string(&arg_seg, "y");
  message_builder_segment_push_int64(&arg_seg, sub->y);
  message_builder_segment_push_string(&arg_seg, "width");
  message_builder_segment_push_int64(&arg_seg, sub->width);
  message_builder_segment_push_string(&arg_seg, "height");
  message_builder_segment_push_int64(&arg_seg, sub->height);
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

static void send_subsurface_unmap(struct fwr_subsurface *sub) {
  struct fwr_instance *instance = sub->parent_view->instance;

  struct message_builder msg = message_builder_new();
  struct message_builder_segment msg_seg = message_builder_segment(&msg);
  message_builder_segment_push_string(&msg_seg, "subsurface_unmap");
  message_builder_segment_finish(&msg_seg);

  msg_seg = message_builder_segment(&msg);
  struct message_builder_segment arg_seg =
      message_builder_segment_push_map(&msg_seg, 2);
  message_builder_segment_push_string(&arg_seg, "handle");
  message_builder_segment_push_int64(&arg_seg, sub->handle);
  message_builder_segment_push_string(&arg_seg, "parent_handle");
  message_builder_segment_push_int64(&arg_seg, sub->parent_view->handle);
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

static void send_subsurface_position(struct fwr_subsurface *sub) {
  struct fwr_instance *instance = sub->parent_view->instance;

  struct message_builder msg = message_builder_new();
  struct message_builder_segment msg_seg = message_builder_segment(&msg);
  message_builder_segment_push_string(&msg_seg, "subsurface_position");
  message_builder_segment_finish(&msg_seg);

  msg_seg = message_builder_segment(&msg);
  struct message_builder_segment arg_seg =
      message_builder_segment_push_map(&msg_seg, 6);
  message_builder_segment_push_string(&arg_seg, "handle");
  message_builder_segment_push_int64(&arg_seg, sub->handle);
  message_builder_segment_push_string(&arg_seg, "parent_handle");
  message_builder_segment_push_int64(&arg_seg, sub->parent_view->handle);
  message_builder_segment_push_string(&arg_seg, "x");
  message_builder_segment_push_int64(&arg_seg, sub->x);
  message_builder_segment_push_string(&arg_seg, "y");
  message_builder_segment_push_int64(&arg_seg, sub->y);
  message_builder_segment_push_string(&arg_seg, "width");
  message_builder_segment_push_int64(&arg_seg, sub->width);
  message_builder_segment_push_string(&arg_seg, "height");
  message_builder_segment_push_int64(&arg_seg, sub->height);
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

static void subsurface_handle_map(struct wl_listener *listener, void *data) {
  struct fwr_subsurface *sub = wl_container_of(listener, sub, map);
  struct fwr_instance *instance = sub->parent_view->instance;

  wlr_log(WLR_INFO, "Subsurface MAP: handle=%d parent=%d",
          sub->handle, sub->parent_view->handle);

  // Get surface dimensions
  sub->width = sub->surface->current.width;
  sub->height = sub->surface->current.height;

  // Get position relative to parent
  sub->x = sub->wlr_subsurface->current.x;
  sub->y = sub->wlr_subsurface->current.y;

  // Register external texture
  // Offset subsurface texture IDs by 100000 to avoid collision with view texture IDs
  sub->texture_id = (int64_t)(100000 + sub->handle);
  FlutterEngineResult result = instance->fl_proc_table.RegisterExternalTexture(
      instance->engine, sub->texture_id);
  if (result == kSuccess) {
    sub->texture_registered = true;
    wlr_log(WLR_INFO, "Registered subsurface texture %ld (handle=%d)", sub->texture_id, sub->handle);
  } else {
    wlr_log(WLR_ERROR, "Failed to register subsurface texture");
    sub->texture_registered = false;
  }

  // Send to Dart
  send_subsurface_map(sub);
}

static void subsurface_handle_unmap(struct wl_listener *listener, void *data) {
  struct fwr_subsurface *sub = wl_container_of(listener, sub, unmap);

  wlr_log(WLR_INFO, "Subsurface UNMAP: handle=%d", sub->handle);

  // Send to Dart
  send_subsurface_unmap(sub);
}

static void subsurface_handle_commit(struct wl_listener *listener, void *data) {
  struct fwr_subsurface *sub = wl_container_of(listener, sub, commit);
  struct fwr_instance *instance = sub->parent_view->instance;

  // Update dimensions and position
  int old_x = sub->x;
  int old_y = sub->y;
  int old_w = sub->width;
  int old_h = sub->height;

  sub->width = sub->surface->current.width;
  sub->height = sub->surface->current.height;
  sub->x = sub->wlr_subsurface->current.x;
  sub->y = sub->wlr_subsurface->current.y;

  // Notify Flutter that texture has new frame
  if (sub->texture_registered) {
    instance->fl_proc_table.MarkExternalTextureFrameAvailable(
        instance->engine, sub->texture_id);
  }

  // If position/size changed, notify Dart
  if (old_x != sub->x || old_y != sub->y || old_w != sub->width || old_h != sub->height) {
    send_subsurface_position(sub);
  }
}

static void subsurface_handle_destroy(struct wl_listener *listener, void *data) {
  struct fwr_subsurface *sub = wl_container_of(listener, sub, destroy);
  struct fwr_instance *instance = sub->parent_view->instance;

  wlr_log(WLR_INFO, "Subsurface DESTROY: handle=%d", sub->handle);

  // Unregister texture
  if (sub->texture_registered) {
    instance->fl_proc_table.UnregisterExternalTexture(instance->engine, sub->texture_id);
    sub->texture_registered = false;
  }

  // Clean up cached GL resources
  fwr_cached_texture_destroy(instance, &sub->cache);

  // Remove from handle map
  handle_map_remove(instance->subsurfaces, sub->handle);

  // Remove listeners
  wl_list_remove(&sub->map.link);
  wl_list_remove(&sub->unmap.link);
  wl_list_remove(&sub->destroy.link);
  wl_list_remove(&sub->commit.link);
  wl_list_remove(&sub->link);

  free(sub);
}

static void handle_new_subsurface(struct wl_listener *listener, void *data) {
  struct fwr_view *view = wl_container_of(listener, view, new_subsurface);
  struct wlr_subsurface *wlr_subsurface = data;
  struct fwr_instance *instance = view->instance;

  wlr_log(WLR_INFO, "New subsurface for view %d", view->handle);

  struct fwr_subsurface *sub = calloc(1, sizeof(struct fwr_subsurface));
  sub->parent_view = view;
  sub->wlr_subsurface = wlr_subsurface;
  sub->surface = wlr_subsurface->surface;

  // Add to handle map for texture lookup
  sub->handle = handle_map_add(instance->subsurfaces, (void *)sub);

  // Add to parent's subsurface list
  wl_list_insert(&view->subsurfaces, &sub->link);

  // Setup listeners
  sub->map.notify = subsurface_handle_map;
  wl_signal_add(&wlr_subsurface->surface->events.map, &sub->map);

  sub->unmap.notify = subsurface_handle_unmap;
  wl_signal_add(&wlr_subsurface->surface->events.unmap, &sub->unmap);

  sub->destroy.notify = subsurface_handle_destroy;
  wl_signal_add(&wlr_subsurface->events.destroy, &sub->destroy);

  sub->commit.notify = subsurface_handle_commit;
  wl_signal_add(&wlr_subsurface->surface->events.commit, &sub->commit);
}

// ============================================================================
// Popup handling (menus, dropdowns, tooltips)
// ============================================================================

static void send_popup_map(struct fwr_popup *popup) {
  struct fwr_instance *instance = popup->instance;

  // Get popup geometry - prefer current over scheduled
  struct wlr_box geo = popup->xdg_popup->current.geometry;
  if (geo.width == 0 || geo.height == 0) {
    geo = popup->xdg_popup->scheduled.geometry;
  }

  // Use surface dimensions if geometry dimensions are 0
  int width = geo.width > 0 ? geo.width : popup->xdg_surface->surface->current.width;
  int height = geo.height > 0 ? geo.height : popup->xdg_surface->surface->current.height;

  popup->x = geo.x;
  popup->y = geo.y;
  popup->width = width;
  popup->height = height;

  wlr_log(WLR_INFO, "Popup map: handle=%d, parent=%d, pos=(%d,%d), size=%dx%d, surface=%dx%d",
          popup->handle, popup->parent_view_handle, popup->x, popup->y,
          popup->width, popup->height,
          popup->xdg_surface->surface->current.width,
          popup->xdg_surface->surface->current.height);

  struct message_builder msg = message_builder_new();
  struct message_builder_segment msg_seg = message_builder_segment(&msg);
  message_builder_segment_push_string(&msg_seg, "popup_map");
  message_builder_segment_finish(&msg_seg);

  msg_seg = message_builder_segment(&msg);
  struct message_builder_segment arg_seg =
      message_builder_segment_push_map(&msg_seg, 7);
  message_builder_segment_push_string(&arg_seg, "handle");
  message_builder_segment_push_int64(&arg_seg, popup->handle);
  message_builder_segment_push_string(&arg_seg, "parent_handle");
  message_builder_segment_push_int64(&arg_seg, popup->parent_view_handle);
  message_builder_segment_push_string(&arg_seg, "x");
  message_builder_segment_push_int64(&arg_seg, popup->x);
  message_builder_segment_push_string(&arg_seg, "y");
  message_builder_segment_push_int64(&arg_seg, popup->y);
  message_builder_segment_push_string(&arg_seg, "width");
  message_builder_segment_push_int64(&arg_seg, popup->width);
  message_builder_segment_push_string(&arg_seg, "height");
  message_builder_segment_push_int64(&arg_seg, popup->height);
  message_builder_segment_push_string(&arg_seg, "texture_id");
  message_builder_segment_push_int64(&arg_seg, popup->texture_id);
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

static void send_popup_unmap(struct fwr_popup *popup) {
  struct fwr_instance *instance = popup->instance;

  wlr_log(WLR_INFO, "Popup unmap: handle=%d", popup->handle);

  struct message_builder msg = message_builder_new();
  struct message_builder_segment msg_seg = message_builder_segment(&msg);
  message_builder_segment_push_string(&msg_seg, "popup_unmap");
  message_builder_segment_finish(&msg_seg);

  msg_seg = message_builder_segment(&msg);
  struct message_builder_segment arg_seg =
      message_builder_segment_push_map(&msg_seg, 1);
  message_builder_segment_push_string(&arg_seg, "handle");
  message_builder_segment_push_int64(&arg_seg, popup->handle);
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

static void popup_handle_map(struct wl_listener *listener, void *data) {
  struct fwr_popup *popup = wl_container_of(listener, popup, map);
  struct fwr_instance *instance = popup->instance;

  wlr_log(WLR_INFO, "Popup surface mapped: handle=%d", popup->handle);

  // Register external texture for this popup
  if (!popup->texture_registered) {
    popup->texture_id = popup->handle + 200000;  // Offset to avoid collision with views/subsurfaces
    FlutterEngineResult result = instance->fl_proc_table.RegisterExternalTexture(
        instance->engine, popup->texture_id);
    if (result == kSuccess) {
      popup->texture_registered = true;
      wlr_log(WLR_INFO, "Registered popup texture: %ld", popup->texture_id);
    } else {
      wlr_log(WLR_ERROR, "Failed to register popup texture");
    }
  }

  send_popup_map(popup);
}

static void popup_handle_unmap(struct wl_listener *listener, void *data) {
  struct fwr_popup *popup = wl_container_of(listener, popup, unmap);
  struct fwr_instance *instance = popup->instance;

  wlr_log(WLR_INFO, "Popup surface unmapped: handle=%d", popup->handle);

  send_popup_unmap(popup);

  if (popup->texture_registered) {
    instance->fl_proc_table.UnregisterExternalTexture(instance->engine, popup->texture_id);
    popup->texture_registered = false;
  }
}

static void popup_handle_destroy(struct wl_listener *listener, void *data) {
  struct fwr_popup *popup = wl_container_of(listener, popup, destroy);
  struct fwr_instance *instance = popup->instance;

  wlr_log(WLR_INFO, "Popup destroyed: handle=%d", popup->handle);

  // Destroy scene tree (wlroots handles rendering/input)
  if (popup->scene_tree != NULL) {
    wlr_scene_node_destroy(&popup->scene_tree->node);
    popup->scene_tree = NULL;
  }

  // Clean up Flutter texture resources (legacy, may not be used anymore)
  if (popup->texture_registered) {
    instance->fl_proc_table.UnregisterExternalTexture(instance->engine, popup->texture_id);
    popup->texture_registered = false;
  }

  // Clean up cached GL resources
  fwr_cached_texture_destroy(instance, &popup->cache);

  wl_list_remove(&popup->map.link);
  wl_list_remove(&popup->unmap.link);
  wl_list_remove(&popup->destroy.link);
  wl_list_remove(&popup->commit.link);
  wl_list_remove(&popup->reposition.link);

  handle_map_remove(instance->popups, popup->handle);
  free(popup);
}

static void popup_handle_commit(struct wl_listener *listener, void *data) {
  struct fwr_popup *popup = wl_container_of(listener, popup, commit);
  struct fwr_instance *instance = popup->instance;

  wlr_log(WLR_DEBUG, "Popup commit: handle=%d, mapped=%s, tex_reg=%s, surface=%dx%d",
          popup->handle,
          popup->xdg_surface->surface->mapped ? "true" : "false",
          popup->texture_registered ? "true" : "false",
          popup->xdg_surface->surface->current.width,
          popup->xdg_surface->surface->current.height);

  // Unconstrain on first commit (when surface is initialized)
  if (!popup->unconstrained) {
    struct wlr_box output_box = {0};
    if (instance->output != NULL && instance->output->wlr_output != NULL) {
      output_box.width = instance->output->wlr_output->width;
      output_box.height = instance->output->wlr_output->height;
    } else {
      output_box.width = 1920;
      output_box.height = 1080;
    }
    output_box.x = -popup->parent_view->x;
    output_box.y = -popup->parent_view->y;
    wlr_xdg_popup_unconstrain_from_box(popup->xdg_popup, &output_box);
    popup->unconstrained = true;
    wlr_log(WLR_INFO, "Unconstrained popup %d with box: (%d,%d,%dx%d)",
            popup->handle, output_box.x, output_box.y, output_box.width, output_box.height);
  }

  if (!popup->xdg_surface->surface->mapped) {
    return;
  }

  // Fallback: if surface is mapped but texture isn't registered, do it now
  // This handles cases where the map event was missed
  if (!popup->texture_registered) {
    wlr_log(WLR_INFO, "Popup commit: surface mapped but texture not registered, registering now");
    popup->texture_id = popup->handle + 200000;
    FlutterEngineResult result = instance->fl_proc_table.RegisterExternalTexture(
        instance->engine, popup->texture_id);
    if (result == kSuccess) {
      popup->texture_registered = true;
      wlr_log(WLR_INFO, "Registered popup texture via commit fallback: %ld", popup->texture_id);
      send_popup_map(popup);
    } else {
      wlr_log(WLR_ERROR, "Failed to register popup texture via commit fallback");
    }
  }

  // Mark texture as needing update
  if (popup->texture_registered) {
    wlr_log(WLR_DEBUG, "Popup marking frame available: texture_id=%ld", popup->texture_id);
    instance->fl_proc_table.MarkExternalTextureFrameAvailable(
        instance->engine, popup->texture_id);
  }
}

static void popup_handle_reposition(struct wl_listener *listener, void *data) {
  struct fwr_popup *popup = wl_container_of(listener, popup, reposition);

  // Update local position tracking from new geometry
  struct wlr_box geo = popup->xdg_popup->scheduled.geometry;
  popup->x = geo.x;
  popup->y = geo.y;
  popup->width = geo.width;
  popup->height = geo.height;

  wlr_log(WLR_INFO, "Popup repositioned: handle=%d, pos=(%d,%d), size=%dx%d",
          popup->handle, popup->x, popup->y, popup->width, popup->height);

  // Scene tree position is automatic - popup is child of parent's xdg surface tree
  // wlr_scene_xdg_surface handles popup positioning internally

  // Send updated position to Flutter (for any overlay UI)
  if (popup->xdg_surface->surface->mapped) {
    send_popup_map(popup);  // Re-send with new position
  }
}

void fwr_new_xdg_popup(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance =
      wl_container_of(listener, instance, new_xdg_popup);
  struct wlr_xdg_popup *xdg_popup = data;

  wlr_log(WLR_INFO, "New xdg_popup created");

  // Find the parent view
  struct wlr_xdg_surface *parent_xdg =
      wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
  if (parent_xdg == NULL) {
    wlr_log(WLR_ERROR, "Popup parent is not an xdg_surface");
    return;
  }

  struct fwr_view *parent_view = NULL;
  struct fwr_view *v;
  wl_list_for_each(v, &instance->views_list, link) {
    if (v->xdg_surface == parent_xdg) {
      parent_view = v;
      break;
    }
  }

  if (parent_view == NULL) {
    wlr_log(WLR_ERROR, "Could not find parent view for popup");
    return;
  }

  struct fwr_popup *popup = calloc(1, sizeof(struct fwr_popup));
  popup->instance = instance;
  popup->xdg_popup = xdg_popup;
  popup->xdg_surface = xdg_popup->base;
  popup->parent_view = parent_view;
  popup->parent_view_handle = parent_view->handle;

  // Get initial geometry
  struct wlr_box geo = xdg_popup->scheduled.geometry;
  popup->x = geo.x;
  popup->y = geo.y;
  popup->width = geo.width;
  popup->height = geo.height;

  popup->handle = handle_map_add(instance->popups, (void *)popup);

  wlr_log(WLR_INFO, "Created popup: handle=%d, parent=%d, geo=(%d,%d,%dx%d)",
          popup->handle, parent_view->handle, popup->x, popup->y,
          popup->width, popup->height);

  // Create popup scene tree as child of parent's scene tree (like tinywl)
  // This way popup positioning is automatic - relative to parent
  if (parent_view->scene_xdg_tree != NULL) {
    popup->scene_tree = wlr_scene_xdg_surface_create(
        parent_view->scene_xdg_tree, xdg_popup->base);
    if (popup->scene_tree != NULL) {
      popup->scene_tree->node.data = popup;
      wlr_log(WLR_INFO, "Created popup scene as child of parent view %d", parent_view->handle);
    }
  } else {
    wlr_log(WLR_ERROR, "Parent view %d has no scene tree for popup", parent_view->handle);
  }

  // Store popup in xdg_surface data for hit testing
  xdg_popup->base->data = popup;

  // Setup listeners
  popup->map.notify = popup_handle_map;
  wl_signal_add(&xdg_popup->base->surface->events.map, &popup->map);

  popup->unmap.notify = popup_handle_unmap;
  wl_signal_add(&xdg_popup->base->surface->events.unmap, &popup->unmap);

  popup->destroy.notify = popup_handle_destroy;
  wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);

  popup->commit.notify = popup_handle_commit;
  wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

  popup->reposition.notify = popup_handle_reposition;
  wl_signal_add(&xdg_popup->events.reposition, &popup->reposition);

  // Check if surface is already mapped (can happen in some cases)
  wlr_log(WLR_INFO, "Popup surface mapped state: %s",
          xdg_popup->base->surface->mapped ? "true" : "false");
  if (xdg_popup->base->surface->mapped) {
    wlr_log(WLR_INFO, "Popup surface already mapped, triggering map handler");
    popup_handle_map(&popup->map, NULL);
  }
}
