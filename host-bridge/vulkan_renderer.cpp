#include "vulkan_renderer.h"
#if !defined(_WIN32)

#include <dlfcn.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>

namespace refract::host {

namespace {

constexpr VkImageLayout kAcquiredLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;  // OpenXR's Vulkan rule.

bool vk_ok(VkResult result, const char* what)
{
    if (result == VK_SUCCESS) return true;
    std::fprintf(stderr, "Refract Vulkan: %s failed: %d\n", what, static_cast<int>(result));
    return false;
}

bool xr_ok(XrResult result, const char* what)
{
    if (result == XR_SUCCESS) return true;
    std::fprintf(stderr, "Refract Vulkan: %s failed: %d\n", what, static_cast<int>(result));
    return false;
}

VkImageMemoryBarrier layout_barrier(VkImage image, uint32_t baseLayer, uint32_t layers, VkImageLayout from, VkImageLayout to,
                                    VkAccessFlags srcAccess, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, baseLayer, layers};
    return barrier;
}

} // namespace

VulkanRenderer::~VulkanRenderer()
{
    if (device_ != VK_NULL_HANDLE) {
        deviceWaitIdle_(device_);
        if (staging_ != VK_NULL_HANDLE) destroyBuffer_(device_, staging_, nullptr);
        if (stagingMemory_ != VK_NULL_HANDLE) freeMemory_(device_, stagingMemory_, nullptr);
        if (fence_ != VK_NULL_HANDLE) destroyFence_(device_, fence_, nullptr);
        if (pool_ != VK_NULL_HANDLE) destroyCommandPool_(device_, pool_, nullptr);
        destroyDevice_(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE && destroyInstance_ != nullptr) destroyInstance_(instance_, nullptr);
    if (library_ != nullptr) dlclose(library_);
}

bool VulkanRenderer::create(XrInstance instance, XrSystemId system, PFN_xrGetInstanceProcAddr getInstanceProcAddr)
{
    library_ = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (library_ == nullptr) {
        std::fprintf(stderr, "Refract Vulkan: cannot load libvulkan.so.1\n");
        return false;
    }
    getInstanceProcAddr_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library_, "vkGetInstanceProcAddr"));
    if (getInstanceProcAddr_ == nullptr) return false;

    PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements = nullptr;
    PFN_xrCreateVulkanInstanceKHR createInstance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR getDevice = nullptr;
    PFN_xrCreateVulkanDeviceKHR createDevice = nullptr;
    const auto xr_func = [&](const char* name, auto* out) {
        PFN_xrVoidFunction function = nullptr;
        if (getInstanceProcAddr(instance, name, &function) != XR_SUCCESS || function == nullptr) {
            std::fprintf(stderr, "Refract Vulkan: runtime lacks %s\n", name);
            return false;
        }
        *out = reinterpret_cast<std::remove_pointer_t<decltype(out)>>(function);
        return true;
    };
    if (!xr_func("xrGetVulkanGraphicsRequirements2KHR", &getRequirements) ||
        !xr_func("xrCreateVulkanInstanceKHR", &createInstance) ||
        !xr_func("xrGetVulkanGraphicsDevice2KHR", &getDevice) ||
        !xr_func("xrCreateVulkanDeviceKHR", &createDevice)) return false;

    XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (!xr_ok(getRequirements(instance, system, &requirements), "xrGetVulkanGraphicsRequirements2KHR")) return false;
    // Vulkan 1.1 covers everything used here; ask for more only if the runtime insists.
    const XrVersion minimum = requirements.minApiVersionSupported;
    const uint32_t apiVersion = (std::max)(VK_API_VERSION_1_1,
        VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(minimum), XR_VERSION_MINOR(minimum), 0));

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Refract host bridge";
    app.pEngineName = "Refract";
    app.apiVersion = apiVersion;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    XrVulkanInstanceCreateInfoKHR xrInstanceInfo{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xrInstanceInfo.systemId = system;
    xrInstanceInfo.pfnGetInstanceProcAddr = getInstanceProcAddr_;
    xrInstanceInfo.vulkanCreateInfo = &instanceInfo;
    VkResult vkResult = VK_SUCCESS;
    if (!xr_ok(createInstance(instance, &xrInstanceInfo, &instance_, &vkResult), "xrCreateVulkanInstanceKHR") ||
        !vk_ok(vkResult, "vkCreateInstance")) return false;

    const auto vk_instance_func = [&](const char* name, auto* out) {
        *out = reinterpret_cast<std::remove_pointer_t<decltype(out)>>(getInstanceProcAddr_(instance_, name));
        if (*out == nullptr) std::fprintf(stderr, "Refract Vulkan: missing %s\n", name);
        return *out != nullptr;
    };
    if (!vk_instance_func("vkDestroyInstance", &destroyInstance_) ||
        !vk_instance_func("vkGetPhysicalDeviceProperties", &getPhysicalDeviceProperties_) ||
        !vk_instance_func("vkGetPhysicalDeviceMemoryProperties", &getPhysicalDeviceMemoryProperties_) ||
        !vk_instance_func("vkGetPhysicalDeviceQueueFamilyProperties", &getPhysicalDeviceQueueFamilyProperties_) ||
        !vk_instance_func("vkGetDeviceProcAddr", &getDeviceProcAddr_)) return false;

    // The runtime picks the GPU the headset is on (with two GPUs, not necessarily the first).
    XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    deviceGetInfo.systemId = system;
    deviceGetInfo.vulkanInstance = instance_;
    if (!xr_ok(getDevice(instance, &deviceGetInfo, &physical_), "xrGetVulkanGraphicsDevice2KHR")) return false;

    uint32_t familyCount = 0;
    getPhysicalDeviceQueueFamilyProperties_(physical_, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    getPhysicalDeviceQueueFamilyProperties_(physical_, &familyCount, families.data());
    queueFamily_ = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount && queueFamily_ == UINT32_MAX; ++i)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) queueFamily_ = i;
    if (queueFamily_ == UINT32_MAX) {
        std::fprintf(stderr, "Refract Vulkan: the runtime's GPU has no graphics queue\n");
        return false;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    XrVulkanDeviceCreateInfoKHR xrDeviceInfo{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xrDeviceInfo.systemId = system;
    xrDeviceInfo.pfnGetInstanceProcAddr = getInstanceProcAddr_;
    xrDeviceInfo.vulkanPhysicalDevice = physical_;
    xrDeviceInfo.vulkanCreateInfo = &deviceInfo;
    if (!xr_ok(createDevice(instance, &xrDeviceInfo, &device_, &vkResult), "xrCreateVulkanDeviceKHR") ||
        !vk_ok(vkResult, "vkCreateDevice")) {
        device_ = VK_NULL_HANDLE;
        return false;
    }
    if (!load_device_functions()) {
        // Without them the destructor could not release the device either.
        device_ = VK_NULL_HANDLE;
        return false;
    }
    getDeviceQueue_(device_, queueFamily_, 0, &queue_);

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    if (!vk_ok(createCommandPool_(device_, &poolInfo, nullptr, &pool_), "vkCreateCommandPool")) return false;
    VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = pool_;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    if (!vk_ok(allocateCommandBuffers_(device_, &allocateInfo, &commands_), "vkAllocateCommandBuffers")) return false;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!vk_ok(createFence_(device_, &fenceInfo, nullptr, &fence_), "vkCreateFence")) return false;

    binding_.instance = instance_;
    binding_.physicalDevice = physical_;
    binding_.device = device_;
    binding_.queueFamilyIndex = queueFamily_;
    binding_.queueIndex = 0;

    VkPhysicalDeviceProperties properties{};
    getPhysicalDeviceProperties_(physical_, &properties);
    std::fprintf(stderr, "Refract Vulkan: graphics binding ready on %s (Vulkan %u.%u)\n", properties.deviceName,
        VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion));
    return true;
}

bool VulkanRenderer::load_device_functions()
{
    const auto func = [&](const char* name, auto* out) {
        *out = reinterpret_cast<std::remove_pointer_t<decltype(out)>>(getDeviceProcAddr_(device_, name));
        if (*out == nullptr) std::fprintf(stderr, "Refract Vulkan: missing %s\n", name);
        return *out != nullptr;
    };
    return func("vkDestroyDevice", &destroyDevice_) && func("vkDeviceWaitIdle", &deviceWaitIdle_) &&
        func("vkGetDeviceQueue", &getDeviceQueue_) && func("vkCreateCommandPool", &createCommandPool_) &&
        func("vkDestroyCommandPool", &destroyCommandPool_) && func("vkAllocateCommandBuffers", &allocateCommandBuffers_) &&
        func("vkBeginCommandBuffer", &beginCommandBuffer_) && func("vkEndCommandBuffer", &endCommandBuffer_) &&
        func("vkResetCommandBuffer", &resetCommandBuffer_) && func("vkCmdPipelineBarrier", &cmdPipelineBarrier_) &&
        func("vkCmdCopyBufferToImage", &cmdCopyBufferToImage_) && func("vkCmdCopyImage", &cmdCopyImage_) &&
        func("vkQueueSubmit", &queueSubmit_) && func("vkCreateFence", &createFence_) &&
        func("vkDestroyFence", &destroyFence_) && func("vkWaitForFences", &waitForFences_) &&
        func("vkResetFences", &resetFences_) && func("vkCreateBuffer", &createBuffer_) &&
        func("vkDestroyBuffer", &destroyBuffer_) && func("vkGetBufferMemoryRequirements", &getBufferMemoryRequirements_) &&
        func("vkAllocateMemory", &allocateMemory_) && func("vkFreeMemory", &freeMemory_) &&
        func("vkBindBufferMemory", &bindBufferMemory_) && func("vkMapMemory", &mapMemory_);
}

// One host-visible buffer, grown to the largest frame seen. Every copy waits for its fence, so the next
// frame can overwrite it.
bool VulkanRenderer::ensure_staging(VkDeviceSize size)
{
    if (size <= stagingSize_) return true;
    if (staging_ != VK_NULL_HANDLE) destroyBuffer_(device_, staging_, nullptr);
    if (stagingMemory_ != VK_NULL_HANDLE) freeMemory_(device_, stagingMemory_, nullptr);
    staging_ = VK_NULL_HANDLE;
    stagingMemory_ = VK_NULL_HANDLE;
    stagingSize_ = 0;
    stagingMapped_ = nullptr;

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!vk_ok(createBuffer_(device_, &bufferInfo, nullptr, &staging_), "vkCreateBuffer")) return false;
    VkMemoryRequirements requirements{};
    getBufferMemoryRequirements_(device_, staging_, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    getPhysicalDeviceMemoryProperties_(physical_, &memory);
    constexpr VkMemoryPropertyFlags kWanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & kWanted) == kWanted) type = i;
    if (type == UINT32_MAX) {
        std::fprintf(stderr, "Refract Vulkan: no host-visible memory for frame uploads\n");
        return false;
    }
    VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = type;
    if (!vk_ok(allocateMemory_(device_, &allocateInfo, nullptr, &stagingMemory_), "vkAllocateMemory") ||
        !vk_ok(bindBufferMemory_(device_, staging_, stagingMemory_, 0), "vkBindBufferMemory") ||
        !vk_ok(mapMemory_(device_, stagingMemory_, 0, VK_WHOLE_SIZE, 0, &stagingMapped_), "vkMapMemory")) return false;
    stagingSize_ = size;
    return true;
}

bool VulkanRenderer::begin()
{
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vk_ok(resetCommandBuffer_(commands_, 0), "vkResetCommandBuffer") &&
        vk_ok(beginCommandBuffer_(commands_, &beginInfo), "vkBeginCommandBuffer");
}

// Waits for the copy: the staging buffer is reused, and the caller releases the image to the runtime next.
bool VulkanRenderer::submit()
{
    if (!vk_ok(endCommandBuffer_(commands_), "vkEndCommandBuffer")) return false;
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commands_;
    if (!vk_ok(resetFences_(device_, 1, &fence_), "vkResetFences") ||
        !vk_ok(queueSubmit_(queue_, 1, &submitInfo, fence_), "vkQueueSubmit")) return false;
    return vk_ok(waitForFences_(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
}

bool VulkanRenderer::upload(VkImage image, const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t layers)
{
    if (device_ == VK_NULL_HANDLE || image == VK_NULL_HANDLE || !width || !height || !layers) return false;
    const VkDeviceSize layerBytes = static_cast<VkDeviceSize>(width) * height * 4;
    if (!ensure_staging(layerBytes * layers)) return false;
    std::memcpy(stagingMapped_, pixels, static_cast<size_t>(layerBytes * layers));
    if (!begin()) return false;

    VkImageMemoryBarrier toTransfer = layout_barrier(image, 0, layers, kAcquiredLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    cmdPipelineBarrier_(commands_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 0, nullptr, 1, &toTransfer);
    std::vector<VkBufferImageCopy> regions(layers);
    for (uint32_t layer = 0; layer < layers; ++layer) {
        auto& region = regions[layer];
        region.bufferOffset = layerBytes * layer;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        region.imageExtent = {width, height, 1};
    }
    cmdCopyBufferToImage_(commands_, staging_, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layers, regions.data());
    VkImageMemoryBarrier back = layout_barrier(image, 0, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kAcquiredLayout,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    cmdPipelineBarrier_(commands_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
        0, nullptr, 0, nullptr, 1, &back);
    return submit();
}

bool VulkanRenderer::copy(VkImage source, uint32_t sourceLayer, VkImage dest, uint32_t width, uint32_t height)
{
    if (device_ == VK_NULL_HANDLE || source == VK_NULL_HANDLE || dest == VK_NULL_HANDLE || !width || !height) return false;
    if (!begin()) return false;
    const VkImageMemoryBarrier toTransfer[] = {
        layout_barrier(source, sourceLayer, 1, kAcquiredLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
        layout_barrier(dest, 0, 1, kAcquiredLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    cmdPipelineBarrier_(commands_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, toTransfer);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, sourceLayer, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {width, height, 1};
    cmdCopyImage_(commands_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    const VkImageMemoryBarrier back[] = {
        layout_barrier(source, sourceLayer, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kAcquiredLayout,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT),
        layout_barrier(dest, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kAcquiredLayout,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT),
    };
    cmdPipelineBarrier_(commands_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
        0, nullptr, 0, nullptr, 2, back);
    return submit();
}

void VulkanRenderer::wait_idle()
{
    if (device_ != VK_NULL_HANDLE) deviceWaitIdle_(device_);
}

} // namespace refract::host
#endif
