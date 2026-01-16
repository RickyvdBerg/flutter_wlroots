#include "flutter_embedder.h"
#include <stdint.h>
#include <stdlib.h>
#include <linux/input-event-codes.h>

#include <wayland-server-protocol.h>

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_pointer.h>

#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>

#include <wlr/types/wlr_touch.h>
#include <wlr/backend.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <stdlib.h>
#include <math.h>
#include <locale.h>

#include "input.h"
#include "shaders.h"
#include "instance.h"
#include "surface.h"
#include "handle_map.h"
#include "messages.h"
#include "text_input.h"

static int64_t flutter_button_mask_from_linux(uint32_t button) {
  switch (button) {
  case BTN_LEFT:
    return kFlutterPointerButtonMousePrimary;
  case BTN_RIGHT:
    return kFlutterPointerButtonMouseSecondary;
  case BTN_MIDDLE:
    return kFlutterPointerButtonMouseMiddle;
  case BTN_BACK:
    return kFlutterPointerButtonMouseBack;
  case BTN_FORWARD:
    return kFlutterPointerButtonMouseForward;
  default:
    return 0;
  }
}

static void send_flutter_mouse_event(struct fwr_instance *instance,
    FlutterPointerPhase phase,
    FlutterPointerSignalKind signal_kind,
    double scroll_delta_x,
    double scroll_delta_y) {
  if (instance->engine == NULL) {
    return;
  }

  FlutterPointerEvent pointer_event = {};
  pointer_event.struct_size = sizeof(FlutterPointerEvent);
  pointer_event.phase = phase;
  pointer_event.timestamp = instance->fl_proc_table.GetCurrentTime();
  pointer_event.x = instance->cursor->x;
  pointer_event.y = instance->cursor->y;
  pointer_event.device = 0;
  pointer_event.signal_kind = signal_kind;
  pointer_event.scroll_delta_x = scroll_delta_x;
  pointer_event.scroll_delta_y = scroll_delta_y;
  pointer_event.device_kind = kFlutterPointerDeviceKindMouse;
  pointer_event.buttons = instance->input.fl_mouse_button_mask;
  instance->fl_proc_table.SendPointerEvent(instance->engine, &pointer_event, 1);
}

static void send_surface_position(struct fwr_instance *instance, struct fwr_view *view);
static void send_grab_end(struct fwr_instance *instance, struct fwr_view *view);

static void process_cursor_motion(struct fwr_instance *instance, uint32_t time) {
  // Handle window move grab
  if (instance->input.grab.type == FWR_GRAB_MOVE) {
    struct fwr_view *view;
    if (handle_map_get(instance->views, instance->input.grab.view_handle, (void **)&view) && view != NULL) {
      double dx = instance->cursor->x - instance->input.grab.start_cursor_x;
      double dy = instance->cursor->y - instance->input.grab.start_cursor_y;
      view->x = instance->input.grab.start_view_x + (int)dx;
      view->y = instance->input.grab.start_view_y + (int)dy;
      if (view->scene_tree != NULL) {
        wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
      }
      send_surface_position(instance, view);
    }
    send_flutter_mouse_event(instance, kMove, kFlutterPointerSignalKindNone, 0.0, 0.0);
    return;
  }

  // Handle window resize grab
  if (instance->input.grab.type == FWR_GRAB_RESIZE) {
    struct fwr_view *view;
    if (handle_map_get(instance->views, instance->input.grab.view_handle, (void **)&view) && view != NULL) {
      double dx = instance->cursor->x - instance->input.grab.start_cursor_x;
      double dy = instance->cursor->y - instance->input.grab.start_cursor_y;

      int new_x = view->x;
      int new_y = view->y;
      int new_width = instance->input.grab.start_view_width;
      int new_height = instance->input.grab.start_view_height;

      uint32_t edges = instance->input.grab.resize_edges;
      if (edges & 1) {
        new_height = instance->input.grab.start_view_height - (int)dy;
        new_y = instance->input.grab.start_view_y + (int)dy;
      }
      if (edges & 2) {
        new_height = instance->input.grab.start_view_height + (int)dy;
      }
      if (edges & 4) {
        new_width = instance->input.grab.start_view_width - (int)dx;
        new_x = instance->input.grab.start_view_x + (int)dx;
      }
      if (edges & 8) {
        new_width = instance->input.grab.start_view_width + (int)dx;
      }

      if (new_width < 100) new_width = 100;
      if (new_height < 100) new_height = 100;

      view->x = new_x;
      view->y = new_y;
      view->width = new_width;
      view->height = new_height;
      if (view->scene_tree != NULL) {
        wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
      }
      if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
        wlr_xdg_toplevel_set_size(view->toplevel, new_width, new_height);
      }
      send_surface_position(instance, view);
    }
    send_flutter_mouse_event(instance, kMove, kFlutterPointerSignalKindNone, 0.0, 0.0);
    return;
  }

  // Direct input mode - bypass Flutter for low-latency gaming
  if (instance->direct_input_mode && instance->direct_input_surface != 0) {
    struct fwr_view *view;
    if (handle_map_get(instance->views, instance->direct_input_surface, (void **)&view) && view != NULL) {
      struct wlr_surface *surface = view->xdg_surface->surface;
      // Calculate surface-local coordinates
      int titlebar_height = view->uses_ssd ? 38 : 0;
      double sx = instance->cursor->x - view->x;
      double sy = instance->cursor->y - view->y - titlebar_height;
      wlr_seat_pointer_notify_enter(instance->seat, surface, sx, sy);
      wlr_seat_pointer_notify_motion(instance->seat, time, sx, sy);
      wlr_seat_pointer_notify_frame(instance->seat);
      return;
    }
  }

  // Flutter-first: Send all motion events to Flutter
  // Flutter's widget tree does hit testing and forwards to surfaces as needed
  FlutterPointerPhase phase = instance->input.fl_mouse_button_mask != 0 ? kMove : kHover;
  send_flutter_mouse_event(instance, phase, kFlutterPointerSignalKindNone, 0.0, 0.0);
}

static void on_server_cursor_motion(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance = wl_container_of(listener, instance, cursor_motion);
  struct wlr_pointer_motion_event *event = data;

  wlr_cursor_move(instance->cursor, &event->pointer->base, event->delta_x,
                  event->delta_y);
  process_cursor_motion(instance, event->time_msec);
}

static void on_server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance = wl_container_of(listener, instance, cursor_motion_absolute);
  struct wlr_pointer_motion_absolute_event *event = data;

  wlr_cursor_warp_absolute(instance->cursor, &event->pointer->base, event->x, event->y);
  process_cursor_motion(instance, event->time_msec);
}

static void on_server_cursor_button(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance = wl_container_of(listener, instance, cursor_button);
  struct wlr_pointer_button_event *event = data;

  int64_t flutter_button_mask = flutter_button_mask_from_linux(event->button);
  if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
    instance->input.fl_mouse_button_mask |= flutter_button_mask;
  } else {
    instance->input.fl_mouse_button_mask &= ~flutter_button_mask;
  }

  // Handle grab release (window move/resize)
  if (event->state == WL_POINTER_BUTTON_STATE_RELEASED &&
      event->button == BTN_LEFT &&
      instance->input.grab.type != FWR_GRAB_NONE) {
    struct fwr_view *view;
    if (handle_map_get(instance->views, instance->input.grab.view_handle, (void **)&view) && view != NULL) {
      send_grab_end(instance, view);
    }
    instance->input.grab.type = FWR_GRAB_NONE;
    instance->input.grab.view_handle = 0;

    send_flutter_mouse_event(instance, kUp, kFlutterPointerSignalKindNone, 0.0, 0.0);
    wlr_seat_pointer_notify_button(instance->seat, event->time_msec, event->button, event->state);
    wlr_seat_pointer_notify_frame(instance->seat);
    return;
  }

  // Direct input mode - bypass Flutter for low-latency gaming
  if (instance->direct_input_mode && instance->direct_input_surface != 0) {
    struct fwr_view *view;
    if (handle_map_get(instance->views, instance->direct_input_surface, (void **)&view) && view != NULL) {
      wlr_seat_pointer_notify_button(instance->seat, event->time_msec, event->button, event->state);
      wlr_seat_pointer_notify_frame(instance->seat);
      return;
    }
  }

  // Check if a popup grab is active - wlroots needs button events for grab handling
  bool has_popup_grab = (instance->seat->pointer_state.grab !=
                         instance->seat->pointer_state.default_grab);
  if (has_popup_grab) {
    // Send to both Flutter and seat for popup dismiss handling
    send_flutter_mouse_event(instance,
      event->state == WL_POINTER_BUTTON_STATE_PRESSED ? kDown : kUp,
      kFlutterPointerSignalKindNone, 0.0, 0.0);
    wlr_seat_pointer_notify_button(instance->seat, event->time_msec, event->button, event->state);
    wlr_seat_pointer_notify_frame(instance->seat);
    return;
  }

  // Flutter-first: Send all button events to Flutter
  // Flutter's widget tree does hit testing and forwards to surfaces as needed
  send_flutter_mouse_event(instance,
    event->state == WL_POINTER_BUTTON_STATE_PRESSED ? kDown : kUp,
    kFlutterPointerSignalKindNone, 0.0, 0.0);
}

static void on_server_cursor_axis(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance = wl_container_of(listener, instance, cursor_axis);
  struct wlr_pointer_axis_event *event = data;

  // Direct input mode - bypass Flutter for low-latency gaming
  if (instance->direct_input_mode && instance->direct_input_surface != 0) {
    struct fwr_view *view;
    if (handle_map_get(instance->views, instance->direct_input_surface, (void **)&view) && view != NULL) {
      wlr_seat_pointer_notify_axis(instance->seat, event->time_msec, event->orientation,
        event->delta, event->delta_discrete, event->source, event->relative_direction);
      wlr_seat_pointer_notify_frame(instance->seat);
      return;
    }
  }

  // Flutter-first: Send all scroll events to Flutter
  // Flutter's widget tree does hit testing and forwards to surfaces as needed
  double scroll_delta_x = 0.0;
  double scroll_delta_y = 0.0;
  if (event->orientation == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
    scroll_delta_x = -event->delta;
  } else {
    scroll_delta_y = -event->delta;
  }

  send_flutter_mouse_event(instance, kHover, kFlutterPointerSignalKindScroll, scroll_delta_x, scroll_delta_y);
}

static void on_server_cursor_frame(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance = wl_container_of(listener, instance, cursor_frame);
  (void)instance;
  (void)data;
}

static void on_server_cursor_touch_down(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance = wl_container_of(listener, instance, cursor_touch_down);
  struct wlr_touch_down_event *event = data;
  struct fwr_input_device_state *state = event->touch->base.data;

  // Guard against touch events before Flutter engine is initialized
  if (instance->engine == NULL) {
    return;
  }

  if (event->touch_id >= FWR_MULTITOUCH_MAX) return;
  state->touch_points[event->touch_id].x = event->x;
  state->touch_points[event->touch_id].y = event->y;

  double screen_width = 1.0;
  double screen_height = 1.0;
  if (instance->output != NULL) {
    screen_width = instance->output->wlr_output->width;
    screen_height = instance->output->wlr_output->height;
  }

  wlr_log(WLR_INFO, "touch down %f %f", event->x, event->y);

  FlutterPointerEvent pointer_event = {};
  pointer_event.struct_size = sizeof(FlutterPointerEvent);
  pointer_event.device_kind = kFlutterPointerDeviceKindTouch;
  pointer_event.signal_kind = kFlutterPointerSignalKindNone;
  pointer_event.device = event->touch_id;
  pointer_event.phase = kAdd;
  pointer_event.x = event->x * screen_width;
  pointer_event.y = event->y * screen_height;
  pointer_event.scroll_delta_x = 0;
  pointer_event.scroll_delta_y = 0;
  pointer_event.timestamp = instance->fl_proc_table.GetCurrentTime();
  instance->fl_proc_table.SendPointerEvent(instance->engine, &pointer_event, 1);

  pointer_event.phase = kDown;
  instance->fl_proc_table.SendPointerEvent(instance->engine, &pointer_event, 1);
}

static void on_server_cursor_touch_up(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance = wl_container_of(listener, instance, cursor_touch_up);
  struct wlr_touch_up_event *event = data;
  struct fwr_input_device_state *state = event->touch->base.data;

  // Guard against touch events before Flutter engine is initialized
  if (instance->engine == NULL) {
    return;
  }

  if (event->touch_id >= FWR_MULTITOUCH_MAX) return;

  double screen_width = 1.0;
  double screen_height = 1.0;
  if (instance->output != NULL) {
    screen_width = instance->output->wlr_output->width;
    screen_height = instance->output->wlr_output->height;
  }

  FlutterPointerEvent pointer_event = {};
  pointer_event.struct_size = sizeof(FlutterPointerEvent);
  pointer_event.device_kind = kFlutterPointerDeviceKindTouch;
  pointer_event.signal_kind = kFlutterPointerSignalKindNone;
  pointer_event.device = event->touch_id;
  pointer_event.phase = kUp;
  pointer_event.x = state->touch_points[event->touch_id].x * screen_width;
  pointer_event.y = state->touch_points[event->touch_id].y * screen_height;
  pointer_event.scroll_delta_x = 0;
  pointer_event.scroll_delta_y = 0;
  pointer_event.timestamp = instance->fl_proc_table.GetCurrentTime();
  instance->fl_proc_table.SendPointerEvent(instance->engine, &pointer_event, 1);

  pointer_event.phase = kRemove;
  instance->fl_proc_table.SendPointerEvent(instance->engine, &pointer_event, 1);
}

static void on_server_cursor_touch_motion(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance = wl_container_of(listener, instance, cursor_touch_motion);
  struct wlr_touch_motion_event *event = data;
  struct fwr_input_device_state *state = event->touch->base.data;

  // Guard against touch events before Flutter engine is initialized
  if (instance->engine == NULL) {
    return;
  }

  if (event->touch_id >= FWR_MULTITOUCH_MAX) return;
  state->touch_points[event->touch_id].x = event->x;
  state->touch_points[event->touch_id].y = event->y;

  double screen_width = 1.0;
  double screen_height = 1.0;
  if (instance->output != NULL) {
    screen_width = instance->output->wlr_output->width;
    screen_height = instance->output->wlr_output->height;
  }

  FlutterPointerEvent pointer_event = {};
  pointer_event.struct_size = sizeof(FlutterPointerEvent);
  pointer_event.device_kind = kFlutterPointerDeviceKindTouch;
  pointer_event.signal_kind = kFlutterPointerSignalKindNone;
  pointer_event.device = event->touch_id;
  pointer_event.phase = kMove;
  pointer_event.x = event->x * screen_width;
  pointer_event.y = event->y * screen_height;
  pointer_event.scroll_delta_x = 0;
  pointer_event.scroll_delta_y = 0;
  pointer_event.timestamp = instance->fl_proc_table.GetCurrentTime();
  instance->fl_proc_table.SendPointerEvent(instance->engine, &pointer_event, 1);
}

static void on_server_cursor_touch_frame(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance = wl_container_of(listener, instance, cursor_touch_frame);
}

static void on_seat_request_cursor(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance = wl_container_of(listener, instance, request_cursor);
  struct wlr_seat_pointer_request_set_cursor_event *event = data;

  struct wlr_seat_client *focused_client = instance->seat->pointer_state.focused_client;
  if (focused_client == event->seat_client) {
    // Track client cursor for software rendering
    instance->client_cursor_surface = event->surface;
    instance->client_cursor_hotspot_x = event->hotspot_x;
    instance->client_cursor_hotspot_y = event->hotspot_y;

    // Clear xcursor name when using client surface
    if (event->surface != NULL) {
      free(instance->current_xcursor_name);
      instance->current_xcursor_name = NULL;
    }

    wlr_cursor_set_surface(instance->cursor, event->surface,
                           event->hotspot_x, event->hotspot_y);
  }
}

static void on_seat_request_set_selection(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance = wl_container_of(listener, instance, request_set_selection);
  struct wlr_seat_request_set_selection_event *event = data;
  wlr_seat_set_selection(instance->seat, event->source, event->serial);
}

static void on_seat_request_set_primary_selection(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance = wl_container_of(listener, instance, request_set_primary_selection);
  struct wlr_seat_request_set_primary_selection_event *event = data;
  wlr_seat_set_primary_selection(instance->seat, event->source, event->serial);
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
    
  /* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	struct fwr_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	/*
	 * A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same seat. You can swap out the underlying wlr_keyboard like this and
	 * wlr_seat handles this transparently.
	 */
	wlr_seat_set_keyboard(keyboard->instance->seat, keyboard->keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(keyboard->instance->seat,
		&keyboard->keyboard->modifiers);


}

static struct fwr_view *get_focused_view(struct fwr_instance *instance) {
  if (instance->current_focused_view == 0) {
    return NULL;
  }
  struct fwr_view *view;
  if (!handle_map_get(instance->views, instance->current_focused_view, (void **)&view)) {
    return NULL;
  }
  return view;
}

static struct fwr_view *get_next_mapped_view(struct fwr_instance *instance, struct fwr_view *start, bool reverse) {
  if (wl_list_empty(&instance->views_list)) {
    return NULL;
  }

  struct wl_list *iter = reverse ? start->link.prev : start->link.next;
  while (iter != &instance->views_list) {
    struct fwr_view *candidate = wl_container_of(iter, candidate, link);
    if (candidate->scene_tree != NULL && candidate->scene_tree->node.enabled) {
      return candidate;
    }
    iter = reverse ? iter->prev : iter->next;
  }

  iter = reverse ? instance->views_list.prev : instance->views_list.next;
  while (iter != (reverse ? start->link.prev : start->link.next)) {
    if (iter == &instance->views_list) {
      iter = reverse ? instance->views_list.prev : instance->views_list.next;
      continue;
    }
    struct fwr_view *candidate = wl_container_of(iter, candidate, link);
    if (candidate->scene_tree != NULL && candidate->scene_tree->node.enabled) {
      return candidate;
    }
    iter = reverse ? iter->prev : iter->next;
  }

  return NULL;
}

static void focus_view_and_raise(struct fwr_view *view) {
  fwr_focus_view(view);
  if (view->scene_tree != NULL) {
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
    wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
  }
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {

	struct fwr_keyboard *keyboard =	wl_container_of(listener, keyboard, key);
  struct wlr_keyboard_key_event *event = data;

  wlr_seat_set_keyboard(keyboard->instance->seat, keyboard->keyboard);

  if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->keyboard);
    xkb_keysym_t sym = xkb_state_key_get_one_sym(keyboard->keyboard->xkb_state, event->keycode + 8);

    if ((modifiers & WLR_MODIFIER_ALT) && sym == XKB_KEY_Tab) {
      struct fwr_instance *instance = keyboard->instance;
      struct fwr_view *focused = get_focused_view(instance);
      if (focused == NULL) {
        // Guard against empty views_list - wl_container_of would compute garbage pointer
        if (wl_list_empty(&instance->views_list)) {
          return;
        }
        struct fwr_view *first = wl_container_of(instance->views_list.next, first, link);
        if (first->scene_tree != NULL && first->scene_tree->node.enabled) {
          focus_view_and_raise(first);
        }
      } else {
        struct fwr_view *next = get_next_mapped_view(instance, focused, (modifiers & WLR_MODIFIER_SHIFT) != 0);
        if (next != NULL) {
          focus_view_and_raise(next);
        }
      }
      return;
    }

    if (modifiers & WLR_MODIFIER_LOGO) {
      struct fwr_instance *instance = keyboard->instance;
      struct fwr_view *view = get_focused_view(instance);
      if (view == NULL || instance->output == NULL) {
        return;
      }

      int output_width = instance->output->wlr_output->width;
      int output_height = instance->output->wlr_output->height;

      if (sym == XKB_KEY_Left || sym == XKB_KEY_Right) {
        view->maximized = false;
        if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
          wlr_xdg_toplevel_set_maximized(view->toplevel, false);
        }

        int half_width = output_width / 2;
        view->x = (sym == XKB_KEY_Left) ? 0 : half_width;
        view->y = 0;
        view->width = half_width;
        view->height = output_height;

        if (view->scene_tree != NULL) {
          wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
        }
        if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
          wlr_xdg_toplevel_set_size(view->toplevel, view->width, view->height);
        }
        send_surface_position(instance, view);
        return;
      }

      if (sym == XKB_KEY_Up) {
        view->maximized = true;
        if (view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
          wlr_xdg_toplevel_set_maximized(view->toplevel, true);
        }
        view->x = 0;
        view->y = 0;
        if (view->scene_tree != NULL) {
          wlr_scene_node_set_position(&view->scene_tree->node, view->x, view->y);
        }
        send_surface_position(instance, view);
        return;
      }
    }
  }

  wlr_seat_keyboard_notify_key(keyboard->instance->seat, event->time_msec, event->keycode, event->state);

}

static void keyboard_destroy(struct wl_listener *listener, void *data) {

	struct fwr_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
  xkb_compose_state_unref(keyboard->compose_state);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void server_new_keyboard(struct fwr_instance *instance,
		struct wlr_input_device *device) {

  struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

  struct fwr_keyboard *keyboard = calloc(1, sizeof(struct fwr_keyboard));
  keyboard->instance = instance;
  keyboard->keyboard = wlr_keyboard;

  // Prepare XKB keymap and asing to keyboard, default layout is "us"
  struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

	if (!context) {
	  wlr_log(WLR_ERROR, "Failed to create XKB context");
		exit(1);
	}


  struct xkb_rule_names rules = { 0 };
	rules.rules = getenv("XKB_DEFAULT_RULES");
	rules.model = getenv("XKB_DEFAULT_MODEL");
	rules.layout = getenv("XKB_DEFAULT_LAYOUT");
	rules.variant = getenv("XKB_DEFAULT_VARIANT");
	rules.options = getenv("XKB_DEFAULT_OPTIONS");
  struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);

  keyboard->compose_state = xkb_compose_state_new(xkb_compose_table_new_from_locale(context, setlocale(LC_CTYPE, NULL), XKB_COMPOSE_COMPILE_NO_FLAGS), XKB_COMPOSE_STATE_NO_FLAGS);
  if (keyboard->compose_state == NULL) {
    wlr_log(WLR_ERROR, "Could not create new XKB compose state.\n");
  }

  wlr_keyboard_set_keymap(wlr_keyboard, keymap);
  xkb_keymap_unref(keymap);
  xkb_context_unref(context);
  wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

  keyboard->modifiers.notify = keyboard_handle_modifiers;
  wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
  keyboard->key.notify = keyboard_handle_key;
  wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
  keyboard->destroy.notify = keyboard_destroy;
  wl_signal_add(&wlr_keyboard->base.events.destroy, &keyboard->destroy);

  wlr_seat_set_keyboard(instance->seat, wlr_keyboard);

  // add keyboard to list of keyboards
  wl_list_insert(&instance->keyboards, &keyboard->link);

}

static void server_new_pointer(struct fwr_instance *instance,
		struct wlr_input_device *device) {
  wlr_cursor_attach_input_device(instance->cursor, device);
}

static void server_new_touch(struct fwr_instance *instance,
		struct wlr_input_device *device) {
  wlr_cursor_attach_input_device(instance->cursor, device);
}

static void on_server_new_input(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance = wl_container_of(listener, instance, new_input);
  struct wlr_input_device *device = data;

  struct fwr_input_device_state *state = calloc(1, sizeof(struct fwr_input_device_state));
  device->data = state;

  switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(instance, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(instance, device);
		break;
  case WLR_INPUT_DEVICE_TOUCH:
    server_new_touch(instance, device);
    break;
	default:
		break;
	}

  // TODO seat caps
  uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
  if (!wl_list_empty(&instance->keyboards)) {
   caps |= WL_SEAT_CAPABILITY_KEYBOARD;
  }
  wlr_seat_set_capabilities(instance->seat, caps);
}

void fwr_input_init(struct fwr_instance *instance) {
  instance->input.mouse_button_mask = 0;
  instance->input.fl_mouse_button_mask = 0;

  instance->input.simulating_pointer_from_touch = false;
  instance->input.touch_pointer_simulation_id = 0;
  for (int i = 0; i < FWR_MULTITOUCH_MAX; i++) {
    instance->input.touch_ids[i] = -1;
  }

  instance->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(instance->cursor, instance->output_layout);

  instance->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
  wlr_xcursor_manager_load(instance->cursor_mgr, 1);

  // Initialize default cursor
  instance->current_xcursor_name = strdup("left_ptr");
  instance->client_cursor_surface = NULL;
  instance->client_cursor_hotspot_x = 0;
  instance->client_cursor_hotspot_y = 0;

  wlr_cursor_set_xcursor(instance->cursor, instance->cursor_mgr, "left_ptr");


	instance->seat = wlr_seat_create(instance->wl_display, "seat0");

  instance->request_cursor.notify = on_seat_request_cursor;
  wl_signal_add(&instance->seat->events.request_set_cursor, &instance->request_cursor);

  // Clipboard selection handlers - allows clients to copy/paste
  instance->request_set_selection.notify = on_seat_request_set_selection;
  wl_signal_add(&instance->seat->events.request_set_selection, &instance->request_set_selection);

  // Primary selection (middle-click paste)
  instance->request_set_primary_selection.notify = on_seat_request_set_primary_selection;
  wl_signal_add(&instance->seat->events.request_set_primary_selection, &instance->request_set_primary_selection);

  instance->cursor_motion.notify = on_server_cursor_motion;
  wl_signal_add(&instance->cursor->events.motion, &instance->cursor_motion);
  instance->cursor_motion_absolute.notify = on_server_cursor_motion_absolute;
  wl_signal_add(&instance->cursor->events.motion_absolute, &instance->cursor_motion_absolute);
  instance->cursor_button.notify = on_server_cursor_button;
  wl_signal_add(&instance->cursor->events.button, &instance->cursor_button);
  instance->cursor_axis.notify = on_server_cursor_axis;
  wl_signal_add(&instance->cursor->events.axis, &instance->cursor_axis);
  instance->cursor_frame.notify = on_server_cursor_frame;
  wl_signal_add(&instance->cursor->events.frame, &instance->cursor_frame);
  instance->cursor_touch_down.notify = on_server_cursor_touch_down;
  wl_signal_add(&instance->cursor->events.touch_down, &instance->cursor_touch_down);
  instance->cursor_touch_up.notify = on_server_cursor_touch_up;
  wl_signal_add(&instance->cursor->events.touch_up, &instance->cursor_touch_up);
  instance->cursor_touch_motion.notify = on_server_cursor_touch_motion;
  wl_signal_add(&instance->cursor->events.touch_motion, &instance->cursor_touch_motion);
  instance->cursor_touch_frame.notify = on_server_cursor_touch_frame;
  wl_signal_add(&instance->cursor->events.touch_frame, &instance->cursor_touch_frame);

  wl_list_init(&instance->keyboards);

  instance->new_input.notify = on_server_new_input;
	wl_signal_add(&instance->backend->events.new_input, &instance->new_input);
}

static const uint8_t pointerDownEvent = 1;
static const uint8_t pointerUpEvent = 2;
static const uint8_t pointerHoverEvent = 3;
static const uint8_t pointerMoveEvent = 4;
static const uint8_t pointerEnterEvent = 5;
static const uint8_t pointerExitEvent = 6;
static const uint8_t pointerScrollEvent = 7;

static bool flutter_mouse_button_to_linux(int64_t flutter_button, uint32_t *linux_button_out) {
  switch (flutter_button) {
  case kFlutterPointerButtonMousePrimary:
    *linux_button_out = BTN_LEFT;
    return true;
  case kFlutterPointerButtonMouseSecondary:
    *linux_button_out = BTN_RIGHT;
    return true;
  case kFlutterPointerButtonMouseMiddle:
    *linux_button_out = BTN_MIDDLE;
    return true;
  case kFlutterPointerButtonMouseBack:
    *linux_button_out = BTN_BACK;
    return true;
  case kFlutterPointerButtonMouseForward:
    *linux_button_out = BTN_FORWARD;
    return true;
  default:
    return false;
  }
}

// Track button state per surface (more reliable than per-pointer which can change)
struct surface_button_state {
  uint32_t surface_handle;
  int64_t buttons;
  bool active;
};

static struct surface_button_state surface_button_states[32] = {0};

static int64_t get_surface_buttons(uint32_t surface_handle) {
  for (size_t i = 0; i < sizeof(surface_button_states) / sizeof(surface_button_states[0]); i++) {
    if (surface_button_states[i].active && surface_button_states[i].surface_handle == surface_handle) {
      return surface_button_states[i].buttons;
    }
  }
  return 0;
}

#define SURFACE_BUTTON_STATES_SIZE 32

static void set_surface_buttons(uint32_t surface_handle, int64_t buttons) {
  // First, try to find existing entry for this surface
  for (size_t i = 0; i < SURFACE_BUTTON_STATES_SIZE; i++) {
    if (surface_button_states[i].active && surface_button_states[i].surface_handle == surface_handle) {
      surface_button_states[i].buttons = buttons;
      return;
    }
  }

  // No existing entry, find an empty slot
  for (size_t i = 0; i < SURFACE_BUTTON_STATES_SIZE; i++) {
    if (!surface_button_states[i].active) {
      surface_button_states[i].active = true;
      surface_button_states[i].surface_handle = surface_handle;
      surface_button_states[i].buttons = buttons;
      return;
    }
  }

  // Array full - this indicates a leak (surfaces destroyed without calling fwr_clear_surface_buttons)
  wlr_log(WLR_ERROR, "surface_button_states array full (%d entries) - button tracking may be incorrect. "
          "Ensure fwr_clear_surface_buttons() is called when surfaces are destroyed.",
          SURFACE_BUTTON_STATES_SIZE);
}

void fwr_clear_surface_buttons(uint32_t surface_handle) {
  for (size_t i = 0; i < SURFACE_BUTTON_STATES_SIZE; i++) {
    if (surface_button_states[i].active && surface_button_states[i].surface_handle == surface_handle) {
      surface_button_states[i].active = false;
      surface_button_states[i].surface_handle = 0;
      surface_button_states[i].buttons = 0;
      return;
    }
  }
}

void fwr_handle_surface_pointer_event_message(
    struct fwr_instance *instance,
    const FlutterPlatformMessageResponseHandle *handle,
    struct dart_value *args) {
  struct surface_pointer_event_message message;
  if (!decode_surface_pointer_event_message(args, &message)) {
    goto error;
  }

  struct fwr_view *view;
  if (!handle_map_get(instance->views, message.surface_handle, (void **)&view) || view == NULL) {
    goto success;
  }

  struct wlr_surface *parent_surface = view->xdg_surface->surface;
  if (parent_surface == NULL) {
    goto success;
  }

  // Resolve subsurfaces - use wlr_surface_surface_at to find the actual surface
  // at the pointer position (may be a subsurface within the parent)
  double sub_x = message.local_pos_x;
  double sub_y = message.local_pos_y;
  struct wlr_surface *surface = wlr_surface_surface_at(
      parent_surface, message.local_pos_x, message.local_pos_y, &sub_x, &sub_y);
  if (surface == NULL) {
    // No surface at this position, use parent with original coordinates
    surface = parent_surface;
    sub_x = message.local_pos_x;
    sub_y = message.local_pos_y;
  }

  uint32_t time_msec = (uint32_t)(message.timestamp / 1000);

  // Track Flutter cursor position for grab operations
  instance->input.flutter_cursor_x = message.global_pos_x;
  instance->input.flutter_cursor_y = message.global_pos_y;

  // Handle active grabs (move/resize initiated by CSD apps via request_move/resize)
  if (instance->input.grab.type != FWR_GRAB_NONE) {
    // Button release ends the grab
    if (message.event_type == pointerUpEvent) {
      wlr_log(WLR_INFO, "Grab ended (type=%d) for view %d",
              instance->input.grab.type, instance->input.grab.view_handle);
      struct fwr_view *grab_view;
      if (handle_map_get(instance->views, instance->input.grab.view_handle, (void **)&grab_view) && grab_view != NULL) {
        send_grab_end(instance, grab_view);
      }
      instance->input.grab.type = FWR_GRAB_NONE;
      instance->input.grab.view_handle = 0;
    }
    // Motion during grab - update window position/size
    else if (message.event_type == pointerMoveEvent || message.event_type == pointerHoverEvent) {
      // Use global position for grab calculations
      double cursor_x = message.global_pos_x;
      double cursor_y = message.global_pos_y;

      if (instance->input.grab.type == FWR_GRAB_MOVE) {
        struct fwr_view *grab_view;
        if (handle_map_get(instance->views, instance->input.grab.view_handle, (void **)&grab_view) && grab_view != NULL) {
          double dx = cursor_x - instance->input.grab.start_cursor_x;
          double dy = cursor_y - instance->input.grab.start_cursor_y;
          grab_view->x = instance->input.grab.start_view_x + (int)dx;
          grab_view->y = instance->input.grab.start_view_y + (int)dy;
          if (grab_view->scene_tree != NULL) {
            wlr_scene_node_set_position(&grab_view->scene_tree->node, grab_view->x, grab_view->y);
          }
          send_surface_position(instance, grab_view);
        }
      } else if (instance->input.grab.type == FWR_GRAB_RESIZE) {
        struct fwr_view *grab_view;
        if (handle_map_get(instance->views, instance->input.grab.view_handle, (void **)&grab_view) && grab_view != NULL) {
          double dx = cursor_x - instance->input.grab.start_cursor_x;
          double dy = cursor_y - instance->input.grab.start_cursor_y;

          int new_x = grab_view->x;
          int new_y = grab_view->y;
          int new_width = instance->input.grab.start_view_width;
          int new_height = instance->input.grab.start_view_height;

          uint32_t edges = instance->input.grab.resize_edges;
          if (edges & 1) { // top
            new_height = instance->input.grab.start_view_height - (int)dy;
            new_y = instance->input.grab.start_view_y + (int)dy;
          }
          if (edges & 2) { // bottom
            new_height = instance->input.grab.start_view_height + (int)dy;
          }
          if (edges & 4) { // left
            new_width = instance->input.grab.start_view_width - (int)dx;
            new_x = instance->input.grab.start_view_x + (int)dx;
          }
          if (edges & 8) { // right
            new_width = instance->input.grab.start_view_width + (int)dx;
          }

          if (new_width < 100) new_width = 100;
          if (new_height < 100) new_height = 100;

          grab_view->x = new_x;
          grab_view->y = new_y;
          grab_view->width = new_width;
          grab_view->height = new_height;
          if (grab_view->scene_tree != NULL) {
            wlr_scene_node_set_position(&grab_view->scene_tree->node, grab_view->x, grab_view->y);
          }
          if (grab_view->xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
            wlr_xdg_toplevel_set_size(grab_view->toplevel, new_width, new_height);
          }
          send_surface_position(instance, grab_view);
        }
      }
    }
    goto success;  // Don't process normal events during grab
  }

  // Use subsurface-relative coordinates if we hit a subsurface
  // sub_x/sub_y were set by wlr_surface_surface_at above
  double local_x = sub_x;
  double local_y = sub_y;

  // If we temporarily scale the client buffer during interactive resize to
  // match the Flutter widget size, we must also scale pointer coordinates back
  // into the surface's current coordinate space so hit-testing stays correct.
  // Note: This scaling is relative to the parent surface, so only apply if
  // we're on the parent surface (not a subsurface)
  if (surface == parent_surface && message.widget_size_x > 0.0 && message.widget_size_y > 0.0) {
    int surf_w = surface->current.width;
    int surf_h = surface->current.height;
    if (surf_w > 0 && surf_h > 0) {
      double sx = (double)surf_w / message.widget_size_x;
      double sy = (double)surf_h / message.widget_size_y;
      if (isfinite(sx) && isfinite(sy) && sx > 0.0 && sy > 0.0) {
        local_x *= sx;
        local_y *= sy;
      }
    }
  }

  if (message.event_type == pointerEnterEvent) {
    wlr_seat_pointer_notify_enter(instance->seat, surface, local_x, local_y);
    wlr_seat_pointer_notify_frame(instance->seat);
  } else if (message.event_type == pointerExitEvent) {
    wlr_seat_pointer_clear_focus(instance->seat);
    // Don't clear button state on exit - track by surface handle instead
  } else if (message.event_type == pointerHoverEvent || message.event_type == pointerMoveEvent) {
    wlr_seat_pointer_notify_enter(instance->seat, surface, local_x, local_y);
    wlr_seat_pointer_notify_motion(instance->seat, time_msec, local_x, local_y);
    wlr_seat_pointer_notify_frame(instance->seat);
  } else if (message.event_type == pointerDownEvent || message.event_type == pointerUpEvent) {
    wlr_seat_pointer_notify_enter(instance->seat, surface, local_x, local_y);
    fwr_focus_view(view);
    if (view->scene_tree != NULL) {
      wlr_scene_node_raise_to_top(&view->scene_tree->node);
    }

    // Track button state by surface handle (more reliable than pointer ID which can change)
    int64_t prev_buttons = get_surface_buttons(message.surface_handle);
    int64_t next_buttons = message.buttons;
    int64_t changed = prev_buttons ^ next_buttons;

    const int64_t flutter_buttons[] = {
        kFlutterPointerButtonMousePrimary,
        kFlutterPointerButtonMouseSecondary,
        kFlutterPointerButtonMouseMiddle,
        kFlutterPointerButtonMouseBack,
        kFlutterPointerButtonMouseForward,
    };

    for (size_t i = 0; i < sizeof(flutter_buttons) / sizeof(flutter_buttons[0]); i++) {
      int64_t flutter_button = flutter_buttons[i];
      if ((changed & flutter_button) == 0) {
        continue;
      }

      uint32_t linux_button;
      if (!flutter_mouse_button_to_linux(flutter_button, &linux_button)) {
        continue;
      }

      enum wl_pointer_button_state state =
          (next_buttons & flutter_button) ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
      wlr_seat_pointer_notify_button(instance->seat, time_msec, linux_button, state);
    }

    set_surface_buttons(message.surface_handle, next_buttons);
    wlr_seat_pointer_notify_frame(instance->seat);
  } else if (message.event_type == pointerScrollEvent) {
    wlr_seat_pointer_notify_enter(instance->seat, surface, local_x, local_y);

    if (message.scroll_delta_x != 0.0) {
      wlr_seat_pointer_notify_axis(instance->seat, time_msec, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
          -message.scroll_delta_x, 0, WL_POINTER_AXIS_SOURCE_WHEEL, 0);
    }
    if (message.scroll_delta_y != 0.0) {
      wlr_seat_pointer_notify_axis(instance->seat, time_msec, WL_POINTER_AXIS_VERTICAL_SCROLL,
          -message.scroll_delta_y, 0, WL_POINTER_AXIS_SOURCE_WHEEL, 0);
    }

    wlr_seat_pointer_notify_frame(instance->seat);
  }

success:
  instance->fl_proc_table.SendPlatformMessageResponse(
      instance->engine, handle, method_call_null_success,
      sizeof(method_call_null_success));
  return;

error:
  wlr_log(WLR_ERROR, "Invalid surface pointer event message");
  instance->fl_proc_table.SendPlatformMessageResponse(instance->engine, handle, NULL, 0);
}

void fwr_handle_popup_pointer_event_message(
    struct fwr_instance *instance,
    const FlutterPlatformMessageResponseHandle *handle,
    struct dart_value *args) {
  wlr_log(WLR_DEBUG, "popup_pointer_event: received message");

  // Reuse the same message structure as surface_pointer_event
  // but lookup popup by handle instead of view
  struct surface_pointer_event_message message;
  if (!decode_surface_pointer_event_message(args, &message)) {
    wlr_log(WLR_ERROR, "popup_pointer_event: failed to decode message");
    goto error;
  }

  wlr_log(WLR_DEBUG, "popup_pointer_event: decoded handle=%d type=%d",
          message.surface_handle, message.event_type);

  struct fwr_popup *popup;
  if (!handle_map_get(instance->popups, message.surface_handle, (void **)&popup) || popup == NULL) {
    wlr_log(WLR_DEBUG, "popup_pointer_event: popup handle %d not found", message.surface_handle);
    goto success;
  }

  struct wlr_surface *popup_surface = popup->xdg_surface->surface;
  if (popup_surface == NULL) {
    goto success;
  }

  uint32_t time_msec = (uint32_t)(message.timestamp / 1000);

  // Find the actual surface under cursor - Firefox renders content to subsurfaces
  double local_x = message.local_pos_x;
  double local_y = message.local_pos_y;
  double sub_x, sub_y;
  struct wlr_surface *surface = wlr_surface_surface_at(popup_surface, local_x, local_y, &sub_x, &sub_y);
  if (surface != NULL) {
    // Use coordinates relative to the found surface (possibly a subsurface)
    local_x = sub_x;
    local_y = sub_y;
  } else {
    // Fallback to popup surface if nothing found at coordinates
    surface = popup_surface;
  }

  wlr_log(WLR_DEBUG, "Popup %d pointer event type=%d at (%.1f,%.1f) -> surface=%p (%.1f,%.1f)",
          message.surface_handle, message.event_type,
          message.local_pos_x, message.local_pos_y,
          (void*)surface, local_x, local_y);

  if (message.event_type == pointerEnterEvent) {
    wlr_seat_pointer_notify_enter(instance->seat, surface, local_x, local_y);
    wlr_seat_pointer_notify_frame(instance->seat);
  } else if (message.event_type == pointerExitEvent) {
    // Don't clear focus on popup exit - let the parent or next popup handle it
    wlr_seat_pointer_notify_frame(instance->seat);
  } else if (message.event_type == pointerHoverEvent || message.event_type == pointerMoveEvent) {
    wlr_seat_pointer_notify_enter(instance->seat, surface, local_x, local_y);
    wlr_seat_pointer_notify_motion(instance->seat, time_msec, local_x, local_y);
    wlr_seat_pointer_notify_frame(instance->seat);
  } else if (message.event_type == pointerDownEvent || message.event_type == pointerUpEvent) {
    wlr_seat_pointer_notify_enter(instance->seat, surface, local_x, local_y);

    // Track button state by popup handle
    int64_t prev_buttons = get_surface_buttons(message.surface_handle + 200000); // Offset to avoid collision with surface handles
    int64_t next_buttons = message.buttons;
    int64_t changed = prev_buttons ^ next_buttons;

    const int64_t flutter_buttons[] = {
        kFlutterPointerButtonMousePrimary,
        kFlutterPointerButtonMouseSecondary,
        kFlutterPointerButtonMouseMiddle,
        kFlutterPointerButtonMouseBack,
        kFlutterPointerButtonMouseForward,
    };

    for (size_t i = 0; i < sizeof(flutter_buttons) / sizeof(flutter_buttons[0]); i++) {
      int64_t flutter_button = flutter_buttons[i];
      if ((changed & flutter_button) == 0) {
        continue;
      }

      uint32_t linux_button;
      if (!flutter_mouse_button_to_linux(flutter_button, &linux_button)) {
        continue;
      }

      enum wl_pointer_button_state state =
          (next_buttons & flutter_button) ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
      wlr_seat_pointer_notify_button(instance->seat, time_msec, linux_button, state);
    }

    set_surface_buttons(message.surface_handle + 200000, next_buttons);
    wlr_seat_pointer_notify_frame(instance->seat);
  } else if (message.event_type == pointerScrollEvent) {
    wlr_seat_pointer_notify_enter(instance->seat, surface, local_x, local_y);

    if (message.scroll_delta_x != 0.0) {
      wlr_seat_pointer_notify_axis(instance->seat, time_msec, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
          -message.scroll_delta_x, 0, WL_POINTER_AXIS_SOURCE_WHEEL, 0);
    }
    if (message.scroll_delta_y != 0.0) {
      wlr_seat_pointer_notify_axis(instance->seat, time_msec, WL_POINTER_AXIS_VERTICAL_SCROLL,
          -message.scroll_delta_y, 0, WL_POINTER_AXIS_SOURCE_WHEEL, 0);
    }

    wlr_seat_pointer_notify_frame(instance->seat);
  }

success:
  instance->fl_proc_table.SendPlatformMessageResponse(
      instance->engine, handle, method_call_null_success,
      sizeof(method_call_null_success));
  return;

error:
  wlr_log(WLR_ERROR, "Invalid popup pointer event message");
  instance->fl_proc_table.SendPlatformMessageResponse(instance->engine, handle, NULL, 0);
}

void fwr_handle_surface_keyboard_key_message(
    struct fwr_instance *instance,
    const FlutterPlatformMessageResponseHandle *handle,
    struct dart_value *args) {
  struct surface_keyboard_key_message message;
  if (!decode_surface_keyboard_key_message(args, &message)) {
    goto error;
  }

  struct fwr_view *view;
  if (!handle_map_get(instance->views, message.surface_handle, (void **)&view) || view == NULL) {
    goto success;
  }

  fwr_focus_view(view);

  struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(instance->seat);
  if (keyboard != NULL) {
    wlr_seat_set_keyboard(instance->seat, keyboard);
  }

  enum wl_keyboard_key_state state =
      message.event_type != 0 ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;
  wlr_seat_keyboard_notify_key(instance->seat, (uint32_t)(message.timestamp / 1000),
      (uint32_t)message.keycode, state);

success:
  instance->fl_proc_table.SendPlatformMessageResponse(
      instance->engine, handle, method_call_null_success,
      sizeof(method_call_null_success));
  return;

error:
  wlr_log(WLR_ERROR, "Invalid surface keyboard key message");
  instance->fl_proc_table.SendPlatformMessageResponse(instance->engine, handle, NULL, 0);
}

static void send_surface_position(struct fwr_instance *instance, struct fwr_view *view) {
  struct message_builder msg = message_builder_new();
  struct message_builder_segment msg_seg = message_builder_segment(&msg);
  message_builder_segment_push_string(&msg_seg, "surface_position");
  message_builder_segment_finish(&msg_seg);

  msg_seg = message_builder_segment(&msg);
  struct message_builder_segment arg_seg =
      message_builder_segment_push_map(&msg_seg, 5);
  message_builder_segment_push_string(&arg_seg, "handle");
  message_builder_segment_push_int64(&arg_seg, view->handle);
  message_builder_segment_push_string(&arg_seg, "x");
  message_builder_segment_push_int64(&arg_seg, view->x);
  message_builder_segment_push_string(&arg_seg, "y");
  message_builder_segment_push_int64(&arg_seg, view->y);
  message_builder_segment_push_string(&arg_seg, "width");
  message_builder_segment_push_int64(&arg_seg, view->width);
  message_builder_segment_push_string(&arg_seg, "height");
  message_builder_segment_push_int64(&arg_seg, view->height);
  message_builder_segment_finish(&arg_seg);

  message_builder_segment_finish(&msg_seg);
  uint8_t *msg_buf;
  size_t msg_buf_len;
  message_builder_finish(&msg, &msg_buf, &msg_buf_len);

  FlutterPlatformMessage platform_message = {};
  platform_message.struct_size = sizeof(FlutterPlatformMessage);
  platform_message.channel = "wlroots";
  platform_message.message = msg_buf;
  platform_message.message_size = msg_buf_len;
  platform_message.response_handle = NULL;
  instance->fl_proc_table.SendPlatformMessage(instance->engine, &platform_message);

  free(msg_buf);
}

static void send_grab_end(struct fwr_instance *instance, struct fwr_view *view) {
  struct message_builder msg = message_builder_new();
  struct message_builder_segment msg_seg = message_builder_segment(&msg);
  message_builder_segment_push_string(&msg_seg, "surface_grab_end");
  message_builder_segment_finish(&msg_seg);

  msg_seg = message_builder_segment(&msg);
  struct message_builder_segment arg_seg =
      message_builder_segment_push_map(&msg_seg, 5);
  message_builder_segment_push_string(&arg_seg, "handle");
  message_builder_segment_push_int64(&arg_seg, view->handle);
  message_builder_segment_push_string(&arg_seg, "x");
  message_builder_segment_push_int64(&arg_seg, view->x);
  message_builder_segment_push_string(&arg_seg, "y");
  message_builder_segment_push_int64(&arg_seg, view->y);
  message_builder_segment_push_string(&arg_seg, "cursor_x");
  message_builder_segment_push_float64(&arg_seg, instance->cursor->x);
  message_builder_segment_push_string(&arg_seg, "cursor_y");
  message_builder_segment_push_float64(&arg_seg, instance->cursor->y);
  message_builder_segment_finish(&arg_seg);

  message_builder_segment_finish(&msg_seg);
  uint8_t *msg_buf;
  size_t msg_buf_len;
  message_builder_finish(&msg, &msg_buf, &msg_buf_len);

  FlutterPlatformMessage platform_message = {};
  platform_message.struct_size = sizeof(FlutterPlatformMessage);
  platform_message.channel = "wlroots";
  platform_message.message = msg_buf;
  platform_message.message_size = msg_buf_len;
  platform_message.response_handle = NULL;
  instance->fl_proc_table.SendPlatformMessage(instance->engine, &platform_message);

  free(msg_buf);
}

void fwr_handle_surface_begin_move(
    struct fwr_instance *instance,
    const FlutterPlatformMessageResponseHandle *handle,
    struct dart_value *args) {
  struct surface_begin_move_message message;
  if (!decode_surface_begin_move_message(args, &message)) {
    goto error;
  }

  struct fwr_view *view;
  if (!handle_map_get(instance->views, message.surface_handle, (void **)&view) || view == NULL) {
    goto success;
  }

  instance->input.grab.type = FWR_GRAB_MOVE;
  instance->input.grab.view_handle = view->handle;
  instance->input.grab.start_cursor_x = instance->cursor->x;
  instance->input.grab.start_cursor_y = instance->cursor->y;
  instance->input.grab.start_view_x = view->x;
  instance->input.grab.start_view_y = view->y;

  fwr_focus_view(view);
  if (view->scene_tree != NULL) {
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
  }

success:
  instance->fl_proc_table.SendPlatformMessageResponse(
      instance->engine, handle, method_call_null_success,
      sizeof(method_call_null_success));
  return;

error:
  wlr_log(WLR_ERROR, "Invalid surface begin move message");
  instance->fl_proc_table.SendPlatformMessageResponse(instance->engine, handle, NULL, 0);
}

void fwr_handle_surface_begin_resize(
    struct fwr_instance *instance,
    const FlutterPlatformMessageResponseHandle *handle,
    struct dart_value *args) {
  struct surface_begin_resize_message message;
  if (!decode_surface_begin_resize_message(args, &message)) {
    goto error;
  }

  struct fwr_view *view;
  if (!handle_map_get(instance->views, message.surface_handle, (void **)&view) || view == NULL) {
    goto success;
  }

  instance->input.grab.type = FWR_GRAB_RESIZE;
  instance->input.grab.view_handle = view->handle;
  instance->input.grab.start_cursor_x = instance->cursor->x;
  instance->input.grab.start_cursor_y = instance->cursor->y;
  instance->input.grab.start_view_x = view->x;
  instance->input.grab.start_view_y = view->y;
  instance->input.grab.start_view_width = view->width;
  instance->input.grab.start_view_height = view->height;
  instance->input.grab.resize_edges = (uint32_t)message.edges;

  fwr_focus_view(view);
  if (view->scene_tree != NULL) {
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
  }

success:
  instance->fl_proc_table.SendPlatformMessageResponse(
      instance->engine, handle, method_call_null_success,
      sizeof(method_call_null_success));
  return;

error:
  wlr_log(WLR_ERROR, "Invalid surface begin resize message");
  instance->fl_proc_table.SendPlatformMessageResponse(instance->engine, handle, NULL, 0);
}

