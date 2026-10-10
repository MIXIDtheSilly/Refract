// Vulkan through the host thunks: instance, GPUs, a device and a buffer.
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

int main(void) {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    printf("instance extensions: %u\n", n);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vktest", 1, "none", 1, VK_API_VERSION_1_1};
    const char* exts[] = {"VK_KHR_surface", "VK_KHR_android_surface"};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, exts};
    VkInstance inst;
    VkResult r = vkCreateInstance(&ci, NULL, &inst);
    printf("vkCreateInstance = %d\n", r);
    if (r) return 1;
    uint32_t gpus = 0;
    vkEnumeratePhysicalDevices(inst, &gpus, NULL);
    VkPhysicalDevice pd[8];
    if (gpus > 8) gpus = 8;
    vkEnumeratePhysicalDevices(inst, &gpus, pd);
    for (uint32_t i = 0; i < gpus; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        printf("GPU %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_VERSION_MAJOR(p.apiVersion),
               VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &q, 0, NULL, 0, NULL, NULL};
    VkDevice dev;
    r = vkCreateDevice(pd[0], &dci, NULL, &dev);
    printf("vkCreateDevice = %d\n", r);
    PFN_vkCreateBuffer cb = (PFN_vkCreateBuffer)vkGetDeviceProcAddr(dev, "vkCreateBuffer");
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 65536, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer buf;
    r = cb(dev, &bci, NULL, &buf);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    printf("vkCreateBuffer via GetDeviceProcAddr = %d, size %llu\n", r, (unsigned long long)req.size);
    vkDestroyBuffer(dev, buf, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("vulkan ok\n");
    return 0;
}
