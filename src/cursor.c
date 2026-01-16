#include "cursor.h"
#include "instance.h"
#include "platform_channel.h"
#include "plugin_registry.h"

#include <string.h>
#include <stdlib.h>

#include <wlr/util/log.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>

static const char* flutter_cursor_to_xcursor(const char *flutter_kind) {
  if (flutter_kind == NULL) return "left_ptr";

  if (strcmp(flutter_kind, "basic") == 0) return "left_ptr";
  if (strcmp(flutter_kind, "none") == 0) return "left_ptr";
  if (strcmp(flutter_kind, "click") == 0) return "pointer";
  if (strcmp(flutter_kind, "pointer") == 0) return "pointer";
  if (strcmp(flutter_kind, "text") == 0) return "xterm";
  if (strcmp(flutter_kind, "verticalText") == 0) return "vertical-text";
  if (strcmp(flutter_kind, "resizeLeft") == 0) return "left_side";
  if (strcmp(flutter_kind, "resizeRight") == 0) return "right_side";
  if (strcmp(flutter_kind, "resizeUp") == 0) return "top_side";
  if (strcmp(flutter_kind, "resizeDown") == 0) return "bottom_side";
  if (strcmp(flutter_kind, "resizeUpLeft") == 0) return "top_left_corner";
  if (strcmp(flutter_kind, "resizeUpRight") == 0) return "top_right_corner";
  if (strcmp(flutter_kind, "resizeDownLeft") == 0) return "bottom_left_corner";
  if (strcmp(flutter_kind, "resizeDownRight") == 0) return "bottom_right_corner";
  if (strcmp(flutter_kind, "resizeColumn") == 0) return "col-resize";
  if (strcmp(flutter_kind, "resizeRow") == 0) return "row-resize";
  if (strcmp(flutter_kind, "resizeUpDown") == 0) return "ns-resize";
  if (strcmp(flutter_kind, "resizeLeftRight") == 0) return "ew-resize";
  if (strcmp(flutter_kind, "resizeUpLeftDownRight") == 0) return "nwse-resize";
  if (strcmp(flutter_kind, "resizeUpRightDownLeft") == 0) return "nesw-resize";
  if (strcmp(flutter_kind, "grab") == 0) return "grab";
  if (strcmp(flutter_kind, "grabbing") == 0) return "grabbing";
  if (strcmp(flutter_kind, "move") == 0) return "fleur";
  if (strcmp(flutter_kind, "allScroll") == 0) return "all-scroll";
  if (strcmp(flutter_kind, "forbidden") == 0) return "not-allowed";
  if (strcmp(flutter_kind, "noDrop") == 0) return "no-drop";
  if (strcmp(flutter_kind, "wait") == 0) return "wait";
  if (strcmp(flutter_kind, "progress") == 0) return "progress";
  if (strcmp(flutter_kind, "contextMenu") == 0) return "context-menu";
  if (strcmp(flutter_kind, "help") == 0) return "help";
  if (strcmp(flutter_kind, "cell") == 0) return "cell";
  if (strcmp(flutter_kind, "precise") == 0) return "crosshair";
  if (strcmp(flutter_kind, "crosshair") == 0) return "crosshair";
  if (strcmp(flutter_kind, "copy") == 0) return "copy";
  if (strcmp(flutter_kind, "alias") == 0) return "alias";
  if (strcmp(flutter_kind, "zoomIn") == 0) return "zoom-in";
  if (strcmp(flutter_kind, "zoomOut") == 0) return "zoom-out";
  
  wlr_log(WLR_DEBUG, "Unknown cursor kind: %s, using default", flutter_kind);
  return "left_ptr";
}

static bool cursor_handle_message(struct fwr_instance *instance,
                                   const FlutterPlatformMessage *message,
                                   void *data) {
  struct platch_obj obj;
  int ok;

  if (message->message_size == 0) {
    return false;
  }

  ok = platch_decode((uint8_t*)message->message, message->message_size, kStandardMethodCall, &obj);
  if (ok != 0) {
    wlr_log(WLR_ERROR, "Failed to decode mousecursor message");
    return false;
  }

  FlutterPlatformMessageResponseHandle *handle = (FlutterPlatformMessageResponseHandle*)message->response_handle;

  if (strcmp(obj.method, "activateSystemCursor") == 0) {
    if (obj.std_arg.type == kStdMap) {
      struct std_value *kind_value = stdmap_get_str(&obj.std_arg, "kind");
      if (kind_value != NULL && kind_value->type == kStdString) {
        const char *flutter_kind = kind_value->string_value;
        const char *xcursor_name = flutter_cursor_to_xcursor(flutter_kind);

        if (xcursor_name != NULL) {
          wlr_log(WLR_DEBUG, "Setting cursor: flutter=%s xcursor=%s", flutter_kind, xcursor_name);

          // Store current xcursor name for software rendering
          free(instance->current_xcursor_name);
          instance->current_xcursor_name = strdup(xcursor_name);

          // Clear client cursor surface when using xcursor
          instance->client_cursor_surface = NULL;

          wlr_cursor_set_xcursor(instance->cursor, instance->cursor_mgr, xcursor_name);
        }
      }
    }
    platch_respond_success_std(instance, handle, NULL);
  }
  else if (strcmp(obj.method, "createSystemCursor") == 0 ||
           strcmp(obj.method, "deleteSystemCursor") == 0) {
    platch_respond_success_std(instance, handle, NULL);
  }
  else {
    wlr_log(WLR_INFO, "Unhandled mousecursor method: %s", obj.method);
    platch_free_obj(&obj);
    return false;
  }

  platch_free_obj(&obj);
  return true;
}

void fwr_cursor_init(struct fwr_instance *instance) {
  fwr_plugin_registry_channel_handler_register(
    &instance->plugin_registry,
    "flutter/mousecursor",
    instance,
    cursor_handle_message
  );

  wlr_log(WLR_INFO, "Mouse cursor plugin initialized");
}
