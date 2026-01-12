#include "flutter_embedder.h"
#include <stdint.h>
#include <stdlib.h>
#include <linux/input-event-codes.h>

#include <wayland-server-protocol.h>

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_pointer.h>

#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>

#include <wlr/types/wlr_touch.h>
#include <wlr/backend.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
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

struct hit_test_result {
  struct wlr_scene_node *node;
  struct wlr_surface *surface;
  struct fwr_view *view;
  bool is_decoration;
  bool is_flutter;
  double nx;
  double ny;
};

static struct hit_test_result hit_test_cursor(struct fwr_instance *instance) {
  struct hit_test_result res = {0};
  res.surface = NULL;
  res.view = NULL;
  res.is_decoration = false;
  res.is_flutter = false;

  if (instance->scene == NULL) {
    return res;
  }

  double nx = 0.0;
  double ny = 0.0;
  struct wlr_scene_node *node = wlr_scene_node_at(&instance->scene->tree.node,
      instance->cursor->x, instance->cursor->y, &nx, &ny);
  res.node = node;
  res.nx = nx;
  res.ny = ny;

  if (node == NULL) {
    // No scene node hit - this means we're over the Flutter shell background
    // Flutter renders everything, so if no Wayland surface is hit, Flutter gets the event
    res.is_flutter = true;
    return res;
  }

  if (node->type == WLR_SCENE_NODE_RECT) {
    res.is_decoration = true;
    res.is_flutter = true;
    res.view = node->data;
    return res;
  }

  if (node->type != WLR_SCENE_NODE_BUFFER) {
    return res;
  }

  struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
  struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
  if (scene_surface == NULL) {
    if (node->data == instance) {
      res.is_flutter = true;
    }
    return res;
  }

  res.surface = scene_surface->surface;

  struct wlr_xdg_surface *xdg_surface = wlr_xdg_surface_try_from_wlr_surface(res.surface);
  if (xdg_surface != NULL) {
    res.view = xdg_surface->data;
  }

  return res;
}

static void send_surface_position(struct fwr_instance *instance, struct fwr_view *view);
static void send_grab_end(struct fwr_instance *instance, struct fwr_view *view);

static void process_cursor_motion(struct fwr_instance *instance, uint32_t time) {
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

  struct hit_test_result hit = hit_test_cursor(instance);

  if (hit.is_flutter) {
    wlr_seat_pointer_clear_focus(instance->seat);

    FlutterPointerPhase phase = instance->input.fl_mouse_button_mask != 0 ? kMove : kHover;
    send_flutter_mouse_event(instance, phase, kFlutterPointerSignalKindNone, 0.0, 0.0);
    return;
  }

  if (hit.surface == NULL) {
    wlr_seat_pointer_clear_focus(instance->seat);
    return;
  }

  wlr_seat_pointer_notify_enter(instance->seat, hit.surface, hit.nx, hit.ny);
  wlr_seat_pointer_notify_motion(instance->seat, time, hit.nx, hit.ny);
  wlr_seat_pointer_notify_frame(instance->seat);
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

  struct hit_test_result hit = hit_test_cursor(instance);

  if (hit.is_flutter) {
    wlr_seat_pointer_clear_focus(instance->seat);

    if (hit.is_decoration && event->state == WL_POINTER_BUTTON_STATE_PRESSED && hit.view != NULL) {
      fwr_focus_view(hit.view);
      if (hit.view->scene_tree != NULL) {
        wlr_scene_node_raise_to_top(&hit.view->scene_tree->node);
      }
    }

    send_flutter_mouse_event(instance,
      event->state == WL_POINTER_BUTTON_STATE_PRESSED ? kDown : kUp,
      kFlutterPointerSignalKindNone,
      0.0,
      0.0);

    wlr_seat_pointer_notify_button(instance->seat, event->time_msec, event->button, event->state);
    wlr_seat_pointer_notify_frame(instance->seat);
    return;
  }

  if (hit.surface != NULL && event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
    if (hit.view != NULL) {
      fwr_focus_view(hit.view);
      if (hit.view->scene_tree != NULL) {
        wlr_scene_node_raise_to_top(&hit.view->scene_tree->node);
      }
    }
  }

  wlr_seat_pointer_notify_button(instance->seat, event->time_msec, event->button, event->state);
  wlr_seat_pointer_notify_frame(instance->seat);
}

static void on_server_cursor_axis(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance = wl_container_of(listener, instance, cursor_axis);
  struct wlr_pointer_axis_event *event = data;

  struct hit_test_result hit = hit_test_cursor(instance);

  if (hit.is_flutter) {
    wlr_seat_pointer_clear_focus(instance->seat);

    double scroll_delta_x = 0.0;
    double scroll_delta_y = 0.0;
    if (event->orientation == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
      scroll_delta_x = -event->delta;
    } else {
      scroll_delta_y = -event->delta;
    }

    send_flutter_mouse_event(instance, kHover, kFlutterPointerSignalKindScroll, scroll_delta_x, scroll_delta_y);

    wlr_seat_pointer_notify_axis(instance->seat, event->time_msec, event->orientation,
      event->delta, event->delta_discrete, event->source, event->relative_direction);
    wlr_seat_pointer_notify_frame(instance->seat);
    return;
  }

  wlr_seat_pointer_notify_axis(instance->seat, event->time_msec, event->orientation,
    event->delta, event->delta_discrete, event->source, event->relative_direction);
  wlr_seat_pointer_notify_frame(instance->seat);
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

  if (event->touch_id >= 10) return;
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

  if (event->touch_id >= 10) return;

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

  if (event->touch_id >= 10) return;
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
    wlr_cursor_set_surface(instance->cursor, event->surface,
                           event->hotspot_x, event->hotspot_y);
  }
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
  wlr_cursor_set_xcursor(instance->cursor, instance->cursor_mgr, "left_ptr");


	instance->seat = wlr_seat_create(instance->wl_display, "seat0");

  instance->request_cursor.notify = on_seat_request_cursor;
  wl_signal_add(&instance->seat->events.request_set_cursor, &instance->request_cursor);

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

struct injected_pointer_state {
  int64_t pointer;
  int64_t buttons;
  bool active;
};

static struct injected_pointer_state injected_pointer_states[16] = {0};

static int64_t injected_get_buttons(int64_t pointer) {
  for (size_t i = 0; i < sizeof(injected_pointer_states) / sizeof(injected_pointer_states[0]); i++) {
    if (injected_pointer_states[i].active && injected_pointer_states[i].pointer == pointer) {
      return injected_pointer_states[i].buttons;
    }
  }
  return 0;
}

static void injected_set_buttons(int64_t pointer, int64_t buttons) {
  for (size_t i = 0; i < sizeof(injected_pointer_states) / sizeof(injected_pointer_states[0]); i++) {
    if (injected_pointer_states[i].active && injected_pointer_states[i].pointer == pointer) {
      injected_pointer_states[i].buttons = buttons;
      return;
    }
  }

  for (size_t i = 0; i < sizeof(injected_pointer_states) / sizeof(injected_pointer_states[0]); i++) {
    if (!injected_pointer_states[i].active) {
      injected_pointer_states[i].active = true;
      injected_pointer_states[i].pointer = pointer;
      injected_pointer_states[i].buttons = buttons;
      return;
    }
  }
}

static void injected_clear_pointer(int64_t pointer) {
  for (size_t i = 0; i < sizeof(injected_pointer_states) / sizeof(injected_pointer_states[0]); i++) {
    if (injected_pointer_states[i].active && injected_pointer_states[i].pointer == pointer) {
      injected_pointer_states[i].active = false;
      injected_pointer_states[i].pointer = 0;
      injected_pointer_states[i].buttons = 0;
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

  struct wlr_surface *surface = view->xdg_surface->surface;
  if (surface == NULL) {
    goto success;
  }

  uint32_t time_msec = (uint32_t)(message.timestamp / 1000);

  // If we temporarily scale the client buffer during interactive resize to
  // match the Flutter widget size, we must also scale pointer coordinates back
  // into the surface's current coordinate space so hit-testing stays correct.
  double local_x = message.local_pos_x;
  double local_y = message.local_pos_y;
  if (message.widget_size_x > 0.0 && message.widget_size_y > 0.0) {
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
    injected_clear_pointer(message.pointer);
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

    int64_t prev_buttons = injected_get_buttons(message.pointer);
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

    injected_set_buttons(message.pointer, next_buttons);
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

