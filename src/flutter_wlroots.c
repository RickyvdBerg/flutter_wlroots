#include "flutter_embedder.h"
#include "renderer.h"
#include "standard_message_codec.h"

#include <bits/pthreadtypes.h>
#include <bits/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <stdatomic.h>

#include <string.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>
#include <wayland-server-core.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>


#include <wlr/backend.h>
#include <wlr/util/log.h>
#include <wlr/render/gles2.h>
#include <wlr/render/egl.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/render/dmabuf.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/edges.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_data_control_v1.h>

#include "wlroots_hacks.h"

#include "flutter_wlroots.h"
#include "instance.h"
#include "shaders.h"
#include "input.h"
#include "task.h"
#include "output.h"
#include "surface.h"
#include "platform_channel.h"
#include "text_input.h"
#include "cursor.h"

//#define eglGetProcAddr eglGetProcAddress
//#define __glintercept_log(...) wlr_log(WLR_INFO, __VA_ARGS__)
//#include "gl_intercept_debug.h"

#define GL_ASSERT_ERROR(instance) do {\
  GLenum err = instance->glGetError();\
  if (err != GL_NO_ERROR) {\
    wlr_log(WLR_ERROR, "GL ERROR: %d", err);\
  }\
} while (0)

// Handler for decoration mode request - client wants to change decoration mode
static void handle_decoration_request_mode(struct wl_listener *listener, void *data) {
  struct fwr_view *view =
      wl_container_of(listener, view, decoration_request_mode);

  wlr_log(WLR_INFO, "=== DECORATION MODE REQUEST from client (requested=%d) ===",
          view->decoration->requested_mode);

  // Always enforce server-side decorations regardless of what client wants
  wlr_xdg_toplevel_decoration_v1_set_mode(view->decoration,
      WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

  wlr_log(WLR_INFO, "Enforced SSD mode for view %d", view->handle);
}

// Handler for decoration destroy - clean up when decoration is destroyed
static void handle_decoration_destroy(struct wl_listener *listener, void *data) {
  struct fwr_view *view =
      wl_container_of(listener, view, decoration_destroy);

  wlr_log(WLR_INFO, "Decoration destroyed for view %d", view->handle);

  wl_list_remove(&view->decoration_request_mode.link);
  wl_list_remove(&view->decoration_destroy.link);
  view->decoration = NULL;
}

// Handler for new xdg-decoration requests - tell apps to use server-side decorations
static void handle_new_toplevel_decoration(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance =
      wl_container_of(listener, instance, new_toplevel_decoration);
  struct wlr_xdg_toplevel_decoration_v1 *decoration = data;

  wlr_log(WLR_INFO, "=== XDG DECORATION REQUEST from client ===");

  // Find the view that owns this toplevel
  struct fwr_view *view = NULL;
  struct fwr_view *v;
  wl_list_for_each(v, &instance->views_list, link) {
    if (v->toplevel == decoration->toplevel) {
      view = v;
      break;
    }
  }

  if (view != NULL) {
    view->decoration = decoration;
    view->uses_ssd = true;

    // Listen for mode change requests and destroy
    view->decoration_request_mode.notify = handle_decoration_request_mode;
    wl_signal_add(&decoration->events.request_mode, &view->decoration_request_mode);

    view->decoration_destroy.notify = handle_decoration_destroy;
    wl_signal_add(&decoration->events.destroy, &view->decoration_destroy);

    // Tell client its top edge is "tiled" - this signals it should NOT render
    // rounded corners at the top (since our SSD decoration is there)
    wlr_xdg_toplevel_set_tiled(view->toplevel, WLR_EDGE_TOP);

    wlr_log(WLR_INFO, "Linked XDG decoration to view %d, enforcing SSD + tiled top", view->handle);

    // Notify Flutter of decoration change (fixes timing issue where surface_map
    // might be sent before decoration negotiation completes)
    fwr_send_decoration_update(view);
  }

  // Tell the client to use server-side decorations (we provide the title bar)
  wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
      WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

  wlr_log(WLR_INFO, "Set SSD mode for surface");
}

// Handler for legacy KDE server decoration protocol (used by GTK3, Firefox, etc.)
static void handle_new_server_decoration(struct wl_listener *listener, void *data) {
  struct fwr_instance *instance =
      wl_container_of(listener, instance, new_server_decoration);
  struct wlr_server_decoration *decoration = data;

  wlr_log(WLR_INFO, "=== LEGACY KDE DECORATION from client (surface=%p, mode=%d) ===",
          decoration->surface, decoration->mode);

  // The default mode is already SERVER, but let's find and mark the view
  // The decoration->surface is the wlr_surface, we need to find the view that owns it
  struct fwr_view *view;
  wl_list_for_each(view, &instance->views_list, link) {
    if (view->xdg_surface && view->xdg_surface->surface == decoration->surface) {
      view->uses_ssd = true;

      // Tell client its top edge is "tiled" - no rounded corners at top
      wlr_xdg_toplevel_set_tiled(view->toplevel, WLR_EDGE_TOP);

      wlr_log(WLR_INFO, "Linked legacy decoration to view %d, mode=%d (SERVER=2), tiled top",
              view->handle, decoration->mode);

      // Notify Flutter of decoration change
      fwr_send_decoration_update(view);
      break;
    }
  }
}

static bool engine_cb_renderer_make_current(void *user_data) {
  struct fwr_instance *instance = user_data;

  eglMakeCurrent(instance->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, instance->fwr_renderer.flutter_egl_context);

  return true;
}
static bool engine_cb_renderer_clear_current(void *user_data) {
  struct fwr_instance *instance = user_data;
  //wlr_egl_unset_current(instance->egl);

  eglMakeCurrent(instance->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

  //wlr_log(WLR_DEBUG, "engine_cb_renderer_clear_current");
  return true;
}

static bool engine_cb_renderer_make_resource_current(void *user_data) {
  struct fwr_instance *instance = user_data;
  eglMakeCurrent(instance->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, instance->fwr_renderer.flutter_resource_egl_context);
  return true;
}

static void* engine_cb_renderer_gl_proc_resolve(void *user_data, const char *name) {
  return eglGetProcAddress(name);
}

// Helper function to provide texture for a wlr_surface
// Two paths: DMA-BUF (preferred, zero-copy) or wlroots texture (fallback for SHM)
//
// Clean separation: Once buffer type is detected, we use only that path.
// This avoids overhead from repeatedly trying DMA-BUF for SHM surfaces.
static bool provide_surface_texture(struct fwr_instance *instance,
                                    struct wlr_surface *surface,
                                    struct fwr_cached_texture *cache,
                                    size_t requested_width,
                                    size_t requested_height,
                                    FlutterOpenGLTexture *texture_out) {
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

  // Safety check for destroyed cache
  if (cache->destroyed) {
    return false;
  }

  // Get actual buffer dimensions (the real texture size)
  size_t actual_width = surface->current.buffer_width;
  size_t actual_height = surface->current.buffer_height;

  // Fallback to surface dimensions if buffer dimensions not available
  if (actual_width == 0) actual_width = surface->current.width;
  if (actual_height == 0) actual_height = surface->current.height;

  // If dimensions are still 0, use cached fallback or fail
  if (actual_width == 0 || actual_height == 0) {
    wlr_log(WLR_DEBUG, "Zero dimensions: buffer=%dx%d surface=%dx%d, using cache",
            surface->current.buffer_width, surface->current.buffer_height,
            surface->current.width, surface->current.height);
    if (cache->tex != 0 && cache->width > 0 && cache->height > 0) {
      texture_out->target = GL_TEXTURE_2D;
      texture_out->name = cache->tex;
      texture_out->format = GL_RGBA8;
      texture_out->user_data = NULL;
      texture_out->destruction_callback = NULL;
      texture_out->width = cache->width;
      texture_out->height = cache->height;
      return true;
    }
    return false;
  }

  // ========== DMA-BUF PATH ==========
  // Only try DMA-BUF if we haven't determined it's SHM
  if (cache->buffer_type != FWR_BUFFER_SHM) {
    if (fwr_renderer_import_surface_dmabuf(instance, surface, cache)) {
      // Mark as DMA-BUF surface for future frames
      cache->buffer_type = FWR_BUFFER_DMABUF;

      texture_out->target = cache->is_external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
      texture_out->name = cache->tex;
      texture_out->format = GL_RGBA8;
      texture_out->user_data = NULL;
      texture_out->destruction_callback = NULL;
      texture_out->width = actual_width;
      texture_out->height = actual_height;
      return true;
    }

    // DMA-BUF import failed - check if this is an SHM surface
    // wlr_buffer_get_dmabuf returns false for SHM buffers
    struct wlr_client_buffer *client_buffer = surface->buffer;
    if (client_buffer != NULL && client_buffer->source != NULL) {
      struct wlr_dmabuf_attributes dmabuf_attribs;
      if (!wlr_buffer_get_dmabuf(client_buffer->source, &dmabuf_attribs)) {
        // Confirmed SHM surface - mark it so we skip DMA-BUF next time
        if (cache->buffer_type == FWR_BUFFER_UNKNOWN) {
          cache->buffer_type = FWR_BUFFER_SHM;
          wlr_log(WLR_INFO, "Surface detected as SHM, will use wlroots texture path");
        }
      }
    }
  }

  // ========== SHM/WLROOTS PATH ==========
  // For SHM buffers, use wlroots' texture (which handles SHM upload to GL)

  // Clean up any stale DMA-BUF resources if we switched from DMA-BUF to SHM
  if (cache->current_egl_image != NULL) {
    struct fwr_renderer *renderer = &instance->fwr_renderer;
    pthread_mutex_lock(&renderer->texture_mutex);
    if (renderer->eglDestroyImageKHR) {
      for (int i = 0; i < FWR_EGLIMAGE_CACHE_SIZE; i++) {
        if (cache->egl_cache[i].egl_image != NULL) {
          renderer->eglDestroyImageKHR(instance->egl_display, cache->egl_cache[i].egl_image);
          cache->egl_cache[i].egl_image = NULL;
          cache->egl_cache[i].buffer = NULL;
        }
      }
    }
    cache->current_egl_image = NULL;
    if (cache->tex != 0) {
      renderer->fns.glDeleteTextures(1, &cache->tex);
      cache->tex = 0;
    }
    pthread_mutex_unlock(&renderer->texture_mutex);
  }

  // Get wlroots' texture
  struct wlr_texture *wlr_tex = wlr_surface_get_texture(surface);
  if (wlr_tex == NULL) {
    // No texture available - use cached fallback to prevent black frames
    if (cache->tex != 0 && cache->width > 0 && cache->height > 0) {
      wlr_log(WLR_DEBUG, "wlr_texture NULL, using cached %dx%d (requested %zux%zu)",
              cache->width, cache->height, requested_width, requested_height);
      texture_out->target = GL_TEXTURE_2D;
      texture_out->name = cache->tex;
      texture_out->format = GL_RGBA8;
      texture_out->user_data = NULL;
      texture_out->destruction_callback = NULL;
      texture_out->width = cache->width;
      texture_out->height = cache->height;
      return true;
    }
    wlr_log(WLR_DEBUG, "wlr_texture NULL, no cache - returning false");
    return false;
  }

  struct wlr_gles2_texture_attribs attribs;
  wlr_gles2_texture_get_attribs(wlr_tex, &attribs);

  // Check if the texture ID is valid
  if (attribs.tex == 0) {
    wlr_log(WLR_DEBUG, "wlr_texture has tex=0, using cached fallback");
    if (cache->tex != 0 && cache->width > 0 && cache->height > 0) {
      texture_out->target = GL_TEXTURE_2D;
      texture_out->name = cache->tex;
      texture_out->format = GL_RGBA8;
      texture_out->user_data = NULL;
      texture_out->destruction_callback = NULL;
      texture_out->width = cache->width;
      texture_out->height = cache->height;
      return true;
    }
    return false;
  }

  // Log dimension mismatches during resize (helps debug flickering)
  if (requested_width > 0 && requested_height > 0 &&
      (requested_width != actual_width || requested_height != actual_height)) {
    static int mismatch_count = 0;
    if (mismatch_count++ % 30 == 0) {
      wlr_log(WLR_DEBUG, "SHM resize: requested=%zux%zu actual=%zux%zu cache=%dx%d",
              requested_width, requested_height, actual_width, actual_height,
              cache->width, cache->height);
    }
  }

  // Copy wlroots texture to our cached texture
  // This ensures we have a stable texture during resize transitions
  GLuint copied_tex = fwr_renderer_copy_texture(instance, attribs.tex, attribs.target,
                                                 actual_width, actual_height, cache);
  if (copied_tex == 0) {
    // Copy failed - try cached fallback
    wlr_log(WLR_DEBUG, "texture copy failed, trying cached fallback");
    if (cache->tex != 0 && cache->width > 0 && cache->height > 0) {
      texture_out->target = GL_TEXTURE_2D;
      texture_out->name = cache->tex;
      texture_out->format = GL_RGBA8;
      texture_out->user_data = NULL;
      texture_out->destruction_callback = NULL;
      texture_out->width = cache->width;
      texture_out->height = cache->height;
      return true;
    }
    wlr_log(WLR_DEBUG, "no cached fallback available");
    return false;
  }

  texture_out->target = GL_TEXTURE_2D;
  texture_out->name = copied_tex;
  texture_out->format = GL_RGBA8;
  texture_out->user_data = NULL;
  texture_out->destruction_callback = NULL;
  texture_out->width = actual_width;
  texture_out->height = actual_height;

  // Log every texture provision during active resize (dimension mismatch)
  if (requested_width > 0 && requested_height > 0 &&
      (requested_width != actual_width || requested_height != actual_height)) {
    static int frame_count = 0;
    wlr_log(WLR_DEBUG, "FRAME %d: provided tex=%u %zux%zu for widget %zux%zu",
            frame_count++, copied_tex, actual_width, actual_height,
            requested_width, requested_height);
  }

  return true;
}

static bool engine_cb_external_texture(void *user_data, int64_t texture_id, size_t width, size_t height, FlutterOpenGLTexture *texture_out) {
  struct fwr_instance *instance = user_data;

  // First, try to find a view (toplevel surface)
  struct fwr_view *view = NULL;
  if (handle_map_get(instance->views, (uint32_t)texture_id, (void**)&view)) {
    if (view->xdg_surface == NULL || view->xdg_surface->surface == NULL) {
      wlr_log(WLR_DEBUG, "texture_id=%ld: view surface NULL", texture_id);
      return false;
    }
    // Pass Flutter's requested dimensions for resize smoothing
    bool result = provide_surface_texture(instance, view->xdg_surface->surface,
                                    &view->cache, width, height, texture_out);
    if (!result) {
      wlr_log(WLR_DEBUG, "texture_id=%ld: provide_surface_texture returned false", texture_id);
    }
    return result;
  }

  // If not a view, try to find a subsurface
  // Subsurface texture IDs are offset by 100000 to avoid collision with view IDs
  struct fwr_subsurface *sub = NULL;
  if (texture_id >= 100000 && texture_id < 200000) {
    uint32_t sub_handle = (uint32_t)(texture_id - 100000);
    if (handle_map_get(instance->subsurfaces, sub_handle, (void**)&sub)) {
      if (sub->surface == NULL) {
        return false;
      }
      // Subsurfaces use actual size (no resize smoothing needed)
      return provide_surface_texture(instance, sub->surface,
                                     &sub->cache, 0, 0, texture_out);
    }
  }

  // If not a subsurface, try to find a popup
  // Popup texture IDs are offset by 200000 to avoid collision
  struct fwr_popup *popup = NULL;
  if (texture_id >= 200000) {
    uint32_t popup_handle = (uint32_t)(texture_id - 200000);
    if (handle_map_get(instance->popups, popup_handle, (void**)&popup)) {
      if (popup->xdg_surface == NULL || popup->xdg_surface->surface == NULL) {
        return false;
      }
      struct wlr_surface *surf = popup->xdg_surface->surface;

      // Check for subsurfaces - Firefox/browsers render popup content to subsurfaces
      struct wlr_subsurface *first_subsurface = NULL;
      struct wlr_subsurface *subsurface;
      wl_list_for_each(subsurface, &surf->current.subsurfaces_below, current.link) {
        if (first_subsurface == NULL) first_subsurface = subsurface;
      }
      wl_list_for_each(subsurface, &surf->current.subsurfaces_above, current.link) {
        if (first_subsurface == NULL) first_subsurface = subsurface;
      }

      // Use subsurface if available (Firefox renders content there)
      struct wlr_surface *content_surface = surf;
      if (first_subsurface != NULL && first_subsurface->surface != NULL) {
        content_surface = first_subsurface->surface;
      }

      if (wlr_surface_get_texture(content_surface) == NULL) {
        return false;
      }

      // Popups use actual size (no resize smoothing needed)
      bool result = provide_surface_texture(instance, content_surface,
                                            &popup->cache, 0, 0, texture_out);
      
      // Keep requesting updates to catch subsurface content changes
      if (result && popup->texture_registered) {
        instance->fl_proc_table.MarkExternalTextureFrameAvailable(
            instance->engine, popup->texture_id);
      }
      return result;
    }
  }

  return false;
}

static uint32_t engine_cb_renderer_fbo(void *user_data, const FlutterFrameInfo *frame_info) {
#ifdef FLUTTER_COMPOSITOR
  wlr_log(WLR_ERROR, "engine_cb_renderer_fbo called!");
  return 0;
#else // FLUTTER_COMPOSITOR
  struct fwr_instance *instance = user_data;

  GLuint fbo = fwr_renderer_get_active_fbo(instance);
  wlr_log(WLR_INFO, "==== FRAME EVENT ==== Engine given fbo: %d", fbo);

  return fbo;
#endif // FLUTTER_COMPOSITOR
}
static bool engine_cb_renderer_present(void *user_data, const FlutterPresentInfo *present_info) {
#ifdef FLUTTER_COMPOSITOR
  wlr_log(WLR_ERROR, "engine_cb_renderer_present called!");
  return false;
#else // FLUTTER_COMPOSITOR
  struct fwr_instance *instance = user_data;
  wlr_log(WLR_INFO, "==== FRAME EVENT ==== Engine called present with FBO: %d", present_info->fbo_id);

  fwr_renderer_flip_fbo(instance);

  return true;
#endif // FLUTTER_COMPOSITOR
}

static void engine_cb_platform_message(
    const FlutterPlatformMessage *engine_message,
    void *user_data) {
  struct fwr_instance *instance = user_data;

  if (engine_message->struct_size != sizeof(FlutterPlatformMessage)) {
    wlr_log(WLR_ERROR, "Invalid platform message size received. Expected %ld but received %ld", sizeof(FlutterPlatformMessage), engine_message->struct_size);
    return;
  }

  struct dart_value name = {};
  struct dart_value args = {};

  if (strcmp(engine_message->channel, "wlroots") == 0) {
    size_t offset = 0;

    if (!message_read(engine_message->message, engine_message->message_size, &offset, &name)) {
      wlr_log(WLR_ERROR, "Error decoding platform message name");
      goto error;
    }

    if (!message_read(engine_message->message, engine_message->message_size, &offset, &args)) {
      wlr_log(WLR_ERROR, "Error decoding platform message args");
      goto error;
    }

    if (name.type != dvString) {
      goto error;
    }
    const char *method_name = name.string.string;

    if (strcmp(method_name, "surface_toplevel_set_size") == 0) {
      fwr_handle_surface_toplevel_set_size(instance, engine_message->response_handle, &args);
      return;
    }
    if (strcmp(method_name, "surface_toplevel_set_maximized") == 0) {
      fwr_handle_surface_toplevel_set_maximized(instance, engine_message->response_handle, &args);
      return;
    }
    if (strcmp(method_name, "surface_toplevel_close") == 0) {
      fwr_handle_surface_toplevel_close(instance, engine_message->response_handle, &args);
      return;
    }
    if (strcmp(method_name, "surface_focus") == 0) {
      fwr_handle_surface_focus(instance, engine_message->response_handle, &args);
      return;
    }
    if (strcmp(method_name, "surface_begin_move") == 0) {
      fwr_handle_surface_begin_move(instance, engine_message->response_handle, &args);
      return;
    }
    if (strcmp(method_name, "surface_begin_resize") == 0) {
      fwr_handle_surface_begin_resize(instance, engine_message->response_handle, &args);
      return;
    }
    if (strcmp(method_name, "surface_set_position") == 0) {
      fwr_handle_surface_set_position(instance, engine_message->response_handle, &args);
      return;
    }
    if (strcmp(method_name, "surface_request_resize") == 0) {
      fwr_handle_surface_request_resize(instance, engine_message->response_handle, &args);
      return;
    }
    if (strcmp(method_name, "surface_end_resize") == 0) {
      fwr_handle_surface_end_resize(instance, engine_message->response_handle, &args);
      return;
    }
    if (strcmp(method_name, "surface_pointer_event") == 0) {
      fwr_handle_surface_pointer_event_message(instance, engine_message->response_handle, &args);
      return;
    }
    if (strcmp(method_name, "popup_pointer_event") == 0) {
      wlr_log(WLR_DEBUG, "Routing popup_pointer_event message");
      fwr_handle_popup_pointer_event_message(instance, engine_message->response_handle, &args);
      return;
    }
    if (strcmp(method_name, "surface_keyboard_key") == 0) {
      fwr_handle_surface_keyboard_key_message(instance, engine_message->response_handle, &args);
      return;
    }
    if (strcmp(method_name, "surface_clear_focus") == 0) {
      wlr_seat_pointer_clear_focus(instance->seat);
      wlr_seat_keyboard_clear_focus(instance->seat);
      instance->fl_proc_table.SendPlatformMessageResponse(
          instance->engine, engine_message->response_handle,
          method_call_null_success, sizeof(method_call_null_success));
      return;
    }

    // Direct input mode - bypass Flutter for low-latency gaming
    if (strcmp(method_name, "set_direct_input_mode") == 0) {
      if (args.type == dvList && args.list.length >= 2) {
        bool enabled = args.list.values[0].type == dvBool && args.list.values[0].boolean;
        uint32_t surface_handle = 0;
        if (args.list.values[1].type == dvInteger) {
          surface_handle = (uint32_t)args.list.values[1].integer;
        }
        instance->direct_input_mode = enabled;
        instance->direct_input_surface = surface_handle;
        wlr_log(WLR_INFO, "Direct input mode: %s for surface %u",
                enabled ? "enabled" : "disabled", surface_handle);
      }
      instance->fl_proc_table.SendPlatformMessageResponse(
          instance->engine, engine_message->response_handle,
          method_call_null_success, sizeof(method_call_null_success));
      return;
    }

    if (strcmp(method_name, "is_compositor") == 0) {
      // Just send a success response.
      instance->fl_proc_table.SendPlatformMessageResponse(
          instance->engine, engine_message->response_handle,
          method_call_null_success, sizeof(method_call_null_success));
      return;
    }

    // Dart signals it's ready to receive messages - send all existing outputs
    if (strcmp(method_name, "compositor_ready") == 0) {
      wlr_log(WLR_INFO, "Dart compositor ready, sending %d existing outputs",
              wl_list_length(&instance->outputs));
      fwr_send_all_outputs(instance);
      instance->fl_proc_table.SendPlatformMessageResponse(
          instance->engine, engine_message->response_handle,
          method_call_null_success, sizeof(method_call_null_success));
      return;
    }

    if (strcmp(method_name, "get_socket_paths") == 0) {
      platch_respond_success_std(instance, (FlutterPlatformMessageResponseHandle *)engine_message->response_handle, &(struct std_value) {
        .type = kStdMap,
        .size = 2,
        .keys = (struct std_value[2]) {
          {
            .type = kStdString,
            .string_value = "wayland"
          },
          {
            .type = kStdString,
            .string_value = "x"
          }
        },
        .values = (struct std_value[2]) {
          {
            .type = kStdString,
            .string_value = (char*) instance->wl_socket,
          },
          {
            .type = kStdString,
            .string_value = "",
          }
        },
      });
      return;
    }

    // Multi-monitor configuration commands
    if (strcmp(method_name, "set_vsync_output") == 0) {
      if (args.type == dvList && args.list.length >= 1) {
        uint32_t output_id = 0;
        if (args.list.values[0].type == dvInteger) {
          output_id = (uint32_t)args.list.values[0].integer;
        }
        fwr_set_vsync_output(instance, output_id);
      }
      instance->fl_proc_table.SendPlatformMessageResponse(
          instance->engine, engine_message->response_handle,
          method_call_null_success, sizeof(method_call_null_success));
      return;
    }

    if (strcmp(method_name, "set_vsync_rate_limit") == 0) {
      if (args.type == dvList && args.list.length >= 1) {
        int max_hz = 0;
        if (args.list.values[0].type == dvInteger) {
          max_hz = (int)args.list.values[0].integer;
        }
        fwr_set_vsync_rate_limit(instance, max_hz);
      }
      instance->fl_proc_table.SendPlatformMessageResponse(
          instance->engine, engine_message->response_handle,
          method_call_null_success, sizeof(method_call_null_success));
      return;
    }

    if (strcmp(method_name, "set_output_mode") == 0) {
      bool success = false;
      if (args.type == dvList && args.list.length >= 4) {
        uint32_t output_id = 0;
        int width = 0, height = 0, refresh = 0;
        if (args.list.values[0].type == dvInteger) {
          output_id = (uint32_t)args.list.values[0].integer;
        }
        if (args.list.values[1].type == dvInteger) {
          width = (int)args.list.values[1].integer;
        }
        if (args.list.values[2].type == dvInteger) {
          height = (int)args.list.values[2].integer;
        }
        if (args.list.values[3].type == dvInteger) {
          refresh = (int)args.list.values[3].integer;
        }
        success = fwr_set_output_mode(instance, output_id, width, height, refresh);
      }
      if (success) {
        instance->fl_proc_table.SendPlatformMessageResponse(
            instance->engine, engine_message->response_handle,
            method_call_null_success, sizeof(method_call_null_success));
      } else {
        platch_respond_error_std(instance,
            (FlutterPlatformMessageResponseHandle *)engine_message->response_handle,
            "error", "Failed to set output mode", NULL);
      }
      return;
    }

    if (strcmp(method_name, "set_output_position") == 0) {
      bool success = false;
      if (args.type == dvList && args.list.length >= 3) {
        uint32_t output_id = 0;
        int x = 0, y = 0;
        if (args.list.values[0].type == dvInteger) {
          output_id = (uint32_t)args.list.values[0].integer;
        }
        if (args.list.values[1].type == dvInteger) {
          x = (int)args.list.values[1].integer;
        }
        if (args.list.values[2].type == dvInteger) {
          y = (int)args.list.values[2].integer;
        }
        success = fwr_set_output_position(instance, output_id, x, y);
      }
      if (success) {
        instance->fl_proc_table.SendPlatformMessageResponse(
            instance->engine, engine_message->response_handle,
            method_call_null_success, sizeof(method_call_null_success));
      } else {
        platch_respond_error_std(instance,
            (FlutterPlatformMessageResponseHandle *)engine_message->response_handle,
            "error", "Failed to set output position", NULL);
      }
      return;
    }

    if (strcmp(method_name, "set_output_scale") == 0) {
      bool success = false;
      if (args.type == dvList && args.list.length >= 2) {
        uint32_t output_id = 0;
        double scale = 1.0;
        if (args.list.values[0].type == dvInteger) {
          output_id = (uint32_t)args.list.values[0].integer;
        }
        if (args.list.values[1].type == dvFloat64) {
          scale = args.list.values[1].f64;
        } else if (args.list.values[1].type == dvInteger) {
          scale = (double)args.list.values[1].integer;
        }
        success = fwr_set_output_scale(instance, output_id, scale);
      }
      if (success) {
        instance->fl_proc_table.SendPlatformMessageResponse(
            instance->engine, engine_message->response_handle,
            method_call_null_success, sizeof(method_call_null_success));
      } else {
        platch_respond_error_std(instance,
            (FlutterPlatformMessageResponseHandle *)engine_message->response_handle,
            "error", "Failed to set output scale", NULL);
      }
      return;
    }

    if (strcmp(method_name, "set_primary_output") == 0) {
      // Primary output is tracked on the Dart side only
      instance->fl_proc_table.SendPlatformMessageResponse(
          instance->engine, engine_message->response_handle,
          method_call_null_success, sizeof(method_call_null_success));
      return;
    }

    wlr_log(WLR_INFO, "Unhandled platform message: channel: %s %s", engine_message->channel, method_name);
    goto error;
  }

  // Forward to channel handler plugins.
  for (int i = 0; i < instance->plugin_registry.plugin_channel_handlers_len; i++) {
    struct fwr_plugin_channel_handler *channel_handler = &instance->plugin_registry.plugin_channel_handlers[i];
    if (strcmp(engine_message->channel, channel_handler->name) == 0) {
      if (channel_handler->handle_message(instance, engine_message, channel_handler->data)) {
        return;
      } else {
        goto error;
      }
    }
  }


error:
  // TODO(hansihe): Handle messages
  wlr_log(WLR_INFO, "Unhandled platform message: channel: %s", engine_message->channel);

  message_free(&name);
  message_free(&args);

  // Send failure
  instance->fl_proc_table.SendPlatformMessageResponse(instance->engine, engine_message->response_handle, NULL, 0);
}

int quit_ctr = 0;

static void engine_cb_log_message(const char *tag, const char *message, void *user_data) {
  wlr_log(WLR_INFO, "DART [%s] %s", tag, message);
}

bool fwr_instance_create(struct fwr_instance_opts opts, struct fwr_instance **instance_out) {
  wlr_log_init(WLR_DEBUG, NULL);

  struct fwr_instance *instance = calloc(1, sizeof(struct fwr_instance));

  pthread_mutexattr_t mutex_attr;
  pthread_mutexattr_init(&mutex_attr);
  pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
  if (pthread_mutex_init(&instance->platform_task_list_mutex, &mutex_attr) != 0) {
    wlr_log(WLR_ERROR, "Could not init render task list mutex");
  }

  instance->fl_proc_table.struct_size = sizeof(FlutterEngineProcTable);
  if (FlutterEngineGetProcAddresses(&instance->fl_proc_table) != kSuccess) {
    wlr_log(WLR_ERROR, "Could not get engine proc table");
    return false;
  }

  instance->wl_display = wl_display_create();
  instance->wl_event_loop = wl_display_get_event_loop(instance->wl_display);

	instance->backend = wlr_backend_autocreate(instance->wl_event_loop, &instance->session);

  //server->renderer = wlr_renderer_autocreate(server->backend);

  int drm_fd = -1;

  // TODO(hansihe): allow selection of DRM node by env variable?
  // https://gitlab.freedesktop.org/wlroots/wlroots/-/blob/master/render/wlr_renderer.c#L369

  if (drm_fd < 0) {
    drm_fd = wlr_backend_get_drm_fd(instance->backend);
  }

  if (drm_fd < 0) {
    wlr_log(WLR_ERROR, "Could not open render node!");
    return false;
  }

  struct wlr_egl *egl = NULL;
  egl = wlr_egl_create_with_drm_fd(drm_fd);
  if (egl == NULL) {
    wlr_log(WLR_ERROR, "Failed to create EGL");
    return false;
  }

  struct wlr_renderer *renderer = NULL;
  renderer = wlr_gles2_renderer_create(egl);
  if (renderer == NULL) {
    wlr_log(WLR_ERROR, "Failed to create GLES2 renderer");
    wlr_egl_destroy(egl);
    return false;
  }
  instance->renderer = renderer;

  // wlr_egl is opaque in 0.18 - extract EGL display/context via accessors
  instance->egl_display = wlr_egl_get_display(egl);
  instance->egl_context = wlr_egl_get_context(egl);

  wlr_renderer_init_wl_display(instance->renderer, instance->wl_display);

  instance->allocator = wlr_allocator_autocreate(instance->backend, instance->renderer);

  wlr_compositor_create(instance->wl_display, 6, instance->renderer);
  wlr_subcompositor_create(instance->wl_display);  // Required by Firefox and other browsers
  wlr_data_device_manager_create(instance->wl_display);

  // Primary selection (middle-click paste) - essential for Linux workflow
  wlr_primary_selection_v1_device_manager_create(instance->wl_display);

  // Data control - clipboard access for wl-copy/wl-paste and some terminal emulators
  wlr_data_control_manager_v1_create(instance->wl_display);

  // Idle notification - lets apps know when user is idle (for screen lock, power management)
  wlr_idle_notifier_v1_create(instance->wl_display);

  // Idle inhibit - allows apps to prevent idle (video playback, presentations)
  wlr_idle_inhibit_v1_create(instance->wl_display);

  // Screencopy - enables screenshots (grim) and screen recording
  wlr_screencopy_manager_v1_create(instance->wl_display);

  instance->output_layout = wlr_output_layout_create(instance->wl_display);
  instance->scene = wlr_scene_create();
  instance->scene_output_layout = wlr_scene_attach_output_layout(instance->scene, instance->output_layout);
  instance->flutter_scene_buffer = NULL;

  // Initialize multi-output support
  wl_list_init(&instance->outputs);
  instance->vsync_output = NULL;
  instance->next_output_id = 0;
  instance->vsync_rate_limit = 0;

  // XDG output manager - provides output info to clients (needed by grim, etc.)
  wlr_xdg_output_manager_v1_create(instance->wl_display, instance->output_layout);

  // Export DMA-BUF - enables screen sharing (OBS, Discord, Zoom)
  wlr_export_dmabuf_manager_v1_create(instance->wl_display);

  instance->new_output.notify = fwr_server_new_output;
  wl_signal_add(&instance->backend->events.new_output, &instance->new_output);

  instance->xdg_shell = wlr_xdg_shell_create(instance->wl_display, 3);
  instance->new_xdg_toplevel.notify = fwr_new_xdg_toplevel;
  wl_signal_add(&instance->xdg_shell->events.new_toplevel, &instance->new_xdg_toplevel);

  // Handle popup surfaces (menus, dropdowns, tooltips)
  instance->new_xdg_popup.notify = fwr_new_xdg_popup;
  wl_signal_add(&instance->xdg_shell->events.new_popup, &instance->new_xdg_popup);

  // xdg-decoration: tell apps to use server-side decorations (we provide title bars)
  instance->decoration_manager = wlr_xdg_decoration_manager_v1_create(instance->wl_display);
  instance->new_toplevel_decoration.notify = handle_new_toplevel_decoration;
  wl_signal_add(&instance->decoration_manager->events.new_toplevel_decoration, &instance->new_toplevel_decoration);

  // Legacy KDE server decoration protocol - set default mode to SERVER
  // This tells older clients (GTK3, some Qt, Firefox) that we prefer server-side decorations
  instance->legacy_decoration_manager =
      wlr_server_decoration_manager_create(instance->wl_display);
  wlr_server_decoration_manager_set_default_mode(instance->legacy_decoration_manager,
      WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
  instance->new_server_decoration.notify = handle_new_server_decoration;
  wl_signal_add(&instance->legacy_decoration_manager->events.new_decoration,
      &instance->new_server_decoration);
  wlr_log(WLR_INFO, "Enabled KDE server decoration protocol with SERVER mode default");

  fwr_input_init(instance);

  const char *socket = wl_display_add_socket_auto(instance->wl_display);
	if (!socket) {
    wlr_log(WLR_ERROR, "Failed to create Wayland socket");
		wlr_backend_destroy(instance->backend);
    return false;
	}
  wlr_log(WLR_INFO, "Wayland socket: %s", socket);
  instance->wl_socket = socket;

  if (!wlr_backend_start(instance->backend)) {
		wlr_backend_destroy(instance->backend);
		wl_display_destroy(instance->wl_display);
		return false;
	}

  instance->views = handle_map_new();
  instance->subsurfaces = handle_map_new();
  instance->popups = handle_map_new();
  wl_list_init(&instance->views_list);

  fwr_renderer_init(instance, eglGetProcAddress);
  //fwr_renderer_ensure_fbo(instance, 300, 300);

  instance->presentation = wlr_presentation_create(instance->wl_display, instance->backend);

  fwr_tasks_init(instance);

  FlutterRendererConfig renderer_config = {};
  renderer_config.type = kOpenGL;
  renderer_config.open_gl.struct_size = sizeof(FlutterOpenGLRendererConfig);
  renderer_config.open_gl.make_current = engine_cb_renderer_make_current;
  renderer_config.open_gl.clear_current = engine_cb_renderer_clear_current;
  renderer_config.open_gl.make_resource_current = engine_cb_renderer_make_resource_current;
  renderer_config.open_gl.fbo_reset_after_present = true;
  renderer_config.open_gl.gl_proc_resolver = engine_cb_renderer_gl_proc_resolve;
  renderer_config.open_gl.gl_external_texture_frame_callback = engine_cb_external_texture;
  renderer_config.open_gl.fbo_with_frame_info_callback = engine_cb_renderer_fbo;
  renderer_config.open_gl.present_with_info = engine_cb_renderer_present;

  FlutterProjectArgs project_args = {};
  project_args.struct_size = sizeof(FlutterProjectArgs);
  project_args.command_line_argc = opts.argc;
  project_args.command_line_argv = opts.argv;
  project_args.assets_path = opts.assets_path;
  project_args.icu_data_path = opts.icu_data_path;
  project_args.platform_message_callback = engine_cb_platform_message;
  project_args.log_message_callback = engine_cb_log_message;
  project_args.custom_task_runners = &instance->custom_task_runners;
  #ifdef FLUTTER_COMPOSITOR
  project_args.compositor = &instance->fl_compositor;
  #endif
  project_args.vsync_callback = fwr_engine_vsync_callback;

  if (instance->fl_proc_table.RunsAOTCompiledDartCode()) {
    FlutterEngineAOTDataSource aot_source;
    FlutterEngineAOTData aot_data;

    aot_source = (FlutterEngineAOTDataSource) {
          .elf_path = opts.elf_file_path,
          .type = kFlutterEngineAOTDataSourceTypeElfPath
    };

    FlutterEngineResult engine_result = FlutterEngineCreateAOTData(&aot_source, &aot_data);
    if (engine_result != kSuccess) {
     wlr_log(WLR_ERROR, "Could not load AOT data. FlutterEngineCreateAOTData failed.");
     return false;
    }

    project_args.aot_data = aot_data;

  }

  fwr_plugin_registry_init(&instance->plugin_registry);
  fwr_text_input_init(instance);
  fwr_cursor_init(instance);

  wlr_log(WLR_INFO, "Pre engine run");
  FlutterEngineResult fl_result = instance->fl_proc_table.Run(
    FLUTTER_ENGINE_VERSION,
    &renderer_config,
    &project_args,
    (void*) instance,
    &instance->engine
  );

  if (fl_result != kSuccess) {
    wlr_log(WLR_ERROR, "Flutter Engine Run failed!");
  }

  // Note: Outputs are sent to Flutter when Dart signals "compositor_ready"
  // This ensures Dart's message handlers are registered before we send data

  // Send initial window metrics based on total output bounds
  if (!wl_list_empty(&instance->outputs)) {
    struct wlr_box total_box = {0};
    wlr_output_layout_get_box(instance->output_layout, NULL, &total_box);

    FlutterWindowMetricsEvent window_metrics = {};
    window_metrics.struct_size = sizeof(FlutterWindowMetricsEvent);
    window_metrics.width = total_box.width;
    window_metrics.height = total_box.height;
    // Keep Flutter's coordinate space consistent - use 1.0 for multi-output
    // Individual outputs may have different scales, handled per-output
    window_metrics.pixel_ratio = 1.0;
    wlr_log(WLR_INFO, "Sending Flutter window metrics: %dx%d, pixel_ratio=%.2f",
            total_box.width, total_box.height, window_metrics.pixel_ratio);
    instance->fl_proc_table.SendWindowMetricsEvent(instance->engine, &window_metrics);
  }

  FlutterPointerEvent pointer_event = {};
  pointer_event.struct_size = sizeof(FlutterPointerEvent);
  pointer_event.phase = kAdd;
  pointer_event.timestamp = instance->fl_proc_table.GetCurrentTime();
  pointer_event.x = 0;
  pointer_event.y = 0;
  pointer_event.device = 0;
  pointer_event.signal_kind = kFlutterPointerSignalKindNone;
  pointer_event.scroll_delta_x = 0;
  pointer_event.scroll_delta_y = 0;
  pointer_event.device_kind = kFlutterPointerDeviceKindMouse;
  pointer_event.buttons = 0;
  instance->fl_proc_table.SendPointerEvent(instance->engine, &pointer_event, 1);

  //pthread_join(engine_bootstrap_thread, NULL);

  wlr_log(WLR_INFO, "Engine Run success!");

  //wlr_egl_make_current(instance->egl);

  wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s", socket);

  // Write socket name to file for helper scripts
  FILE *socket_file = fopen("/tmp/avio-wayland-socket", "w");
  if (socket_file) {
    fprintf(socket_file, "%s", socket);
    fclose(socket_file);
  }

  wl_display_run(instance->wl_display);

  // Clean up socket file
  unlink("/tmp/avio-wayland-socket");

  wl_display_destroy_clients(instance->wl_display);
  wl_display_destroy(instance->wl_display);

  *instance_out = instance;
  return true;
}

bool fwr_instance_run(struct fwr_instance *instance) {

  return true;
}
