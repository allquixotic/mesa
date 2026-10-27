/* SPDX-License-Identifier: MIT */
#define VK_NO_PROTOTYPES
#include "volk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }
static int cmp_double(const void *a,const void *b) { double x=*(const double*)a,y=*(const double*)b; return (x>y)-(x<y); }
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include "compute.comp.h"
#include "exact_draw.vert.h"

#define CHECK(call) do { VkResult result=(call); if(result!=VK_SUCCESS){fprintf(stderr,"%s failed: %d at %d\n",#call,result,__LINE__);exit(2);} } while(0)
static VkDevice dev;
static VkPhysicalDevice gpu;
typedef struct Buffer { VkBuffer handle; VkDeviceMemory memory; void *mapped; } Buffer;
static Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer out={0}; VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,.usage=usage};
    CHECK(vkCreateBuffer(dev,&bi,NULL,&out.handle));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev,out.handle,&req);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(gpu,&mp);
    uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<mp.memoryTypeCount;i++) if((req.memoryTypeBits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){type=i;break;}
    if(type==UINT32_MAX){fprintf(stderr,"No coherent host memory\n");exit(2);}
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
    CHECK(vkAllocateMemory(dev,&ai,NULL,&out.memory)); CHECK(vkBindBufferMemory(dev,out.handle,out.memory,0)); CHECK(vkMapMemory(dev,out.memory,0,VK_WHOLE_SIZE,0,&out.mapped)); return out;
}
static void free_buffer(Buffer b) { vkUnmapMemory(dev,b.memory); vkDestroyBuffer(dev,b.handle,NULL); vkFreeMemory(dev,b.memory,NULL); }
static VkShaderModule shader(const uint32_t *code,size_t size) { VkShaderModule s; VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=size,.pCode=code}; CHECK(vkCreateShaderModule(dev,&ci,NULL,&s));return s; }
int main(int argc,char**argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    const uint32_t max_draws=getenv("PROBE_MAX_DRAWS")?atoi(getenv("PROBE_MAX_DRAWS")):1024;
    uint32_t real_draws=64;
    if(max_draws<real_draws || max_draws>65536) return 2;
#ifdef _WIN32
    HMODULE lib=LoadLibraryA("vulkan-1.dll");
    PFN_vkGetInstanceProcAddr get=(PFN_vkGetInstanceProcAddr)GetProcAddress(lib,"vkGetInstanceProcAddr");
#else
    if(argc<2) return 2;
    void*lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); if(!lib){fprintf(stderr,"%s\n",dlerror());return 2;}
    PFN_vkGetInstanceProcAddr get=(PFN_vkGetInstanceProcAddr)dlsym(lib,"vkGetInstanceProcAddr");
#endif
    if(!get)return 2; volkInitializeCustom(get);
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Enshrouded runtime audit probe",.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app}; VkInstance instance;
    CHECK(vkCreateInstance(&ici,NULL,&instance)); volkLoadInstance(instance);
    uint32_t n=1; CHECK(vkEnumeratePhysicalDevices(instance,&n,&gpu));
    VkPhysicalDeviceDriverProperties driver={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&driver}; vkGetPhysicalDeviceProperties2(gpu,&props);
    printf("device=%s driver=%s info=%s\n",props.properties.deviceName,driver.driverName,driver.driverInfo);
    n=0;vkGetPhysicalDeviceQueueFamilyProperties(gpu,&n,NULL);VkQueueFamilyProperties*qps=calloc(n,sizeof(*qps));vkGetPhysicalDeviceQueueFamilyProperties(gpu,&n,qps);
    uint32_t family=UINT32_MAX;for(uint32_t i=0;i<n;i++)if((qps[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)){family=i;break;} free(qps); if(family==UINT32_MAX)return 2;
    VkPhysicalDeviceVulkan11Features f11={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,.shaderDrawParameters=VK_TRUE};
    VkPhysicalDeviceConditionalRenderingFeaturesEXT cr={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT,.conditionalRendering=VK_TRUE};
    VkPhysicalDeviceVulkan13Features f13={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,.pNext=&f13};
    VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&f12};vkGetPhysicalDeviceFeatures2(gpu,&features);
    printf("drawIndirectCount=%u dynamicRendering=%u vertexStores=%u\n",f12.drawIndirectCount,f13.dynamicRendering,features.features.vertexPipelineStoresAndAtomics);
    if(!f12.drawIndirectCount||!f13.dynamicRendering||!features.features.vertexPipelineStoresAndAtomics)return 2;
    f12=(VkPhysicalDeviceVulkan12Features){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,.pNext=&f13,.drawIndirectCount=VK_TRUE};
    f13=(VkPhysicalDeviceVulkan13Features){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.dynamicRendering=VK_TRUE};
    VkPhysicalDeviceFeatures enabled={.multiDrawIndirect=VK_TRUE,.vertexPipelineStoresAndAtomics=VK_TRUE};
    n=0;CHECK(vkEnumerateDeviceExtensionProperties(gpu,NULL,&n,NULL));VkExtensionProperties*eps=calloc(n,sizeof(*eps));CHECK(vkEnumerateDeviceExtensionProperties(gpu,NULL,&n,eps));
    const char*extensions[2];uint32_t extn=0;for(uint32_t i=0;i<n;i++)if(!strcmp(eps[i].extensionName,"VK_KHR_portability_subset"))extensions[extn++]="VK_KHR_portability_subset";free(eps);
    extensions[extn++]="VK_EXT_conditional_rendering"; f13.pNext=&f11; f11.pNext=&cr; enabled.drawIndirectFirstInstance=VK_TRUE; enabled.robustBufferAccess=getenv("PROBE_ROBUST")!=NULL;
    float priority=1;VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&f12,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.pEnabledFeatures=&enabled,.enabledExtensionCount=extn,.ppEnabledExtensionNames=extensions};CHECK(vkCreateDevice(gpu,&di,NULL,&dev));volkLoadDevice(dev);
    VkQueue queue;vkGetDeviceQueue(dev,family,0,&queue);
    int indexed=getenv("PROBE_INDEXED")!=NULL;
    Buffer index=buffer(64,VK_BUFFER_USAGE_INDEX_BUFFER_BIT); for(int i=0;i<16;i++)if(getenv("PROBE_UINT16"))((uint16_t*)index.mapped)[i]=100+i;else ((uint32_t*)index.mapped)[i]=100+i;
    Buffer condition=buffer(4,VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT); *(uint32_t*)condition.mapped=1;
    Buffer data=buffer(256,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT),readback=buffer(256,VK_BUFFER_USAGE_TRANSFER_DST_BIT),indirect=buffer(max_draws*sizeof(VkDrawIndexedIndirectCommand),VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT),count=buffer(4,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    memset(data.mapped,0,256);memset(readback.mapped,0,256);*(uint32_t*)count.mapped=real_draws;
    if(indexed){VkDrawIndexedIndirectCommand*draws=indirect.mapped;for(int i=0;i<max_draws;i++)draws[i]=(VkDrawIndexedIndirectCommand){.indexCount=i%7?1+i%2:0,.instanceCount=i%5?1+i%3:0,.firstIndex=i%3,.vertexOffset=i%7-3,.firstInstance=i%11};}
    else {VkDrawIndirectCommand*draws=indirect.mapped;for(int i=0;i<max_draws;i++)draws[i]=(VkDrawIndirectCommand){.vertexCount=i%7?1+i%2:0,.instanceCount=i%5?1+i%3:0,.firstVertex=i%13,.firstInstance=i%11};}
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_VERTEX_BIT};
    VkDescriptorSetLayoutCreateInfo sli={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};VkDescriptorSetLayout sl;CHECK(vkCreateDescriptorSetLayout(dev,&sli,NULL,&sl));
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};VkDescriptorPool dp;CHECK(vkCreateDescriptorPool(dev,&dpi,NULL,&dp));
    VkDescriptorSetAllocateInfo sai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=dp,.descriptorSetCount=1,.pSetLayouts=&sl};VkDescriptorSet ds;CHECK(vkAllocateDescriptorSets(dev,&sai,&ds));
    VkDescriptorBufferInfo dbi={data.handle,0,256};VkWriteDescriptorSet w={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&dbi};vkUpdateDescriptorSets(dev,1,&w,0,NULL);
    VkPipelineLayoutCreateInfo pli={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&sl};VkPipelineLayout pl;CHECK(vkCreatePipelineLayout(dev,&pli,NULL,&pl));
    VkShaderModule cs=shader(compute_spv,sizeof(compute_spv)),vs=shader(vertex_spv,sizeof(vertex_spv));
    VkComputePipelineCreateInfo cpi={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=cs,.pName="main"},.layout=pl};VkPipeline cp;CHECK(vkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&cpi,NULL,&cp));
    VkCommandPoolCreateInfo cpooli={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};VkCommandPool cpool;CHECK(vkCreateCommandPool(dev,&cpooli,NULL,&cpool));
    VkCommandBufferAllocateInfo cbi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cpool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer cb;CHECK(vkAllocateCommandBuffers(dev,&cbi,&cb));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};CHECK(vkBeginCommandBuffer(cb,&begin));vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,cp);vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&ds,0,NULL);
    VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
    for(int i=0;i<64;i++){vkCmdDispatch(cb,1,1,1);vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,1,&mb,0,NULL,0,NULL);}
    mb.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,NULL,0,NULL);VkBufferCopy copy={0,0,256};vkCmdCopyBuffer(cb,data.handle,readback.handle,1,&copy);mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};CHECK(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
    for(int i=0;i<64;i++)if(((uint32_t*)readback.mapped)[i]!=64){fprintf(stderr,"COMPUTE BARRIER FAILURE at %d: %u\n",i,((uint32_t*)readback.mapped)[i]);return 3;}printf("compute_barriers_and_deferred_copy=PASS\n");
    VkPipelineShaderStageCreateInfo stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vs,.pName="main"};VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_POINT_LIST};VkViewport viewport={0,0,1,1,0,1};VkRect2D rect={{0,0},{1,1}};VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&rect};VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.rasterizerDiscardEnable=VK_TRUE,.polygonMode=VK_POLYGON_MODE_FILL,.lineWidth=1};VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};VkPipelineRenderingCreateInfo rendering={.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};VkGraphicsPipelineCreateInfo gi={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&rendering,.stageCount=1,.pStages=&stage,.pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&rs,.pMultisampleState=&ms,.layout=pl};VkPipeline gp;CHECK(vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gi,NULL,&gp));
    double record_start=now_ms();
    CHECK(vkResetCommandBuffer(cb,0));CHECK(vkBeginCommandBuffer(cb,&begin));VkRenderingInfo ri={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea=rect,.layerCount=1};vkCmdBeginRendering(cb,&ri);vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,gp);vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pl,0,1,&ds,0,NULL);VkConditionalRenderingBeginInfoEXT cond={.sType=VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT,.buffer=condition.handle,.flags=getenv("PROBE_INVERT")?VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT:0}; if(getenv("PROBE_CONDITIONAL"))vkCmdBeginConditionalRenderingEXT(cb,&cond); if(indexed){vkCmdBindIndexBuffer(cb,index.handle,0,getenv("PROBE_UINT16")?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32);vkCmdDrawIndexedIndirectCount(cb,indirect.handle,0,count.handle,0,max_draws,sizeof(VkDrawIndexedIndirectCommand));}else{vkCmdDrawIndirectCount(cb,indirect.handle,0,count.handle,0,max_draws,sizeof(VkDrawIndirectCommand));}if(getenv("PROBE_CONDITIONAL"))vkCmdEndConditionalRenderingEXT(cb); vkCmdEndRendering(cb);mb.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(cb));
    double record_ms=now_ms()-record_start;
    VkCommandBuffer producer; CHECK(vkAllocateCommandBuffers(dev,&cbi,&producer));
    int failed=0;
    const uint32_t counts[]={0,1,31,32,65,255,511,1024,65536,64,0,4096};
    for(unsigned pass=0;pass<sizeof(counts)/sizeof(counts[0]);pass++){
        memset(data.mapped,0,256); *(uint32_t*)count.mapped=counts[pass];
        *(uint32_t*)condition.mapped=pass%2;
        uint32_t actual=counts[pass]<max_draws?counts[pass]:max_draws;
        if(getenv("PROBE_CONDITIONAL") && ((pass%2!=0)!=(getenv("PROBE_INVERT")==NULL)))actual=0;
        uint32_t expected[6]={0};
        for(uint32_t i=0;i<actual;i++)for(uint32_t inst=0;inst<(i%5?1+i%3:0);inst++)for(uint32_t v=0;v<(i%7?1+i%2:0);v++){
          expected[0]++; expected[1]+=i; expected[2]+=indexed?100+i%3+v+(int)(i%7)-3:i%13+v;
          expected[3]+=i%11+inst; expected[4]+=indexed?(int)(i%7)-3:i%13; expected[5]+=i%11;
        }
        if(getenv("PROBE_GPU_COUNT")){
          *(uint32_t*)count.mapped=0xdeadbeefu;
          CHECK(vkResetCommandBuffer(producer,0)); CHECK(vkBeginCommandBuffer(producer,&begin));
          vkCmdUpdateBuffer(producer,count.handle,0,4,&counts[pass]);
          VkMemoryBarrier prod={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_INDIRECT_COMMAND_READ_BIT};
          vkCmdPipelineBarrier(producer,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,0,1,&prod,0,NULL,0,NULL);
          CHECK(vkEndCommandBuffer(producer)); VkSubmitInfo psi=si;psi.pCommandBuffers=&producer;CHECK(vkQueueSubmit(queue,1,&psi,VK_NULL_HANDLE));
        }
        unsigned repeats=getenv("PROBE_SIMULTANEOUS")?3:1;
        for(unsigned r=0;r<repeats;r++)CHECK(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(queue));
        for(int j=0;j<6;j++)if(((uint32_t*)data.mapped)[j]!=expected[j]*repeats){fprintf(stderr,"FAIL pass=%u count=%u field=%d got=%u expected=%u\n",pass,counts[pass],j,((uint32_t*)data.mapped)[j],expected[j]*repeats);failed=1;}
        printf("EXACT count=%u actual=%u indexed=%d repeats=%u result=%s\n",counts[pass],actual,indexed,repeats,failed?"FAIL":"PASS");
    }
    CHECK(vkDeviceWaitIdle(dev));vkDestroyPipeline(dev,gp,NULL);vkDestroyPipeline(dev,cp,NULL);vkDestroyShaderModule(dev,vs,NULL);vkDestroyShaderModule(dev,cs,NULL);vkDestroyCommandPool(dev,cpool,NULL);vkDestroyPipelineLayout(dev,pl,NULL);vkDestroyDescriptorPool(dev,dp,NULL);vkDestroyDescriptorSetLayout(dev,sl,NULL);free_buffer(data);free_buffer(readback);free_buffer(indirect);free_buffer(count);free_buffer(index);free_buffer(condition);vkDestroyDevice(dev,NULL);vkDestroyInstance(instance,NULL);return failed?4:0;
}
