/*
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "mtl_command_queue.h"
#include "mtl_progress.h"
#include "util/log.h"
#include "util/u_debug.h"
#include "util/os_time.h"

#include <pthread.h>

#include <Metal/MTLDevice.h>
#include <Metal/MTLCommandQueue.h>

/* Opt-in flight recorder. Keep CPU logging out of normal submission and retain
 * only copied metadata, never command buffers or resources. A completed commit
 * may already have returned its objects to the driver's pools. */
DEBUG_GET_ONCE_BOOL_OPTION(kk_trace_queue, "MESA_KK_TRACE_QUEUE", false)

#define KK_QUEUE_TRACE_SIZE 1024
static pthread_mutex_t kk_trace_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t kk_trace_next;
static bool kk_trace_dumped;
static struct {
   uint64_t serial, time_ns, value;
   uintptr_t queue, object;
   const char *operation;
   char label[128];
   bool complete;
   double gpu_start, gpu_end;
} kk_trace[KK_QUEUE_TRACE_SIZE];

static uint64_t
kk_trace_record(void *queue, const char *operation, void *object, uint64_t value,
                const char *label)
{
   if (!debug_get_option_kk_trace_queue())
      return 0;

   pthread_mutex_lock(&kk_trace_mutex);
   const uint64_t serial = ++kk_trace_next;
   const unsigned slot = serial % KK_QUEUE_TRACE_SIZE;
   memset(&kk_trace[slot], 0, sizeof(kk_trace[slot]));
   kk_trace[slot].serial = serial;
   kk_trace[slot].time_ns = os_time_get_nano();
   kk_trace[slot].queue = (uintptr_t)queue;
   kk_trace[slot].operation = operation;
   kk_trace[slot].object = (uintptr_t)object;
   kk_trace[slot].value = value;
   snprintf(kk_trace[slot].label, sizeof(kk_trace[slot].label), "%s",
            label ? label : "");
   pthread_mutex_unlock(&kk_trace_mutex);
   return serial;
}

static void
kk_trace_complete(uint64_t serial, double start, double end, bool error)
{
   pthread_mutex_lock(&kk_trace_mutex);
   const unsigned slot = serial % KK_QUEUE_TRACE_SIZE;
   if (kk_trace[slot].serial == serial) {
      kk_trace[slot].complete = true;
      kk_trace[slot].gpu_start = start;
      kk_trace[slot].gpu_end = end;
   }
   if (error && !kk_trace_dumped) {
      kk_trace_dumped = true;
      mesa_loge("kk queue trace: failed commit=%llu; last %u operations follow "
                "(completion is callback delivery, not a GPU dependency)",
                (unsigned long long)serial, KK_QUEUE_TRACE_SIZE);
      const uint64_t first = kk_trace_next >= KK_QUEUE_TRACE_SIZE
                                ? kk_trace_next - KK_QUEUE_TRACE_SIZE + 1 : 1;
      for (uint64_t s = first; s <= kk_trace_next; s++) {
         const unsigned i = s % KK_QUEUE_TRACE_SIZE;
         mesa_loge("kk trace #%llu cpu_ns=%llu queue=%p %s object=%p "
                   "value=%llu complete=%u gpu=[%.6f,%.6f] label=\"%s\"",
                   (unsigned long long)s,
                   (unsigned long long)kk_trace[i].time_ns,
                   (void *)kk_trace[i].queue, kk_trace[i].operation,
                   (void *)kk_trace[i].object,
                   (unsigned long long)kk_trace[i].value, kk_trace[i].complete,
                   kk_trace[i].gpu_start, kk_trace[i].gpu_end, kk_trace[i].label);
      }
   }
   pthread_mutex_unlock(&kk_trace_mutex);
}

/* MTL4CommitOptions */
mtl_commit_options *
mtl_new_commit_options(void)
{
   @autoreleasepool {
      return [MTL4CommitOptions new];
   }
}

void
mtl_commit_options_add_feedback_handler(mtl_commit_options *options,
                                        mtl_feedback_handler_callback callback,
                                        void *data)
{
   @autoreleasepool {
      MTL4CommitOptions* opt = (MTL4CommitOptions *)options;
      [opt addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
         @autoreleasepool {
            NSError *error = feedback.error;
            if (error && debug_get_bool_option("MESA_KK_DIAGNOSTICS", false))
               mesa_loge("kk diagnostics: Metal submission error: %s",
                         error.description.UTF8String);
            struct mtl_feedback_data feedback_data = (struct mtl_feedback_data){
               .user_data = data,
               .error_message =
                  error ? error.localizedDescription.UTF8String : NULL,
               .gpu_start = feedback.GPUStartTime,
               .gpu_end = feedback.GPUEndTime,
               .error = error
                           ? (enum mtl_command_queue_error)error.code
                           : MTL_COMMAND_QUEUE_ERROR_NONE,
            };
            callback(&feedback_data);
         }
      }];
   }
}

/* MTL4CommandQueue */
mtl_command_queue *
mtl_new_command_queue(mtl_device *device)
{
   @autoreleasepool {
      id<MTLDevice> dev = (id<MTLDevice>)device;
      return [dev newMTL4CommandQueue];
   }
}

void
mtl_command_queue_add_residency_set(mtl_command_queue *cmd_queue,
                                    mtl_residency_set *set)
{
   @autoreleasepool {
      id<MTL4CommandQueue> queue = (id<MTL4CommandQueue>)cmd_queue;
      id<MTLResidencySet> s = (id<MTLResidencySet>)set;
      return [queue addResidencySet:s];
   }
}

void
mtl_command_queue_remove_residency_set(mtl_command_queue *cmd_queue,
                                       mtl_residency_set *set)
{
   @autoreleasepool {
      id<MTL4CommandQueue> queue = (id<MTL4CommandQueue>)cmd_queue;
      id<MTLResidencySet> s = (id<MTLResidencySet>)set;
      return [queue removeResidencySet:s];
   }
}

void
mtl_signal_event(mtl_command_queue *queue, mtl_event *event, uint64_t value)
{
   kk_trace_record(queue, "signal-event", event, value, NULL);
   @autoreleasepool {
      id<MTL4CommandQueue> q = (id<MTL4CommandQueue>)queue;
      id<MTLEvent> e = (id<MTLEvent>)event;
      [q signalEvent:e value:value];
   }
}

void
mtl_wait_for_event(mtl_command_queue *queue, mtl_event *event, uint64_t value)
{
   kk_trace_record(queue, "wait-event", event, value, NULL);
   @autoreleasepool {
      id<MTL4CommandQueue> q = (id<MTL4CommandQueue>)queue;
      id<MTLEvent> e = (id<MTLEvent>)event;
      [q waitForEvent:e value:value];
   }
}

void
mtl_command_queue_update_buffer_mappings(
   mtl_command_queue *queue, mtl_buffer *buffer, mtl_heap *heap,
   const struct mtl_sparse_buffer_mapping *mappings, uint32_t count)
{
   @autoreleasepool {
      id<MTL4CommandQueue> q = (id<MTL4CommandQueue>)queue;
      MTL4UpdateSparseBufferMappingOperation ops[count];
      for (uint32_t i = 0; i < count; i++) {
         ops[i] = (MTL4UpdateSparseBufferMappingOperation){
            .mode = mappings[i].map ? MTLSparseTextureMappingModeMap
                                    : MTLSparseTextureMappingModeUnmap,
            .bufferRange =
               NSMakeRange(mappings[i].first_page, mappings[i].page_count),
            .heapOffset = mappings[i].heap_page,
         };
      }
      [q updateBufferMappings:(id<MTLBuffer>)buffer
                         heap:(id<MTLHeap>)heap
                   operations:ops
                        count:count];
   }
}

void
mtl_command_queue_update_texture_mappings(
   mtl_command_queue *queue, mtl_texture *texture, mtl_heap *heap,
   const struct mtl_sparse_texture_mapping *mappings, uint32_t count)
{
   @autoreleasepool {
      id<MTL4CommandQueue> q = (id<MTL4CommandQueue>)queue;
      MTL4UpdateSparseTextureMappingOperation ops[count];
      for (uint32_t i = 0; i < count; i++) {
         const struct mtl_sparse_texture_mapping *m = &mappings[i];
         ops[i] = (MTL4UpdateSparseTextureMappingOperation){
            .mode = m->map ? MTLSparseTextureMappingModeMap
                           : MTLSparseTextureMappingModeUnmap,
            .textureRegion = MTLRegionMake3D(m->origin.x, m->origin.y,
                                             m->origin.z, m->size.x,
                                             m->size.y, m->size.z),
            .textureLevel = m->level,
            .textureSlice = m->slice,
            .heapOffset = m->heap_page,
         };
      }
      [q updateTextureMappings:(id<MTLTexture>)texture
                          heap:(id<MTLHeap>)heap
                    operations:ops
                         count:count];
   }
}

void
mtl_command_queue_commit_mapping_barrier(mtl_command_queue *queue,
                                         mtl_device *device,
                                         mtl_buffer *fill_buffer,
                                         const struct mtl_buffer_fill *fills,
                                         uint32_t fill_count)
{
   @autoreleasepool {
      id<MTL4CommandQueue> q = (id<MTL4CommandQueue>)queue;
      id<MTLDevice> dev = (id<MTLDevice>)device;
      id<MTL4CommandAllocator> allocator = [dev newCommandAllocator];
      id<MTL4CommandBuffer> cmd_buf = [dev newCommandBuffer];
      [cmd_buf beginCommandBufferWithAllocator:allocator];
      id<MTL4ComputeCommandEncoder> enc = [cmd_buf computeCommandEncoder];
      [enc barrierAfterQueueStages:MTLStageResourceState
                      beforeStages:MTLStageDispatch | MTLStageBlit
                 visibilityOptions:MTL4VisibilityOptionDevice];
      for (uint32_t i = 0; i < fill_count; i++) {
         [enc fillBuffer:(id<MTLBuffer>)fill_buffer
                   range:NSMakeRange(fills[i].offset, fills[i].size)
                   value:fills[i].value];
      }
      [enc barrierAfterStages:MTLStageDispatch | MTLStageBlit
            beforeQueueStages:MTLStageVertex | MTLStageFragment |
                              MTLStageDispatch | MTLStageBlit
            visibilityOptions:MTL4VisibilityOptionDevice];
      [enc endEncoding];
      [cmd_buf endCommandBuffer];

      MTL4CommitOptions *options = [[MTL4CommitOptions new] autorelease];
      [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
         [cmd_buf release];
         [allocator release];
      }];
      [q commit:&cmd_buf count:1 options:options];
   }
}

void
mtl_command_queue_commit(mtl_command_queue *queue,
                         mtl_command_buffer **command_buffers, uint32_t count,
                         mtl_commit_options *options)
{
   @autoreleasepool {
      id<MTL4CommandQueue> q = (id<MTL4CommandQueue>)queue;
      id<MTL4CommandBuffer> *cmds = (id<MTL4CommandBuffer> *)command_buffers;
      MTL4CommitOptions *opt = (MTL4CommitOptions *)options;
      if (debug_get_option_kk_trace_queue()) {
         NSString *label = count ? [[cmds[0].label copy] autorelease] : nil;
         const uint64_t serial = kk_trace_record(queue, "commit",
                                                   count ? command_buffers[0] : NULL,
                                                   count, label.UTF8String);
         [opt addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
            if (feedback.error)
               mesa_loge("kk failed commit #%llu queue=%p label=\"%s\" "
                         "gpu=[%.6f,%.6f]",
                         (unsigned long long)serial, queue,
                         label.UTF8String ? label.UTF8String : "",
                         feedback.GPUStartTime, feedback.GPUEndTime);
            kk_trace_complete(serial, feedback.GPUStartTime,
                               feedback.GPUEndTime, feedback.error != nil);
         }];
      }
      for (uint32_t i = 0; i < count; i++)
         mtl_progress_feedback(cmds[i], opt);
      [q commit:cmds count:count options:opt];
   }
}

void
mtl_command_queue_wait_for_drawable(mtl_command_queue *queue, void *drawable)
{
   kk_trace_record(queue, "wait-drawable", drawable, 0, NULL);
   @autoreleasepool {
      id<MTL4CommandQueue> q = (id<MTL4CommandQueue>)queue;
      id<MTLDrawable> d = (id<MTLDrawable>)drawable;
      [q waitForDrawable:d];
   }
}

void
mtl_command_queue_signal_drawable(mtl_command_queue *queue, void *drawable)
{
   kk_trace_record(queue, "signal-drawable", drawable, 0, NULL);
   @autoreleasepool {
      id<MTL4CommandQueue> q = (id<MTL4CommandQueue>)queue;
      id<MTLDrawable> d = (id<MTLDrawable>)drawable;
      [q signalDrawable:d];
   }
}
