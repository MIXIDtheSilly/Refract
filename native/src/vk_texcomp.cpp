// ASTC texture emulation for host GPUs without ASTC (desktop NVIDIA/AMD).
//
// Quest games ship ASTC textures. When the device cannot sample them, Unity
// decodes every texture on the CPU, which under the JIT takes minutes. Instead
// we report ASTC LDR as supported, back such images with RGBA8 and decode the
// blocks on the host when the app copies them in (vkCmdCopyBufferToImage):
// the source is read from the mapped staging buffer at record time and the copy
// is redirected to a host staging buffer holding the decoded texels. That
// buffer lives until its command buffer is reset or freed (by then the spec
// guarantees it is no longer pending).
#include <windows.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <astcenc.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "thunks.h"
#include "vulkan_hle.h"

namespace rn {

namespace {

template <typename T>
T Fn(const char* name) {
    return reinterpret_cast<T>(VulkanHostProc(name));
}

// --- formats ------------------------------------------------------------------------

struct AstcFormat {
    uint32_t bw, bh;
    bool srgb;
};

bool AstcInfo(VkFormat f, AstcFormat* out) {
    static const uint8_t dims[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                        {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    if (f < VK_FORMAT_ASTC_4x4_UNORM_BLOCK || f > VK_FORMAT_ASTC_12x12_SRGB_BLOCK)
        return false;
    int i = f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    if (out)
        *out = {dims[i / 2][0], dims[i / 2][1], (i & 1) != 0};
    return true;
}

VkFormat Substitute(VkFormat f) {
    AstcFormat a;
    if (!AstcInfo(f, &a))
        return f;
    return a.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
}

constexpr VkFormatFeatureFlags kEmulatedFeatures =
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
constexpr VkImageUsageFlags kEmulatedUsage =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

std::mutex g_mu;
std::map<VkPhysicalDevice, bool> g_needs;  // per physical device: ASTC missing on the host
VkPhysicalDevice g_last_pd = VK_NULL_HANDLE;
std::unordered_map<VkDevice, VkPhysicalDevice> g_device_pd;

bool NeedsEmulation(VkPhysicalDevice pd) {
    {
        std::lock_guard lock(g_mu);
        auto it = g_needs.find(pd);
        if (it != g_needs.end())
            return it->second;
    }
    VkFormatProperties p{};
    Fn<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(
        pd, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &p);
    bool needs = !(p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    std::lock_guard lock(g_mu);
    if (!g_needs.count(pd) && needs)
        RN_INFO("Vulkan: host GPU has no ASTC; emulating it (RGBA8 images, host-side decode)");
    g_needs[pd] = needs;
    g_last_pd = pd;
    return needs;
}

VkPhysicalDevice DevicePd(VkDevice d) {
    std::lock_guard lock(g_mu);
    auto it = g_device_pd.find(d);
    return it != g_device_pd.end() ? it->second : g_last_pd;
}

bool DeviceNeedsEmulation(VkDevice d) {
    VkPhysicalDevice pd = DevicePd(d);
    return pd && NeedsEmulation(pd);
}

// --- tracked objects ------------------------------------------------------------------

struct EmuImage {
    VkDevice device;
    AstcFormat astc;
};
struct MemoryInfo {
    VkDevice device;
    uint8_t* base = nullptr;  // host pointer to offset 0 while mapped
};
struct BufferBinding {
    VkDeviceMemory memory;
    VkDeviceSize offset;
};
struct Chunk {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t* ptr;
    VkDeviceSize size, used;
};
struct CmdInfo {
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<Chunk> chunks;
};

std::unordered_map<VkImage, EmuImage> g_images;
std::unordered_map<VkDeviceMemory, MemoryInfo> g_memory;
std::unordered_map<VkBuffer, BufferBinding> g_buffers;
std::unordered_map<VkCommandBuffer, CmdInfo> g_cmds;

void FreeChunks(VkDevice device, std::vector<Chunk>& chunks) {
    if (chunks.empty())
        return;
    auto destroy = Fn<PFN_vkDestroyBuffer>("vkDestroyBuffer");
    auto free_mem = Fn<PFN_vkFreeMemory>("vkFreeMemory");
    for (auto& c : chunks) {
        destroy(device, c.buffer, nullptr);
        free_mem(device, c.memory, nullptr);  // implicitly unmaps
    }
    chunks.clear();
}

// Caller holds g_mu.
void ReleaseCmd(VkCommandBuffer cb, bool forget) {
    auto it = g_cmds.find(cb);
    if (it == g_cmds.end())
        return;
    FreeChunks(it->second.device, it->second.chunks);
    if (forget)
        g_cmds.erase(it);
}

// Host-visible staging space owned by a command buffer. Caller holds g_mu.
uint8_t* StagingAlloc(VkCommandBuffer cb, VkDevice device, VkDeviceSize size, VkBuffer* buffer, VkDeviceSize* offset) {
    CmdInfo& ci = g_cmds[cb];
    ci.device = device;
    if (!ci.chunks.empty()) {
        Chunk& c = ci.chunks.back();
        VkDeviceSize at = (c.used + 15) & ~VkDeviceSize(15);
        if (at + size <= c.size) {
            c.used = at + size;
            *buffer = c.buffer;
            *offset = at;
            return c.ptr + at;
        }
    }
    auto pd = g_device_pd.count(device) ? g_device_pd[device] : g_last_pd;
    VkDeviceSize chunk_size = std::max<VkDeviceSize>(size, 16u << 20);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = chunk_size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Chunk c{};
    if (Fn<PFN_vkCreateBuffer>("vkCreateBuffer")(device, &bi, nullptr, &c.buffer) != VK_SUCCESS)
        return nullptr;
    VkMemoryRequirements req;
    Fn<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, c.buffer, &req);
    VkPhysicalDeviceMemoryProperties mp;
    Fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(pd, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            type = i;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    void* p = nullptr;
    if (type == UINT32_MAX || Fn<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &ai, nullptr, &c.memory) != VK_SUCCESS) {
        Fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, c.buffer, nullptr);
        Log("ASTC emulation: cannot allocate %llu bytes of staging memory", static_cast<unsigned long long>(req.size));
        return nullptr;
    }
    Fn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, c.buffer, c.memory, 0);
    Fn<PFN_vkMapMemory>("vkMapMemory")(device, c.memory, 0, VK_WHOLE_SIZE, 0, &p);
    c.ptr = static_cast<uint8_t*>(p);
    c.size = chunk_size;
    c.used = size;
    ci.chunks.push_back(c);
    *buffer = c.buffer;
    *offset = 0;
    return c.ptr;
}

// --- decoding ------------------------------------------------------------------------

unsigned DecodeThreads() {
    static const unsigned n = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
    return n;
}

// Decodes one image, splitting large ones across DecodeThreads() threads (astcenc hands out
// blocks to every thread that calls in with its own index).
astcenc_error Decompress(astcenc_context* ctx, const uint8_t* data, size_t len, astcenc_image* im,
                         const astcenc_swizzle* swz) {
    const unsigned threads = size_t(im->dim_x) * im->dim_y >= 256 * 256 ? DecodeThreads() : 1;
    std::vector<astcenc_error> errors(threads, ASTCENC_SUCCESS);
    std::vector<std::thread> workers;
    for (unsigned i = 1; i < threads; ++i)
        workers.emplace_back([&, i] { errors[i] = astcenc_decompress_image(ctx, data, len, im, swz, i); });
    errors[0] = astcenc_decompress_image(ctx, data, len, im, swz, 0);
    for (auto& w : workers)
        w.join();
    astcenc_decompress_reset(ctx);
    for (astcenc_error e : errors)
        if (e != ASTCENC_SUCCESS)
            return e;
    return ASTCENC_SUCCESS;
}

astcenc_context* Context(const AstcFormat& f) {
    static std::map<uint32_t, astcenc_context*> contexts;  // under g_mu
    uint32_t key = f.bw << 16 | f.bh << 8 | (f.srgb ? 1 : 0);
    auto it = contexts.find(key);
    if (it != contexts.end())
        return it->second;
    astcenc_config cfg;
    astcenc_context* ctx = nullptr;
    astcenc_error e = astcenc_config_init(f.srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, f.bw, f.bh, 1,
                                          ASTCENC_PRE_FASTEST, ASTCENC_FLG_DECOMPRESS_ONLY, &cfg);
    if (e == ASTCENC_SUCCESS)
        e = astcenc_context_alloc(&cfg, DecodeThreads(), &ctx);
    if (e != ASTCENC_SUCCESS) {
        Log("ASTC emulation: context %ux%u: %s", f.bw, f.bh, astcenc_get_error_string(e));
        ctx = nullptr;
    }
    contexts[key] = ctx;
    return ctx;
}

// Source bytes for a buffer range, mapping the memory temporarily if the app has not.
struct SourceView {
    const uint8_t* ptr = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkDeviceMemory temp = VK_NULL_HANDLE;
    ~SourceView() {
        if (temp)
            Fn<PFN_vkUnmapMemory>("vkUnmapMemory")(device, temp);
    }
};

bool MapSource(VkBuffer buffer, SourceView* v) {
    auto b = g_buffers.find(buffer);
    if (b == g_buffers.end())
        return false;
    auto m = g_memory.find(b->second.memory);
    if (m == g_memory.end())
        return false;
    if (!m->second.base) {
        void* p = nullptr;
        if (Fn<PFN_vkMapMemory>("vkMapMemory")(m->second.device, b->second.memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS)
            return false;
        v->device = m->second.device;
        v->temp = b->second.memory;
        v->ptr = static_cast<const uint8_t*>(p) + b->second.offset;
        return true;
    }
    v->ptr = m->second.base + b->second.offset;
    return true;
}

// Decodes one copy region into staging; returns the rewritten region. Caller holds g_mu.
bool DecodeRegion(VkCommandBuffer cb, const EmuImage& img, const uint8_t* src, const VkBufferImageCopy& r,
                  VkBuffer* out_buffer, VkBufferImageCopy* out) {
    const AstcFormat& f = img.astc;
    uint32_t w = r.imageExtent.width, h = r.imageExtent.height, depth = std::max(1u, r.imageExtent.depth);
    uint32_t layers = std::max(1u, r.imageSubresource.layerCount);
    uint32_t row_texels = r.bufferRowLength ? r.bufferRowLength : w;
    uint32_t img_rows = r.bufferImageHeight ? r.bufferImageHeight : h;
    uint32_t bx = (w + f.bw - 1) / f.bw, by = (h + f.bh - 1) / f.bh;
    uint32_t row_blocks = (row_texels + f.bw - 1) / f.bw;
    size_t slice_bytes = size_t(row_blocks) * ((img_rows + f.bh - 1) / f.bh) * 16;
    size_t out_slice = size_t(w) * h * 4;
    VkDeviceSize out_offset;
    uint8_t* dst = StagingAlloc(cb, img.device, out_slice * depth * layers, out_buffer, &out_offset);
    astcenc_context* ctx = Context(f);
    if (!dst || !ctx)
        return false;
    std::vector<uint8_t> packed(size_t(bx) * by * 16);
    const astcenc_swizzle swz{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    for (uint32_t s = 0; s < depth * layers; ++s) {
        const uint8_t* slice = src + r.bufferOffset + s * slice_bytes;
        for (uint32_t y = 0; y < by; ++y)
            memcpy(&packed[size_t(y) * bx * 16], slice + size_t(y) * row_blocks * 16, size_t(bx) * 16);
        void* plane = dst + s * out_slice;
        astcenc_image im{w, h, 1, ASTCENC_TYPE_U8, &plane};
        astcenc_error e = Decompress(ctx, packed.data(), packed.size(), &im, &swz);
        if (e != ASTCENC_SUCCESS) {
            static int warned = 0;
            if (warned++ < 5)
                Log("ASTC emulation: decode %ux%u (%ux%u blocks): %s", w, h, f.bw, f.bh, astcenc_get_error_string(e));
            memset(plane, 0xff, out_slice);
        }
    }
    *out = r;
    out->bufferOffset = out_offset;
    out->bufferRowLength = 0;
    out->bufferImageHeight = 0;
    return true;
}

// --- intercepted commands -------------------------------------------------------------

void VKAPI_CALL E_vkGetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures* f) {
    Fn<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures")(pd, f);
    if (NeedsEmulation(pd))
        f->textureCompressionASTC_LDR = VK_TRUE;
}

void VKAPI_CALL E_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2* f) {
    Fn<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(pd, f);
    if (NeedsEmulation(pd))
        f->features.textureCompressionASTC_LDR = VK_TRUE;
}

void MaskFormatProperties(VkFormatProperties* p) {
    p->linearTilingFeatures = 0;
    p->optimalTilingFeatures &= kEmulatedFeatures;
    p->bufferFeatures = 0;
}

void VKAPI_CALL E_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice pd, VkFormat f, VkFormatProperties* p) {
    bool emu = AstcInfo(f, nullptr) && NeedsEmulation(pd);
    Fn<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(pd, emu ? Substitute(f) : f, p);
    if (emu)
        MaskFormatProperties(p);
}

void VKAPI_CALL E_vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice pd, VkFormat f, VkFormatProperties2* p) {
    bool emu = AstcInfo(f, nullptr) && NeedsEmulation(pd);
    Fn<PFN_vkGetPhysicalDeviceFormatProperties2>("vkGetPhysicalDeviceFormatProperties2")(pd, emu ? Substitute(f) : f, p);
    if (!emu)
        return;
    MaskFormatProperties(&p->formatProperties);
    for (auto* n = static_cast<VkBaseOutStructure*>(p->pNext); n; n = n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
            auto* p3 = reinterpret_cast<VkFormatProperties3*>(n);
            p3->linearTilingFeatures = 0;
            p3->optimalTilingFeatures &= kEmulatedFeatures;
            p3->bufferFeatures = 0;
        }
}

VkResult VKAPI_CALL E_vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice pd, VkFormat f, VkImageType type,
                                                               VkImageTiling tiling, VkImageUsageFlags usage,
                                                               VkImageCreateFlags flags, VkImageFormatProperties* p) {
    auto host = Fn<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties");
    if (!AstcInfo(f, nullptr) || !NeedsEmulation(pd))
        return host(pd, f, type, tiling, usage, flags, p);
    if ((usage & ~kEmulatedUsage) || tiling != VK_IMAGE_TILING_OPTIMAL)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    return host(pd, Substitute(f), type, tiling, usage, flags & ~VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT, p);
}

VkResult VKAPI_CALL E_vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd,
                                                                const VkPhysicalDeviceImageFormatInfo2* info,
                                                                VkImageFormatProperties2* p) {
    auto host = Fn<PFN_vkGetPhysicalDeviceImageFormatProperties2>("vkGetPhysicalDeviceImageFormatProperties2");
    if (!AstcInfo(info->format, nullptr) || !NeedsEmulation(pd))
        return host(pd, info, p);
    if ((info->usage & ~kEmulatedUsage) || info->tiling != VK_IMAGE_TILING_OPTIMAL)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkPhysicalDeviceImageFormatInfo2 i = *info;
    i.format = Substitute(info->format);
    i.flags &= ~VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
    return host(pd, &i, p);
}

VkResult VKAPI_CALL E_vkCreateImage(VkDevice device, const VkImageCreateInfo* ci, const VkAllocationCallbacks*,
                                    VkImage* out) {
    auto create = Fn<PFN_vkCreateImage>("vkCreateImage");
    AstcFormat astc;
    if (!AstcInfo(ci->format, &astc) || !DeviceNeedsEmulation(device))
        return create(device, ci, nullptr, out);
    VkImageCreateInfo c = *ci;
    c.format = Substitute(ci->format);
    c.flags &= ~VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
    // Rewrite a view format list in place for the call.
    VkImageFormatListCreateInfo* list = nullptr;
    const VkFormat* saved_formats = nullptr;
    std::vector<VkFormat> formats;
    for (auto* n = static_cast<const VkBaseInStructure*>(c.pNext); n; n = n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO)
            list = reinterpret_cast<VkImageFormatListCreateInfo*>(const_cast<VkBaseInStructure*>(n));
    if (list && list->viewFormatCount) {
        formats.assign(list->pViewFormats, list->pViewFormats + list->viewFormatCount);
        for (auto& f : formats)
            f = Substitute(f);
        saved_formats = list->pViewFormats;
        list->pViewFormats = formats.data();
    }
    VkResult r = create(device, &c, nullptr, out);
    if (list && saved_formats)
        list->pViewFormats = saved_formats;
    if (r == VK_SUCCESS) {
        std::lock_guard lock(g_mu);
        g_images[*out] = {device, astc};
    }
    return r;
}

void VKAPI_CALL E_vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks*) {
    {
        std::lock_guard lock(g_mu);
        g_images.erase(image);
    }
    Fn<PFN_vkDestroyImage>("vkDestroyImage")(device, image, nullptr);
}

VkResult VKAPI_CALL E_vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* ci,
                                        const VkAllocationCallbacks*, VkImageView* out) {
    auto create = Fn<PFN_vkCreateImageView>("vkCreateImageView");
    bool emu;
    {
        std::lock_guard lock(g_mu);
        emu = g_images.count(ci->image) != 0;
    }
    if (!emu)
        return create(device, ci, nullptr, out);
    VkImageViewCreateInfo c = *ci;
    c.format = Substitute(ci->format);
    return create(device, &c, nullptr, out);
}

VkResult VKAPI_CALL E_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
                                  VkMemoryMapFlags flags, void** data) {
    VkResult r = Fn<PFN_vkMapMemory>("vkMapMemory")(device, memory, offset, size, flags, data);
    if (r == VK_SUCCESS) {
        std::lock_guard lock(g_mu);
        g_memory[memory] = {device, static_cast<uint8_t*>(*data) - offset};
    }
    return r;
}

VkResult VKAPI_CALL E_vkMapMemory2(VkDevice device, const VkMemoryMapInfo* info, void** data) {
    VkResult r = Fn<PFN_vkMapMemory2>("vkMapMemory2")(device, info, data);
    if (r == VK_SUCCESS) {
        std::lock_guard lock(g_mu);
        g_memory[info->memory] = {device, static_cast<uint8_t*>(*data) - info->offset};
    }
    return r;
}

VkResult VKAPI_CALL E_vkMapMemory2KHR(VkDevice device, const VkMemoryMapInfo* info, void** data) {
    VkResult r = Fn<PFN_vkMapMemory2>("vkMapMemory2KHR")(device, info, data);
    if (r == VK_SUCCESS) {
        std::lock_guard lock(g_mu);
        g_memory[info->memory] = {device, static_cast<uint8_t*>(*data) - info->offset};
    }
    return r;
}

void VKAPI_CALL E_vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    {
        std::lock_guard lock(g_mu);
        g_memory[memory] = {device, nullptr};
    }
    Fn<PFN_vkUnmapMemory>("vkUnmapMemory")(device, memory);
}

VkResult VKAPI_CALL E_vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks*,
                                       VkDeviceMemory* out) {
    VkResult r = Fn<PFN_vkAllocateMemory>("vkAllocateMemory")(device, ai, nullptr, out);
    if (r == VK_SUCCESS) {
        std::lock_guard lock(g_mu);
        g_memory[*out] = {device, nullptr};
    }
    return r;
}

void VKAPI_CALL E_vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    {
        std::lock_guard lock(g_mu);
        g_memory.erase(memory);
    }
    Fn<PFN_vkFreeMemory>("vkFreeMemory")(device, memory, nullptr);
}

VkResult VKAPI_CALL E_vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                                         VkDeviceSize offset) {
    VkResult r = Fn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, buffer, memory, offset);
    if (r == VK_SUCCESS) {
        std::lock_guard lock(g_mu);
        g_buffers[buffer] = {memory, offset};
    }
    return r;
}

VkResult BindBufferMemory2(const char* name, VkDevice device, uint32_t n, const VkBindBufferMemoryInfo* infos) {
    VkResult r = Fn<PFN_vkBindBufferMemory2>(name)(device, n, infos);
    if (r == VK_SUCCESS) {
        std::lock_guard lock(g_mu);
        for (uint32_t i = 0; i < n; ++i)
            g_buffers[infos[i].buffer] = {infos[i].memory, infos[i].memoryOffset};
    }
    return r;
}
VkResult VKAPI_CALL E_vkBindBufferMemory2(VkDevice d, uint32_t n, const VkBindBufferMemoryInfo* infos) {
    return BindBufferMemory2("vkBindBufferMemory2", d, n, infos);
}
VkResult VKAPI_CALL E_vkBindBufferMemory2KHR(VkDevice d, uint32_t n, const VkBindBufferMemoryInfo* infos) {
    return BindBufferMemory2("vkBindBufferMemory2KHR", d, n, infos);
}

void VKAPI_CALL E_vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks*) {
    {
        std::lock_guard lock(g_mu);
        g_buffers.erase(buffer);
    }
    Fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, buffer, nullptr);
}

VkResult VKAPI_CALL E_vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* ai,
                                               VkCommandBuffer* out) {
    VkResult r = Fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, ai, out);
    if (r == VK_SUCCESS && DeviceNeedsEmulation(device)) {
        std::lock_guard lock(g_mu);
        for (uint32_t i = 0; i < ai->commandBufferCount; ++i)
            g_cmds[out[i]] = {device, ai->commandPool, {}};
    }
    return r;
}

void VKAPI_CALL E_vkFreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t n, const VkCommandBuffer* cbs) {
    {
        std::lock_guard lock(g_mu);
        for (uint32_t i = 0; i < n; ++i)
            ReleaseCmd(cbs[i], true);
    }
    Fn<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(device, pool, n, cbs);
}

VkResult VKAPI_CALL E_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    {
        std::lock_guard lock(g_mu);
        ReleaseCmd(cb, false);
    }
    return Fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(cb, bi);
}

VkResult VKAPI_CALL E_vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
    {
        std::lock_guard lock(g_mu);
        ReleaseCmd(cb, false);
    }
    return Fn<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(cb, flags);
}

void ReleasePool(VkCommandPool pool, bool forget) {
    std::lock_guard lock(g_mu);
    for (auto it = g_cmds.begin(); it != g_cmds.end();) {
        if (it->second.pool == pool) {
            FreeChunks(it->second.device, it->second.chunks);
            if (forget) {
                it = g_cmds.erase(it);
                continue;
            }
        }
        ++it;
    }
}

VkResult VKAPI_CALL E_vkResetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    ReleasePool(pool, false);
    return Fn<PFN_vkResetCommandPool>("vkResetCommandPool")(device, pool, flags);
}

void VKAPI_CALL E_vkDestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks*) {
    ReleasePool(pool, true);
    Fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device, pool, nullptr);
}

std::atomic<uint64_t> g_decoded_texels{0};

void NoteDecoded(const std::vector<VkBufferImageCopy>& rs) {
    uint64_t n = 0;
    for (auto& r : rs)
        n += uint64_t(r.imageExtent.width) * r.imageExtent.height * std::max(1u, r.imageExtent.depth) *
             std::max(1u, r.imageSubresource.layerCount);
    uint64_t before = g_decoded_texels.fetch_add(n);
    if (before == 0 || before >> 26 != (before + n) >> 26)
        RN_INFO("ASTC emulation: %llu Mtexels decoded so far", static_cast<unsigned long long>((before + n) >> 20));
}

void VKAPI_CALL E_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout layout,
                                         uint32_t n, const VkBufferImageCopy* regions) {
    auto host = Fn<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
    std::vector<VkBufferImageCopy> out;
    VkBuffer staging = VK_NULL_HANDLE;
    {
        std::lock_guard lock(g_mu);
        auto img = g_images.find(dst);
        if (img == g_images.end()) {
            host(cb, src, dst, layout, n, regions);
            return;
        }
        SourceView view;
        if (!MapSource(src, &view)) {
            Log("ASTC emulation: copy source buffer is not host-visible; texture left undefined");
            return;
        }
        // Each region may land in a different chunk; issue one copy per chunk run.
        for (uint32_t i = 0; i < n; ++i) {
            VkBuffer b;
            VkBufferImageCopy r;
            if (!DecodeRegion(cb, img->second, view.ptr, regions[i], &b, &r))
                continue;
            if (b != staging && !out.empty()) {
                host(cb, staging, dst, layout, static_cast<uint32_t>(out.size()), out.data());
                NoteDecoded(out);
                out.clear();
            }
            staging = b;
            out.push_back(r);
        }
    }
    if (!out.empty()) {
        host(cb, staging, dst, layout, static_cast<uint32_t>(out.size()), out.data());
        NoteDecoded(out);
    }
}

void CopyBufferToImage2(const char* name, VkCommandBuffer cb, const VkCopyBufferToImageInfo2* info) {
    bool emu;
    {
        std::lock_guard lock(g_mu);
        emu = g_images.count(info->dstImage) != 0;
    }
    if (!emu) {
        Fn<PFN_vkCmdCopyBufferToImage2>(name)(cb, info);
        return;
    }
    std::vector<VkBufferImageCopy> rs(info->regionCount);
    for (uint32_t i = 0; i < info->regionCount; ++i) {
        const VkBufferImageCopy2& r = info->pRegions[i];
        rs[i] = {r.bufferOffset, r.bufferRowLength, r.bufferImageHeight, r.imageSubresource, r.imageOffset,
                 r.imageExtent};
    }
    E_vkCmdCopyBufferToImage(cb, info->srcBuffer, info->dstImage, info->dstImageLayout, info->regionCount, rs.data());
}
void VKAPI_CALL E_vkCmdCopyBufferToImage2(VkCommandBuffer cb, const VkCopyBufferToImageInfo2* info) {
    CopyBufferToImage2("vkCmdCopyBufferToImage2", cb, info);
}
void VKAPI_CALL E_vkCmdCopyBufferToImage2KHR(VkCommandBuffer cb, const VkCopyBufferToImageInfo2* info) {
    CopyBufferToImage2("vkCmdCopyBufferToImage2KHR", cb, info);
}

}  // namespace

void NoteVulkanDevice(void* device, void* pd) {
    std::lock_guard lock(g_mu);
    g_device_pd[static_cast<VkDevice>(device)] = static_cast<VkPhysicalDevice>(pd);
}

// Emulated features must not reach the host's vkCreateDevice. Clears them in a
// copy of pEnabledFeatures and, for a VkPhysicalDeviceFeatures2 chain entry, in
// place (restored by the returned object's destructor).
DeviceFeatureFix::DeviceFeatureFix(void* create_info) {
    auto* ci = static_cast<VkDeviceCreateInfo*>(create_info);
    if (ci->pEnabledFeatures && ci->pEnabledFeatures->textureCompressionASTC_LDR) {
        auto* copy = new VkPhysicalDeviceFeatures(*ci->pEnabledFeatures);
        copy->textureCompressionASTC_LDR = VK_FALSE;
        owned_ = copy;
        ci->pEnabledFeatures = copy;
    }
    for (auto* n = static_cast<const VkBaseInStructure*>(ci->pNext); n; n = n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            auto* f2 = reinterpret_cast<VkPhysicalDeviceFeatures2*>(const_cast<VkBaseInStructure*>(n));
            if (f2->features.textureCompressionASTC_LDR) {
                f2->features.textureCompressionASTC_LDR = VK_FALSE;
                patched_ = &f2->features.textureCompressionASTC_LDR;
            }
        }
}

DeviceFeatureFix::~DeviceFeatureFix() {
    if (patched_)
        *static_cast<VkBool32*>(patched_) = VK_TRUE;
    delete static_cast<VkPhysicalDeviceFeatures*>(owned_);
}

void AddTextureEmulationThunks(std::map<std::string, ThunkFn>& m) {
    m["vkGetPhysicalDeviceFeatures"] = &Wrap<&E_vkGetPhysicalDeviceFeatures>;
    m["vkGetPhysicalDeviceFeatures2"] = &Wrap<&E_vkGetPhysicalDeviceFeatures2>;
    m["vkGetPhysicalDeviceFeatures2KHR"] = &Wrap<&E_vkGetPhysicalDeviceFeatures2>;
    m["vkGetPhysicalDeviceFormatProperties"] = &Wrap<&E_vkGetPhysicalDeviceFormatProperties>;
    m["vkGetPhysicalDeviceFormatProperties2"] = &Wrap<&E_vkGetPhysicalDeviceFormatProperties2>;
    m["vkGetPhysicalDeviceFormatProperties2KHR"] = &Wrap<&E_vkGetPhysicalDeviceFormatProperties2>;
    m["vkGetPhysicalDeviceImageFormatProperties"] = &Wrap<&E_vkGetPhysicalDeviceImageFormatProperties>;
    m["vkGetPhysicalDeviceImageFormatProperties2"] = &Wrap<&E_vkGetPhysicalDeviceImageFormatProperties2>;
    m["vkGetPhysicalDeviceImageFormatProperties2KHR"] = &Wrap<&E_vkGetPhysicalDeviceImageFormatProperties2>;
    m["vkCreateImage"] = &Wrap<&E_vkCreateImage>;
    m["vkDestroyImage"] = &Wrap<&E_vkDestroyImage>;
    m["vkCreateImageView"] = &Wrap<&E_vkCreateImageView>;
    m["vkAllocateMemory"] = &Wrap<&E_vkAllocateMemory>;
    m["vkFreeMemory"] = &Wrap<&E_vkFreeMemory>;
    m["vkMapMemory"] = &Wrap<&E_vkMapMemory>;
    m["vkMapMemory2"] = &Wrap<&E_vkMapMemory2>;
    m["vkMapMemory2KHR"] = &Wrap<&E_vkMapMemory2KHR>;
    m["vkUnmapMemory"] = &Wrap<&E_vkUnmapMemory>;
    m["vkBindBufferMemory"] = &Wrap<&E_vkBindBufferMemory>;
    m["vkBindBufferMemory2"] = &Wrap<&E_vkBindBufferMemory2>;
    m["vkBindBufferMemory2KHR"] = &Wrap<&E_vkBindBufferMemory2KHR>;
    m["vkDestroyBuffer"] = &Wrap<&E_vkDestroyBuffer>;
    m["vkAllocateCommandBuffers"] = &Wrap<&E_vkAllocateCommandBuffers>;
    m["vkFreeCommandBuffers"] = &Wrap<&E_vkFreeCommandBuffers>;
    m["vkBeginCommandBuffer"] = &Wrap<&E_vkBeginCommandBuffer>;
    m["vkResetCommandBuffer"] = &Wrap<&E_vkResetCommandBuffer>;
    m["vkResetCommandPool"] = &Wrap<&E_vkResetCommandPool>;
    m["vkDestroyCommandPool"] = &Wrap<&E_vkDestroyCommandPool>;
    m["vkCmdCopyBufferToImage"] = &Wrap<&E_vkCmdCopyBufferToImage>;
    m["vkCmdCopyBufferToImage2"] = &Wrap<&E_vkCmdCopyBufferToImage2>;
    m["vkCmdCopyBufferToImage2KHR"] = &Wrap<&E_vkCmdCopyBufferToImage2KHR>;
}

}  // namespace rn
