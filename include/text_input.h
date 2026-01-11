#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <xkbcommon/xkbcommon.h>

struct fwr_instance;

#define TEXT_INPUT_MAX_LENGTH 8192

struct text_input_state {
  bool active;
  int64_t connection_id;
  
  char text[TEXT_INPUT_MAX_LENGTH];
  size_t text_length;
  
  int32_t selection_base;
  int32_t selection_extent;
  int32_t composing_base;
  int32_t composing_extent;
  
  char input_action[64];
  bool multiline;
};

void fwr_text_input_init(struct fwr_instance *instance);
void fwr_text_input_handle_key(struct fwr_instance *instance, 
                                xkb_keysym_t keysym, 
                                uint32_t unicode,
                                bool pressed);
