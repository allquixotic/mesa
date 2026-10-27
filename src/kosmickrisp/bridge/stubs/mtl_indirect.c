/* SPDX-License-Identifier: MIT */
#include "../mtl_indirect.h"
mtl_library *mtl_new_indirect_draw_library(mtl_compiler *compiler) { return NULL; }
mtl_indirect_command_buffer *mtl_new_indirect_draw_buffer(mtl_device *device, uint32_t count) { return NULL; }
uint64_t mtl_indirect_draw_buffer_id(mtl_indirect_command_buffer *buffer) { return 0; }
void mtl_execute_indirect_draws(mtl_render_encoder *encoder, mtl_indirect_command_buffer *buffer, uint64_t range) {}
