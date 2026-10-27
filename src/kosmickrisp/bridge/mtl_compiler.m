/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "mtl_compiler.h"
#include "mtl_format.h"
#include "mtl_progress.h"
#include "util/mesa-blake3.h"
#include "util/u_debug.h"

#include <Metal/MTL4ComputePipeline.h>
#include <Metal/MTL4LibraryDescriptor.h>
#include <Metal/MTL4LibraryFunctionDescriptor.h>
#include <Metal/MTL4RenderPipeline.h>
#include <Metal/MTLDevice.h>

#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Opt-in diagnostic/workaround candidate. Source capture after library
 * compilation changes whether one observed workload hangs. Serializing the
 * synchronous Metal compiler entry points tests a possible concurrency/timing
 * dependency without shader dumps, GPU counters, or changes to shader code.
 * Metal permits concurrent compilation; this does not establish an API misuse
 * or a compiler bug. Keep the normal concurrent path as the default. */
DEBUG_GET_ONCE_BOOL_OPTION(kk_serialize_compilation,
                           "MESA_KK_SERIALIZE_COMPILATION", false)
static pthread_mutex_t kk_compilation_mutex = PTHREAD_MUTEX_INITIALIZER;

static void
mtl_compilation_lock(void)
{
   if (debug_get_option_kk_serialize_compilation())
      pthread_mutex_lock(&kk_compilation_mutex);
}

static void
mtl_compilation_unlock(void)
{
   if (debug_get_option_kk_serialize_compilation())
      pthread_mutex_unlock(&kk_compilation_mutex);
}

/* Diagnostic-only source identities survive pipeline/library object reuse.
 * Save each distinct generated source once, with a per-process byte budget.
 * Failure to capture a source must never affect shader compilation. */
static void
mtl_progress_library(id<MTLLibrary> lib, const char *src)
{
   if (!lib || !mtl_progress_enabled())
      return;

   const size_t size = strlen(src);
   blake3_hash hash;
   char hex[BLAKE3_HEX_LEN];
   _mesa_blake3_compute(src, size, hash);
   _mesa_blake3_format(hex, hash);
   lib.label = [NSString stringWithFormat:@"msl:%s", hex];

   const char *dir = getenv("MESA_KK_PROGRESS_SHADER_DIR");
   if (!dir || !dir[0])
      return;
   char path[PATH_MAX];
   if (snprintf(path, sizeof(path), "%s/%s.metal", dir, hex) >= sizeof(path))
      return;

   static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
   static size_t captured;
   static bool warned;
   const size_t limit = 256u * 1024u * 1024u;
   pthread_mutex_lock(&mutex);
   if (size > limit - captured) {
      if (!warned)
         fprintf(stderr, "kk progress: shader capture reached its 256 MiB budget\n");
      warned = true;
      pthread_mutex_unlock(&mutex);
      return;
   }
   const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
   if (fd >= 0) {
      size_t written = 0;
      while (written < size) {
         ssize_t n = write(fd, src + written, size - written);
         if (n < 0 && errno == EINTR)
            continue;
         if (n <= 0)
            break;
         written += n;
      }
      const int closed = close(fd);
      if (written == size && closed == 0) {
         captured += size;
      } else {
         unlink(path);
         fprintf(stderr, "kk progress: incomplete shader capture: %s\n", path);
      }
   } else if (errno != EEXIST) {
      fprintf(stderr, "kk progress: cannot capture shader %s: %s\n", path,
              strerror(errno));
   }
   pthread_mutex_unlock(&mutex);
}

static NSString *
mtl_progress_function_label(MTL4FunctionDescriptor *function)
{
   if (![function isKindOfClass:[MTL4LibraryFunctionDescriptor class]])
      return @"unknown";
   MTL4LibraryFunctionDescriptor *fn = (MTL4LibraryFunctionDescriptor *)function;
   return [NSString stringWithFormat:@"%@:%@", fn.library.label ?: @"unlabeled",
                                      fn.name ?: @"unknown"];
}

/* Compiler */
mtl_compiler *
mtl_new_compiler(mtl_device *device)
{
   @autoreleasepool {
      id<MTLDevice> dev = (id<MTLDevice>)device;

      MTL4CompilerDescriptor *desc = [[MTL4CompilerDescriptor new] autorelease];

      NSError *error;
      id<MTL4Compiler> compiler = [dev newCompilerWithDescriptor:desc error:&error];

      if (compiler == nil) {
         fprintf(stderr, "Failed to create MTL4Compiler: %s\n", [error.localizedDescription UTF8String]);
      }

      return compiler;
   }
}

/* Library */
mtl_library *
mtl_new_library(mtl_compiler *compiler, const char *src,
                enum mtl_language_version msl_version,
                enum mtl_math_mode math_mode,
                enum mtl_math_floating_point_functions math_fp_fns)
{
   @autoreleasepool {
      id<MTL4Compiler> comp = (id<MTL4Compiler>)compiler;
      NSString *ns_src = [NSString stringWithCString:src encoding:NSASCIIStringEncoding];

      MTLCompileOptions *opts = [[MTLCompileOptions new] autorelease];
      opts.languageVersion = (MTLLanguageVersion)msl_version;
      opts.mathMode = (MTLMathMode)math_mode;
      opts.mathFloatingPointFunctions = (MTLMathFloatingPointFunctions)math_fp_fns;

      MTL4LibraryDescriptor *desc = [[MTL4LibraryDescriptor new] autorelease];
      desc.source = ns_src;
      desc.options = opts;

      NSError *error;
      mtl_compilation_lock();
      id<MTLLibrary> lib = [comp newLibraryWithDescriptor:desc error:&error];
      mtl_compilation_unlock();
      mtl_progress_library(lib, src);

      if (lib == nil) {
         fprintf(stderr, "Failed to create MTLLibrary: %s\n", [error.localizedDescription UTF8String]);

         const char *dir = getenv("MESA_KK_FAILED_MSL_DIR");
         if (dir) {
            static _Atomic uint32_t count;
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%d-%u.metal", dir, getpid(), count++);
            FILE *f = fopen(path, "w");
            if (f) {
               fputs(src, f);
               fclose(f);
               fprintf(stderr, "MSL written to %s\n", path);
            }
         }
      }

      return lib;
   }
}

mtl_function_descriptor *
mtl_new_library_function_descriptor(mtl_library *library, const char *entry_point)
{
   @autoreleasepool {
      id<MTLLibrary> lib = (id<MTLLibrary>)library;
      NSString *ns_entry_point = [NSString stringWithCString:entry_point encoding:NSASCIIStringEncoding];

      MTL4LibraryFunctionDescriptor *desc = [MTL4LibraryFunctionDescriptor new];
      desc.name = ns_entry_point;
      desc.library = lib;
      return desc;
   }
}

/* Compute pipeline */
mtl_compute_pipeline_state *
mtl_new_compute_pipeline_state(mtl_compiler *compiler,
                               mtl_function_descriptor *function,
                               uint64_t max_total_threads_per_threadgroup)
{
   @autoreleasepool {
      id<MTL4Compiler> comp = (id<MTL4Compiler>)compiler;

      MTL4ComputePipelineDescriptor *desc = [[MTL4ComputePipelineDescriptor new] autorelease];
      desc.computeFunctionDescriptor = (MTL4FunctionDescriptor *)function;
      desc.maxTotalThreadsPerThreadgroup = max_total_threads_per_threadgroup;
      if (mtl_progress_enabled())
         desc.label = mtl_progress_function_label((MTL4FunctionDescriptor *)function);

      NSError *error;
      mtl_compilation_lock();
      id<MTLComputePipelineState> pipeline = [comp newComputePipelineStateWithDescriptor:desc compilerTaskOptions:nil
                                                                                   error:&error];
      mtl_compilation_unlock();

      if (pipeline == nil) {
         fprintf(stderr, "Failed to create MTLComputePipelineState: %s\n", [error.localizedDescription UTF8String]);
      }

      return pipeline;
   }
}

/* Render pipeline descriptor */
mtl_render_pipeline_descriptor *
mtl_new_render_pipeline_descriptor(bool indirect)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = [MTL4RenderPipelineDescriptor new];
      /* Ordinary draws keep the upstream descriptor defaults. Only an ICB
       * pipeline variant opts into indirect-command support. */
      if (indirect)
         desc.supportIndirectCommandBuffers = MTL4IndirectCommandBufferSupportStateEnabled;
      return desc;
   }
}

void
mtl_render_pipeline_descriptor_set_vertex_shader(mtl_render_pipeline_descriptor *descriptor,
                                                 mtl_function_descriptor *shader)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;
      desc.vertexFunctionDescriptor = (MTL4FunctionDescriptor *)shader;
   }
}

void
mtl_render_pipeline_descriptor_set_fragment_shader(mtl_render_pipeline_descriptor *descriptor,
                                                   mtl_function_descriptor *shader)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;
      desc.fragmentFunctionDescriptor = (MTL4FunctionDescriptor *)shader;
   }
}

void
mtl_render_pipeline_descriptor_set_input_primitive_topology(mtl_render_pipeline_descriptor *descriptor,
                                                            enum mtl_primitive_topology_class class)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;
      desc.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)class;
   }
}

void
mtl_render_pipeline_descriptor_set_color_attachment_format(mtl_render_pipeline_descriptor *descriptor,
                                                           uint8_t index,
                                                           enum mtl_pixel_format format)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;
      desc.colorAttachments[index].pixelFormat = (MTLPixelFormat)format;
   }
}

void
mtl_render_pipeline_descriptor_set_raster_sample_count(mtl_render_pipeline_descriptor *descriptor,
                                                       uint32_t sample_count)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;
      desc.rasterSampleCount = sample_count;
   }
}

void
mtl_render_pipeline_descriptor_set_alpha_to_coverage(mtl_render_pipeline_descriptor *descriptor,
                                                     bool enabled)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;
      desc.alphaToCoverageState = enabled ? MTL4AlphaToCoverageStateEnabled
                                          : MTL4AlphaToCoverageStateDisabled;
   }
}

void
mtl_render_pipeline_descriptor_set_alpha_to_one(mtl_render_pipeline_descriptor *descriptor,
                                                bool enabled)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;
      desc.alphaToOneState = enabled ? MTL4AlphaToOneStateEnabled
                                     : MTL4AlphaToOneStateDisabled;
   }
}

void
mtl_render_pipeline_descriptor_set_color_attachment_blend(mtl_render_pipeline_descriptor *descriptor,
                                                          uint8_t index,
                                                          const struct mtl_color_attachment_blend *blend)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;
      MTL4RenderPipelineColorAttachmentDescriptor *att = desc.colorAttachments[index];
      att.writeMask = (MTLColorWriteMask)blend->write_mask;
      att.blendingState = blend->blend_enable ? MTL4BlendStateEnabled
                                              : MTL4BlendStateDisabled;
      if (blend->blend_enable) {
         att.sourceRGBBlendFactor = (MTLBlendFactor)blend->src_rgb;
         att.destinationRGBBlendFactor = (MTLBlendFactor)blend->dst_rgb;
         att.rgbBlendOperation = (MTLBlendOperation)blend->rgb_op;
         att.sourceAlphaBlendFactor = (MTLBlendFactor)blend->src_alpha;
         att.destinationAlphaBlendFactor = (MTLBlendFactor)blend->dst_alpha;
         att.alphaBlendOperation = (MTLBlendOperation)blend->alpha_op;
      }
   }
}

void
mtl_render_pipeline_descriptor_set_rasterization_enabled(mtl_render_pipeline_descriptor *descriptor,
                                                         bool enabled)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;
      desc.rasterizationEnabled = enabled;
   }
}

void
mtl_render_pipeline_descriptor_set_max_vertex_amplification_count(mtl_render_pipeline_descriptor *descriptor,
                                                                  uint32_t count)
{
   @autoreleasepool {
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;
      desc.maxVertexAmplificationCount = count;
   }
}

/* Render pipeline */
mtl_render_pipeline_state *
mtl_new_render_pipeline(mtl_compiler *compiler, mtl_render_pass_descriptor *descriptor)
{
   @autoreleasepool {
      id<MTL4Compiler> comp = (id<MTL4Compiler>)compiler;
      MTL4RenderPipelineDescriptor *desc = (MTL4RenderPipelineDescriptor *)descriptor;

      if (mtl_progress_enabled())
         desc.label = [NSString stringWithFormat:@"vs=%@ fs=%@",
            mtl_progress_function_label(desc.vertexFunctionDescriptor),
            mtl_progress_function_label(desc.fragmentFunctionDescriptor)];

      NSError *error = nil;
      mtl_compilation_lock();
      id<MTLRenderPipelineState> pipeline = [comp newRenderPipelineStateWithDescriptor:desc compilerTaskOptions:nil
                                                                                 error:&error];
      mtl_compilation_unlock();

      if (pipeline == nil) {
         fprintf(stderr, "Failed to create MTLRenderPipelineState: %s\n", [error.localizedDescription UTF8String]);
      }

      return pipeline;
   }
}
