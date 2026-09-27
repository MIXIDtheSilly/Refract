#pragma once
#if defined(__ANDROID__)
#include "openxr_dispatch/openxr_minimal.h"
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>
#include <android/hardware_buffer.h>
#include <array>
#include <vector>
#include <map>
#include "windows_gpu_frame.h"

struct XrGraphicsBindingVulkanKHR {
    XrStructureType type; const void* next;
    VkInstance instance; VkPhysicalDevice physicalDevice; VkDevice device;
    uint32_t queueFamilyIndex; uint32_t queueIndex;
};
struct XrVulkanInstanceCreateInfoKHR {
    XrStructureType type; const void* next; XrSystemId systemId; XrFlags64 createFlags;
    PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr;
    const VkInstanceCreateInfo* vulkanCreateInfo; const VkAllocationCallbacks* vulkanAllocator;
};
struct XrVulkanDeviceCreateInfoKHR {
    XrStructureType type; const void* next; XrSystemId systemId; XrFlags64 createFlags;
    PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr; VkPhysicalDevice vulkanPhysicalDevice;
    const VkDeviceCreateInfo* vulkanCreateInfo; const VkAllocationCallbacks* vulkanAllocator;
};
struct XrVulkanGraphicsDeviceGetInfoKHR {
    XrStructureType type; const void* next; XrSystemId systemId; VkInstance vulkanInstance;
};
namespace refract::runtime {
struct VulkanSwapchain {
    std::array<VkImage, 3> images{};
    std::array<VkDeviceMemory, 3> memory{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t layers = 0;
    // Imported AHardwareBuffers written by GLES: owned by the foreign queue family and
    // in GENERAL layout outside our copies (the acquire makes gfxstream sync GL -> Vulkan).
    bool external = false;
};
class VulkanBackend {
public:
    const refract::protocol::WindowsGpuMarker& gpu_marker() const { return gpuMarker_; }
    void disable_gpu_export() { gpuExportEnabled_ = false; }
    // A new consumer may take shared frames again. Fresh export sessions keep
    // textures from an uncertain completion out of use.
    bool reenable_gpu_export()
    {
        if (!gpuExportRequested_) return false;
        exportConfigurations_.clear();
        exportRings_.clear();
        gpuExportEnabled_ = true;
        return true;
    }
    bool active() const { return device_ != VK_NULL_HANDLE; }
    bool gpu_export_enabled() const { return gpuExportEnabled_; }
    static VkPhysicalDevice choose_device(VkInstance instance);
    bool initialize(const XrGraphicsBindingVulkanKHR& binding);
    // For GLES sessions: our own instance and device (with AHardwareBuffer import), so
    // GL frames can take the same export path. shutdown() destroys them.
    bool initialize_private();
    bool owns_device() const { return ownedInstance_ != VK_NULL_HANDLE; }
    // Imports a GPU-sampled/color-output AHardwareBuffer as a 1-layer image (see VulkanSwapchain::external).
    bool import_hardware_buffer(AHardwareBuffer* buffer, uint32_t width, uint32_t height, VkFormat format,
                                VkImage& image, VkDeviceMemory& memory);
    void destroy_image(VkImage image, VkDeviceMemory memory);
    void shutdown();
    XrResult create(VulkanSwapchain& sc, const XrSwapchainCreateInfo& info);
    void destroy(VulkanSwapchain& sc);
    bool readback(const VulkanSwapchain* const swapchains[2], const uint32_t indices[2],
                  const XrSwapchainSubImage* const subimages[2], uint32_t width, uint32_t height, std::vector<uint8_t> rgba[2]);
    // Pixel readback without stalling the app: records and submits the copy of both eyes into
    // a staging slot and returns its index at once (-1 if every slot is still in use). Another
    // thread then calls readback_wait (both eyes back to back, bottom-up rows like readback)
    // and readback_release. *nv12 (in: wanted, out: done) converts on the GPU instead: the slot
    // then holds one tight NV12 picture of both eyes side by side (even width and height only).
    int readback_async(const VulkanSwapchain* const swapchains[2], const uint32_t indices[2],
                       const XrSwapchainSubImage* const subimages[2], uint32_t width, uint32_t height, bool* nv12);
    bool readback_ready(int slot);  // The slot's copy has finished (no waiting).
    const uint8_t* readback_wait(int slot);
    void readback_release(int slot);
    // Shared GPU export without stalling the app: records and submits the copy of both eyes into
    // the next of kExportRing shared texture pairs and returns its index (-1: export unavailable).
    // The caller must know the viewer is done with that pair: it has acknowledged every frame
    // but the last kExportRing - 2 sent. export_wait then waits for the copy and, when the host
    // layer exported it, returns true with the marker naming the pair.
    static constexpr int kExportRing = 4;
    int export_async(const VulkanSwapchain* const swapchains[2], const uint32_t indices[2],
                     const XrSwapchainSubImage* const subimages[2], uint32_t width, uint32_t height);
    bool export_wait(int index, refract::protocol::WindowsGpuMarker* marker);
    // One region of a swapchain image, copied (scaled) into atlas texture `texture` at (x, y, width, height).
    struct AtlasBlit {
        const VulkanSwapchain* swapchain = nullptr;
        uint32_t index = 0;
        XrSwapchainSubImage sub{};
        uint32_t texture = 0;
        int32_t x = 0, y = 0, width = 0, height = 0;
    };
    // export_async for a composite frame (scene and panels): blits every region into a width x height
    // sRGB atlas pair, then exports that pair through the same ring; export_wait as usual.
    int export_atlas_async(const AtlasBlit* blits, uint32_t count, uint32_t width, uint32_t height);
private:
    // Takes the next export ring slot of `configuration` (width, height, two formats), records the
    // copies with `record(markerBuffer, marker)` (which must write the marker before the two export
    // blits) and submits them. Returns the slot index, or -1.
    template <typename Record>
    int export_ring(const std::array<uint32_t, 4>& configuration, Record&& record);
    bool ensure_atlas(uint32_t width, uint32_t height);
    struct AtlasImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        bool initialized = false;  // Recorded into a layout at least once.
    };
    // Atlas textures 0 and 1, then a sink: the export blits' nominal destination, written only
    // if the host layer declines to redirect them.
    AtlasImage atlas_[3];
    uint32_t atlasWidth_ = 0, atlasHeight_ = 0;
    struct ReadbackSlot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize bytes = 0;
        VkDescriptorSet set = VK_NULL_HANDLE;  // NV12 conversion inputs/output.
        bool submitted = false;  // The fence will signal (or has); wait before reuse.
        bool busy = false;       // Handed to a consumer, not yet released.
    };
    // (Re)creates a mapped host-visible transfer target of at least `bytes`.
    bool host_buffer(VkBuffer& buffer, VkDeviceMemory& memory, void*& mapped, VkDeviceSize& size, VkDeviceSize bytes);
    bool ensure_scaled(const VulkanSwapchain* const swapchains[2], uint32_t width, uint32_t height, bool sampled = false);
    bool ensure_nv12_pipeline();
    void record_eye_copies(const VulkanSwapchain* const swapchains[2], const uint32_t indices[2],
                           const XrSwapchainSubImage* const subimages[2], uint32_t width, uint32_t height,
                           VkBuffer target, bool flipRows);
    static constexpr int kReadbackSlots = 4;  // One being encoded, two queued, one being filled.
    std::array<ReadbackSlot, kReadbackSlots> slots_{};  // busy flags are guarded by a file-scope mutex.
    bool ensure_buffer(VkDeviceSize bytes);
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags);
    bool allocate_image(VkImage& image, VkDeviceMemory& memory, VkFormat format,
                        uint32_t width, uint32_t height, uint32_t layers, VkImageUsageFlags usage,
                        VkImageCreateFlags flags = 0, uint32_t mipLevels = 1);
    bool begin();
    bool finish();
    void barrier(VkImage image, uint32_t layers, VkImageLayout before, VkImageLayout after,
                 VkAccessFlags src, VkAccessFlags dst,
                 uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED, uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED,
                 uint32_t mipLevels = 1);
    VkInstance ownedInstance_ = VK_NULL_HANDLE;  // initialize_private() created instance and device_.
    uint32_t queueFamily_ = 0;
    bool gpuExportEnabled_ = false;
    bool gpuExportRequested_ = false;
    refract::protocol::WindowsGpuMarker gpuMarker_{};
    uint64_t nextExportSession_ = 0;
    std::map<std::array<uint32_t, 4>, uint64_t> exportConfigurations_;
    // Pipelined export: one session (shared texture pair) per ring slot and configuration.
    std::map<std::array<uint32_t, 4>, std::array<uint64_t, kExportRing>> exportRings_;
    std::array<ReadbackSlot, kExportRing> exportSlots_{};  // Marker buffer, commands and fence.
    std::array<refract::protocol::WindowsGpuMarker, kExportRing> exportMarkers_{};
    uint64_t exportFrames_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory bufferMemory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    VkDeviceSize bufferBytes_ = 0;
    struct ScaledImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageView view = VK_NULL_HANDLE;  // UNORM (raw bytes) view; only for sampled images.
        bool initialized = false, sampled = false;
        uint32_t width = 0, height = 0;
    } scaled_[2];
    // RGBA -> NV12 compute pass (shaders/rgba_to_nv12.comp).
    VkDescriptorSetLayout nv12SetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout nv12Layout_ = VK_NULL_HANDLE;
    VkPipeline nv12Pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool nv12Pool_ = VK_NULL_HANDLE;
    VkSampler nv12Sampler_ = VK_NULL_HANDLE;
    bool nv12Failed_ = false;
};
}
#endif
