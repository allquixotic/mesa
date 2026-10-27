/* SPDX-License-Identifier: MIT */
#include "mtl_indirect.h"
#include <Metal/Metal.h>
#include <stdio.h>

mtl_library *
mtl_new_indirect_draw_library(mtl_compiler *compiler)
{
   @autoreleasepool {
      /* All pointers are to command-buffer-owned storage or Vulkan resources
       * whose lifetime the application must extend through submission. No
       * CPU readback, history-based draw limit, or floating-point arithmetic. */
      NSString *source =
         @"#include <metal_stdlib>\n"
          "using namespace metal;\n"
          "struct Args { command_buffer icb; device uint *draws; "
          "device uint *count; device uint *conditional; device uchar *indices; "
          "device uchar *root; device uchar *samplers; device uint *per_draw; "
          "device uint *per_draw_template; device uint2 *range; device uint *grid; "
          "uint max_draws, stride, primitive, index_size, conditional_mode, per_draw_words; };\n"
          "kernel void prepare(constant Args &a [[buffer(0)]]) {\n"
          " uint n = a.count ? min(*a.count, a.max_draws) : a.max_draws;\n"
          " if (a.conditional_mode && ((*a.conditional != 0) != (a.conditional_mode == 1))) n = 0;\n"
          " *a.range = uint2(0,n); a.grid[0] = max(1u, (n + 63u) / 64u); a.grid[1]=1; a.grid[2]=1;\n"
          "}\n"
          "kernel void generate(constant Args &a [[buffer(0)]], uint i [[thread_position_in_grid]]) {\n"
          " if (i >= a.range->y) return;\n"
          " device uint *d = a.draws + ulong(i) * a.stride;\n"
          " render_command c(a.icb,i); c.reset();\n"
          " c.set_vertex_buffer(a.root,0); c.set_fragment_buffer(a.root,0);\n"
          " c.set_vertex_buffer(a.samplers,1); c.set_fragment_buffer(a.samplers,1);\n"
          " if (a.per_draw_words) {\n"
          "   device uint *p = a.per_draw + ulong(i) * a.per_draw_words;\n"
          "   for (uint j=0;j<a.per_draw_words;j++) p[j]=a.per_draw_template[j]; p[0]=i;\n"
          "   c.set_vertex_buffer(p,2); c.set_fragment_buffer(p,2);\n"
          " }\n"
          " if (!d[0] || !d[1]) return;\n"
          " if (a.index_size == 2) c.draw_indexed_primitives(primitive_type(a.primitive),d[0],(device ushort*)a.indices+d[2],d[1],d[3],d[4]);\n"
          " else if (a.index_size == 4) c.draw_indexed_primitives(primitive_type(a.primitive),d[0],(device uint*)a.indices+d[2],d[1],d[3],d[4]);\n"
          " else c.draw_primitives(primitive_type(a.primitive),d[2],d[0],d[1],d[3]);\n"
          "}\n";
      MTL4LibraryDescriptor *desc = [[MTL4LibraryDescriptor new] autorelease];
      desc.source = source;
      desc.options = [[MTLCompileOptions new] autorelease];
      desc.options.languageVersion = MTLLanguageVersion4_0;
      NSError *error = nil;
      id<MTLLibrary> lib = [(id<MTL4Compiler>)compiler newLibraryWithDescriptor:desc error:&error];
      if (!lib)
         fprintf(stderr, "kk: indirect command kernel unavailable: %s\n", error.localizedDescription.UTF8String);
      return lib;
   }
}

mtl_indirect_command_buffer *
mtl_new_indirect_draw_buffer(mtl_device *device, uint32_t count)
{
   @autoreleasepool {
      MTLIndirectCommandBufferDescriptor *desc = [[MTLIndirectCommandBufferDescriptor new] autorelease];
      desc.commandTypes = MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawIndexed;
      desc.inheritPipelineState = YES;
      desc.inheritBuffers = NO;
      desc.maxVertexBufferBindCount = 3;
      desc.maxFragmentBufferBindCount = 3;
      return [(id<MTLDevice>)device newIndirectCommandBufferWithDescriptor:desc
                                                        maxCommandCount:count
                                                                options:MTLResourceStorageModePrivate];
   }
}

uint64_t
mtl_indirect_draw_buffer_id(mtl_indirect_command_buffer *buffer)
{
   return [(id<MTLIndirectCommandBuffer>)buffer gpuResourceID]._impl;
}

void
mtl_execute_indirect_draws(mtl_render_encoder *encoder,
                           mtl_indirect_command_buffer *buffer, uint64_t range)
{
   [(id<MTL4RenderCommandEncoder>)encoder executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer
                                                indirectBuffer:range];
}
