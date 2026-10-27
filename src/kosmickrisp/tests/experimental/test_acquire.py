# SPDX-License-Identifier: MIT
from pathlib import Path
import subprocess
import sys
import tempfile

# Compile the actual acquire function against a controlled fence/drawable backend.
# The unpatched function must fail the pending-image reuse test.
repo = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]
_temporary = tempfile.TemporaryDirectory(prefix='kk-acquire-test-')
root = Path(_temporary.name)
relative = 'src/vulkan/wsi/wsi_common_metal.c'
prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define VK_NULL_HANDLE 0
#define VK_SUCCESS 0
#define VK_NOT_READY 1
#define VK_TIMEOUT 2
#define VK_ERROR_DEVICE_LOST (-4)
#define WSI_METAL_FETCH_WAIT_FRAMES 4
#define thrd_success 0
#define thrd_timedout 1
#define MIN2(a,b) ((a)<(b)?(a):(b))
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
typedef int VkResult;
typedef uint64_t VkFence;
typedef void CAMetalDrawable;
typedef struct { uint64_t timeout; } VkAcquireNextImageInfoKHR;
struct fake_metal { void (*bind_drawable_to_vkimage)(void*,void*); };
struct fake_wsi {
   bool sw;
   VkResult (*WaitForFences)(void*,uint32_t,const VkFence*,bool,uint64_t);
   struct fake_metal metal;
};
struct wsi_image { struct { void *image; } blit; };
struct wsi_metal_image { struct wsi_image base; void *drawable; };
struct wsi_swapchain {
   uint32_t image_count;
   VkFence fences[3];
   struct fake_wsi *wsi;
   void *device;
   int image_info;
};
struct wsi_metal_present_info {
   bool fetch_done, fetch_running;
   void *fetched, *fetch_queue;
   int mutex, present_id_cond;
};
struct wsi_metal_swapchain {
   struct wsi_swapchain base;
   struct wsi_metal_present_info *present_info;
   uint32_t current_image_index, last_presented_index;
   void *layer;
   struct wsi_metal_image images[3];
};
static unsigned waits, binds, records;
static uint64_t ticks, wait_timeout;
static VkResult fence_result;
static uint64_t os_time_get_nano(void) { ticks+=10; return ticks; }
static uint64_t os_time_get_absolute_timeout(uint64_t t) { return t==UINT64_MAX?t:os_time_get_nano()+t; }
static uint64_t wsi_metal_swapchain_frame_period_ns(void*p) { return 1000; }
static void mtx_lock(void*p) {}
static void mtx_unlock(void*p) {}
static void wsi_metal_retain_present_info(void*p) {}
static void wsi_metal_fetch_complete(void) {}
static void wsi_metal_layer_fetch_drawable(void*a,void*b,void(*c)(void),void*d) { abort(); }
static void wsi_metal_swapchain_query_refresh_rate(void*p) {}
static bool wsi_metal_swapchain_shows_nothing(void*p,VkFence f) { return false; }
static void timespec_from_nsec(struct timespec*p,uint64_t t) {}
static int u_cnd_monotonic_timedwait(void*a,void*b,void*c) { abort(); }
static void bind_image(void*a,void*b) { binds++; }
static VkResult wsi_cmd_blit_image_to_image(void*a,void*b,void*c) { records++; return VK_SUCCESS; }
static VkResult wait_fence(void*d,uint32_t n,const VkFence*f,bool all,uint64_t timeout) {
   if(n!=1 || *f!=42 || !all) abort();
   waits++;wait_timeout=timeout;return fence_result;
}
'''
suffix = r'''
#define EXPECT(x) do { if(!(x)){ fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } }while(0)
int main(void) {
   for(unsigned test=0;test<6;test++) {
      waits=binds=records=0;ticks=100;wait_timeout=0;
      struct fake_wsi wsi={.WaitForFences=wait_fence,.metal={bind_image}};
      struct wsi_metal_present_info pi={.fetch_done=true,.fetched=(void*)123};
      struct wsi_metal_swapchain c={.base={.image_count=3,.fences={0,42,0},.wsi=&wsi},.present_info=&pi,.current_image_index=1,.last_presented_index=1};
      VkAcquireNextImageInfoKHR info={.timeout=1000};
      fence_result=VK_TIMEOUT;
      if(test==1) info.timeout=0;
      if(test==2) fence_result=VK_ERROR_DEVICE_LOST;
      if(test>=3) fence_result=VK_SUCCESS;
      if(test==4) c.base.fences[1]=VK_NULL_HANDLE;
      if(test==5) info.timeout=UINT64_MAX;
      uint32_t index=999;
      VkResult r=wsi_metal_swapchain_acquire_next_image(&c.base,&info,&index);
      if(test<3) {
         EXPECT(r==(test==0?VK_TIMEOUT:test==1?VK_NOT_READY:VK_ERROR_DEVICE_LOST));
         EXPECT(index==999 && c.current_image_index==1);
         EXPECT(binds==0 && records==0 && pi.fetch_done && pi.fetched==(void*)123);
      } else {
         EXPECT(r==VK_SUCCESS && index==1 && c.current_image_index==2);
         EXPECT(binds==1 && records==1 && !pi.fetch_done);
      }
      EXPECT(waits==(test==4?0:1));
      if(test==0 || test==2 || test==3) EXPECT(wait_timeout>0 && wait_timeout<info.timeout);
      if(test==1) EXPECT(wait_timeout==0);
      if(test==5) EXPECT(wait_timeout==UINT64_MAX);
      printf("acquire fence case %u PASS\n",test);
   }
   return 0;
}
'''
base = subprocess.check_output(['git', '-C', str(repo), 'show', '4d597245b76aa01bfda433da5e1c95d0e036f045:' + relative], text=True)
for name, source in [('before', base), ('after', (repo/relative).read_text())]:
    start = source.index('static VkResult\nwsi_metal_swapchain_acquire_next_image(')
    end = source.index('\nstatic VkResult\nwsi_metal_swapchain_release_images(', start)
    test = root/f'acquire-{name}.c'
    test.write_text(prefix+source[start:end]+suffix)
    binary = root/f'acquire-{name}'
    subprocess.run(['xcrun','clang','-std=c11','-Wno-incompatible-pointer-types','-o',str(binary),str(test)],check=True)
    result = subprocess.run([str(binary)],capture_output=True,text=True)
    (root/f'acquire-{name}.log').write_text(result.stdout+result.stderr)
    print(name, 'exit', result.returncode, result.stdout+result.stderr)
    assert result.returncode == (1 if name=='before' else 0)
