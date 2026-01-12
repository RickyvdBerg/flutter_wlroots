#pragma once

#include <wayland-server-core.h>

#include <flutter_embedder.h>

#include "instance.h"

void fwr_new_xdg_toplevel(struct wl_listener *listener, void *data);
void fwr_handle_surface_toplevel_set_size(struct fwr_instance *instance, const FlutterPlatformMessageResponseHandle *handle, struct dart_value *args);
void fwr_handle_surface_toplevel_set_maximized(struct fwr_instance *instance, const FlutterPlatformMessageResponseHandle *handle, struct dart_value *args);
void fwr_handle_surface_toplevel_close(struct fwr_instance *instance, const FlutterPlatformMessageResponseHandle *handle, struct dart_value *args);
void fwr_handle_surface_focus(struct fwr_instance *instance, const FlutterPlatformMessageResponseHandle *handle, struct dart_value *args);
void fwr_handle_surface_set_position(struct fwr_instance *instance, const FlutterPlatformMessageResponseHandle *handle, struct dart_value *args);

void fwr_focus_view(struct fwr_view *view);
