/* SPDX-License-Identifier: MIT */
#define VK_NO_PROTOTYPES
#include "volk.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <time.h>
#endif
#define CHECK(x) do { VkResult r=(x); if(r!=VK_SUCCESS){fprintf(stderr,"%s failed %d at %d\n",#x,r,__LINE__);exit(2);} }while(0)
static VkPhysicalDevice gpu;static VkDevice dev;static VkQueue queue;static _Thread_local VkCommandPool pool;
static pthread_mutex_t queue_mutex=PTHREAD_MUTEX_INITIALIZER;
static uint32_t memtype(uint32_t bits,VkMemoryPropertyFlags flags){VkPhysicalDeviceMemoryProperties p;vkGetPhysicalDeviceMemoryProperties(gpu,&p);for(unsigned i=0;i<p.memoryTypeCount;i++)if((bits&(1u<<i))&&((p.memoryTypes[i].propertyFlags&flags)==flags))return i;fprintf(stderr,"memory type unavailable\n");exit(2);}
static void barrier(VkCommandBuffer cb){VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,0,0,0);}
struct Format {VkFormat vk;unsigned bytes;int integer;uint8_t seed[16],clear[16];const char*name;};
static void run(struct Format f,unsigned layers,int volume){
 const unsigned w=37,h=19,depth=volume?5:1,levels=volume?3:4;
 VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=volume?VK_IMAGE_TYPE_3D:VK_IMAGE_TYPE_2D,.format=f.vk,.extent={w,h,depth},.mipLevels=levels,.arrayLayers=layers,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_STORAGE_BIT,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
 if(f.vk!=VK_FORMAT_R64_UINT)ii.usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
 if(f.vk==VK_FORMAT_R8G8B8A8_SRGB)ii.usage&=~VK_IMAGE_USAGE_STORAGE_BIT;
 VkImageFormatProperties fp;CHECK(vkGetPhysicalDeviceImageFormatProperties(gpu,ii.format,ii.imageType,ii.tiling,ii.usage,ii.flags,&fp));
 VkImage image;CHECK(vkCreateImage(dev,&ii,0,&image));VkMemoryRequirements mr;vkGetImageMemoryRequirements(dev,image,&mr);VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=memtype(mr.memoryTypeBits,0)};VkDeviceMemory imem;CHECK(vkAllocateMemory(dev,&ma,0,&imem));CHECK(vkBindImageMemory(dev,image,imem,0));
 VkBufferImageCopy copies[12]={0};unsigned n=0;size_t bytes=0;
 for(unsigned l=0;l<levels;l++)for(unsigned a=0;a<layers;a++){unsigned x=w>>l,y=h>>l,z=depth>>l;if(!x)x=1;if(!y)y=1;if(!z)z=1;copies[n++]=(VkBufferImageCopy){.bufferOffset=bytes,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,l,a,1},.imageExtent={x,y,z}};bytes+=(size_t)x*y*z*f.bytes;}
 VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes+16,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT};VkBuffer buffer;CHECK(vkCreateBuffer(dev,&bi,0,&buffer));vkGetBufferMemoryRequirements(dev,buffer,&mr);ma.allocationSize=mr.size;ma.memoryTypeIndex=memtype(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);VkDeviceMemory bmem;CHECK(vkAllocateMemory(dev,&ma,0,&bmem));CHECK(vkBindBufferMemory(dev,buffer,bmem,0));uint8_t*map;CHECK(vkMapMemory(dev,bmem,0,VK_WHOLE_SIZE,0,(void**)&map));memset(map,0,bytes+16);
 VkQueryPoolCreateInfo qi={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_PIPELINE_STATISTICS,.queryCount=1,.pipelineStatistics=VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT};VkQueryPool qp;CHECK(vkCreateQueryPool(dev,&qi,0,&qp));
 VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer cb;CHECK(vkAllocateCommandBuffers(dev,&ai,&cb));VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};CHECK(vkBeginCommandBuffer(cb,&begin));
 VkImageMemoryBarrier ib={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,levels,0,layers}};vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,0,0,0,1,&ib);
 VkClearColorValue seed={0},value={0};if(f.integer){seed.uint32[0]=0x01020304;seed.uint32[1]=0x05060708;seed.uint32[2]=0x09101112;seed.uint32[3]=0x13141516;value.uint32[0]=0xa1b2c3d4;value.uint32[1]=0x55667788;value.uint32[2]=0x12345678;value.uint32[3]=0x89abcdef;}else{seed.float32[1]=seed.float32[3]=1;value.float32[0]=value.float32[3]=1;}
 vkCmdClearColorImage(cb,image,VK_IMAGE_LAYOUT_GENERAL,&seed,1,&ib.subresourceRange);barrier(cb);
 vkCmdResetQueryPool(cb,qp,0,1);vkCmdBeginQuery(cb,qp,0,0);
 const size_t predicate=(bytes+3)&~(size_t)3;VkConditionalRenderingBeginInfoEXT condition={.sType=VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT,.buffer=buffer,.offset=predicate};vkCmdBeginConditionalRenderingEXT(cb,&condition);
 VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,1,volume?VK_REMAINING_MIP_LEVELS:2,layers>1?1:0,1};vkCmdClearColorImage(cb,image,VK_IMAGE_LAYOUT_GENERAL,&value,1,&range);
 if(!volume){VkImageSubresourceRange remaining={VK_IMAGE_ASPECT_COLOR_BIT,3,VK_REMAINING_MIP_LEVELS,layers>1?1:0,VK_REMAINING_ARRAY_LAYERS};vkCmdClearColorImage(cb,image,VK_IMAGE_LAYOUT_GENERAL,&value,1,&remaining);}
 vkCmdEndQuery(cb,qp,0);barrier(cb);
 /* ClearAttachments IS conditional. A zero predicate must still suppress it
    after the unconditional ClearColorImage has temporarily used meta draws. */
 VkImageView view=VK_NULL_HANDLE;
 if(!volume && f.vk!=VK_FORMAT_R64_UINT){VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=f.vk,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,1,1,layers>1?1:0,1}};CHECK(vkCreateImageView(dev,&vi,0,&view));VkRenderingAttachmentInfo att={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL,.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE};VkRenderingInfo ri={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{w>>1,h>>1}},.layerCount=1,.colorAttachmentCount=1,.pColorAttachments=&att};vkCmdBeginRendering(cb,&ri);VkClearAttachment ca={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.clearValue={.color=seed}};VkClearRect rect={.rect=ri.renderArea,.layerCount=1};vkCmdClearAttachments(cb,1,&ca,1,&rect);vkCmdEndRendering(cb);barrier(cb);}
 vkCmdEndConditionalRenderingEXT(cb);
 vkCmdCopyImageToBuffer(cb,image,VK_IMAGE_LAYOUT_GENERAL,buffer,n,copies);barrier(cb);CHECK(vkEndCommandBuffer(cb));VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};pthread_mutex_lock(&queue_mutex);CHECK(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));pthread_mutex_unlock(&queue_mutex);
 uint64_t stats=UINT64_MAX;CHECK(vkGetQueryPoolResults(dev,qp,0,1,sizeof(stats),&stats,sizeof(stats),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));if(stats){fprintf(stderr,"FAIL meta clear leaked %llu fragment invocations\n",(unsigned long long)stats);exit(3);}
 for(unsigned i=0;i<n;i++){VkBufferImageCopy*c=&copies[i];unsigned l=c->imageSubresource.mipLevel,a=c->imageSubresource.baseArrayLayer;int changed=l>=1 && (volume || (l<3?a==(layers>1?1:0):a>=(layers>1?1:0)));const uint8_t*want=changed?f.clear:f.seed;size_t count=(size_t)c->imageExtent.width*c->imageExtent.height*c->imageExtent.depth;for(size_t j=0;j<count;j++)if(memcmp(map+c->bufferOffset+j*f.bytes,want,f.bytes)){fprintf(stderr,"FAIL %s layers=%u volume=%d mip=%u layer=%u texel=%zu got=",f.name,layers,volume,l,a,j);for(unsigned k=0;k<f.bytes;k++)fprintf(stderr,"%02x",map[c->bufferOffset+j*f.bytes+k]);fprintf(stderr," expected=");for(unsigned k=0;k<f.bytes;k++)fprintf(stderr,"%02x",want[k]);fputc('\n',stderr);exit(3);}}
 printf("PASS %s layers=%u volume=%d bytes=%zu subresources=%u conditional_clear=ignored statistics=0\n",f.name,layers,volume,bytes,n);
 if(view)vkDestroyImageView(dev,view,0);vkFreeCommandBuffers(dev,pool,1,&cb);vkDestroyQueryPool(dev,qp,0);vkUnmapMemory(dev,bmem);vkDestroyBuffer(dev,buffer,0);vkFreeMemory(dev,bmem,0);vkDestroyImage(dev,image,0);vkFreeMemory(dev,imem,0);
}
static void *worker(void *data){
 struct Format *f=data;
 VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
 CHECK(vkCreateCommandPool(dev,&pci,0,&pool));
 run(*f,1,0);run(*f,3,0);
 vkDestroyCommandPool(dev,pool,0);return 0;
}
int main(int argc,char**argv){setvbuf(stdout,0,_IONBF,0);
#ifdef _WIN32
 HMODULE lib=LoadLibraryA("vulkan-1.dll");PFN_vkGetInstanceProcAddr get=(PFN_vkGetInstanceProcAddr)GetProcAddress(lib,"vkGetInstanceProcAddr");
#else
 if(argc<2)return 2;void*lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib){puts(dlerror());return 2;}PFN_vkGetInstanceProcAddr get=(PFN_vkGetInstanceProcAddr)dlsym(lib,"vkGetInstanceProcAddr");
#endif
 volkInitializeCustom(get);VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="KK clear correctness probe",.apiVersion=VK_API_VERSION_1_3};VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};VkInstance instance;CHECK(vkCreateInstance(&ici,0,&instance));volkLoadInstance(instance);uint32_t n=1;CHECK(vkEnumeratePhysicalDevices(instance,&n,&gpu));float priority=1;VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=0,.queueCount=1,.pQueuePriorities=&priority};const char*extensions[]={VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME};VkPhysicalDeviceConditionalRenderingFeaturesEXT cond={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT,.conditionalRendering=VK_TRUE};VkPhysicalDeviceVulkan13Features f13={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.pNext=&cond,.dynamicRendering=VK_TRUE};VkPhysicalDeviceFeatures features={.pipelineStatisticsQuery=VK_TRUE};VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&f13,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,.enabledExtensionCount=1,.ppEnabledExtensionNames=extensions,.pEnabledFeatures=&features};CHECK(vkCreateDevice(gpu,&dci,0,&dev));volkLoadDevice(dev);vkGetDeviceQueue(dev,0,0,&queue);VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};CHECK(vkCreateCommandPool(dev,&pci,0,&pool));
 struct Format formats[]={
 {VK_FORMAT_R8G8B8A8_UNORM,4,0,{0,255,0,255},{255,0,0,255},"RGBA8"},
 {VK_FORMAT_R8G8B8A8_SRGB,4,0,{0,255,0,255},{255,0,0,255},"RGBA8_sRGB"},
 {VK_FORMAT_B8G8R8A8_UNORM,4,0,{0,255,0,255},{0,0,255,255},"BGRA8"},
 {VK_FORMAT_R16G16B16A16_SFLOAT,8,0,{0,0,0,0x3c,0,0,0,0x3c},{0,0x3c,0,0,0,0,0,0x3c},"RGBA16F"},
 {VK_FORMAT_R32G32_UINT,8,1,{4,3,2,1,8,7,6,5},{0xd4,0xc3,0xb2,0xa1,0x88,0x77,0x66,0x55},"RG32UI"},
 {VK_FORMAT_R64_UINT,8,1,{4,3,2,1,8,7,6,5},{0xd4,0xc3,0xb2,0xa1,0x88,0x77,0x66,0x55},"R64UI"}};
 pthread_t workers[6];for(unsigned i=0;i<6;i++){if(pthread_create(&workers[i],0,worker,&formats[i]))return 4;}for(unsigned i=0;i<6;i++)pthread_join(workers[i],0);run(formats[4],1,1);
 CHECK(vkDeviceWaitIdle(dev));
#ifdef _WIN32
 Sleep(500);
#else
 struct timespec t={0,500000000};nanosleep(&t,0);
#endif
 vkDestroyCommandPool(dev,pool,0);vkDestroyDevice(dev,0);vkDestroyInstance(instance,0);return 0;
}
