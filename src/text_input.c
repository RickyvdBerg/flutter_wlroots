#include "text_input.h"
#include "instance.h"
#include "platform_channel.h"
#include "plugin_registry.h"

#include <string.h>
#include <stdlib.h>
#include <xkbcommon/xkbcommon.h>

#define WLR_USE_UNSTABLE
#include <wlr/util/log.h>

static struct text_input_state g_text_input = {0};

static void send_editing_state(struct fwr_instance *instance) {
  if (!g_text_input.active) return;

  platch_call_json(
    instance,
    "flutter/textinput",
    "TextInputClient.updateEditingState",
    &(struct json_value) {
      .type = kJsonArray,
      .size = 2,
      .array = (struct json_value[2]) {
        {.type = kJsonNumber, .number_value = (double)g_text_input.connection_id},
        {.type = kJsonObject, .size = 5,
          .keys = (char*[5]) {"text", "selectionBase", "selectionExtent", "composingBase", "composingExtent"},
          .values = (struct json_value[5]) {
            {.type = kJsonString, .string_value = g_text_input.text},
            {.type = kJsonNumber, .number_value = (double)g_text_input.selection_base},
            {.type = kJsonNumber, .number_value = (double)g_text_input.selection_extent},
            {.type = kJsonNumber, .number_value = (double)g_text_input.composing_base},
            {.type = kJsonNumber, .number_value = (double)g_text_input.composing_extent}
          }
        }
      }
    },
    NULL, NULL
  );
}

static void perform_action(struct fwr_instance *instance, const char *action) {
  if (!g_text_input.active) return;

  platch_call_json(
    instance,
    "flutter/textinput",
    "TextInputClient.performAction",
    &(struct json_value) {
      .type = kJsonArray,
      .size = 2,
      .array = (struct json_value[2]) {
        {.type = kJsonNumber, .number_value = (double)g_text_input.connection_id},
        {.type = kJsonString, .string_value = (char*)action}
      }
    },
    NULL, NULL
  );
}

static bool text_input_handle_message(struct fwr_instance *instance, 
                                       const FlutterPlatformMessage *message,
                                       void *data) {
  struct platch_obj obj;
  uint8_t *buffer = NULL;
  bool handled = true;

  if (message->message_size > 0) {
    buffer = malloc(message->message_size + 1);
    if (!buffer) {
      wlr_log(WLR_ERROR, "Out of memory decoding textinput message");
      return false;
    }
    memcpy(buffer, message->message, message->message_size);
    buffer[message->message_size] = '\0';
  }

  int ok = platch_decode(buffer, message->message_size, kJSONMethodCall, &obj);
  if (ok != 0) {
    wlr_log(WLR_ERROR, "Failed to decode textinput message");
    free(buffer);
    return false;
  }

  FlutterPlatformMessageResponseHandle *handle = (FlutterPlatformMessageResponseHandle*)message->response_handle;

  if (strcmp(obj.method, "TextInput.setClient") == 0) {
    if (obj.json_arg.type == kJsonArray && obj.json_arg.size >= 2) {
      g_text_input.active = true;
      g_text_input.connection_id = (int64_t)obj.json_arg.array[0].number_value;
      
      memset(g_text_input.text, 0, sizeof(g_text_input.text));
      g_text_input.text_length = 0;
      g_text_input.selection_base = 0;
      g_text_input.selection_extent = 0;
      g_text_input.composing_base = -1;
      g_text_input.composing_extent = -1;
      g_text_input.multiline = false;
      memset(g_text_input.input_action, 0, sizeof(g_text_input.input_action));

      struct json_value *config = &obj.json_arg.array[1];
      if (config->type == kJsonObject) {
        for (int i = 0; i < config->size; i++) {
          if (strcmp(config->keys[i], "inputAction") == 0 && config->values[i].type == kJsonString) {
            strncpy(g_text_input.input_action, config->values[i].string_value, sizeof(g_text_input.input_action) - 1);
          }
          if (strcmp(config->keys[i], "inputType") == 0 && config->values[i].type == kJsonObject) {
            for (int j = 0; j < config->values[i].size; j++) {
              if (strcmp(config->values[i].keys[j], "name") == 0 && 
                  config->values[i].values[j].type == kJsonString &&
                  strstr(config->values[i].values[j].string_value, "multiline")) {
                g_text_input.multiline = true;
              }
            }
          }
        }
      }
      wlr_log(WLR_INFO, "TextInput.setClient: connection_id=%ld", g_text_input.connection_id);
    }
    platch_respond_success_json(instance, handle, NULL);
  } 
  else if (strcmp(obj.method, "TextInput.setEditingState") == 0) {
    struct json_value *state = &obj.json_arg;
    if (state->type == kJsonObject) {
      for (int i = 0; i < state->size; i++) {
        if (strcmp(state->keys[i], "text") == 0 && state->values[i].type == kJsonString) {
          strncpy(g_text_input.text, state->values[i].string_value, sizeof(g_text_input.text) - 1);
          g_text_input.text_length = strlen(g_text_input.text);
        } else if (strcmp(state->keys[i], "selectionBase") == 0 && state->values[i].type == kJsonNumber) {
          g_text_input.selection_base = (int32_t)state->values[i].number_value;
        } else if (strcmp(state->keys[i], "selectionExtent") == 0 && state->values[i].type == kJsonNumber) {
          g_text_input.selection_extent = (int32_t)state->values[i].number_value;
        } else if (strcmp(state->keys[i], "composingBase") == 0 && state->values[i].type == kJsonNumber) {
          g_text_input.composing_base = (int32_t)state->values[i].number_value;
        } else if (strcmp(state->keys[i], "composingExtent") == 0 && state->values[i].type == kJsonNumber) {
          g_text_input.composing_extent = (int32_t)state->values[i].number_value;
        }
      }
    }
    wlr_log(WLR_DEBUG, "TextInput.setEditingState: text='%s' sel=%d-%d", 
            g_text_input.text, g_text_input.selection_base, g_text_input.selection_extent);
    platch_respond_success_json(instance, handle, NULL);
  }
  else if (strcmp(obj.method, "TextInput.clearClient") == 0) {
    g_text_input.active = false;
    g_text_input.connection_id = 0;
    memset(g_text_input.text, 0, sizeof(g_text_input.text));
    g_text_input.text_length = 0;
    wlr_log(WLR_INFO, "TextInput.clearClient");
    platch_respond_success_json(instance, handle, NULL);
  }
  else if (strcmp(obj.method, "TextInput.show") == 0 ||
           strcmp(obj.method, "TextInput.hide") == 0 ||
           strcmp(obj.method, "TextInput.setEditableSizeAndTransform") == 0 ||
           strcmp(obj.method, "TextInput.setMarkedTextRect") == 0 ||
           strcmp(obj.method, "TextInput.setStyle") == 0 ||
           strcmp(obj.method, "TextInput.setCaretRect") == 0 ||
           strcmp(obj.method, "TextInput.requestAutofill") == 0 ||
           strcmp(obj.method, "TextInput.finishAutofillContext") == 0) {
    platch_respond_success_json(instance, handle, NULL);
  }
  else {
    wlr_log(WLR_INFO, "Unhandled TextInput method: %s", obj.method);
    handled = false;
  }

  platch_free_obj(&obj);
  free(buffer);
  return handled;
}

void fwr_text_input_init(struct fwr_instance *instance) {
  memset(&g_text_input, 0, sizeof(g_text_input));
  g_text_input.composing_base = -1;
  g_text_input.composing_extent = -1;

  fwr_plugin_registry_channel_handler_register(
    &instance->plugin_registry,
    "flutter/textinput",
    instance,
    text_input_handle_message
  );

  wlr_log(WLR_INFO, "Text input plugin initialized");
}

static void delete_selection(void) {
  if (g_text_input.selection_base == g_text_input.selection_extent) return;
  
  int32_t start = g_text_input.selection_base < g_text_input.selection_extent 
                  ? g_text_input.selection_base : g_text_input.selection_extent;
  int32_t end = g_text_input.selection_base > g_text_input.selection_extent 
                ? g_text_input.selection_base : g_text_input.selection_extent;
  
  memmove(&g_text_input.text[start], &g_text_input.text[end], 
          g_text_input.text_length - end + 1);
  g_text_input.text_length -= (end - start);
  g_text_input.selection_base = start;
  g_text_input.selection_extent = start;
}

static void insert_text(const char *str, size_t len) {
  if (g_text_input.text_length + len >= TEXT_INPUT_MAX_LENGTH) return;
  
  delete_selection();
  
  int32_t pos = g_text_input.selection_base;
  memmove(&g_text_input.text[pos + len], &g_text_input.text[pos],
          g_text_input.text_length - pos + 1);
  memcpy(&g_text_input.text[pos], str, len);
  g_text_input.text_length += len;
  g_text_input.selection_base = pos + len;
  g_text_input.selection_extent = pos + len;
}

void fwr_text_input_handle_key(struct fwr_instance *instance, 
                                xkb_keysym_t keysym, 
                                uint32_t unicode,
                                bool pressed) {
  if (!g_text_input.active || !pressed) return;

  bool changed = false;

  switch (keysym) {
    case XKB_KEY_BackSpace:
      if (g_text_input.selection_base != g_text_input.selection_extent) {
        delete_selection();
        changed = true;
      } else if (g_text_input.selection_base > 0) {
        int32_t pos = g_text_input.selection_base - 1;
        memmove(&g_text_input.text[pos], &g_text_input.text[pos + 1],
                g_text_input.text_length - pos);
        g_text_input.text_length--;
        g_text_input.selection_base = pos;
        g_text_input.selection_extent = pos;
        changed = true;
      }
      break;

    case XKB_KEY_Delete:
      if (g_text_input.selection_base != g_text_input.selection_extent) {
        delete_selection();
        changed = true;
      } else if (g_text_input.selection_base < (int32_t)g_text_input.text_length) {
        int32_t pos = g_text_input.selection_base;
        memmove(&g_text_input.text[pos], &g_text_input.text[pos + 1],
                g_text_input.text_length - pos);
        g_text_input.text_length--;
        changed = true;
      }
      break;

    case XKB_KEY_Left:
      if (g_text_input.selection_base > 0) {
        g_text_input.selection_base--;
        g_text_input.selection_extent = g_text_input.selection_base;
        changed = true;
      }
      break;

    case XKB_KEY_Right:
      if (g_text_input.selection_base < (int32_t)g_text_input.text_length) {
        g_text_input.selection_base++;
        g_text_input.selection_extent = g_text_input.selection_base;
        changed = true;
      }
      break;

    case XKB_KEY_Home:
      g_text_input.selection_base = 0;
      g_text_input.selection_extent = 0;
      changed = true;
      break;

    case XKB_KEY_End:
      g_text_input.selection_base = g_text_input.text_length;
      g_text_input.selection_extent = g_text_input.text_length;
      changed = true;
      break;

    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
      if (g_text_input.multiline) {
        insert_text("\n", 1);
        changed = true;
      } else {
        perform_action(instance, g_text_input.input_action[0] ? g_text_input.input_action : "TextInputAction.done");
      }
      break;

    case XKB_KEY_Tab:
      break;

    default:
      if (unicode >= 0x20 && unicode != 0x7F) {
        char utf8[8];
        int len = 0;
        if (unicode < 0x80) {
          utf8[0] = unicode;
          len = 1;
        } else if (unicode < 0x800) {
          utf8[0] = 0xC0 | (unicode >> 6);
          utf8[1] = 0x80 | (unicode & 0x3F);
          len = 2;
        } else if (unicode < 0x10000) {
          utf8[0] = 0xE0 | (unicode >> 12);
          utf8[1] = 0x80 | ((unicode >> 6) & 0x3F);
          utf8[2] = 0x80 | (unicode & 0x3F);
          len = 3;
        } else {
          utf8[0] = 0xF0 | (unicode >> 18);
          utf8[1] = 0x80 | ((unicode >> 12) & 0x3F);
          utf8[2] = 0x80 | ((unicode >> 6) & 0x3F);
          utf8[3] = 0x80 | (unicode & 0x3F);
          len = 4;
        }
        insert_text(utf8, len);
        changed = true;
      }
      break;
  }

  if (changed) {
    send_editing_state(instance);
  }
}
