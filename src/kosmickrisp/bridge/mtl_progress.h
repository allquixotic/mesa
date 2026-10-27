/* SPDX-License-Identifier: MIT */
#ifndef KK_MTL_PROGRESS_H
#define KK_MTL_PROGRESS_H

#include <stdbool.h>
#import <Foundation/Foundation.h>

bool mtl_progress_enabled(void);
void mtl_progress_begin(id command_buffer);
void mtl_progress_attach(id command_buffer, id encoder, bool render);
void mtl_progress_render_pass(id encoder, id descriptor);
void mtl_progress_mark(id encoder, NSString *operation);
void mtl_progress_pipeline(id encoder, id pipeline);
void mtl_progress_feedback(id command_buffer, id options);

#define MTL_PROGRESS(encoder, ...) do { \
   if (mtl_progress_enabled()) \
      mtl_progress_mark((id)(encoder), [NSString stringWithFormat:__VA_ARGS__]); \
} while (0)

#endif
