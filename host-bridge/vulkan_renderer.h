#pragma once
#if !defined(_WIN32)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES  // libvulkan.so.1 is opened at run time, like the OpenXR loader.
#endif
#include <vulkan/vulkan.h>
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>

namespace refract::host {

// The Linux host's GPU side: a Vulkan device made through the PC runtime (XR_KHR_vulkan_enable2) and the
// copies that put Android frames into its swapchain images. Only the frame loop's thread uses it, and that
// thread is also the only one calling the runtime, which shares the queue.
class VulkanRenderer {
public:
    VulkanRenderer() = default;
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;
    ~VulkanRenderer();

    bool create(XrInstance instance, XrSystemId system, PFN_xrGetInstanceProcAddr getInstanceProcAddr);
    const XrGraphicsBindingVulkan2KHR& binding() const { return binding_; }

    // Copies `layers` tightly packed RGBA8 images of width x height into the top-left of the first layers of
    // a swapchain image the caller has acquired and waited for (so it is in COLOR_ATTACHMENT_OPTIMAL).
    bool upload(VkImage image, const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t layers);
    // Copies the top-left width x height of one layer of `source` into layer 0 of `dest`; both are acquired
    // swapchain images.
    bool copy(VkImage source, uint32_t sourceLayer, VkImage dest, uint32_t width, uint32_t height);
    // Before destroying swapchains whose images a copy may still read.
    void wait_idle();

private:
    bool load_device_functions();
    bool ensure_staging(VkDeviceSize size);
    bool begin();
    bool submit();

    void* library_ = nullptr;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
    VkDeviceSize stagingSize_ = 0;
    void* stagingMapped_ = nullptr;
    XrGraphicsBindingVulkan2KHR binding_{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};

    PFN_vkDestroyInstance destroyInstance_ = nullptr;
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties_ = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties_ = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getPhysicalDeviceQueueFamilyProperties_ = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr_ = nullptr;
    PFN_vkDestroyDevice destroyDevice_ = nullptr;
    PFN_vkDeviceWaitIdle deviceWaitIdle_ = nullptr;
    PFN_vkGetDeviceQueue getDeviceQueue_ = nullptr;
    PFN_vkCreateCommandPool createCommandPool_ = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool_ = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers_ = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer_ = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer_ = nullptr;
    PFN_vkResetCommandBuffer resetCommandBuffer_ = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier_ = nullptr;
    PFN_vkCmdCopyBufferToImage cmdCopyBufferToImage_ = nullptr;
    PFN_vkCmdCopyImage cmdCopyImage_ = nullptr;
    PFN_vkQueueSubmit queueSubmit_ = nullptr;
    PFN_vkCreateFence createFence_ = nullptr;
    PFN_vkDestroyFence destroyFence_ = nullptr;
    PFN_vkWaitForFences waitForFences_ = nullptr;
    PFN_vkResetFences resetFences_ = nullptr;
    PFN_vkCreateBuffer createBuffer_ = nullptr;
    PFN_vkDestroyBuffer destroyBuffer_ = nullptr;
    PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements_ = nullptr;
    PFN_vkAllocateMemory allocateMemory_ = nullptr;
    PFN_vkFreeMemory freeMemory_ = nullptr;
    PFN_vkBindBufferMemory bindBufferMemory_ = nullptr;
    PFN_vkMapMemory mapMemory_ = nullptr;
};

} // namespace refract::host
#endif
