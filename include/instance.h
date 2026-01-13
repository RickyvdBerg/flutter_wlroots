#pragma once

#include "flutter_embedder.h"

#include <stdatomic.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-server-core.h>

#include "shaders.h"
#include "renderer.h"
#include "input.h"
#include "handle_map.h"
#include "plugin_registry.h"

struct wlr_xdg_decoration_manager_v1;
struct wlr_xdg_toplevel_decoration_v1;
struct wlr_scene;
struct wlr_scene_output_layout;
struct wlr_scene_output;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_output_layout_output;

struct fwr_instance {
  struct wl_display *wl_display;
  struct wl_event_loop *wl_event_loop;
  struct wlr_backend *backend;
  struct wlr_session *session;
  struct wlr_renderer *renderer;
  struct wlr_allocator *allocator;
  struct wlr_presentation *presentation;

  EGLDisplay egl_display;
  EGLContext egl_context;

  const char *wl_socket;

  struct wlr_xdg_shell *xdg_shell;
  struct wl_listener new_xdg_toplevel;

  struct wlr_xdg_decoration_manager_v1 *decoration_manager;
  struct wl_listener new_toplevel_decoration;

  struct handle_map *views;
  struct wl_list views_list;
  uint32_t current_focused_view;

  struct wlr_cursor *cursor;
  struct wlr_xcursor_manager *cursor_mgr;
  struct wl_listener cursor_motion;
  struct wl_listener cursor_motion_absolute;
  struct wl_listener cursor_button;
  struct wl_listener cursor_axis;
  struct wl_listener cursor_frame;
  struct wl_listener cursor_touch_down;
  struct wl_listener cursor_touch_up;
  struct wl_listener cursor_touch_motion;
  struct wl_listener cursor_touch_frame;
  struct wl_listener request_cursor;
  struct fwr_input_state input;

  struct wl_listener new_input;
  struct wl_list keyboards;

  struct wlr_seat *seat;

  struct wlr_output_layout *output_layout;
  struct wlr_scene *scene;
  struct wlr_scene_output_layout *scene_output_layout;
  struct wlr_scene_buffer *flutter_scene_buffer;
  struct fwr_output *output;
  struct wl_listener new_output;

  FlutterEngineProcTable fl_proc_table;
  FlutterEngine engine;
  FlutterCustomTaskRunners custom_task_runners;
  FlutterTaskRunnerDescription platform_task_runner;
  FlutterCompositor fl_compositor;

  pid_t platform_tid;

  int platform_notify_fd;
  struct wl_event_source *platform_notify_event_source;
  struct wl_event_source *platform_timer_event_source;
  pthread_mutex_t platform_task_list_mutex;
  struct fwr_render_task *queued_platform_tasks;

  atomic_intptr_t vsync_baton;

  struct fwr_renderer fwr_renderer;

  struct fwr_plugin_registry plugin_registry;
};

struct fwr_output {
	struct wl_list link;

  struct wlr_output *wlr_output;
  struct wlr_scene_output *scene_output;
  struct wlr_output_layout_output *layout_output;
  struct fwr_instance *instance;

  struct wl_listener frame;
  struct wl_listener request_state;
  struct wl_listener present;
};

struct fwr_view {
  struct wl_list link;
  uint32_t handle;

  struct fwr_instance *instance;
  struct wlr_xdg_toplevel *toplevel;
  struct wlr_xdg_surface *xdg_surface;

  int x;
  int y;
  int width;
  int height;

  bool maximized;
  bool fullscreen;
  bool activated;

  // Flutter external texture ID for this surface (same as handle for simplicity)
  int64_t texture_id;
  bool texture_registered;

  GLuint cached_tex;
  GLuint cached_fbo;

  struct wlr_scene_tree *scene_tree;
  struct wlr_scene_tree *scene_xdg_tree;
  struct wlr_scene_rect *scene_frame;
  struct wlr_scene_rect *scene_titlebar;

  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener destroy;
  struct wl_listener commit;
  struct wl_listener set_title;
  struct wl_listener set_app_id;
};

struct fwr_keyboard {
  struct wl_list link;
  struct fwr_instance *instance;
  struct wlr_keyboard *keyboard;
  struct xkb_compose_state *compose_state;

  struct wl_listener modifiers;
  struct wl_listener key;
  struct wl_listener destroy;
};
