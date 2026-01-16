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

// EGLImage cache entry for triple-buffered DMA-BUF surfaces
#define FWR_EGLIMAGE_CACHE_SIZE 4
struct fwr_eglimage_cache_entry {
  struct wlr_buffer *buffer;  // Source buffer (key)
  void *egl_image;            // EGLImageKHR
  int width;                  // Buffer dimensions (for invalidation on resize)
  int height;
};

// Cached texture for zero-copy DMA-BUF texture sharing
struct fwr_cached_texture {
  GLuint tex;
  GLuint fbo;           // Only used for fallback copy path
  int width;
  int height;
  uint32_t last_seq;    // Surface commit sequence - skip reimport if unchanged
  bool is_external;     // True if texture requires GL_TEXTURE_EXTERNAL_OES
  bool tex_params_set;  // True if texture parameters have been configured

  // EGLImage cache for triple-buffering (avoids recreating EGLImage every frame)
  struct fwr_eglimage_cache_entry egl_cache[FWR_EGLIMAGE_CACHE_SIZE];
  int egl_cache_next;   // Next slot to use (round-robin)
  void *current_egl_image;  // Currently bound EGLImage
};

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
  struct wl_listener new_xdg_popup;

  struct wlr_xdg_decoration_manager_v1 *decoration_manager;
  struct wl_listener new_toplevel_decoration;

  // Legacy KDE server decoration protocol
  struct wlr_server_decoration_manager *legacy_decoration_manager;
  struct wl_listener new_server_decoration;

  struct handle_map *views;
  struct handle_map *subsurfaces;  // Handle map for subsurface texture lookup
  struct handle_map *popups;       // Handle map for popup surfaces
  struct wl_list views_list;
  uint32_t current_focused_view;

  struct wlr_cursor *cursor;
  struct wlr_xcursor_manager *cursor_mgr;
  char *current_xcursor_name;  // Current xcursor name for software rendering
  struct wlr_surface *client_cursor_surface;  // Client-provided cursor surface
  int32_t client_cursor_hotspot_x;
  int32_t client_cursor_hotspot_y;
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
  struct wl_listener request_set_selection;
  struct wl_listener request_set_primary_selection;

  // Direct input mode - bypasses Flutter for low-latency gaming
  bool direct_input_mode;
  uint32_t direct_input_surface;  // Surface handle for direct input

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

  // Geometry offset - where visible content starts within the buffer
  // Used by CSD apps that include shadows in their buffer
  int geo_x;
  int geo_y;

  bool maximized;
  bool fullscreen;
  bool activated;

  // Decoration tracking - true if we successfully negotiated SSD with the client
  struct wlr_xdg_toplevel_decoration_v1 *decoration;
  bool uses_ssd;
  struct wl_listener decoration_request_mode;
  struct wl_listener decoration_destroy;

  // Flutter external texture ID for this surface (same as handle for simplicity)
  int64_t texture_id;
  bool texture_registered;

  struct fwr_cached_texture cache;

  // Subsurface tracking
  struct wl_list subsurfaces;  // List of fwr_subsurface
  struct wl_listener new_subsurface;

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
  struct wl_listener request_move;
  struct wl_listener request_resize;
  struct wl_listener request_minimize;
  struct wl_listener request_maximize;
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

// Subsurface tracking for zero-copy texture approach
struct fwr_subsurface {
  struct wl_list link;  // Link in parent view's subsurface list
  uint32_t handle;      // Unique handle for this subsurface

  struct fwr_view *parent_view;
  struct wlr_subsurface *wlr_subsurface;
  struct wlr_surface *surface;

  int x, y;             // Position relative to parent
  int width, height;

  int64_t texture_id;
  bool texture_registered;

  struct fwr_cached_texture cache;

  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener destroy;
  struct wl_listener commit;
};

// Popup surfaces (menus, dropdowns, tooltips)
struct fwr_popup {
  uint32_t handle;

  struct fwr_instance *instance;
  struct wlr_xdg_popup *xdg_popup;
  struct wlr_xdg_surface *xdg_surface;

  // Parent can be a toplevel view or another popup
  struct fwr_view *parent_view;
  uint32_t parent_view_handle;

  // Position relative to parent (from xdg_positioner)
  int x, y;
  int width, height;

  // Scene tree for wlroots rendering and input handling
  struct wlr_scene_tree *scene_tree;

  int64_t texture_id;
  bool texture_registered;
  bool unconstrained;  // Whether unconstrain has been called

  struct fwr_cached_texture cache;

  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener destroy;
  struct wl_listener commit;
  struct wl_listener reposition;
};
