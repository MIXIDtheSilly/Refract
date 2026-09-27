#include "vulkan_backend.h"
#if defined(__ANDROID__)
#include "perf_stats.h"
#include "image_frame.h"
#include "shaders/rgba_to_nv12.spv.h"
#include <android/log.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <chrono>
#include <sys/system_properties.h>
#include <mutex>
#include <thread>

namespace refract::runtime {
namespace {
std::mutex g_slotMutex;  // Guards VulkanBackend::slots_[].busy (app thread vs. sender thread).
bool ok(VkResult result, const char* operation) {
    if (result == VK_SUCCESS) return true;
    __android_log_print(ANDROID_LOG_ERROR, "Refract.Vulkan", "%s failed: %d", operation, result);
    return false;
}
}
VkPhysicalDevice VulkanBackend::choose_device(VkInstance instance) {
    if (!instance) return VK_NULL_HANDLE;
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS) return VK_NULL_HANDLE;
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkPhysicalDevice fallback = VK_NULL_HANDLE;
    for (auto device : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        if (props.vendorID == 0x10de) return device;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
            props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) fallback = device;
    }
    return fallback;
}
uint32_t VulkanBackend::memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}
bool VulkanBackend::initialize(const XrGraphicsBindingVulkanKHR& binding) {
    if (active() || !binding.instance || !binding.device || !binding.physicalDevice ||
        binding.physicalDevice != choose_device(binding.instance)) return false;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(binding.physicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(binding.physicalDevice, &count, families.data());
    if (binding.queueFamilyIndex >= count || binding.queueIndex >= families[binding.queueFamilyIndex].queueCount ||
        !(families[binding.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT)) return false;
    device_ = binding.device; physical_ = binding.physicalDevice; queueFamily_ = binding.queueFamilyIndex;
    vkGetDeviceQueue(device_, binding.queueFamilyIndex, binding.queueIndex, &queue_);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = binding.queueFamilyIndex;
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; alloc.commandBufferCount = 1;
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!ok(vkCreateCommandPool(device_, &pool, nullptr, &pool_), "create pool")) { shutdown(); return false; }
    alloc.commandPool = pool_;
    if (!ok(vkAllocateCommandBuffers(device_, &alloc, &cmd_), "allocate commands") ||
        !ok(vkCreateFence(device_, &fence, nullptr, &fence_), "create fence") ||
        !ensure_buffer(sizeof(gpuMarker_))) { shutdown(); return false; }
    VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(physical_, &props);
    __android_log_print(ANDROID_LOG_INFO, "Refract.GPU", "Vulkan device=%s vendor=0x%x type=%u", props.deviceName, props.vendorID, props.deviceType);
    char gpuMode[PROP_VALUE_MAX]{};
    __system_property_get("debug.refract.gpu_share", gpuMode);
    gpuExportEnabled_ = gpuExportRequested_ = std::strcmp(gpuMode, "1") == 0;
    nextExportSession_ = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    exportConfigurations_.clear();
    return true;
}
bool VulkanBackend::initialize_private() {
    if (active()) return false;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Refract GLES export"; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (!ok(vkCreateInstance(&instanceInfo, nullptr, &instance), "create private instance")) return false;
    const VkPhysicalDevice physical = choose_device(instance);
    uint32_t family = UINT32_MAX, count = 0;
    if (physical) {
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        for (uint32_t i = 0; i < count && family == UINT32_MAX; ++i)
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) family = i;
    }
    // AHardwareBuffer import and its dependencies (most are core in 1.1).
    const char* wanted[] = {VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME, VK_KHR_BIND_MEMORY_2_EXTENSION_NAME, VK_KHR_MAINTENANCE1_EXTENSION_NAME};
    std::vector<const char*> enabled;
    bool hasImport = false;
    if (family != UINT32_MAX) {
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data());
        for (const char* name : wanted)
            for (const auto& extension : available)
                if (std::strcmp(extension.extensionName, name) == 0) { enabled.push_back(name); break; }
        hasImport = !enabled.empty() && std::strcmp(enabled[0], wanted[0]) == 0;
    }
    if (!hasImport) {
        __android_log_print(ANDROID_LOG_WARN, "Refract.GPU", "GLES export: no Vulkan device with AHardwareBuffer import");
        vkDestroyInstance(instance, nullptr);
        return false;
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = family; queue.queueCount = 1; queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queue;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(enabled.size()); deviceInfo.ppEnabledExtensionNames = enabled.data();
    VkDevice device = VK_NULL_HANDLE;
    if (!ok(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "create private device")) {
        vkDestroyInstance(instance, nullptr);
        return false;
    }
    const XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, nullptr, instance, physical, device, family, 0};
    if (!initialize(binding)) {
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return false;
    }
    ownedInstance_ = instance;
    __android_log_print(ANDROID_LOG_INFO, "Refract.GPU", "GLES export: private Vulkan device ready (%zu extensions)", enabled.size());
    return true;
}
bool VulkanBackend::import_hardware_buffer(AHardwareBuffer* buffer, uint32_t width, uint32_t height, VkFormat format,
                                           VkImage& image, VkDeviceMemory& memory) {
    image = VK_NULL_HANDLE; memory = VK_NULL_HANDLE;
    static auto getProperties = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
        vkGetDeviceProcAddr(device_, "vkGetAndroidHardwareBufferPropertiesANDROID"));
    if (!getProperties || !buffer) return false;
    VkAndroidHardwareBufferFormatPropertiesANDROID formatProperties{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID properties{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    properties.pNext = &formatProperties;
    if (!ok(getProperties(device_, buffer, &properties), "query hardware buffer")) return false;
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.pNext = &external;
    info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {width, height, 1};
    info.mipLevels = 1; info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!ok(vkCreateImage(device_, &info, nullptr, &image), "create hardware buffer image")) return false;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = image;
    VkImportAndroidHardwareBufferInfoANDROID import{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    import.pNext = &dedicated; import.buffer = buffer;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.pNext = &import; alloc.allocationSize = properties.allocationSize;
    alloc.memoryTypeIndex = memory_type(properties.memoryTypeBits, 0);
    if (alloc.memoryTypeIndex == UINT32_MAX ||
        !ok(vkAllocateMemory(device_, &alloc, nullptr, &memory), "import hardware buffer") ||
        !ok(vkBindImageMemory(device_, image, memory, 0), "bind hardware buffer")) {
        destroy_image(image, memory);
        image = VK_NULL_HANDLE; memory = VK_NULL_HANDLE;
        return false;
    }
    // Define the layout once, then hand the image to GLES (the foreign queue family).
    if (!begin()) { destroy_image(image, memory); image = VK_NULL_HANDLE; memory = VK_NULL_HANDLE; return false; }
    barrier(image, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, 0, queueFamily_, VK_QUEUE_FAMILY_FOREIGN_EXT);
    if (!finish()) { destroy_image(image, memory); image = VK_NULL_HANDLE; memory = VK_NULL_HANDLE; return false; }
    return true;
}
void VulkanBackend::destroy_image(VkImage image, VkDeviceMemory memory) {
    if (!device_) return;
    vkQueueWaitIdle(queue_);  // Export copies may still read it.
    if (image) vkDestroyImage(device_, image, nullptr);
    if (memory) vkFreeMemory(device_, memory, nullptr);
}
bool VulkanBackend::ensure_buffer(VkDeviceSize bytes) {
    // All previous readback commands have completed their fence before reuse.
    return host_buffer(buffer_, bufferMemory_, mapped_, bufferBytes_, bytes);
}
bool VulkanBackend::host_buffer(VkBuffer& target, VkDeviceMemory& targetMemory, void*& mapped, VkDeviceSize& size, VkDeviceSize bytes) {
    if (mapped && size >= bytes) return true;
    if (mapped) vkUnmapMemory(device_, targetMemory);
    if (target) vkDestroyBuffer(device_, target, nullptr);
    if (targetMemory) vkFreeMemory(device_, targetMemory, nullptr);
    mapped = nullptr; target = VK_NULL_HANDLE; targetMemory = VK_NULL_HANDLE; size = 0;
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = bytes; buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!ok(vkCreateBuffer(device_, &buffer, nullptr, &target), "create staging buffer")) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, target, &req);
    VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memory.allocationSize = req.size;
    // Cached memory: the CPU reads every byte back.
    memory.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (memory.memoryTypeIndex == UINT32_MAX)
        memory.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    __android_log_print(ANDROID_LOG_INFO, "Refract.Vulkan", "staging memory type=%u (%llu bytes)", memory.memoryTypeIndex,
        static_cast<unsigned long long>(bytes));
    if (memory.memoryTypeIndex == UINT32_MAX ||
        !ok(vkAllocateMemory(device_, &memory, nullptr, &targetMemory), "allocate staging memory") ||
        !ok(vkBindBufferMemory(device_, target, targetMemory, 0), "bind staging memory") ||
        !ok(vkMapMemory(device_, targetMemory, 0, bytes, 0, &mapped), "map staging memory")) return false;
    size = bytes;
    return true;
}
void VulkanBackend::shutdown() {
    if (!device_) return;
    // The sender thread may still read a staging slot; give it a moment to let go.
    for (int attempt = 0; attempt < 100; ++attempt) {
        bool busy = false;
        {
            std::lock_guard<std::mutex> lock(g_slotMutex);
            for (const auto& slot : slots_) busy = busy || slot.busy;
        }
        if (!busy) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    vkQueueWaitIdle(queue_);
    for (auto& slot : exportSlots_) {
        if (slot.mapped) vkUnmapMemory(device_, slot.memory);
        if (slot.buffer) vkDestroyBuffer(device_, slot.buffer, nullptr);
        if (slot.memory) vkFreeMemory(device_, slot.memory, nullptr);
        if (slot.fence) vkDestroyFence(device_, slot.fence, nullptr);
    }
    for (auto& slot : slots_) {
        if (slot.mapped) vkUnmapMemory(device_, slot.memory);
        if (slot.buffer) vkDestroyBuffer(device_, slot.buffer, nullptr);
        if (slot.memory) vkFreeMemory(device_, slot.memory, nullptr);
        if (slot.fence) vkDestroyFence(device_, slot.fence, nullptr);
    }
    if (mapped_) vkUnmapMemory(device_, bufferMemory_);
    if (buffer_) vkDestroyBuffer(device_, buffer_, nullptr);
    if (bufferMemory_) vkFreeMemory(device_, bufferMemory_, nullptr);
    if (nv12Pipeline_) vkDestroyPipeline(device_, nv12Pipeline_, nullptr);
    if (nv12Layout_) vkDestroyPipelineLayout(device_, nv12Layout_, nullptr);
    if (nv12SetLayout_) vkDestroyDescriptorSetLayout(device_, nv12SetLayout_, nullptr);
    if (nv12Pool_) vkDestroyDescriptorPool(device_, nv12Pool_, nullptr);
    if (nv12Sampler_) vkDestroySampler(device_, nv12Sampler_, nullptr);
    for (auto& scaled : scaled_) {
        if (scaled.view) vkDestroyImageView(device_, scaled.view, nullptr);
        if (scaled.image) vkDestroyImage(device_, scaled.image, nullptr);
        if (scaled.memory) vkFreeMemory(device_, scaled.memory, nullptr);
    }
    if (fence_) vkDestroyFence(device_, fence_, nullptr);
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
    if (ownedInstance_) {
        vkDestroyDevice(device_, nullptr);
        vkDestroyInstance(ownedInstance_, nullptr);
    }
    *this = {};
}
bool VulkanBackend::allocate_image(VkImage& image, VkDeviceMemory& memory, VkFormat format,
                                  uint32_t width, uint32_t height, uint32_t layers, VkImageUsageFlags usage, VkImageCreateFlags flags) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = flags;
    info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {width, height, 1};
    info.mipLevels = 1; info.arrayLayers = layers; info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = usage;
    if (!ok(vkCreateImage(device_, &info, nullptr, &image), "create image")) return false;
    VkMemoryRequirements req{}; vkGetImageMemoryRequirements(device_, image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size; alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX) alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, 0);
    return alloc.memoryTypeIndex != UINT32_MAX &&
        ok(vkAllocateMemory(device_, &alloc, nullptr, &memory), "allocate image memory") &&
        ok(vkBindImageMemory(device_, image, memory, 0), "bind image memory");
}
bool VulkanBackend::begin() {
    if (!ok(vkResetCommandBuffer(cmd_, 0), "reset commands")) return false;
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return ok(vkBeginCommandBuffer(cmd_, &info), "begin commands");
}
bool VulkanBackend::finish() {
    if (!ok(vkEndCommandBuffer(cmd_), "end commands") || !ok(vkResetFences(device_, 1, &fence_), "reset fence")) return false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd_;
    return ok(vkQueueSubmit(queue_, 1, &submit, fence_), "submit") &&
        ok(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "wait fence");
}
void VulkanBackend::barrier(VkImage image, uint32_t layers, VkImageLayout before, VkImageLayout after,
                            VkAccessFlags src, VkAccessFlags dst, uint32_t srcFamily, uint32_t dstFamily) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = before; b.newLayout = after; b.srcAccessMask = src; b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = srcFamily; b.dstQueueFamilyIndex = dstFamily;
    b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}
XrResult VulkanBackend::create(VulkanSwapchain& sc, const XrSwapchainCreateInfo& info) {
    __android_log_print(ANDROID_LOG_INFO, "Refract.Swapchain", "create %ux%u layers=%u mips=%u samples=%u faces=%u flags=%llu usage=%llu", info.width, info.height, info.arraySize, info.mipCount, info.sampleCount, info.faceCount, (unsigned long long)info.createFlags, (unsigned long long)info.usageFlags);
    if (info.format != VK_FORMAT_R8G8B8A8_UNORM && info.format != VK_FORMAT_R8G8B8A8_SRGB) return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    constexpr XrFlags64 supportedUsage = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
        XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    if (info.sampleCount != 1 || info.mipCount != 1 || info.faceCount != 1 || (info.createFlags & ~XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ||
        info.width > 16384 || info.height > 16384 || info.arraySize > 4 || (info.usageFlags & ~supportedUsage)) return XR_ERROR_FEATURE_UNSUPPORTED;
    sc.format = static_cast<VkFormat>(info.format); sc.layers = info.arraySize;
    VkFormatProperties props{}; vkGetPhysicalDeviceFormatProperties(physical_, sc.format, &props);
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((props.optimalTilingFeatures & required) != required) return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    // Engines also clear color swapchains with vkCmdClearColorImage. Permit
    // transfer clears even when the application requested color attachment only.
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (info.usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (info.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const uint32_t imageCount = (info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ? 1 : 3;
    for (uint32_t i = 0; i < imageCount; ++i) {
        if (!allocate_image(sc.images[i], sc.memory[i], sc.format, info.width, info.height, sc.layers, usage)) { destroy(sc); return XR_ERROR_RUNTIME_FAILURE; }
    }
    // Initialize before handing images to the app; xrWaitSwapchainImage never touches its queue.
    if (!begin()) { destroy(sc); return XR_ERROR_RUNTIME_FAILURE; }
    for (auto image : sc.images) if (image) barrier(image, sc.layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if (!finish()) { destroy(sc); return XR_ERROR_RUNTIME_FAILURE; }
    return XR_SUCCESS;
}
void VulkanBackend::destroy(VulkanSwapchain& sc) {
    if (device_) {
        vkQueueWaitIdle(queue_);
        for (auto image : sc.images) if (image) vkDestroyImage(device_, image, nullptr);
        for (auto memory : sc.memory) if (memory) vkFreeMemory(device_, memory, nullptr);
    }
    sc = {};
}
bool VulkanBackend::ensure_scaled(const VulkanSwapchain* const swapchains[2], uint32_t width, uint32_t height, bool sampled) {
    for (uint32_t eye = 0; eye < 2; ++eye) {
        auto& scaled = scaled_[eye];
        if (scaled.format != swapchains[eye]->format || scaled.width != width || scaled.height != height || (sampled && !scaled.sampled)) {
            if (scaled.image) vkQueueWaitIdle(queue_);  // Async copies may still read it (rare: size changes).
            if (scaled.view) vkDestroyImageView(device_, scaled.view, nullptr);
            if (scaled.image) vkDestroyImage(device_, scaled.image, nullptr);
            if (scaled.memory) vkFreeMemory(device_, scaled.memory, nullptr);
            scaled = {};
            // The compute pass reads the stored bytes through a UNORM view, even of sRGB images.
            const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0);
            if (!allocate_image(scaled.image, scaled.memory, swapchains[eye]->format, width, height, 1, usage,
                                sampled ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0)) return false;
            if (sampled) {
                VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                view.image = scaled.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = VK_FORMAT_R8G8B8A8_UNORM;
                view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                if (!ok(vkCreateImageView(device_, &view, nullptr, &scaled.view), "create scaled view")) return false;
            }
            scaled.format = swapchains[eye]->format;
            scaled.width = width; scaled.height = height; scaled.sampled = sampled;
        }
    }
    return true;
}
bool VulkanBackend::ensure_nv12_pipeline() {
    if (nv12Pipeline_) return true;
    if (nv12Failed_) return false;
    nv12Failed_ = true;  // Until everything below succeeded.
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo setLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setLayout.bindingCount = 3; setLayout.pBindings = bindings;
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1; layout.pSetLayouts = &nv12SetLayout_;
    layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
    VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module.codeSize = sizeof(k_rgba_to_nv12_spv); module.pCode = k_rgba_to_nv12_spv;
    VkShaderModule shader = VK_NULL_HANDLE;
    if (!ok(vkCreateDescriptorSetLayout(device_, &setLayout, nullptr, &nv12SetLayout_), "create NV12 set layout") ||
        !ok(vkCreatePipelineLayout(device_, &layout, nullptr, &nv12Layout_), "create NV12 pipeline layout") ||
        !ok(vkCreateShaderModule(device_, &module, nullptr, &shader), "create NV12 shader")) return false;
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr};
    pipeline.layout = nv12Layout_;
    const bool created = ok(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &nv12Pipeline_), "create NV12 pipeline");
    vkDestroyShaderModule(device_, shader, nullptr);
    if (!created) return false;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kReadbackSlots}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kReadbackSlots}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = kReadbackSlots; pool.poolSizeCount = 2; pool.pPoolSizes = sizes;
    if (!ok(vkCreateSampler(device_, &sampler, nullptr, &nv12Sampler_), "create NV12 sampler") ||
        !ok(vkCreateDescriptorPool(device_, &pool, nullptr, &nv12Pool_), "create NV12 descriptor pool")) return false;
    for (auto& slot : slots_) {
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = nv12Pool_; alloc.descriptorSetCount = 1; alloc.pSetLayouts = &nv12SetLayout_;
        if (!ok(vkAllocateDescriptorSets(device_, &alloc, &slot.set), "allocate NV12 descriptors")) return false;
    }
    nv12Failed_ = false;
    __android_log_print(ANDROID_LOG_INFO, "Refract.Vulkan", "GPU NV12 conversion ready");
    return true;
}
// Blits each eye into its scaled image and, with a target, copies it there (eye 1 after eye 0).
// flipRows stores the rows bottom-up (the AXRI pixel convention) during the blit itself.
void VulkanBackend::record_eye_copies(const VulkanSwapchain* const swapchains[2], const uint32_t indices[2],
                                      const XrSwapchainSubImage* const subimages[2], uint32_t width, uint32_t height,
                                      VkBuffer target, bool flipRows) {
    const VkDeviceSize eyeBytes = VkDeviceSize(width) * height * 4;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const auto& sc = *swapchains[eye]; const auto& sub = *subimages[eye];
        const auto index = indices[eye]; auto& scaled = scaled_[eye];
        const uint32_t w = width, h = height;
        // An external (GLES-written) image is acquired from the foreign family and released back after the blit.
        const VkImageLayout home = sc.external ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        const uint32_t foreign = sc.external ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED;
        const uint32_t own = sc.external ? queueFamily_ : VK_QUEUE_FAMILY_IGNORED;
        barrier(sc.images[index], sc.layers, home, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                sc.external ? 0 : VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, foreign, own);
        barrier(scaled.image, 1, scaled.initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, scaled.initialized ? VK_ACCESS_TRANSFER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageBlit blit{}; blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, sub.imageArrayIndex, 1};
        blit.srcOffsets[0] = {sub.imageRect.offset.x, sub.imageRect.offset.y, 0};
        blit.srcOffsets[1] = {sub.imageRect.offset.x + sub.imageRect.extent.width, sub.imageRect.offset.y + sub.imageRect.extent.height, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {0, flipRows ? static_cast<int32_t>(h) : 0, 0};
        blit.dstOffsets[1] = {static_cast<int32_t>(w), flipRows ? 0 : static_cast<int32_t>(h), 1};
        vkCmdBlitImage(cmd_, sc.images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scaled.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        barrier(scaled.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {w, h, 1};
        copy.bufferOffset = eye * eyeBytes;
        if (target) vkCmdCopyImageToBuffer(cmd_, scaled.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, 1, &copy);
        barrier(sc.images[index], sc.layers, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, home, VK_ACCESS_TRANSFER_READ_BIT,
                sc.external ? 0 : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, own, foreign);
    }
}
int VulkanBackend::readback_async(const VulkanSwapchain* const swapchains[2], const uint32_t indices[2],
                                  const XrSwapchainSubImage* const subimages[2], uint32_t width, uint32_t height, bool* nv12) {
    static refract::protocol::PerfStats stats("vulkan-frame-copy");
    refract::protocol::PerfScope scope(stats);
    const bool convert = *nv12 = *nv12 && width % 2 == 0 && height % 2 == 0 && ensure_nv12_pipeline();
    const VkDeviceSize bytes = convert ? VkDeviceSize(width) * 2 * height * 3 / 2 : VkDeviceSize(width) * height * 4 * 2;
    if (!refract::protocol::valid_render_extent(width, height) || bytes > 128ull * 1024 * 1024) return -1;
    int index = -1;
    {
        std::lock_guard<std::mutex> lock(g_slotMutex);
        for (int i = 0; i < kReadbackSlots && index < 0; ++i)
            if (!slots_[i].busy) { slots_[i].busy = true; index = i; }
    }
    if (index < 0) return -1;  // The consumer is behind: skip this frame rather than stall the app.
    auto& slot = slots_[index];
    const auto release = [&] { readback_release(index); return -1; };
    // A released slot's copy may still be in flight if its consumer dropped it unread.
    if (slot.submitted && !ok(vkWaitForFences(device_, 1, &slot.fence, VK_TRUE, UINT64_MAX), "wait readback slot")) return release();
    slot.submitted = false;
    if (!slot.cmd) {
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pool_; alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; alloc.commandBufferCount = 1;
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!ok(vkAllocateCommandBuffers(device_, &alloc, &slot.cmd), "allocate readback commands")) return release();
        if (!ok(vkCreateFence(device_, &fence, nullptr, &slot.fence), "create readback fence")) return release();
    }
    if (!host_buffer(slot.buffer, slot.memory, slot.mapped, slot.bytes, bytes) || !ensure_scaled(swapchains, width, height, convert)) return release();
    if (convert) {
        // The slot's previous commands have completed (its fence was waited on above).
        const VkDescriptorImageInfo eyes[] = {{nv12Sampler_, scaled_[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                              {nv12Sampler_, scaled_[1].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        const VkDescriptorBufferInfo output{slot.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, slot.set, i, 0, 1,
                         i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                         i < 2 ? &eyes[i] : nullptr, i < 2 ? nullptr : &output, nullptr};
        }
        vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
    }
    const VkCommandBuffer shared = cmd_;
    cmd_ = slot.cmd;  // begin()/barrier() record into cmd_.
    bool recorded = begin();
    if (recorded) {
        record_eye_copies(swapchains, indices, subimages, width, height, convert ? VK_NULL_HANDLE : slot.buffer, true);
        if (convert) {
            for (auto& scaled : scaled_)
                barrier(scaled.image, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            const uint32_t layout[4] = {width, height, width * 2, width * 2 * height};
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, nv12Pipeline_);
            vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, nv12Layout_, 0, 1, &slot.set, 0, nullptr);
            vkCmdPushConstants(cmd_, nv12Layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(layout), layout);
            // One invocation per 4x2 block of the (2 * width) x height picture; 8x8 per group.
            vkCmdDispatch(cmd_, (width * 2 / 4 + 7) / 8, (height / 2 + 7) / 8, 1);
            // Back to the layout the next frame's blit expects.
            for (auto& scaled : scaled_)
                barrier(scaled.image, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.srcAccessMask = convert ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = slot.buffer; host.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd_, convert ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
        recorded = ok(vkEndCommandBuffer(cmd_), "end readback commands");
    }
    cmd_ = shared;
    if (!recorded || !ok(vkResetFences(device_, 1, &slot.fence), "reset readback fence")) return release();
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &slot.cmd;
    if (!ok(vkQueueSubmit(queue_, 1, &submit, slot.fence), "submit readback")) return release();
    slot.submitted = true;
    // In queue order the scaled images are now in TRANSFER_SRC layout.
    scaled_[0].initialized = scaled_[1].initialized = true;
    return index;
}
int VulkanBackend::export_async(const VulkanSwapchain* const swapchains[2], const uint32_t indices[2],
                                const XrSwapchainSubImage* const subimages[2], uint32_t width, uint32_t height) {
    static refract::protocol::PerfStats stats("export-submit");
    refract::protocol::PerfScope scope(stats);
    if (!gpuExportEnabled_ || !refract::protocol::valid_render_extent(width, height)) return -1;
    const std::array<uint32_t, 4> configuration{width, height,
        static_cast<uint32_t>(swapchains[0]->format), static_cast<uint32_t>(swapchains[1]->format)};
    auto found = exportRings_.find(configuration);
    if (found == exportRings_.end()) {
        if (exportRings_.size() >= 8) return -1;
        std::array<uint64_t, kExportRing> sessions{};
        for (auto& session : sessions) session = ++nextExportSession_;
        found = exportRings_.emplace(configuration, sessions).first;
    }
    const int index = static_cast<int>(exportFrames_ % kExportRing);
    auto& slot = exportSlots_[index];
    // Normally export_wait already saw this fence signal, kExportRing frames ago.
    if (slot.submitted && !ok(vkWaitForFences(device_, 1, &slot.fence, VK_TRUE, 1'000'000'000ull), "wait export slot")) return -1;
    slot.submitted = false;
    if (!slot.cmd) {
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pool_; alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; alloc.commandBufferCount = 1;
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!ok(vkAllocateCommandBuffers(device_, &alloc, &slot.cmd), "allocate export commands")) return -1;
        if (!ok(vkCreateFence(device_, &fence, nullptr, &slot.fence), "create export fence")) return -1;
    }
    if (!host_buffer(slot.buffer, slot.memory, slot.mapped, slot.bytes, sizeof(refract::protocol::WindowsGpuMarker)) ||
        !ensure_scaled(swapchains, width, height)) return -1;
    auto& marker = exportMarkers_[index];
    marker = {};
    marker.session = found->second[index];
    marker.sequence = ++gpuMarker_.sequence;
    marker.width = width; marker.height = height;
    marker.formats[0] = configuration[2]; marker.formats[1] = configuration[3];
    // The host layer sees the marker and sends the two eye blits that follow into the pair's
    // shared textures, then writes the marker back with status 1.
    const VkCommandBuffer shared = cmd_;
    cmd_ = slot.cmd;  // begin()/barrier() record into cmd_.
    bool recorded = begin();
    if (recorded) {
        vkCmdUpdateBuffer(cmd_, slot.buffer, 0, sizeof(marker), &marker);
        record_eye_copies(swapchains, indices, subimages, width, height, VK_NULL_HANDLE, false);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = slot.buffer; host.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
        recorded = ok(vkEndCommandBuffer(cmd_), "end export commands");
    }
    cmd_ = shared;
    if (!recorded || !ok(vkResetFences(device_, 1, &slot.fence), "reset export fence")) return -1;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &slot.cmd;
    if (!ok(vkQueueSubmit(queue_, 1, &submit, slot.fence), "submit export")) return -1;
    slot.submitted = true;
    ++exportFrames_;
    return index;
}
bool VulkanBackend::export_wait(int index, refract::protocol::WindowsGpuMarker* marker) {
    static refract::protocol::PerfStats stats("export-wait");
    refract::protocol::PerfScope scope(stats);
    if (index < 0 || index >= kExportRing || !exportSlots_[index].submitted) return false;
    auto& slot = exportSlots_[index];
    if (!ok(vkWaitForFences(device_, 1, &slot.fence, VK_TRUE, 1'000'000'000ull), "wait export")) return false;
    refract::protocol::WindowsGpuMarker reply{};
    std::memcpy(&reply, slot.mapped, sizeof(reply));
    const auto& sent = exportMarkers_[index];
    // gpu_marker() tells the mixed-layer path whether shared export works.
    gpuMarker_.status = 0;
    if (reply.magic != sent.magic || reply.session != sent.session || reply.sequence != sent.sequence || reply.status != 1) {
        __android_log_print(ANDROID_LOG_WARN, "Refract.GPU", "Host export unavailable (pipelined); falling back to pixel transfer");
        return false;
    }
    gpuMarker_.status = 1;  // Not the whole reply: gpuMarker_.sequence has moved on to later frames.
    *marker = reply;
    return true;
}
bool VulkanBackend::readback_ready(int index) {
    return index >= 0 && index < kReadbackSlots && vkGetFenceStatus(device_, slots_[index].fence) == VK_SUCCESS;
}
const uint8_t* VulkanBackend::readback_wait(int index) {
    static refract::protocol::PerfStats stats("readback-wait");
    refract::protocol::PerfScope scope(stats);
    if (index < 0 || index >= kReadbackSlots) return nullptr;
    auto& slot = slots_[index];
    if (!ok(vkWaitForFences(device_, 1, &slot.fence, VK_TRUE, 1'000'000'000ull), "wait readback")) return nullptr;
    return static_cast<const uint8_t*>(slot.mapped);
}
void VulkanBackend::readback_release(int index) {
    if (index < 0 || index >= kReadbackSlots) return;
    std::lock_guard<std::mutex> lock(g_slotMutex);
    slots_[index].busy = false;
}
bool VulkanBackend::readback(const VulkanSwapchain* const swapchains[2], const uint32_t indices[2],
                             const XrSwapchainSubImage* const subimages[2], uint32_t width, uint32_t height, std::vector<uint8_t> rgba[2]) {
    static refract::protocol::PerfStats stats("vulkan-frame-copy");
    refract::protocol::PerfScope scope(stats);
    if (!refract::protocol::valid_render_extent(width, height)) return false;
    const VkDeviceSize eyeBytes = VkDeviceSize(width) * height * 4;
    if (!gpuExportEnabled_ && eyeBytes * 2 > 128ull * 1024 * 1024) return false;
    if (!ensure_buffer(gpuExportEnabled_ ? sizeof(gpuMarker_) : eyeBytes * 2)) return false;
    for (uint32_t eye = 0; eye < 2; ++eye) rgba[eye].clear();
    if (!ensure_scaled(swapchains, width, height)) return false;
    gpuMarker_.status = 0;
    if (gpuExportEnabled_) {
        gpuMarker_.width = width;
        gpuMarker_.height = height;
        gpuMarker_.formats[0] = swapchains[0]->format; gpuMarker_.formats[1] = swapchains[1]->format;
        // Loading panels and the scene can use different extents/formats.
        // Each configuration owns a stable shared texture pair; previously
        // acknowledged pairs remain reusable when the app switches back.
        const std::array<uint32_t, 4> configuration{gpuMarker_.width, gpuMarker_.height,
            gpuMarker_.formats[0], gpuMarker_.formats[1]};
        auto found = exportConfigurations_.find(configuration);
        if (found == exportConfigurations_.end()) {
            if (exportConfigurations_.size() >= 16) {
                gpuExportEnabled_ = false;
            } else {
                found = exportConfigurations_.emplace(configuration, ++nextExportSession_).first;
            }
        }
        if (gpuExportEnabled_) gpuMarker_.session = found->second;
        ++gpuMarker_.sequence;
    }
    // The configuration cap may have disabled export; allocate pixel staging
    // before recording the fallback copy, never into the small marker buffer.
    if (!gpuExportEnabled_ && (eyeBytes * 2 > 128ull * 1024 * 1024 || !ensure_buffer(eyeBytes * 2))) return false;
    if (!begin()) return false;
    if (gpuExportEnabled_) {
        vkCmdUpdateBuffer(cmd_, buffer_, 0, sizeof(gpuMarker_), &gpuMarker_);
    }
    record_eye_copies(swapchains, indices, subimages, width, height, gpuExportEnabled_ ? VK_NULL_HANDLE : buffer_, false);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = buffer_; host.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
    if (!finish()) return false;
    if (gpuExportEnabled_) {
        refract::protocol::WindowsGpuMarker reply{};
        std::memcpy(&reply, mapped_, sizeof(reply));
        if (reply.magic == gpuMarker_.magic && reply.session == gpuMarker_.session && reply.sequence == gpuMarker_.sequence && reply.status == 1) {
            gpuMarker_ = reply;
            return true;
        }
        __android_log_print(ANDROID_LOG_WARN, "Refract.GPU", "Host export unavailable; falling back to pixel transfer");
        gpuExportEnabled_ = false;
        return readback(swapchains, indices, subimages, width, height, rgba);
    }
    for (uint32_t eye = 0; eye < 2; ++eye) {
        scaled_[eye].initialized = true;
        const auto& sub = *subimages[eye];
        const uint32_t w = width, h = height;
        rgba[eye].resize(static_cast<size_t>(w) * h * 4);
        // AXRI v2 uses the GLES bottom-up row convention; Vulkan rows start at the top.
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(rgba[eye].data() + static_cast<size_t>(y) * w * 4,
                        static_cast<const uint8_t*>(mapped_) + eye * eyeBytes + static_cast<size_t>(h - 1 - y) * w * 4, w * 4);
    }
    return true;
}
}
#endif
