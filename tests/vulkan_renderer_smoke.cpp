// The Linux host bridge's Vulkan copies on a real GPU. A stand-in for the PC runtime's XR_KHR_vulkan_enable2
// functions hands VulkanRenderer an ordinary Vulkan device; the test reads back what upload() and copy() wrote
// into images laid out like OpenXR swapchain images (COLOR_ATTACHMENT_OPTIMAL, two array layers).
// Without a Vulkan device it is skipped (exit code 77).
#include "vulkan_renderer.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int kSkip = 77;
PFN_vkGetInstanceProcAddr gGetInstanceProcAddr = nullptr;
VkInstance gInstance = VK_NULL_HANDLE;

template <typename T>
T instance_func(const char* name) { return reinterpret_cast<T>(gGetInstanceProcAddr(gInstance, name)); }

XrResult XRAPI_CALL requirements(XrInstance, XrSystemId, XrGraphicsRequirementsVulkanKHR* out)
{
    out->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    out->maxApiVersionSupported = XR_MAKE_VERSION(1, 4, 0);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL create_instance(XrInstance, const XrVulkanInstanceCreateInfoKHR* info, VkInstance* out, VkResult* result)
{
    gGetInstanceProcAddr = info->pfnGetInstanceProcAddr;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(gGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    *result = create(info->vulkanCreateInfo, nullptr, out);
    gInstance = *out;
    return XR_SUCCESS;
}

// Like a runtime choosing the headset's GPU: a real one before a CPU implementation.
XrResult XRAPI_CALL graphics_device(XrInstance, const XrVulkanGraphicsDeviceGetInfoKHR*, VkPhysicalDevice* out)
{
    const auto enumerate = instance_func<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
    const auto properties = instance_func<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
    uint32_t count = 0;
    enumerate(gInstance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    enumerate(gInstance, &count, devices.data());
    if (devices.empty()) return XR_ERROR_RUNTIME_FAILURE;
    *out = devices[0];
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties props{};
        properties(device, &props);
        if (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) { *out = device; break; }
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL create_device(XrInstance, const XrVulkanDeviceCreateInfoKHR* info, VkDevice* out, VkResult* result)
{
    const auto create = instance_func<PFN_vkCreateDevice>("vkCreateDevice");
    *result = create(info->vulkanPhysicalDevice, info->vulkanCreateInfo, nullptr, out);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL get_proc(XrInstance, const char* name, PFN_xrVoidFunction* out)
{
    *out = nullptr;
    if (!std::strcmp(name, "xrGetVulkanGraphicsRequirements2KHR")) *out = reinterpret_cast<PFN_xrVoidFunction>(&requirements);
    if (!std::strcmp(name, "xrCreateVulkanInstanceKHR")) *out = reinterpret_cast<PFN_xrVoidFunction>(&create_instance);
    if (!std::strcmp(name, "xrGetVulkanGraphicsDevice2KHR")) *out = reinterpret_cast<PFN_xrVoidFunction>(&graphics_device);
    if (!std::strcmp(name, "xrCreateVulkanDeviceKHR")) *out = reinterpret_cast<PFN_xrVoidFunction>(&create_device);
    return *out ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
}

// What the test itself does with the renderer's device: make "swapchain" images and read them back.
struct Gpu {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;

    template <typename T>
    T func(const char* name) const { return reinterpret_cast<T>(getDeviceProcAddr(device, name)); }

    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags wanted) const
    {
        VkPhysicalDeviceMemoryProperties memory{};
        instance_func<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &memory);
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
        return UINT32_MAX;
    }

    void run(void (*record)(const Gpu&, void*), void* context) const
    {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        func<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(commands, 0);
        func<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin);
        record(*this, context);
        func<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        func<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, VK_NULL_HANDLE);
        func<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue);
    }

    void transition(VkImage image, uint32_t layers, VkImageLayout from, VkImageLayout to) const
    {
        struct Context { VkImage image; uint32_t layers; VkImageLayout from, to; } context{image, layers, from, to};
        run([](const Gpu& gpu, void* raw) {
            const auto& c = *static_cast<Context*>(raw);
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.oldLayout = c.from;
            barrier.newLayout = c.to;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = c.image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, c.layers};
            gpu.func<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(gpu.commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }, &context);
    }

    // An image as the runtime hands it out after xrWaitSwapchainImage, filled with `fill`.
    VkImage make_image(uint32_t width, uint32_t height, uint32_t layers, uint8_t fill) const
    {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = layers;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImage image = VK_NULL_HANDLE;
        if (func<PFN_vkCreateImage>("vkCreateImage")(device, &info, nullptr, &image) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkMemoryRequirements requirements{};
        func<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device, image, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (allocate.memoryTypeIndex == UINT32_MAX ||
            func<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &allocate, nullptr, &memory) != VK_SUCCESS ||
            func<PFN_vkBindImageMemory>("vkBindImageMemory")(device, image, memory, 0) != VK_SUCCESS) return VK_NULL_HANDLE;
        transition(image, layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        struct Context { VkImage image; uint32_t layers; uint8_t fill; } context{image, layers, fill};
        run([](const Gpu& gpu, void* raw) {
            const auto& c = *static_cast<Context*>(raw);
            const float value = c.fill / 255.0f;
            VkClearColorValue color{};
            color.float32[0] = color.float32[1] = color.float32[2] = color.float32[3] = value;
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, c.layers};
            gpu.func<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(gpu.commands, c.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        }, &context);
        transition(image, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        return image;
    }

    std::vector<uint8_t> read(VkImage image, uint32_t layer, uint32_t width, uint32_t height) const
    {
        const VkDeviceSize size = VkDeviceSize(width) * height * 4;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buffer = VK_NULL_HANDLE;
        func<PFN_vkCreateBuffer>("vkCreateBuffer")(device, &info, nullptr, &buffer);
        VkMemoryRequirements requirements{};
        func<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, buffer, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memory_type(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        func<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &allocate, nullptr, &memory);
        func<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, buffer, memory, 0);
        transition(image, layer + 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        struct Context { VkImage image; VkBuffer buffer; uint32_t layer, width, height; } context{image, buffer, layer, width, height};
        run([](const Gpu& gpu, void* raw) {
            const auto& c = *static_cast<Context*>(raw);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, c.layer, 1};
            region.imageExtent = {c.width, c.height, 1};
            gpu.func<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(gpu.commands, c.image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.buffer, 1, &region);
        }, &context);
        transition(image, layer + 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        void* mapped = nullptr;
        func<PFN_vkMapMemory>("vkMapMemory")(device, memory, 0, size, 0, &mapped);
        std::vector<uint8_t> pixels(static_cast<const uint8_t*>(mapped), static_cast<const uint8_t*>(mapped) + size);
        func<PFN_vkUnmapMemory>("vkUnmapMemory")(device, memory);
        func<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, buffer, nullptr);
        func<PFN_vkFreeMemory>("vkFreeMemory")(device, memory, nullptr);
        return pixels;
    }
};

uint8_t pattern(uint32_t layer, uint32_t x, uint32_t y, uint32_t channel)
{
    return static_cast<uint8_t>(layer * 101 + x * 7 + y * 13 + channel * 29 + 1);
}

int failures = 0;
void expect(bool condition, const char* what)
{
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

// The written width x height corner holds the pattern of `layer`; the rest of the image still holds `fill`.
bool holds(const std::vector<uint8_t>& image, uint32_t imageWidth, uint32_t imageHeight, uint32_t layer,
           uint32_t width, uint32_t height, uint8_t fill)
{
    for (uint32_t y = 0; y < imageHeight; ++y)
        for (uint32_t x = 0; x < imageWidth; ++x)
            for (uint32_t c = 0; c < 4; ++c) {
                const uint8_t expected = x < width && y < height ? pattern(layer, x, y, c) : fill;
                if (image[(size_t(y) * imageWidth + x) * 4 + c] != expected) {
                    std::fprintf(stderr, "  layer %u (%u,%u) channel %u: %u, expected %u\n",
                        layer, x, y, c, image[(size_t(y) * imageWidth + x) * 4 + c], expected);
                    return false;
                }
            }
    return true;
}

} // namespace

int main()
{
    refract::host::VulkanRenderer renderer;
    if (!renderer.create(reinterpret_cast<XrInstance>(1), 1, &get_proc)) {
        std::fprintf(stderr, "No usable Vulkan device; skipping\n");
        return kSkip;
    }
    const auto& binding = renderer.binding();
    Gpu gpu;
    gpu.device = binding.device;
    gpu.physical = binding.physicalDevice;
    gpu.getDeviceProcAddr = instance_func<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    gpu.func<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(gpu.device, binding.queueFamilyIndex, binding.queueIndex, &gpu.queue);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = binding.queueFamilyIndex;
    gpu.func<PFN_vkCreateCommandPool>("vkCreateCommandPool")(gpu.device, &poolInfo, nullptr, &gpu.pool);
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = gpu.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    gpu.func<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(gpu.device, &allocate, &gpu.commands);

    // A projection image larger than the frame, as when the game renders below the headset's resolution.
    constexpr uint32_t kImageWidth = 64, kImageHeight = 40, kWidth = 50, kHeight = 31;
    constexpr uint8_t kFill = 9;
    const VkImage projection = gpu.make_image(kImageWidth, kImageHeight, 2, kFill);
    const VkImage panel = gpu.make_image(kImageWidth, kImageHeight, 1, kFill);
    expect(projection != VK_NULL_HANDLE && panel != VK_NULL_HANDLE, "test images created");
    if (failures) return 1;

    std::vector<uint8_t> frame(size_t(kWidth) * kHeight * 4 * 2);
    for (uint32_t layer = 0; layer < 2; ++layer)
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
                for (uint32_t c = 0; c < 4; ++c)
                    frame[((size_t(layer) * kHeight + y) * kWidth + x) * 4 + c] = pattern(layer, x, y, c);

    // Twice: the second upload reuses the staging buffer and starts from the layout the first left behind.
    for (int round = 0; round < 2; ++round) {
        expect(renderer.upload(projection, frame.data(), kWidth, kHeight, 2), "upload succeeds");
        expect(holds(gpu.read(projection, 0, kImageWidth, kImageHeight), kImageWidth, kImageHeight, 0, kWidth, kHeight, kFill),
            "left eye layer holds the uploaded frame");
        expect(holds(gpu.read(projection, 1, kImageWidth, kImageHeight), kImageWidth, kImageHeight, 1, kWidth, kHeight, kFill),
            "right eye layer holds the uploaded frame");
    }

    expect(renderer.copy(projection, 1, panel, kWidth, kHeight), "panel copy succeeds");
    expect(holds(gpu.read(panel, 0, kImageWidth, kImageHeight), kImageWidth, kImageHeight, 1, kWidth, kHeight, kFill),
        "panel holds the copied layer");
    expect(holds(gpu.read(projection, 1, kImageWidth, kImageHeight), kImageWidth, kImageHeight, 1, kWidth, kHeight, kFill),
        "copy leaves its source intact");

    renderer.wait_idle();
    if (failures) return 1;
    std::fprintf(stderr, "Vulkan renderer smoke OK\n");
    return 0;
}
