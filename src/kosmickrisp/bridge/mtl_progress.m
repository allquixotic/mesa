/* SPDX-License-Identifier: MIT */
#include "mtl_progress.h"
#include "util/log.h"
#include "util/u_debug.h"
#include <stdatomic.h>

#import <Metal/Metal.h>
#import <objc/runtime.h>

#define KK_PROGRESS_COUNT 4096
static char command_progress_key, encoder_progress_key;
static _Atomic uint64_t next_recording;
DEBUG_GET_ONCE_BOOL_OPTION(kk_gpu_progress, "MESA_KK_GPU_PROGRESS", false)
DEBUG_GET_ONCE_BOOL_OPTION(kk_gpu_progress_dump, "MESA_KK_GPU_PROGRESS_DUMP", false)
DEBUG_GET_ONCE_BOOL_OPTION(kk_gpu_progress_full, "MESA_KK_GPU_PROGRESS_FULL", false)

/* One recording owns one heap. The feedback block retains this object, so
 * command-buffer pooling and subsequent game frames cannot erase its data. */
@interface MesaKKProgress : NSObject {
@public
   id<MTL4CounterHeap> heap;
   NSMutableArray<NSString *> *operations;
   uint64_t frequency;
   uint64_t recording;
   bool truncated;
}
@end

@implementation MesaKKProgress
- (void)dealloc
{
   [heap release];
   [operations release];
   [super dealloc];
}
@end

@interface MesaKKEncoderProgress : NSObject {
@public
   MesaKKProgress *progress;
   NSString *pipeline;
   bool render;
}
@end

@implementation MesaKKEncoderProgress
- (void)dealloc
{
   [progress release];
   [pipeline release];
   [super dealloc];
}
@end

bool
mtl_progress_enabled(void)
{
   return debug_get_option_kk_gpu_progress();
}

void
mtl_progress_begin(id command_buffer)
{
   if (!mtl_progress_enabled())
      return;

   id<MTL4CommandBuffer> cb = command_buffer;
   MesaKKProgress *p = [[MesaKKProgress alloc] init];
   p->recording = atomic_fetch_add_explicit(&next_recording, 1,
                                            memory_order_relaxed) + 1;
   p->frequency = [cb.device queryTimestampFrequency];
   MTL4CounterHeapDescriptor *desc = [[MTL4CounterHeapDescriptor alloc] init];
   desc.type = MTL4CounterHeapTypeTimestamp;
   desc.count = KK_PROGRESS_COUNT;
   NSError *error = nil;
   p->heap = [cb.device newCounterHeapWithDescriptor:desc error:&error];
   [desc release];
   if (p->heap) {
      [p->heap invalidateCounterRange:NSMakeRange(0, KK_PROGRESS_COUNT)];
      p->operations = [[NSMutableArray alloc] init];
      objc_setAssociatedObject(cb, &command_progress_key, p,
                                OBJC_ASSOCIATION_RETAIN_NONATOMIC);
   } else {
      mesa_loge("kk progress: counter heap unavailable: %s",
                error.localizedDescription.UTF8String);
      objc_setAssociatedObject(cb, &command_progress_key, nil,
                                OBJC_ASSOCIATION_RETAIN_NONATOMIC);
   }
   [p release];
}

void
mtl_progress_attach(id command_buffer, id encoder, bool render)
{
   if (!mtl_progress_enabled())
      return;
   MesaKKProgress *p = objc_getAssociatedObject(command_buffer,
                                                &command_progress_key);
   if (!p)
      return;
   MesaKKEncoderProgress *e = [[MesaKKEncoderProgress alloc] init];
   e->progress = [p retain];
   e->render = render;
   objc_setAssociatedObject(encoder, &encoder_progress_key, e,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
   [e release];
   mtl_progress_mark(encoder, render ? @"begin render" : @"begin compute");
}

void
mtl_progress_render_pass(id encoder, id descriptor)
{
   if (!mtl_progress_enabled())
      return;
   MesaKKEncoderProgress *e = objc_getAssociatedObject(encoder,
                                                       &encoder_progress_key);
   if (!e || !e->progress->operations.count || e->progress->truncated)
      return;
   MTL4RenderPassDescriptor *desc = descriptor;
   NSMutableString *info = [NSMutableString stringWithFormat:
      @"begin render targets=%lux%lu layers=%lu", desc.renderTargetWidth,
      desc.renderTargetHeight, desc.renderTargetArrayLength];
   for (unsigned i = 0; i < 10; i++) {
      MTLRenderPassAttachmentDescriptor *a = i < 8 ? desc.colorAttachments[i] :
         i == 8 ? desc.depthAttachment : desc.stencilAttachment;
      id<MTLTexture> t = a.texture;
      if (!t)
         continue;
      [info appendFormat:@" att%u={texture=%p type=%lu format=%lu size=%lux%lux%lu "
         @"level=%lu slice=%lu plane=%lu load=%lu store=%lu heap=%p offset=%lu}",
         i, t, t.textureType, t.pixelFormat, t.width, t.height, t.depth,
         a.level, a.slice, a.depthPlane, a.loadAction, a.storeAction,
         t.heap, t.heapOffset];
   }
   /* Enrich the just-recorded beginning sample without adding another GPU
    * timestamp or retaining mutable attachment descriptors/textures. */
   e->progress->operations[e->progress->operations.count - 1] = info;
}

void
mtl_progress_pipeline(id encoder, id pipeline)
{
   if (!mtl_progress_enabled())
      return;
   MesaKKEncoderProgress *e = objc_getAssociatedObject(encoder,
                                                       &encoder_progress_key);
   if (!e)
      return;
   [e->pipeline release];
   e->pipeline = [[NSString alloc] initWithFormat:@"pipeline=%p label=%@",
                      pipeline, [pipeline label] ?: @""];
}

void
mtl_progress_mark(id encoder, NSString *operation)
{
   MesaKKEncoderProgress *e = objc_getAssociatedObject(encoder,
                                                       &encoder_progress_key);
   if (!e)
      return;
   MesaKKProgress *p = e->progress;
   const NSUInteger index = p->operations.count;
   if (index >= KK_PROGRESS_COUNT) {
      p->truncated = true;
      return;
   }
   [p->operations addObject:[NSString stringWithFormat:@"%@ %@ encoder=%@",
                                operation, e->pipeline ?: @"",
                                [encoder label] ?: @""]];
   if (e->render) {
      [(id<MTL4RenderCommandEncoder>)encoder
          writeTimestampWithGranularity:MTL4TimestampGranularityPrecise
                              afterStage:MTLRenderStageFragment
                                intoHeap:p->heap atIndex:index];
   } else {
      [(id<MTL4ComputeCommandEncoder>)encoder
          writeTimestampWithGranularity:MTL4TimestampGranularityPrecise
                                intoHeap:p->heap atIndex:index];
   }
}

void
mtl_progress_feedback(id command_buffer, id options)
{
   if (!mtl_progress_enabled())
      return;
   MesaKKProgress *p = objc_getAssociatedObject(command_buffer,
                                                &command_progress_key);
   if (!p || !p->operations.count)
      return;
   NSString *label = [[[(id<MTL4CommandBuffer>)command_buffer label] copy] autorelease];
   /* The copied feedback block retains p independently of the pooled command
    * buffer. Resolve only after Metal invokes its completion feedback. */
   [(MTL4CommitOptions *)options addFeedbackHandler:^(id<MTL4CommitFeedback> f) {
      @autoreleasepool {
         if (!f.error && !debug_get_option_kk_gpu_progress_dump())
            return;
         const NSUInteger count = p->operations.count;
         NSData *data = [p->heap resolveCounterRange:NSMakeRange(0, count)];
         if (!data || data.length < count * sizeof(MTL4TimestampHeapEntry)) {
            mesa_loge("kk progress: counter resolve unavailable after %s",
                      f.error ? "GPU failure" : "completion");
            return;
         }
         const MTL4TimestampHeapEntry *entries = data.bytes;
         NSInteger last = -1;
         NSUInteger reached = 0;
         for (NSUInteger i = 0; i < count; i++) {
            if (entries[i].timestamp) {
               last = i;
               reached++;
            }
         }
         /* Metal may coalesce render-stage timestamp writes even with precise
          * granularity. A zero entry is not proof its operation never ran. */
         mesa_loge("kk progress: error=%u sampled=%lu/%lu last=%ld truncated=%u "
                   "frequency=%llu record=%llu label=\"%s\" gpu=[%.6f,%.6f]",
                   f.error != nil, reached,
                   count, last, p->truncated, (unsigned long long)p->frequency,
                   (unsigned long long)p->recording, label.UTF8String ?: "",
                   f.GPUStartTime, f.GPUEndTime);
         mesa_loge("kk progress: zero timestamps may be coalesced; timestamps "
                   "emitted during GPU recovery do not prove prior work succeeded");
         /* A watchdog can abort earlier work and let later markers execute.
          * The largest timestamp gap may be at the beginning, so preserve the
          * whole bounded recording on error rather than only its tail. */
         const bool full = f.error || debug_get_option_kk_gpu_progress_full();
         const NSUInteger first = full ? 0 : (last > 16 ? last - 16 : 0);
         const NSUInteger end = full ? count :
            MIN(count, (NSUInteger)(last + 1) + 24);
         uint64_t previous = 0;
         for (NSUInteger i = first; i < end; i++) {
            const uint64_t ts = entries[i].timestamp;
            const double delta_ms = previous && ts >= previous && p->frequency
               ? (ts - previous) * 1000.0 / p->frequency : -1.0;
            mesa_loge("kk progress #%lu timestamp=%llu delta_ms=%.6f record=%llu %s", i,
                      (unsigned long long)ts, delta_ms, (unsigned long long)p->recording,
                      p->operations[i].UTF8String);
            if (ts)
               previous = ts;
         }
      }
   }];
}
