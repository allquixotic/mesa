/* SPDX-License-Identifier: MIT */
#ifndef MTL_INDIRECT_H
#define MTL_INDIRECT_H
#include "mtl_types.h"

typedef void mtl_indirect_command_buffer;

/* ABI shared with the small, integer-only Metal command-generation kernels. */
struct mtl_indirect_draw_args {
   uint64_t icb, draws, count, conditional, indices;
   uint64_t root, samplers, per_draw, per_draw_template;
   uint64_t range, grid;
   uint32_t max_draws, stride, primitive, index_size;
   uint32_t conditional_mode, per_draw_words;
};
_Static_assert(sizeof(struct mtl_indirect_draw_args) == 112,
               "Metal indirect-draw argument ABI");

mtl_library *mtl_new_indirect_draw_library(mtl_compiler *compiler);
mtl_indirect_command_buffer *mtl_new_indirect_draw_buffer(mtl_device *device,
                                                         uint32_t count);
uint64_t mtl_indirect_draw_buffer_id(mtl_indirect_command_buffer *buffer);
void mtl_execute_indirect_draws(mtl_render_encoder *encoder,
                                mtl_indirect_command_buffer *buffer,
                                uint64_t range);
#endif
