#pragma once

namespace rn {

void RegisterVulkanHle();
// Lets instance-level lookups work for instances created elsewhere (e.g. by OpenXR).
void NoteVulkanInstance(void* instance);
// The host loader's vkGetInstanceProcAddr (for XR_KHR_vulkan_enable2).
void* HostVkGetInstanceProcAddr();
// A host Vulkan function (through the first live instance).
void* VulkanHostProc(const char* name);
// Remembers which physical device a VkDevice was created on (vk_texcomp.cpp).
void NoteVulkanDevice(void* device, void* physical_device);

// Clears emulated features (ASTC) in a VkDeviceCreateInfo* for the host's
// vkCreateDevice; undone when the object goes out of scope.
class DeviceFeatureFix {
public:
    explicit DeviceFeatureFix(void* create_info);
    ~DeviceFeatureFix();
    DeviceFeatureFix(const DeviceFeatureFix&) = delete;
    DeviceFeatureFix& operator=(const DeviceFeatureFix&) = delete;

private:
    void* owned_ = nullptr;
    void* patched_ = nullptr;
};

#ifdef VK_VERSION_1_0
// vkCreateInstance/vkCreateDevice with Android -> Windows extension translation.
VkResult VulkanCreateInstanceTranslated(const VkInstanceCreateInfo* ci, VkInstance* out);
VkResult VulkanCreateDeviceTranslated(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, VkDevice* out);
#endif

}  // namespace rn
