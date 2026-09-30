// System-loaded Refract layer. No application library replacement or ELF rewriting.
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include "vulkan_descriptor_template.h"
#include "vulkan_texture_transcoder.h"

namespace {
constexpr const char* layerName="VK_LAYER_REFRACT_runtime";
void* key(const void* h){return h?*reinterpret_cast<void* const*>(h):nullptr;}
struct Instance {VkInstance handle;PFN_vkGetInstanceProcAddr gipa;};
// Descriptor entry points are resolved once in vkCreateDevice: games update
// descriptors thousands of times a frame, and a by-name lookup each time is not free.
struct Device {
    VkDevice handle;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkCreateDescriptorUpdateTemplate createTemplate = nullptr;
    PFN_vkDestroyDescriptorUpdateTemplate destroyTemplate = nullptr;
    PFN_vkUpdateDescriptorSetWithTemplate updateTemplate = nullptr;
    PFN_vkUpdateDescriptorSets updateSets = nullptr;
    uint32_t cachedTypes = 0;    // HOST_VISIBLE + HOST_CACHED + HOST_COHERENT memory types.
    uint32_t uncachedTypes = 0;  // HOST_VISIBLE types without HOST_CACHED.
    std::shared_ptr<refract::TextureTranscoder> transcoder;  // debug.refract.transcode_textures=1 and BC supported
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkCmdCopyBufferToImage copyBufferToImage = nullptr;
};
// Lookups on the per-draw path take a shared lock; only create/destroy calls write.
std::shared_mutex mutex;
std::map<void*,Instance> instances;
std::map<void*,Device> devices;
using Layout=refract::DescriptorTemplateLayout;
std::map<std::pair<VkDevice,VkDescriptorUpdateTemplate>,std::shared_ptr<const Layout>> templates;
Instance instance(const void* h){std::shared_lock lock(mutex);return instances.at(key(h));}
Device device(const void* h){std::shared_lock lock(mutex);return devices.at(key(h));}
template<class T>T function(Instance s,const char* n){return reinterpret_cast<T>(s.gipa(s.handle,n));}
template<class T>T function(Device s,const char* n){return reinterpret_cast<T>(s.gdpa(s.handle,n));}
// Opt-in timings (debug.refract.descriptor_profile=1); the normal path performs no clock reads.
struct DescriptorProfile {
    using Clock=std::chrono::steady_clock;
    static bool enabled(){static const bool active=[] {
        char value[PROP_VALUE_MAX]{};__system_property_get("debug.refract.descriptor_profile",value);
        return !std::strcmp(value,"1");}();return active;}
    struct Totals {uint64_t calls=0,expanded=0,writes=0,lookup=0,expand=0,driver=0;};
    bool active=enabled();
    Clock::time_point start{},lookedUp{},expanded{};
    DescriptorProfile(){if(active)start=Clock::now();}
    void lookupDone(){if(active)lookedUp=Clock::now();}
    void expansionDone(){if(active)expanded=Clock::now();}
    void finish(bool converted,size_t writes){
        if(!active)return;
        const auto end=Clock::now();
        auto ns=[](auto a,auto b){return std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count();};
        thread_local Totals totals;
        ++totals.calls;totals.expanded+=converted;totals.writes+=writes;
        totals.lookup+=ns(start,lookedUp);totals.expand+=ns(lookedUp,expanded);totals.driver+=ns(expanded,end);
        if(totals.calls%4096==0)__android_log_print(ANDROID_LOG_INFO,"Refract.Descriptors",
            "thread calls=%llu expanded=%llu fallback=%llu writes=%llu avg_us lookup=%.3f expand=%.3f downstream=%.3f",
            (unsigned long long)totals.calls,(unsigned long long)totals.expanded,
            (unsigned long long)(totals.calls-totals.expanded),(unsigned long long)totals.writes,
            totals.lookup/(1000.0*totals.calls),totals.expand/(1000.0*totals.calls),totals.driver/(1000.0*totals.calls));
    }
};
struct Promotion {const char* name;uint32_t api,revision;};
constexpr Promotion promotions[]={
    {VK_KHR_MULTIVIEW_EXTENSION_NAME,VK_API_VERSION_1_1,VK_KHR_MULTIVIEW_SPEC_VERSION},
    {VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,VK_API_VERSION_1_2,VK_KHR_CREATE_RENDERPASS_2_SPEC_VERSION},
    {VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,VK_API_VERSION_1_2,VK_KHR_DEPTH_STENCIL_RESOLVE_SPEC_VERSION}};
uint32_t apiVersion(Instance s,VkPhysicalDevice p){VkPhysicalDeviceProperties v{};function<PFN_vkGetPhysicalDeviceProperties>(s,"vkGetPhysicalDeviceProperties")(p,&v);return v.apiVersion;}
bool has(const std::vector<VkExtensionProperties>& es,const char* n){return std::any_of(es.begin(),es.end(),[&](auto& e){return !std::strcmp(e.extensionName,n);});}
VkResult extensions(Instance s,VkPhysicalDevice p,std::vector<VkExtensionProperties>& es){
    auto enumerate=function<PFN_vkEnumerateDeviceExtensionProperties>(s,"vkEnumerateDeviceExtensionProperties");
    for(int i=0;i<4;++i){uint32_t n=0;auto r=enumerate(p,nullptr,&n,nullptr);if(r!=VK_SUCCESS)return r;es.resize(n);r=enumerate(p,nullptr,&n,es.data());es.resize(n);if(r!=VK_INCOMPLETE)return r;}return VK_INCOMPLETE;
}
const char* alias(const char* name){
    if(!std::strcmp(name,"vkCreateRenderPass2KHR"))return "vkCreateRenderPass2";
    if(!std::strcmp(name,"vkCmdBeginRenderPass2KHR"))return "vkCmdBeginRenderPass2";
    if(!std::strcmp(name,"vkCmdNextSubpass2KHR"))return "vkCmdNextSubpass2";
    if(!std::strcmp(name,"vkCmdEndRenderPass2KHR"))return "vkCmdEndRenderPass2";
    return name;
}
PFN_vkVoidFunction intercept(const char*);
// debug.refract.cached_buffer_memory=1 (read once): gfxstream's uncached HOST_VISIBLE memory is very slow
// for the CPU to touch, and Unity maps its dynamic vertex/uniform buffers there. When a buffer can also live
// in a cached+coherent type, hide the uncached ones from its requirements so the game picks the cached one.
// Images, allocations and flags are untouched; a buffer without a cached option keeps the driver's mask.
// (From AXRB-BS PR #9: Batman went 57 -> 88 fps in the same room.)
bool cachedBufferMemory(){
    static const bool active=[] {char value[PROP_VALUE_MAX]{};
        __system_property_get("debug.refract.cached_buffer_memory",value);
        return !std::strcmp(value,"1");}();
    return active;
}
void filterBufferMemory(const Device& d,VkMemoryRequirements* requirements){
    const uint32_t before=requirements->memoryTypeBits;
    if(!(before&d.cachedTypes))return;
    requirements->memoryTypeBits=before&~d.uncachedTypes;
    static std::atomic<unsigned> reports{0};
    if(requirements->memoryTypeBits!=before&&reports.fetch_add(1,std::memory_order_relaxed)<8)
        __android_log_print(ANDROID_LOG_INFO,"Refract.CachedBuffers","buffer size=%llu types %x -> %x",
            (unsigned long long)requirements->size,before,requirements->memoryTypeBits);
}
// debug.refract.transcode_textures=1 (read once): ETC2/EAC textures become BC textures (see vulkan_texture_transcoder.h).
bool transcodeTextures(){
    static const bool active=[] {char value[PROP_VALUE_MAX]{};
        __system_property_get("debug.refract.transcode_textures",value);
        return !std::strcmp(value,"1");}();
    return active;
}
// debug.refract.vram_stats=1 (read once): accounts live device memory per memory type and bound images per
// format, logged under Refract.VRAM every 5 s while something changes. The sizes are what gfxstream reports,
// so an emulated (ASTC/ETC2) image includes its decompressed copy; "raw" is what the texels would take natively.
bool vramStats(){
    static const bool active=[] {char value[PROP_VALUE_MAX]{};
        __system_property_get("debug.refract.vram_stats",value);
        return !std::strcmp(value,"1");}();
    return active;
}
struct VramStats {
    struct Image {VkFormat format;VkExtent3D extent;uint32_t mips,layers,samples;VkImageUsageFlags usage;
        VkDeviceSize size=0;bool bound=false;};
    struct Format {uint64_t count=0,bytes=0,raw=0,mipped=0;};
    std::mutex lock;
    std::map<VkImage,Image> images;
    std::map<VkDeviceMemory,std::pair<VkDeviceSize,uint32_t>> memories;
    uint64_t typeBytes[VK_MAX_MEMORY_TYPES]{};
    uint64_t typeCount[VK_MAX_MEMORY_TYPES]{};
    std::chrono::steady_clock::time_point last{};
    bool dirty=false;
    static bool astc(VkFormat f){return f>=VK_FORMAT_ASTC_4x4_UNORM_BLOCK&&f<=VK_FORMAT_ASTC_12x12_SRGB_BLOCK;}
    static bool etc(VkFormat f){return f>=VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK&&f<=VK_FORMAT_EAC_R11G11_SNORM_BLOCK;}
    // Block footprint and bytes of the compressed formats games upload; 0 for anything else.
    static void block(VkFormat f,uint32_t& w,uint32_t& h,uint32_t& bytes){
        static constexpr uint8_t astcDims[14][2]={{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
        w=h=bytes=0;
        if(astc(f)){auto& d=astcDims[(f-VK_FORMAT_ASTC_4x4_UNORM_BLOCK)/2];w=d[0];h=d[1];bytes=16;}
        else if(etc(f)){w=h=4;bytes=(f<=VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK||f==VK_FORMAT_EAC_R11_UNORM_BLOCK||f==VK_FORMAT_EAC_R11_SNORM_BLOCK)?8:16;}
        else if(f>=VK_FORMAT_BC1_RGB_UNORM_BLOCK&&f<=VK_FORMAT_BC7_SRGB_BLOCK){w=h=4;
            bytes=(f<=VK_FORMAT_BC1_RGBA_SRGB_BLOCK||f==VK_FORMAT_BC4_UNORM_BLOCK||f==VK_FORMAT_BC4_SNORM_BLOCK)?8:16;}
    }
    static uint64_t rawBytes(const Image& i){
        uint32_t bw,bh,bb;block(i.format,bw,bh,bb);if(!bb)return 0;
        uint64_t total=0;
        for(uint32_t m=0;m<i.mips;++m){uint64_t w=std::max(1u,i.extent.width>>m),h=std::max(1u,i.extent.height>>m);
            total+=((w+bw-1)/bw)*((h+bh-1)/bh)*bb*std::max(1u,i.extent.depth>>m);}
        return total*i.layers;
    }
    void changed(){
        dirty=true;const auto now=std::chrono::steady_clock::now();
        if(now-last<std::chrono::seconds(5))return;
        last=now;dirty=false;
        uint64_t total=0;for(auto b:typeBytes)total+=b;
        __android_log_print(ANDROID_LOG_INFO,"Refract.VRAM","device memory %.1f MB in %zu allocations",total/1048576.0,memories.size());
        for(uint32_t t=0;t<VK_MAX_MEMORY_TYPES;++t)if(typeCount[t])
            __android_log_print(ANDROID_LOG_INFO,"Refract.VRAM","  type %u: %.1f MB, %llu allocations",t,typeBytes[t]/1048576.0,(unsigned long long)typeCount[t]);
        std::map<VkFormat,Format> formats;Format attachments,sampled;
        for(auto& [handle,i]:images){if(!i.bound)continue;
            const bool target=i.usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
            auto& f=formats[i.format];++f.count;f.bytes+=i.size;f.raw+=rawBytes(i);f.mipped+=i.mips>1;
            auto& c=target?attachments:sampled;++c.count;c.bytes+=i.size;}
        __android_log_print(ANDROID_LOG_INFO,"Refract.VRAM","images: render targets %.1f MB (%llu), other %.1f MB (%llu)",
            attachments.bytes/1048576.0,(unsigned long long)attachments.count,sampled.bytes/1048576.0,(unsigned long long)sampled.count);
        std::vector<std::pair<VkFormat,Format>> sorted(formats.begin(),formats.end());
        std::sort(sorted.begin(),sorted.end(),[](auto& a,auto& b){return a.second.bytes>b.second.bytes;});
        for(size_t n=0;n<sorted.size()&&n<16;++n){auto& [format,f]=sorted[n];
            __android_log_print(ANDROID_LOG_INFO,"Refract.VRAM","  format %d: %.1f MB, %llu images (%llu mipmapped), raw %.1f MB",
                format,f.bytes/1048576.0,(unsigned long long)f.count,(unsigned long long)f.mipped,f.raw/1048576.0);}
    }
} vram;
}
extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* count,VkLayerProperties* out){
    if(!out){*count=1;return VK_SUCCESS;}if(!*count)return VK_INCOMPLETE;
    *out={};std::strcpy(out->layerName,layerName);std::strcpy(out->description,"Refract Android runtime compatibility");
    out->specVersion=VK_API_VERSION_1_3;out->implementationVersion=1;*count=1;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice,uint32_t* count,VkLayerProperties* out){return vkEnumerateInstanceLayerProperties(count,out);}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* name,uint32_t* count,VkExtensionProperties*){
    if(!name||std::strcmp(name,layerName))return VK_ERROR_LAYER_NOT_PRESENT;*count=0;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice h,const char* name,uint32_t* count,VkExtensionProperties* out){
    if(name){if(std::strcmp(name,layerName))return VK_ERROR_LAYER_NOT_PRESENT;*count=0;return VK_SUCCESS;}
    auto s=instance(h);std::vector<VkExtensionProperties> es;auto r=extensions(s,h,es);if(r!=VK_SUCCESS)return r;
    auto api=apiVersion(s,h);for(auto& p:promotions)if(api>=p.api&&!has(es,p.name)){VkExtensionProperties e{};std::strcpy(e.extensionName,p.name);e.specVersion=p.revision;es.push_back(e);}
    if(!out){*count=es.size();return VK_SUCCESS;}uint32_t n=std::min<uint32_t>(*count,es.size());std::copy_n(es.data(),n,out);*count=n;return n<es.size()?VK_INCOMPLETE:VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* info,const VkAllocationCallbacks* alloc,VkInstance* out){
    auto* chain=reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(info->pNext));
    while(chain&&(chain->sType!=VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO||chain->function!=VK_LAYER_LINK_INFO))chain=reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(chain->pNext));
    if(!chain)return VK_ERROR_INITIALIZATION_FAILED;
    auto gipa=chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto create=reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr,"vkCreateInstance"));
    chain->u.pLayerInfo=chain->u.pLayerInfo->pNext;
    auto result=create(info,alloc,out);
    if(result==VK_SUCCESS){std::lock_guard lock(mutex);instances[key(*out)]={*out,gipa};__android_log_print(ANDROID_LOG_INFO,"Refract.SystemVulkan","Runtime layer active");}
    return result;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance h,const VkAllocationCallbacks* alloc){
    auto s=instance(h);{std::lock_guard lock(mutex);instances.erase(key(h));}function<PFN_vkDestroyInstance>(s,"vkDestroyInstance")(h,alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo* info,const VkAllocationCallbacks* alloc,VkDevice* out){
    auto s=instance(physical);
    auto* chain=reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(info->pNext));
    while(chain&&(chain->sType!=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO||chain->function!=VK_LAYER_LINK_INFO))chain=reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(chain->pNext));
    if(!chain)return VK_ERROR_INITIALIZATION_FAILED;
    auto gdpa=chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    auto create=reinterpret_cast<PFN_vkCreateDevice>(chain->u.pLayerInfo->pfnNextGetInstanceProcAddr(s.handle,"vkCreateDevice"));
    PFN_vkSetDeviceLoaderData setLoaderData=nullptr;  // for command buffers the layer allocates itself
    for(auto* n=reinterpret_cast<const VkLayerDeviceCreateInfo*>(info->pNext);n;n=reinterpret_cast<const VkLayerDeviceCreateInfo*>(n->pNext))
        if(n->sType==VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO&&n->function==VK_LOADER_DATA_CALLBACK)setLoaderData=n->u.pfnSetDeviceLoaderData;
    chain->u.pLayerInfo=chain->u.pLayerInfo->pNext;
    std::vector<VkExtensionProperties> es;auto r=extensions(s,physical,es);if(r!=VK_SUCCESS)return r;
    auto api=apiVersion(s,physical);std::vector<const char*> names;
    for(uint32_t i=0;i<info->enabledExtensionCount;++i){auto n=info->ppEnabledExtensionNames[i];bool promoted=false;for(auto& p:promotions)if(api>=p.api&&!std::strcmp(n,p.name)&&!has(es,n))promoted=true;if(!promoted)names.push_back(n);}
    auto modified=*info;modified.enabledExtensionCount=names.size();modified.ppEnabledExtensionNames=names.data();
    // Transcoded textures are BC, which needs the textureCompressionBC feature on the device.
    bool transcode=false;VkPhysicalDeviceFeatures features{};VkPhysicalDeviceFeatures2* features2=nullptr;VkBool32 savedBc=VK_FALSE;
    if(transcodeTextures()){
        VkPhysicalDeviceFeatures supported{};
        function<PFN_vkGetPhysicalDeviceFeatures>(s,"vkGetPhysicalDeviceFeatures")(physical,&supported);
        transcode=supported.textureCompressionBC;
        if(!transcode)__android_log_print(ANDROID_LOG_WARN,"Refract.Transcode","device has no BC texture support; transcoding off");
        for(auto* n=static_cast<const VkBaseInStructure*>(modified.pNext);transcode&&n;n=n->pNext)
            if(n->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)features2=reinterpret_cast<VkPhysicalDeviceFeatures2*>(const_cast<VkBaseInStructure*>(n));
        if(features2){savedBc=features2->features.textureCompressionBC;features2->features.textureCompressionBC=VK_TRUE;}
        else if(transcode){if(modified.pEnabledFeatures)features=*modified.pEnabledFeatures;features.textureCompressionBC=VK_TRUE;modified.pEnabledFeatures=&features;}
    }
    auto result=create(physical,&modified,alloc,out);
    if(features2)features2->features.textureCompressionBC=savedBc;
    if(result==VK_SUCCESS){
        Device state{*out,gdpa};
        state.createTemplate=function<PFN_vkCreateDescriptorUpdateTemplate>(state,"vkCreateDescriptorUpdateTemplate");
        if(!state.createTemplate)state.createTemplate=function<PFN_vkCreateDescriptorUpdateTemplate>(state,"vkCreateDescriptorUpdateTemplateKHR");
        state.destroyTemplate=function<PFN_vkDestroyDescriptorUpdateTemplate>(state,"vkDestroyDescriptorUpdateTemplate");
        if(!state.destroyTemplate)state.destroyTemplate=function<PFN_vkDestroyDescriptorUpdateTemplate>(state,"vkDestroyDescriptorUpdateTemplateKHR");
        state.updateTemplate=function<PFN_vkUpdateDescriptorSetWithTemplate>(state,"vkUpdateDescriptorSetWithTemplate");
        if(!state.updateTemplate)state.updateTemplate=function<PFN_vkUpdateDescriptorSetWithTemplate>(state,"vkUpdateDescriptorSetWithTemplateKHR");
        state.updateSets=function<PFN_vkUpdateDescriptorSets>(state,"vkUpdateDescriptorSets");
        state.beginCommandBuffer=function<PFN_vkBeginCommandBuffer>(state,"vkBeginCommandBuffer");
        state.copyBufferToImage=function<PFN_vkCmdCopyBufferToImage>(state,"vkCmdCopyBufferToImage");
        if(transcode){
            VkPhysicalDeviceMemoryProperties props{};
            function<PFN_vkGetPhysicalDeviceMemoryProperties>(s,"vkGetPhysicalDeviceMemoryProperties")(physical,&props);
            auto formatProperties=function<PFN_vkGetPhysicalDeviceFormatProperties>(s,"vkGetPhysicalDeviceFormatProperties");
            auto flag=[](const char* name){char value[PROP_VALUE_MAX]{};__system_property_get(name,value);return std::strcmp(value,"0")!=0;};
            // ASTC is transcoded on the GPU by a compute shader, on queue families that have compute.
            refract::TextureTranscoder::GpuSetup gpu;
            gpu.enabled=flag("debug.refract.transcode_gpu");gpu.setLoaderData=setLoaderData;
            uint32_t familyCount=0;auto families=function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(s,"vkGetPhysicalDeviceQueueFamilyProperties");
            families(physical,&familyCount,nullptr);std::vector<VkQueueFamilyProperties> familyProps(familyCount);families(physical,&familyCount,familyProps.data());
            for(uint32_t i=0;i<familyCount&&i<32;++i)if(familyProps[i].queueFlags&VK_QUEUE_COMPUTE_BIT)gpu.computeFamilies|=1u<<i;
            VkPhysicalDeviceProperties deviceProps{};function<PFN_vkGetPhysicalDeviceProperties>(s,"vkGetPhysicalDeviceProperties")(physical,&deviceProps);
            gpu.maxStorageRange=deviceProps.limits.maxStorageBufferRange;
            state.transcoder=std::make_shared<refract::TextureTranscoder>(*out,gdpa,props,[&](VkFormat f){
                VkFormatProperties p{};formatProperties(physical,f,&p);
                constexpr VkFormatFeatureFlags need=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
                return (p.optimalTilingFeatures&need)==need;},flag("debug.refract.transcode_astc"),gpu);  // transcode_astc=0 keeps ASTC emulated
            __android_log_print(ANDROID_LOG_INFO,"Refract.Transcode","ETC2/EAC/ASTC -> BC texture transcoding enabled");
        }
        if(cachedBufferMemory()){
            VkPhysicalDeviceMemoryProperties props{};
            function<PFN_vkGetPhysicalDeviceMemoryProperties>(s,"vkGetPhysicalDeviceMemoryProperties")(physical,&props);
            constexpr VkMemoryPropertyFlags cachedCoherent=VK_MEMORY_PROPERTY_HOST_CACHED_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            for(uint32_t i=0;i<props.memoryTypeCount;++i){
                auto flags=props.memoryTypes[i].propertyFlags;
                if(!(flags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))continue;
                if((flags&cachedCoherent)==cachedCoherent)state.cachedTypes|=1u<<i;
                else if(!(flags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT))state.uncachedTypes|=1u<<i;
            }
            __android_log_print(ANDROID_LOG_INFO,"Refract.CachedBuffers","enabled: cached types %x, uncached %x",
                state.cachedTypes,state.uncachedTypes);
        }
        if(vramStats()){
            VkPhysicalDeviceMemoryProperties props{};
            function<PFN_vkGetPhysicalDeviceMemoryProperties>(s,"vkGetPhysicalDeviceMemoryProperties")(physical,&props);
            for(uint32_t i=0;i<props.memoryHeapCount;++i)__android_log_print(ANDROID_LOG_INFO,"Refract.VRAM","heap %u: %.0f MB flags %x",
                i,props.memoryHeaps[i].size/1048576.0,props.memoryHeaps[i].flags);
            for(uint32_t i=0;i<props.memoryTypeCount;++i)__android_log_print(ANDROID_LOG_INFO,"Refract.VRAM","type %u: heap %u flags %x",
                i,props.memoryTypes[i].heapIndex,props.memoryTypes[i].propertyFlags);
        }
        std::lock_guard lock(mutex);devices[key(*out)]=state;
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice h,const VkAllocationCallbacks* alloc){
    auto s=device(h);{std::lock_guard lock(mutex);devices.erase(key(h));for(auto it=templates.begin();it!=templates.end();)if(it->first.first==h)it=templates.erase(it);else ++it;}
    s.transcoder.reset();  // frees its staging before the device goes
    function<PFN_vkDestroyDevice>(s,"vkDestroyDevice")(h,alloc);
}
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice h,VkBuffer buffer,VkMemoryRequirements* out){
    auto s=device(h);function<PFN_vkGetBufferMemoryRequirements>(s,"vkGetBufferMemoryRequirements")(h,buffer,out);
    filterBufferMemory(s,out);
}
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements2(VkDevice h,const VkBufferMemoryRequirementsInfo2* info,VkMemoryRequirements2* out){
    auto s=device(h);auto next=function<PFN_vkGetBufferMemoryRequirements2>(s,"vkGetBufferMemoryRequirements2");
    if(!next)next=function<PFN_vkGetBufferMemoryRequirements2>(s,"vkGetBufferMemoryRequirements2KHR");
    next(h,info,out);filterBufferMemory(s,&out->memoryRequirements);
}
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements2KHR(VkDevice h,const VkBufferMemoryRequirementsInfo2* info,VkMemoryRequirements2* out){
    vkGetBufferMemoryRequirements2(h,info,out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice h,const VkMemoryAllocateInfo* info,const VkAllocationCallbacks* alloc,VkDeviceMemory* out){
    auto s=device(h);auto result=function<PFN_vkAllocateMemory>(s,"vkAllocateMemory")(h,info,alloc,out);
    if(result!=VK_SUCCESS)return result;
    if(s.transcoder)s.transcoder->addMemory(*out,info->allocationSize,info->memoryTypeIndex);
    if(vramStats()){std::lock_guard lock(vram.lock);vram.memories[*out]={info->allocationSize,info->memoryTypeIndex};
        vram.typeBytes[info->memoryTypeIndex]+=info->allocationSize;++vram.typeCount[info->memoryTypeIndex];vram.changed();}
    return result;
}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice h,VkDeviceMemory memory,const VkAllocationCallbacks* alloc){
    auto s=device(h);
    if(s.transcoder)s.transcoder->removeMemory(memory);
    if(vramStats()){std::lock_guard lock(vram.lock);auto it=vram.memories.find(memory);
        if(it!=vram.memories.end()){vram.typeBytes[it->second.second]-=it->second.first;--vram.typeCount[it->second.second];
            vram.memories.erase(it);vram.changed();}}
    function<PFN_vkFreeMemory>(s,"vkFreeMemory")(h,memory,alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice h,VkDeviceMemory memory,VkDeviceSize offset,VkDeviceSize size,VkMemoryMapFlags flags,void** data){
    auto s=device(h);auto result=function<PFN_vkMapMemory>(s,"vkMapMemory")(h,memory,offset,size,flags,data);
    if(result==VK_SUCCESS&&s.transcoder)s.transcoder->mapMemory(memory,offset,size,*data);
    return result;
}
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice h,VkDeviceMemory memory){
    auto s=device(h);if(s.transcoder)s.transcoder->unmapMemory(memory);
    function<PFN_vkUnmapMemory>(s,"vkUnmapMemory")(h,memory);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice h,const VkBufferCreateInfo* info,const VkAllocationCallbacks* alloc,VkBuffer* out){
    auto s=device(h);auto result=function<PFN_vkCreateBuffer>(s,"vkCreateBuffer")(h,info,alloc,out);
    if(result==VK_SUCCESS&&s.transcoder&&(info->usage&VK_BUFFER_USAGE_TRANSFER_SRC_BIT))s.transcoder->addBuffer(*out,info->size);
    return result;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice h,VkBuffer buffer,const VkAllocationCallbacks* alloc){
    auto s=device(h);if(s.transcoder)s.transcoder->removeBuffer(buffer);
    function<PFN_vkDestroyBuffer>(s,"vkDestroyBuffer")(h,buffer,alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice h,VkBuffer buffer,VkDeviceMemory memory,VkDeviceSize offset){
    auto s=device(h);auto result=function<PFN_vkBindBufferMemory>(s,"vkBindBufferMemory")(h,buffer,memory,offset);
    if(result==VK_SUCCESS&&s.transcoder)s.transcoder->bindBuffer(buffer,memory,offset);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory2(VkDevice h,uint32_t count,const VkBindBufferMemoryInfo* infos){
    auto s=device(h);auto next=function<PFN_vkBindBufferMemory2>(s,"vkBindBufferMemory2");
    if(!next)next=function<PFN_vkBindBufferMemory2>(s,"vkBindBufferMemory2KHR");
    auto result=next(h,count,infos);
    if(result==VK_SUCCESS&&s.transcoder)for(uint32_t i=0;i<count;++i)s.transcoder->bindBuffer(infos[i].buffer,infos[i].memory,infos[i].memoryOffset);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory2KHR(VkDevice h,uint32_t count,const VkBindBufferMemoryInfo* infos){
    return vkBindBufferMemory2(h,count,infos);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice h,const VkImageCreateInfo* info,const VkAllocationCallbacks* alloc,VkImage* out){
    auto s=device(h);VkImageCreateInfo modified=*info;
    const auto codec=s.transcoder?s.transcoder->adjust(modified):refract::texture::Codec::None;
    auto result=function<PFN_vkCreateImage>(s,"vkCreateImage")(h,&modified,alloc,out);
    if(result==VK_SUCCESS&&codec!=refract::texture::Codec::None){
        s.transcoder->addImage(*out,*info);
        static std::atomic<unsigned> reports{0};
        if(reports.fetch_add(1,std::memory_order_relaxed)<8)__android_log_print(ANDROID_LOG_INFO,"Refract.Transcode",
            "image %ux%u mips=%u layers=%u format %d -> %d",modified.extent.width,modified.extent.height,modified.mipLevels,
            modified.arrayLayers,info->format,modified.format);
    }
    if(result==VK_SUCCESS&&vramStats()){std::lock_guard lock(vram.lock);
        vram.images[*out]={modified.format,modified.extent,modified.mipLevels,modified.arrayLayers,modified.samples,modified.usage};}
    return result;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice h,VkImage image,const VkAllocationCallbacks* alloc){
    auto s=device(h);if(s.transcoder)s.transcoder->removeImage(image);
    if(vramStats()){std::lock_guard lock(vram.lock);if(vram.images.erase(image))vram.changed();}
    function<PFN_vkDestroyImage>(s,"vkDestroyImage")(h,image,alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice h,const VkImageViewCreateInfo* info,const VkAllocationCallbacks* alloc,VkImageView* out){
    auto s=device(h);VkImageViewCreateInfo modified=*info;
    if(s.transcoder)modified.format=s.transcoder->viewFormat(info->image,info->format);
    return function<PFN_vkCreateImageView>(s,"vkCreateImageView")(h,&modified,alloc,out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice h,const VkCommandPoolCreateInfo* info,const VkAllocationCallbacks* alloc,VkCommandPool* out){
    auto s=device(h);auto result=function<PFN_vkCreateCommandPool>(s,"vkCreateCommandPool")(h,info,alloc,out);
    if(result==VK_SUCCESS&&s.transcoder)s.transcoder->createdPool(*out,info->queueFamilyIndex);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice h,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* out){
    auto s=device(h);auto result=function<PFN_vkAllocateCommandBuffers>(s,"vkAllocateCommandBuffers")(h,info,out);
    if(result==VK_SUCCESS&&s.transcoder)s.transcoder->allocated(info->commandPool,info->commandBufferCount,out,info->level==VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    return result;
}
// debug.refract.hitch_log (default on): pipeline creation slower than 8 ms is logged as Refract.Hitch, since the
// driver compiles shaders synchronously and a game creating pipelines mid-frame stalls.
bool hitchLog(){static const bool on=[] {char value[PROP_VALUE_MAX]{};__system_property_get("debug.refract.hitch_log",value);
    return std::strcmp(value,"0")!=0;}();return on;}
extern "C++" template<class Call>VkResult timedPipelines(const char* what,uint32_t count,Call call){
    const auto start=std::chrono::steady_clock::now();auto result=call();
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    if(ms>8)__android_log_print(ANDROID_LOG_INFO,"Refract.Hitch","%s: %u pipeline(s) took %.1f ms",what,count,ms);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice h,VkPipelineCache cache,uint32_t count,const VkGraphicsPipelineCreateInfo* infos,const VkAllocationCallbacks* alloc,VkPipeline* out){
    auto next=function<PFN_vkCreateGraphicsPipelines>(device(h),"vkCreateGraphicsPipelines");
    return timedPipelines("vkCreateGraphicsPipelines",count,[&]{return next(h,cache,count,infos,alloc,out);});
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice h,VkPipelineCache cache,uint32_t count,const VkComputePipelineCreateInfo* infos,const VkAllocationCallbacks* alloc,VkPipeline* out){
    auto next=function<PFN_vkCreateComputePipelines>(device(h),"vkCreateComputePipelines");
    return timedPipelines("vkCreateComputePipelines",count,[&]{return next(h,cache,count,infos,alloc,out);});
}
// GPU transcodes recorded for a command buffer run in a layer command buffer placed just before it in its batch.
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue,uint32_t count,const VkSubmitInfo* submits,VkFence fence){
    auto s=device(queue);auto next=function<PFN_vkQueueSubmit>(s,"vkQueueSubmit");
    thread_local std::vector<VkSubmitInfo> modified;thread_local std::vector<std::vector<VkCommandBuffer>> lists;
    if(s.transcoder&&s.transcoder->prepareSubmit(count,submits,modified,lists))return next(queue,count,modified.data(),fence);
    return next(queue,count,submits,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue queue,uint32_t count,const VkSubmitInfo2* submits,VkFence fence){
    auto s=device(queue);auto next=function<PFN_vkQueueSubmit2>(s,"vkQueueSubmit2");
    if(!next)next=function<PFN_vkQueueSubmit2>(s,"vkQueueSubmit2KHR");
    thread_local std::vector<VkSubmitInfo2> modified;thread_local std::vector<std::vector<VkCommandBufferSubmitInfo>> lists;
    if(s.transcoder&&s.transcoder->prepareSubmit2(count,submits,modified,lists))return next(queue,count,modified.data(),fence);
    return next(queue,count,submits,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2KHR(VkQueue queue,uint32_t count,const VkSubmitInfo2* submits,VkFence fence){
    return vkQueueSubmit2(queue,count,submits,fence);
}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer cmd,const VkCommandBufferBeginInfo* info){
    auto s=device(cmd);if(s.transcoder)s.transcoder->reset(cmd);
    return s.beginCommandBuffer(cmd,info);
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer cmd,VkCommandBufferResetFlags flags){
    auto s=device(cmd);if(s.transcoder)s.transcoder->reset(cmd);
    return function<PFN_vkResetCommandBuffer>(s,"vkResetCommandBuffer")(cmd,flags);
}
VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice h,VkCommandPool pool,uint32_t count,const VkCommandBuffer* buffers){
    auto s=device(h);if(s.transcoder)s.transcoder->freed(count,buffers);
    function<PFN_vkFreeCommandBuffers>(s,"vkFreeCommandBuffers")(h,pool,count,buffers);
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice h,VkCommandPool pool,VkCommandPoolResetFlags flags){
    auto s=device(h);if(s.transcoder)s.transcoder->poolReset(pool,false);
    return function<PFN_vkResetCommandPool>(s,"vkResetCommandPool")(h,pool,flags);
}
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice h,VkCommandPool pool,const VkAllocationCallbacks* alloc){
    auto s=device(h);if(s.transcoder)s.transcoder->poolReset(pool,true);
    function<PFN_vkDestroyCommandPool>(s,"vkDestroyCommandPool")(h,pool,alloc);
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer cmd,VkBuffer buffer,VkImage image,VkImageLayout layout,uint32_t count,const VkBufferImageCopy* regions){
    auto s=device(cmd);
    if(s.transcoder){
        thread_local std::vector<VkBufferImageCopy> redirected;VkBuffer staging;
        if(s.transcoder->redirect(cmd,buffer,image,count,regions,redirected,staging)){
            s.copyBufferToImage(cmd,staging,image,layout,count,redirected.data());return;}
    }
    s.copyBufferToImage(cmd,buffer,image,layout,count,regions);
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage2(VkCommandBuffer cmd,const VkCopyBufferToImageInfo2* info){
    auto s=device(cmd);auto next=function<PFN_vkCmdCopyBufferToImage2>(s,"vkCmdCopyBufferToImage2");
    if(!next)next=function<PFN_vkCmdCopyBufferToImage2>(s,"vkCmdCopyBufferToImage2KHR");
    if(s.transcoder){
        thread_local std::vector<VkBufferImageCopy2> redirected;VkBuffer staging;
        if(s.transcoder->redirect(cmd,info->srcBuffer,info->dstImage,info->regionCount,info->pRegions,redirected,staging)){
            auto modified=*info;modified.srcBuffer=staging;modified.pRegions=redirected.data();next(cmd,&modified);return;}
    }
    next(cmd,info);
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage2KHR(VkCommandBuffer cmd,const VkCopyBufferToImageInfo2* info){
    vkCmdCopyBufferToImage2(cmd,info);
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer cmd,VkImage src,VkImageLayout srcLayout,VkImage dst,VkImageLayout dstLayout,uint32_t count,const VkImageCopy* regions){
    auto s=device(cmd);if(s.transcoder)s.transcoder->checkImageCopy(src,dst);
    function<PFN_vkCmdCopyImage>(s,"vkCmdCopyImage")(cmd,src,srcLayout,dst,dstLayout,count,regions);
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer cmd,VkImage src,VkImageLayout layout,VkBuffer dst,uint32_t count,const VkBufferImageCopy* regions){
    auto s=device(cmd);if(s.transcoder)s.transcoder->checkReadback(src);
    function<PFN_vkCmdCopyImageToBuffer>(s,"vkCmdCopyImageToBuffer")(cmd,src,layout,dst,count,regions);
}
static void recordImageSize(VkImage image,VkDeviceSize size){
    std::lock_guard lock(vram.lock);auto it=vram.images.find(image);if(it!=vram.images.end())it->second.size=size;
}
static void recordImageBind(VkImage image){
    std::lock_guard lock(vram.lock);auto it=vram.images.find(image);if(it!=vram.images.end()){it->second.bound=true;vram.changed();}
}
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice h,VkImage image,VkMemoryRequirements* out){
    function<PFN_vkGetImageMemoryRequirements>(device(h),"vkGetImageMemoryRequirements")(h,image,out);recordImageSize(image,out->size);
}
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements2(VkDevice h,const VkImageMemoryRequirementsInfo2* info,VkMemoryRequirements2* out){
    auto s=device(h);auto next=function<PFN_vkGetImageMemoryRequirements2>(s,"vkGetImageMemoryRequirements2");
    if(!next)next=function<PFN_vkGetImageMemoryRequirements2>(s,"vkGetImageMemoryRequirements2KHR");
    next(h,info,out);recordImageSize(info->image,out->memoryRequirements.size);
}
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements2KHR(VkDevice h,const VkImageMemoryRequirementsInfo2* info,VkMemoryRequirements2* out){
    vkGetImageMemoryRequirements2(h,info,out);
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice h,VkImage image,VkDeviceMemory memory,VkDeviceSize offset){
    auto result=function<PFN_vkBindImageMemory>(device(h),"vkBindImageMemory")(h,image,memory,offset);
    if(result==VK_SUCCESS)recordImageBind(image);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory2(VkDevice h,uint32_t count,const VkBindImageMemoryInfo* infos){
    auto s=device(h);auto next=function<PFN_vkBindImageMemory2>(s,"vkBindImageMemory2");
    if(!next)next=function<PFN_vkBindImageMemory2>(s,"vkBindImageMemory2KHR");
    auto result=next(h,count,infos);
    if(result==VK_SUCCESS)for(uint32_t i=0;i<count;++i)recordImageBind(infos[i].image);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory2KHR(VkDevice h,uint32_t count,const VkBindImageMemoryInfo* infos){
    return vkBindImageMemory2(h,count,infos);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorUpdateTemplate(VkDevice h,const VkDescriptorUpdateTemplateCreateInfo* info,const VkAllocationCallbacks* alloc,VkDescriptorUpdateTemplate* out){
    auto s=device(h);std::shared_ptr<const Layout> layout;
    if(info->templateType==VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET && std::all_of(info->pDescriptorUpdateEntries,info->pDescriptorUpdateEntries+info->descriptorUpdateEntryCount,[](auto& e){return refract::descriptor_template_supported(e.descriptorType);}))layout=std::make_shared<const Layout>(info->pDescriptorUpdateEntries,info->descriptorUpdateEntryCount);
    if(!s.createTemplate)return VK_ERROR_EXTENSION_NOT_PRESENT;
    auto result=s.createTemplate(h,info,alloc,out);
    if(result==VK_SUCCESS&&layout){std::lock_guard lock(mutex);templates[{h,*out}]=std::move(layout);}
    return result;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorUpdateTemplate(VkDevice h,VkDescriptorUpdateTemplate value,const VkAllocationCallbacks* alloc){
    Device s;std::shared_ptr<const Layout> retired;
    {std::lock_guard lock(mutex);s=devices.at(key(h));auto it=templates.find({h,value});
        if(it!=templates.end()){retired=std::move(it->second);templates.erase(it);}}
    // An in-flight expansion owns its layout; free it and call the driver outside the registry lock.
    s.destroyTemplate(h,value,alloc);
}
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSetWithTemplate(VkDevice h,VkDescriptorSet set,VkDescriptorUpdateTemplate value,const void* data){
    DescriptorProfile profile;
    Device s;std::shared_ptr<const Layout> layout;
    {std::shared_lock lock(mutex);s=devices.at(key(h));auto it=templates.find({h,value});if(it!=templates.end())layout=it->second;}
    profile.lookupDone();
    if(layout){
        thread_local refract::DescriptorScratchPool pool;
        refract::DescriptorScratchPool::Lease lease(pool);
        lease.scratch.expand(*layout,set,data);
        profile.expansionDone();
        s.updateSets(h,static_cast<uint32_t>(lease.scratch.writes.size()),lease.scratch.writes.data(),0,nullptr);
        profile.finish(true,lease.scratch.writes.size());
        return;
    }
    profile.expansionDone();
    s.updateTemplate(h,set,value,data);
    profile.finish(false,0);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorUpdateTemplateKHR(VkDevice h,const VkDescriptorUpdateTemplateCreateInfo* i,const VkAllocationCallbacks* a,VkDescriptorUpdateTemplate* o){return vkCreateDescriptorUpdateTemplate(h,i,a,o);}
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorUpdateTemplateKHR(VkDevice h,VkDescriptorUpdateTemplate t,const VkAllocationCallbacks* a){vkDestroyDescriptorUpdateTemplate(h,t,a);}
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSetWithTemplateKHR(VkDevice h,VkDescriptorSet s,VkDescriptorUpdateTemplate t,const void* d){vkUpdateDescriptorSetWithTemplate(h,s,t,d);}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance h,const char* name){
    if(auto f=intercept(name))return f;
    if(!h)return nullptr;auto s=instance(h);
    // A Vulkan 1.1 instance can expose KHR_renderpass2 while its 1.2 core
    // entry points are unavailable. Prefer the requested extension function.
    if(auto f=s.gipa(h,name))return f;
    auto promoted=alias(name);return promoted!=name?s.gipa(h,promoted):nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice h,const char* name){
    if(auto f=intercept(name))return f;
    if(!h)return nullptr;auto s=device(h);
    if(auto f=s.gdpa(h,name))return f;
    auto promoted=alias(name);return promoted!=name?s.gdpa(h,promoted):nullptr;
}
}
namespace {
PFN_vkVoidFunction intercept(const char* name){
#define ENTRY(n) if(!std::strcmp(name,#n))return reinterpret_cast<PFN_vkVoidFunction>(n)
    ENTRY(vkGetInstanceProcAddr);ENTRY(vkGetDeviceProcAddr);
    ENTRY(vkCreateInstance);ENTRY(vkDestroyInstance);ENTRY(vkCreateDevice);ENTRY(vkDestroyDevice);
    ENTRY(vkEnumerateInstanceLayerProperties);ENTRY(vkEnumerateDeviceLayerProperties);ENTRY(vkEnumerateInstanceExtensionProperties);
    ENTRY(vkEnumerateDeviceExtensionProperties);
    if(cachedBufferMemory()){
        ENTRY(vkGetBufferMemoryRequirements);ENTRY(vkGetBufferMemoryRequirements2);ENTRY(vkGetBufferMemoryRequirements2KHR);
    }
    if(vramStats()||transcodeTextures()){
        ENTRY(vkAllocateMemory);ENTRY(vkFreeMemory);ENTRY(vkCreateImage);ENTRY(vkDestroyImage);
    }
    if(vramStats()){
        ENTRY(vkGetImageMemoryRequirements);ENTRY(vkGetImageMemoryRequirements2);ENTRY(vkGetImageMemoryRequirements2KHR);
        ENTRY(vkBindImageMemory);ENTRY(vkBindImageMemory2);ENTRY(vkBindImageMemory2KHR);
    }
    if(transcodeTextures()){
        ENTRY(vkMapMemory);ENTRY(vkUnmapMemory);ENTRY(vkCreateBuffer);ENTRY(vkDestroyBuffer);
        ENTRY(vkBindBufferMemory);ENTRY(vkBindBufferMemory2);ENTRY(vkBindBufferMemory2KHR);ENTRY(vkCreateImageView);
        ENTRY(vkCreateCommandPool);ENTRY(vkQueueSubmit);ENTRY(vkQueueSubmit2);ENTRY(vkQueueSubmit2KHR);
        ENTRY(vkAllocateCommandBuffers);ENTRY(vkBeginCommandBuffer);ENTRY(vkResetCommandBuffer);ENTRY(vkFreeCommandBuffers);
        ENTRY(vkResetCommandPool);ENTRY(vkDestroyCommandPool);
        ENTRY(vkCmdCopyBufferToImage);ENTRY(vkCmdCopyBufferToImage2);ENTRY(vkCmdCopyBufferToImage2KHR);
        ENTRY(vkCmdCopyImage);ENTRY(vkCmdCopyImageToBuffer);
    }
    if(hitchLog()){ENTRY(vkCreateGraphicsPipelines);ENTRY(vkCreateComputePipelines);}
    ENTRY(vkCreateDescriptorUpdateTemplate);ENTRY(vkDestroyDescriptorUpdateTemplate);ENTRY(vkUpdateDescriptorSetWithTemplate);
    ENTRY(vkCreateDescriptorUpdateTemplateKHR);ENTRY(vkDestroyDescriptorUpdateTemplateKHR);ENTRY(vkUpdateDescriptorSetWithTemplateKHR);
#undef ENTRY
    return nullptr;
}
}
