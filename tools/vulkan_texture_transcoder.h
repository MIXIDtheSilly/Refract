// Per-device ETC2/EAC/ASTC -> BC texture transcoding for the Android Vulkan layer (debug.refract.transcode_textures=1,
// ASTC unless debug.refract.transcode_astc=0). Images the game creates as ETC2/EAC/ASTC are created as a BC format
// instead (ETC2/EAC: the matching BC1/3/4/5, same size; ASTC: BC3). gfxstream passes BC straight to the host GPU, so
// the texture costs its native size instead of a decompressed RGBA copy plus the original. Uploads
// (vkCmdCopyBufferToImage) are redirected to layer-owned staging memory holding the transcoded blocks, filled at
// record time from the game's mapped staging buffer. That staging is recycled when the command buffer is begun,
// reset or freed (the GPU is done with it by then). Anything unexpected is logged and passed through unchanged.
// ASTC uploads into primary command buffers are transcoded on the GPU (astc_bc3_spirv.h) unless
// debug.refract.transcode_gpu=0: the blocks are copied into layer memory and a compute dispatch is recorded into a
// layer command buffer that vkQueueSubmit puts in front of the game's, so the recording thread never waits for it.
#pragma once
#include "astc_bc3_spirv.h"
#include "astc_decode.h"
#include "texture_transcode.h"
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace refract {

// Splits large uploads across a few threads; the caller works too. One job runs at a time.
class TranscodeWorkers {
public:
    static TranscodeWorkers& get() { static TranscodeWorkers workers; return workers; }
    void run(size_t count, const std::function<void(size_t)>& fn) {
        if (count < 2 || threads_.empty()) { for (size_t i = 0; i < count; ++i) fn(i); return; }
        std::lock_guard caller(callers_);
        {
            std::lock_guard lock(mutex_);
            job_ = &fn; count_ = count; next_ = 0; finished_ = 0; ++generation_;
        }
        wake_.notify_all();
        for (size_t i; (i = next_.fetch_add(1)) < count;) fn(i);
        // Every worker checks in for this generation, so none can still hold `fn` after we return.
        std::unique_lock lock(mutex_);
        done_.wait(lock, [&] { return finished_ == threads_.size(); });
        job_ = nullptr;
    }
private:
    TranscodeWorkers() {
        const unsigned hardware = std::thread::hardware_concurrency();
        const unsigned count = hardware > 2 ? std::min(4u, hardware - 1) : 0;
        for (unsigned i = 0; i < count; ++i) threads_.emplace_back([this] { work(); });
        for (auto& t : threads_) t.detach();
    }
    void work() {
        uint64_t seen = 0;
        for (;;) {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return generation_ != seen; });
            seen = generation_;
            const auto* job = job_;
            const size_t count = count_;
            lock.unlock();
            for (size_t i; (i = next_.fetch_add(1)) < count;) (*job)(i);
            lock.lock();
            if (++finished_ == threads_.size()) done_.notify_all();
        }
    }
    std::mutex callers_, mutex_;
    std::condition_variable wake_, done_;
    std::vector<std::thread> threads_;
    const std::function<void(size_t)>* job_ = nullptr;
    size_t count_ = 0, finished_ = 0;
    std::atomic<size_t> next_{0};
    uint64_t generation_ = 0;
};

class TextureTranscoder {
public:
    // What a source format becomes: BC format, codec, and for ASTC its block footprint.
    struct Target { VkFormat bc = VK_FORMAT_UNDEFINED; texture::Codec codec = texture::Codec::None; uint8_t bw = 4, bh = 4; bool srgb = false; };
    static Target target(VkFormat format) {
        using texture::Codec;
        Target t;
        if (format >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && format <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK) {
            struct Entry { VkFormat bc; Codec codec; };
            static constexpr Entry table[] = {
                {VK_FORMAT_BC1_RGB_UNORM_BLOCK, Codec::Etc2Rgb}, {VK_FORMAT_BC1_RGB_SRGB_BLOCK, Codec::Etc2Rgb},
                {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, Codec::Etc2RgbA1}, {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, Codec::Etc2RgbA1},
                {VK_FORMAT_BC3_UNORM_BLOCK, Codec::Etc2Rgba}, {VK_FORMAT_BC3_SRGB_BLOCK, Codec::Etc2Rgba},
                {VK_FORMAT_BC4_UNORM_BLOCK, Codec::EacR}, {VK_FORMAT_BC4_SNORM_BLOCK, Codec::EacRSigned},
                {VK_FORMAT_BC5_UNORM_BLOCK, Codec::EacRg}, {VK_FORMAT_BC5_SNORM_BLOCK, Codec::EacRgSigned}};
            const auto& e = table[format - VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK];
            t.bc = e.bc; t.codec = e.codec;
        } else if (format >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) {
            // ASTC LDR: decoded and re-encoded as BC3 (8 bits/texel, alpha kept).
            static constexpr uint8_t dims[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8},
                                                    {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
            const int index = format - VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
            t.srgb = index & 1;
            t.bc = t.srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
            t.codec = Codec::Astc; t.bw = dims[index / 2][0]; t.bh = dims[index / 2][1];
        }
        return t;
    }

    using FormatSupported = std::function<bool(VkFormat)>;
    // GPU transcoding needs the queue families with compute (bit mask), the loader's dispatch setter for layer-made
    // command buffers, and the largest storage buffer range.
    struct GpuSetup {
        bool enabled = false; uint32_t computeFamilies = 0;
        PFN_vkSetDeviceLoaderData setLoaderData = nullptr; VkDeviceSize maxStorageRange = 0;
    };
    TextureTranscoder(VkDevice device, PFN_vkGetDeviceProcAddr gdpa, const VkPhysicalDeviceMemoryProperties& memory,
                      const FormatSupported& supported, bool astc, const GpuSetup& gpu)
        : device_(device), memory_(memory), astc_(astc), gpuSetup_(gpu) {
        auto load = [&](auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(gdpa(device, name)); };
        load(createBuffer_, "vkCreateBuffer"); load(destroyBuffer_, "vkDestroyBuffer");
        load(bufferRequirements_, "vkGetBufferMemoryRequirements"); load(allocate_, "vkAllocateMemory");
        load(free_, "vkFreeMemory"); load(bindBuffer_, "vkBindBufferMemory");
        load(map_, "vkMapMemory"); load(unmap_, "vkUnmapMemory");
        for (int f = VK_FORMAT_BC1_RGB_UNORM_BLOCK; f <= VK_FORMAT_BC7_SRGB_BLOCK; ++f)
            bcSupported_[f - VK_FORMAT_BC1_RGB_UNORM_BLOCK] = supported(static_cast<VkFormat>(f));
        if (astc_ && gpu.enabled && gpu.computeFamilies) gpuReady_ = createGpu(gdpa);
        __android_log_print(ANDROID_LOG_INFO, "Refract.Transcode", "ASTC transcoding on the %s", gpuReady_ ? "GPU" : "CPU");
    }
    ~TextureTranscoder() {
        for (auto& c : chunks_) destroyChunk(*c);
        if (gpuReady_) {
            for (auto& entry : ourPools_) destroyCommandPool_(device_, entry.second.pool, nullptr);
            destroyDescriptorPool_(device_, descriptorPool_, nullptr);
            destroyPipeline_(device_, pipeline_, nullptr);
            destroyPipelineLayout_(device_, pipelineLayout_, nullptr);
            destroySetLayout_(device_, setLayout_, nullptr);
        }
    }

    // vkCreateImage: switches a transcodable ETC2/EAC/ASTC image to its BC format. Returns the codec (None = untouched).
    texture::Codec adjust(VkImageCreateInfo& info) const {
        const Target t = target(info.format);
        if (t.bc == VK_FORMAT_UNDEFINED || !bcSupported_[t.bc - VK_FORMAT_BC1_RGB_UNORM_BLOCK]) return texture::Codec::None;
        if (t.codec == texture::Codec::Astc && (!astc_ || info.imageType != VK_IMAGE_TYPE_2D)) return texture::Codec::None;
        constexpr VkImageCreateFlags unsupported = VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT |
            VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_ALIAS_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        if (info.tiling != VK_IMAGE_TILING_OPTIMAL || (info.flags & unsupported)) return texture::Codec::None;
        for (auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next; next = next->pNext)
            if (next->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO ||
                next->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) return texture::Codec::None;
        info.format = t.bc;
        return t.codec;
    }
    // Called with the original create info after a successful adjust().
    void addImage(VkImage image, const VkImageCreateInfo& original) {
        std::lock_guard lock(mutex_); images_[image] = {target(original.format), original.extent};
    }
    void removeImage(VkImage image) { std::lock_guard lock(mutex_); images_.erase(image); }
    bool hasImage(VkImage image) const { std::shared_lock lock(mutex_); return images_.count(image) != 0; }
    // vkCreateImageView on a transcoded image: ETC2/EAC/ASTC view formats become their BC counterparts.
    VkFormat viewFormat(VkImage image, VkFormat format) const {
        const VkFormat bc = target(format).bc;
        return bc == VK_FORMAT_UNDEFINED || !hasImage(image) ? format : bc;
    }

    // Buffer/memory bookkeeping for reading the game's staging data.
    void addBuffer(VkBuffer buffer, VkDeviceSize size) { std::lock_guard lock(mutex_); buffers_[buffer] = {size}; }
    void bindBuffer(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
        std::lock_guard lock(mutex_); auto it = buffers_.find(buffer);
        if (it != buffers_.end()) { it->second.memory = memory; it->second.offset = offset; }
    }
    void removeBuffer(VkBuffer buffer) { std::lock_guard lock(mutex_); buffers_.erase(buffer); }
    void addMemory(VkDeviceMemory memory, VkDeviceSize size, uint32_t type) { std::lock_guard lock(mutex_); memories_[memory] = {size, type}; }
    void mapMemory(VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, void* data) {
        std::lock_guard lock(mutex_); auto it = memories_.find(memory);
        if (it == memories_.end()) return;
        it->second.mapOffset = offset;
        it->second.mapSize = size == VK_WHOLE_SIZE ? it->second.size - offset : size;
        it->second.mapped = static_cast<uint8_t*>(data);
    }
    void unmapMemory(VkDeviceMemory memory) {
        std::lock_guard lock(mutex_); auto it = memories_.find(memory);
        if (it != memories_.end()) it->second.mapped = nullptr;
    }
    void removeMemory(VkDeviceMemory memory) { std::lock_guard lock(mutex_); memories_.erase(memory); }

    // Command buffer lifetime: staging referenced by a command buffer is recycled when it is begun/reset/freed.
    void createdPool(VkCommandPool pool, uint32_t family) { std::lock_guard lock(stagingMutex_); poolFamilies_[pool] = family; }
    void allocated(VkCommandPool pool, uint32_t count, const VkCommandBuffer* buffers, bool primary = true) {
        std::lock_guard lock(stagingMutex_);
        for (uint32_t i = 0; i < count; ++i) pools_[buffers[i]] = {pool, primary};
    }
    void reset(VkCommandBuffer cmd) {
        if (!holders_.load(std::memory_order_relaxed)) return;
        std::lock_guard lock(stagingMutex_); releaseLocked(cmd);
    }
    void freed(uint32_t count, const VkCommandBuffer* buffers) {
        std::lock_guard lock(stagingMutex_);
        for (uint32_t i = 0; i < count; ++i) { releaseLocked(buffers[i]); pools_.erase(buffers[i]); }
    }
    void poolReset(VkCommandPool pool, bool destroyed) {
        std::lock_guard lock(stagingMutex_);
        for (auto it = pools_.begin(); it != pools_.end();) {
            if (it->second.pool != pool) { ++it; continue; }
            releaseLocked(it->first);
            if (destroyed) it = pools_.erase(it); else ++it;
        }
        if (destroyed) poolFamilies_.erase(pool);
    }

    // vkQueueSubmit(2): puts the layer's transcode command buffers in front of the game command buffers that need
    // them, in the same batch (so the batch's fence covers them). Returns false when nothing changes.
    bool prepareSubmit(uint32_t count, const VkSubmitInfo* submits, std::vector<VkSubmitInfo>& out,
                       std::vector<std::vector<VkCommandBuffer>>& lists) {
        if (!recording_.load(std::memory_order_acquire)) return false;
        std::lock_guard lock(gpuMutex_);
        bool changed = false;
        out.assign(submits, submits + count);
        lists.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            lists[i].clear();
            for (uint32_t k = 0; k < submits[i].commandBufferCount; ++k)
                if (VkCommandBuffer ours = takeRecordedLocked(submits[i].pCommandBuffers[k])) lists[i].push_back(ours);
            if (lists[i].empty()) continue;
            lists[i].insert(lists[i].end(), submits[i].pCommandBuffers, submits[i].pCommandBuffers + submits[i].commandBufferCount);
            out[i].commandBufferCount = static_cast<uint32_t>(lists[i].size()); out[i].pCommandBuffers = lists[i].data();
            changed = true;
        }
        return changed;
    }
    bool prepareSubmit2(uint32_t count, const VkSubmitInfo2* submits, std::vector<VkSubmitInfo2>& out,
                        std::vector<std::vector<VkCommandBufferSubmitInfo>>& lists) {
        if (!recording_.load(std::memory_order_acquire)) return false;
        std::lock_guard lock(gpuMutex_);
        bool changed = false;
        out.assign(submits, submits + count);
        lists.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            lists[i].clear();
            for (uint32_t k = 0; k < submits[i].commandBufferInfoCount; ++k)
                if (VkCommandBuffer ours = takeRecordedLocked(submits[i].pCommandBufferInfos[k].commandBuffer)) {
                    VkCommandBufferSubmitInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
                    info.commandBuffer = ours; info.deviceMask = submits[i].pCommandBufferInfos[k].deviceMask;
                    lists[i].push_back(info);
                }
            if (lists[i].empty()) continue;
            lists[i].insert(lists[i].end(), submits[i].pCommandBufferInfos, submits[i].pCommandBufferInfos + submits[i].commandBufferInfoCount);
            out[i].commandBufferInfoCount = static_cast<uint32_t>(lists[i].size()); out[i].pCommandBufferInfos = lists[i].data();
            changed = true;
        }
        return changed;
    }

    // vkCmdCopyBufferToImage(2): when `image` is transcoded, fills `out` with regions that read transcoded blocks
    // from layer staging (`staging`). Returns false to pass the original call through.
    template <class Region>
    bool redirect(VkCommandBuffer cmd, VkBuffer source, VkImage image, uint32_t count, const Region* regions,
                  std::vector<Region>& out, VkBuffer& staging) {
        Image info;
        Buffer buffer;
        Memory memory;
        {
            std::shared_lock lock(mutex_);
            auto imageIt = images_.find(image);
            if (imageIt == images_.end()) return false;
            info = imageIt->second;
            auto bufferIt = buffers_.find(source);
            auto memoryIt = bufferIt == buffers_.end() ? memories_.end() : memories_.find(bufferIt->second.memory);
            if (memoryIt == memories_.end()) return fail("upload source buffer is not tracked");
            buffer = bufferIt->second;
            memory = memoryIt->second;
        }
        const auto start = std::chrono::steady_clock::now();
        const Target& t = info.target;
        const bool astc = t.codec == texture::Codec::Astc;
        const uint32_t sourceBlockBytes = astc ? 16 : texture::block_bytes(t.codec);
        // Source layout (in the game's buffer) and destination layout (tightly packed BC blocks for ASTC, the
        // unchanged layout for ETC2/EAC, whose blocks keep their size).
        struct Plan {
            VkDeviceSize start, bytes, rowBlocks, sliceBlocks, blocksX, blocksY, slices;  // source
            VkDeviceSize outBytes, outBlocksX, outBlocksY;                                 // destination (ASTC)
        };
        std::vector<Plan> plans(count);
        VkDeviceSize total = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& r = regions[i];
            Plan& p = plans[i];
            const VkDeviceSize width = r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width;
            const VkDeviceSize height = r.bufferImageHeight ? r.bufferImageHeight : r.imageExtent.height;
            p.blocksX = (r.imageExtent.width + t.bw - 1) / t.bw; p.blocksY = (r.imageExtent.height + t.bh - 1) / t.bh;
            p.rowBlocks = (width + t.bw - 1) / t.bw; p.sliceBlocks = p.rowBlocks * ((height + t.bh - 1) / t.bh);
            p.slices = static_cast<VkDeviceSize>(r.imageSubresource.layerCount) * std::max(1u, r.imageExtent.depth);
            p.start = r.bufferOffset;
            p.bytes = p.slices && p.blocksY ? ((p.slices - 1) * p.sliceBlocks + (p.blocksY - 1) * p.rowBlocks + p.blocksX) * sourceBlockBytes : 0;
            const VkDeviceSize first = buffer.offset + p.start;
            if (p.start + p.bytes > buffer.size) return fail("upload region outside the source buffer");
            if (memory.mapped && (first < memory.mapOffset || first + p.bytes > memory.mapOffset + memory.mapSize))
                return fail("upload region outside the mapped source");
            if (astc) {
                // BC's 4x4 grid must line up: offsets on it, extents on it or reaching the mip edge.
                const uint32_t mipWidth = std::max(1u, info.extent.width >> r.imageSubresource.mipLevel);
                const uint32_t mipHeight = std::max(1u, info.extent.height >> r.imageSubresource.mipLevel);
                if ((r.imageOffset.x & 3) || (r.imageOffset.y & 3) ||
                    ((r.imageExtent.width & 3) && r.imageOffset.x + r.imageExtent.width != mipWidth) ||
                    ((r.imageExtent.height & 3) && r.imageOffset.y + r.imageExtent.height != mipHeight))
                    return fail("ASTC upload region not on the 4x4 grid");
                p.outBlocksX = (r.imageExtent.width + 3) / 4; p.outBlocksY = (r.imageExtent.height + 3) / 4;
                p.outBytes = p.slices * p.outBlocksX * p.outBlocksY * 16;
            } else {
                p.outBytes = p.bytes;
            }
            total += (p.outBytes + 15) & ~VkDeviceSize(15);
        }
        const uint8_t* base = memory.mapped ? memory.mapped - memory.mapOffset : nullptr;
        void* temporary = nullptr;
        const int gpuFamily = astc && gpuReady_ ? usableFamily(cmd, total, plans, count) : -1;
        if (!base) {  // Not mapped by the game right now: map it ourselves if the memory type allows.
            if (!(memory_.memoryTypes[memory.type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                return fail("upload source is not host-visible");
            if (map_(device_, buffer.memory, 0, VK_WHOLE_SIZE, 0, &temporary) != VK_SUCCESS) return fail("cannot map upload source");
            base = static_cast<const uint8_t*>(temporary);
        }
        if (gpuFamily >= 0) {
            uint64_t texels = 0;
            for (uint32_t i = 0; i < count; ++i)
                texels += static_cast<uint64_t>(plans[i].slices) * regions[i].imageExtent.width * regions[i].imageExtent.height;
            if (redirectGpu(cmd, static_cast<uint32_t>(gpuFamily), t, count, regions, plans, base + buffer.offset, total, out, staging)) {
                if (temporary) unmap_(device_, buffer.memory);
                report(texels, true, std::chrono::steady_clock::now() - start, true);
                return true;
            }
        }
        VkDeviceSize offset = 0;
        Chunk* chunk = allocate(cmd, total, offset);
        if (!chunk) { if (temporary) unmap_(device_, buffer.memory); return fail("staging allocation failed"); }
        out.assign(regions, regions + count);
        uint64_t texels = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const Plan& p = plans[i];
            const uint8_t* from = base + buffer.offset + p.start;
            uint8_t* to = chunk->data + offset;
            out[i].bufferOffset = offset;
            offset += (p.outBytes + 15) & ~VkDeviceSize(15);
            texels += static_cast<uint64_t>(p.slices) * regions[i].imageExtent.width * regions[i].imageExtent.height;
            if (astc) {
                out[i].bufferRowLength = 0; out[i].bufferImageHeight = 0;
                transcodeAstc(t, p.blocksX, p.blocksY, p.rowBlocks, p.sliceBlocks, p.slices, p.outBlocksX, p.outBlocksY,
                              regions[i].imageExtent.width, regions[i].imageExtent.height, from, to);
                continue;
            }
            auto row = [&, from, to](size_t r) {
                const VkDeviceSize slice = r / p.blocksY, y = r % p.blocksY;
                const VkDeviceSize at = (slice * p.sliceBlocks + y * p.rowBlocks) * sourceBlockBytes;
                for (VkDeviceSize x = 0; x < p.blocksX; ++x)
                    texture::transcode_block(t.codec, from + at + x * sourceBlockBytes, to + at + x * sourceBlockBytes);
            };
            const size_t rows = p.slices * p.blocksY;
            if (rows * p.blocksX >= 4096) TranscodeWorkers::get().run(rows, row);
            else for (size_t r = 0; r < rows; ++r) row(r);
        }
        if (temporary) unmap_(device_, buffer.memory);
        staging = chunk->buffer;
        report(texels, astc, std::chrono::steady_clock::now() - start, false);
        return true;
    }

    // Copies between a transcoded and a plain image would reinterpret blocks; only logged.
    void checkImageCopy(VkImage source, VkImage destination) const {
        if (hasImage(source) != hasImage(destination)) fail("image copy between transcoded and plain images");
    }
    void checkReadback(VkImage source) const { if (hasImage(source)) fail("readback of a transcoded image"); }

private:
    struct Image { Target target; VkExtent3D extent{}; };
    struct Buffer { VkDeviceSize size = 0; VkDeviceMemory memory = VK_NULL_HANDLE; VkDeviceSize offset = 0; };
    struct Memory { VkDeviceSize size = 0; uint32_t type = 0; const uint8_t* mapped = nullptr; VkDeviceSize mapOffset = 0, mapSize = 0; };
    // Staging: host-visible chunks (CPU-transcoded blocks, GPU transcode input) and device-local chunks (GPU output).
    struct Chunk { VkBuffer buffer; VkDeviceMemory memory; uint8_t* data; VkDeviceSize size, used = 0; uint32_t refs = 0; bool device = false; };
    static constexpr VkDeviceSize kChunkBytes = 16u << 20;
    struct Pool { VkCommandPool pool; bool primary; };
    // A layer command buffer holding GPU transcodes for one game command buffer: recording until the game command
    // buffer is submitted, then kept until the game resets or frees it (which means it has finished).
    struct Work { VkCommandBuffer ours = VK_NULL_HANDLE; uint32_t family = 0; bool recording = false; };
    struct OurPool { VkCommandPool pool = VK_NULL_HANDLE; std::vector<VkCommandBuffer> idle; };
    struct Push {
        uint32_t sourceOffset, destinationOffset, rowBlocks, sliceBlocks, blocksX, blocksY, outBlocksX, outBlocksY, width, height, format;
    };

    bool createGpu(PFN_vkGetDeviceProcAddr gdpa) {
        auto load = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(gdpa(device_, name)); return fn != nullptr; };
        PFN_vkCreateShaderModule createShader = nullptr; PFN_vkDestroyShaderModule destroyShader = nullptr;
        PFN_vkCreateDescriptorSetLayout createSetLayout = nullptr; PFN_vkCreatePipelineLayout createPipelineLayout = nullptr;
        PFN_vkCreateComputePipelines createPipelines = nullptr; PFN_vkCreateDescriptorPool createDescriptorPool = nullptr;
        bool ok = load(createShader, "vkCreateShaderModule") && load(destroyShader, "vkDestroyShaderModule") &&
            load(createSetLayout, "vkCreateDescriptorSetLayout") && load(destroySetLayout_, "vkDestroyDescriptorSetLayout") &&
            load(createPipelineLayout, "vkCreatePipelineLayout") && load(destroyPipelineLayout_, "vkDestroyPipelineLayout") &&
            load(createPipelines, "vkCreateComputePipelines") && load(destroyPipeline_, "vkDestroyPipeline") &&
            load(createDescriptorPool, "vkCreateDescriptorPool") && load(destroyDescriptorPool_, "vkDestroyDescriptorPool") &&
            load(allocateSets_, "vkAllocateDescriptorSets") && load(freeSets_, "vkFreeDescriptorSets") &&
            load(updateSets_, "vkUpdateDescriptorSets") && load(createCommandPool_, "vkCreateCommandPool") &&
            load(destroyCommandPool_, "vkDestroyCommandPool") && load(allocateCommands_, "vkAllocateCommandBuffers") &&
            load(beginCommands_, "vkBeginCommandBuffer") && load(endCommands_, "vkEndCommandBuffer") &&
            load(bindPipeline_, "vkCmdBindPipeline") && load(bindSets_, "vkCmdBindDescriptorSets") &&
            load(pushConstants_, "vkCmdPushConstants") && load(dispatch_, "vkCmdDispatch") && load(barrier_, "vkCmdPipelineBarrier");
        if (!ok) return false;
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderInfo.codeSize = sizeof(shaders::kAstcBc3); shaderInfo.pCode = shaders::kAstcBc3;
        VkShaderModule shader = VK_NULL_HANDLE;
        if (createShader(device_, &shaderInfo, nullptr, &shader) != VK_SUCCESS) return false;
        VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                                   {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = 2; setInfo.pBindings = bindings;
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1; layoutInfo.pSetLayouts = &setLayout_; layoutInfo.pushConstantRangeCount = 1; layoutInfo.pPushConstantRanges = &range;
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr};
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kMaxSets};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; poolInfo.maxSets = kMaxSets;
        poolInfo.poolSizeCount = 1; poolInfo.pPoolSizes = &poolSize;
        ok = createSetLayout(device_, &setInfo, nullptr, &setLayout_) == VK_SUCCESS &&
             createPipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_) == VK_SUCCESS &&
             (pipelineInfo.layout = pipelineLayout_, createPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_) == VK_SUCCESS) &&
             createDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) == VK_SUCCESS;
        destroyShader(device_, shader, nullptr);
        if (!ok) {
            if (descriptorPool_) destroyDescriptorPool_(device_, descriptorPool_, nullptr);
            if (pipeline_) destroyPipeline_(device_, pipeline_, nullptr);
            if (pipelineLayout_) destroyPipelineLayout_(device_, pipelineLayout_, nullptr);
            if (setLayout_) destroySetLayout_(device_, setLayout_, nullptr);
        }
        return ok;
    }

    // The queue family of a primary game command buffer, when the GPU path can take the upload; otherwise -1.
    template <class PlanT>
    int usableFamily(VkCommandBuffer cmd, VkDeviceSize outTotal, const std::vector<PlanT>& plans, uint32_t count) {
        VkDeviceSize inTotal = 0;
        for (uint32_t i = 0; i < count; ++i) inTotal += (plans[i].bytes + 15) & ~VkDeviceSize(15);
        const VkDeviceSize range = gpuSetup_.maxStorageRange ? gpuSetup_.maxStorageRange : (VkDeviceSize{128} << 20);
        if (!inTotal || inTotal > range || outTotal > range) return -1;
        std::lock_guard lock(stagingMutex_);
        auto pool = pools_.find(cmd);
        if (pool == pools_.end() || !pool->second.primary) return -1;
        auto family = poolFamilies_.find(pool->second.pool);
        if (family == poolFamilies_.end() || family->second >= 32 || !(gpuSetup_.computeFamilies & (1u << family->second))) return -1;
        return static_cast<int>(family->second);
    }

    // Copies the ASTC blocks into layer memory and records the transcode into the layer command buffer of `cmd`;
    // the game's copy then reads BC3 blocks from the device-local output.
    template <class Region, class PlanT>
    bool redirectGpu(VkCommandBuffer cmd, uint32_t family, const Target& t, uint32_t count, const Region* regions,
                     const std::vector<PlanT>& plans, const uint8_t* source, VkDeviceSize outTotal,
                     std::vector<Region>& out, VkBuffer& staging) {
        VkDeviceSize inTotal = 0;
        for (uint32_t i = 0; i < count; ++i) inTotal += (plans[i].bytes + 15) & ~VkDeviceSize(15);
        VkDeviceSize inOffset = 0, outOffset = 0;
        Chunk* input = allocate(cmd, inTotal, inOffset, false);
        Chunk* output = input ? allocate(cmd, outTotal, outOffset, true) : nullptr;
        if (!output) return fail("GPU transcode staging allocation failed");
        const VkDescriptorSet set = descriptorSet(input, output);
        if (!set) return fail("GPU transcode descriptor set allocation failed");
        out.assign(regions, regions + count);
        std::vector<Push>& pushes = pushScratch();
        pushes.resize(count);
        VkDeviceSize inAt = inOffset, outAt = outOffset;
        for (uint32_t i = 0; i < count; ++i) {
            const PlanT& p = plans[i];
            std::memcpy(input->data + inAt, source + p.start, p.bytes);
            pushes[i] = {static_cast<uint32_t>(inAt / 4), static_cast<uint32_t>(outAt / 4), static_cast<uint32_t>(p.rowBlocks),
                         static_cast<uint32_t>(p.sliceBlocks), static_cast<uint32_t>(p.blocksX), static_cast<uint32_t>(p.blocksY),
                         static_cast<uint32_t>(p.outBlocksX), static_cast<uint32_t>(p.outBlocksY), regions[i].imageExtent.width,
                         regions[i].imageExtent.height, uint32_t{t.bw} | uint32_t{t.bh} << 8 | uint32_t{t.srgb} << 16};
            out[i].bufferOffset = outAt; out[i].bufferRowLength = 0; out[i].bufferImageHeight = 0;
            inAt += (p.bytes + 15) & ~VkDeviceSize(15);
            outAt += (p.outBytes + 15) & ~VkDeviceSize(15);
        }
        std::lock_guard lock(gpuMutex_);
        const VkCommandBuffer ours = recordingBufferLocked(cmd, family);
        if (!ours) return fail("GPU transcode command buffer unavailable");
        bindPipeline_(ours, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        bindSets_(ours, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &set, 0, nullptr);
        for (uint32_t i = 0; i < count; ++i) {
            pushConstants_(ours, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &pushes[i]);
            dispatch_(ours, (pushes[i].outBlocksX + 3) / 4, (pushes[i].outBlocksY + 3) / 4, static_cast<uint32_t>(plans[i].slices));
        }
        staging = output->buffer;
        return true;
    }
    static std::vector<Push>& pushScratch() { thread_local std::vector<Push> pushes; return pushes; }

    // The layer command buffer collecting `cmd`'s transcodes, begun on first use.
    VkCommandBuffer recordingBufferLocked(VkCommandBuffer cmd, uint32_t family) {
        Work& work = work_[cmd];
        if (work.ours && !work.recording) return VK_NULL_HANDLE;  // recorded into again after its submit (no reset)
        if (work.ours) return work.ours;
        OurPool& pool = ourPools_[family];
        if (!pool.pool) {
            VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; info.queueFamilyIndex = family;
            if (createCommandPool_(device_, &info, nullptr, &pool.pool) != VK_SUCCESS) { pool.pool = VK_NULL_HANDLE; work_.erase(cmd); return VK_NULL_HANDLE; }
        }
        VkCommandBuffer ours = VK_NULL_HANDLE;
        if (!pool.idle.empty()) { ours = pool.idle.back(); pool.idle.pop_back(); }
        else {
            VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            info.commandPool = pool.pool; info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; info.commandBufferCount = 1;
            if (allocateCommands_(device_, &info, &ours) != VK_SUCCESS) { work_.erase(cmd); return VK_NULL_HANDLE; }
            if (gpuSetup_.setLoaderData) gpuSetup_.setLoaderData(device_, ours);
        }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (beginCommands_(ours, &begin) != VK_SUCCESS) { pool.idle.push_back(ours); work_.erase(cmd); return VK_NULL_HANDLE; }
        work.ours = ours; work.family = family; work.recording = true;
        recording_.fetch_add(1, std::memory_order_release);
        return ours;
    }
    // At submit: ends the layer command buffer of `cmd` (compute writes made visible to the copy) and returns it.
    VkCommandBuffer takeRecordedLocked(VkCommandBuffer cmd) {
        auto it = work_.find(cmd);
        if (it == work_.end() || !it->second.recording) return VK_NULL_HANDLE;
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier_(it->second.ours, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        it->second.recording = false;
        recording_.fetch_sub(1, std::memory_order_release);
        if (endCommands_(it->second.ours) != VK_SUCCESS) { fail("GPU transcode command buffer failed to end"); return VK_NULL_HANDLE; }
        return it->second.ours;
    }
    void releaseWorkLocked(VkCommandBuffer cmd) {  // stagingMutex_ held; takes gpuMutex_
        std::lock_guard lock(gpuMutex_);
        auto it = work_.find(cmd);
        if (it == work_.end()) return;
        if (it->second.recording) recording_.fetch_sub(1, std::memory_order_release);
        ourPools_[it->second.family].idle.push_back(it->second.ours);  // begun again (implicit reset) on reuse
        work_.erase(it);
    }

    // One descriptor set per (input chunk, output chunk) pair, kept until either chunk is destroyed.
    VkDescriptorSet descriptorSet(Chunk* input, Chunk* output) {
        std::lock_guard lock(stagingMutex_);
        const auto key = std::make_pair(input, output);
        auto it = sets_.find(key);
        if (it != sets_.end()) return it->second;
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool = descriptorPool_; info.descriptorSetCount = 1; info.pSetLayouts = &setLayout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (allocateSets_(device_, &info, &set) != VK_SUCCESS) {
            // Pool full: drop sets whose chunks are both idle (no pending command buffer can use them) and retry.
            for (auto s = sets_.begin(); s != sets_.end();) {
                if (s->first.first->refs || s->first.second->refs) { ++s; continue; }
                freeSets_(device_, descriptorPool_, 1, &s->second); s = sets_.erase(s);
            }
            if (allocateSets_(device_, &info, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
        }
        VkDescriptorBufferInfo buffers[2] = {{input->buffer, 0, VK_WHOLE_SIZE}, {output->buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[2] = {};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = set; writes[i].dstBinding = i;
            writes[i].descriptorCount = 1; writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &buffers[i];
        }
        updateSets_(device_, 2, writes, 0, nullptr);
        sets_[key] = set;
        return set;
    }

    // One ASTC upload region: per slice, decode its blocks to RGBA, then encode tightly packed BC3 blocks.
    static void transcodeAstc(const Target& t, VkDeviceSize blocksX, VkDeviceSize blocksY, VkDeviceSize rowBlocks,
                              VkDeviceSize sliceBlocks, VkDeviceSize slices, VkDeviceSize outBlocksX, VkDeviceSize outBlocksY,
                              uint32_t width, uint32_t height, const uint8_t* from, uint8_t* to) {
        const size_t pitch = static_cast<size_t>(blocksX) * t.bw;  // texels per decoded row
        thread_local std::vector<uint8_t> pixels;
        pixels.resize(pitch * blocksY * t.bh * 4);
        uint8_t* image = pixels.data();
        const bool parallel = blocksX * blocksY * t.bw * t.bh >= 65536;
        for (VkDeviceSize slice = 0; slice < slices; ++slice) {
            const uint8_t* src = from + slice * sliceBlocks * 16;
            uint8_t* dst = to + slice * outBlocksX * outBlocksY * 16;
            auto decodeRow = [&](size_t by) {
                uint8_t texels[144][4];
                for (VkDeviceSize bx = 0; bx < blocksX; ++bx) {
                    astc::decode_block(src + (by * rowBlocks + bx) * 16, t.bw, t.bh, t.srgb, texels);
                    for (int y = 0; y < t.bh; ++y)
                        std::memcpy(image + ((by * t.bh + y) * pitch + bx * t.bw) * 4, texels[y * t.bw], t.bw * 4);
                }
            };
            auto encodeRow = [&](size_t by) {
                uint8_t px[16][4];
                for (VkDeviceSize bx = 0; bx < outBlocksX; ++bx) {
                    // Texels past the region edge repeat the last one, so padding never skews the block's endpoints.
                    for (int i = 0; i < 16; ++i) {
                        const size_t x = std::min<size_t>(bx * 4 + (i & 3), width - 1), y = std::min<size_t>(by * 4 + (i >> 2), height - 1);
                        std::memcpy(px[i], image + (y * pitch + x) * 4, 4);
                    }
                    texture::encode_bc3(px, dst + (by * outBlocksX + bx) * 16);
                }
            };
            if (parallel) { TranscodeWorkers::get().run(blocksY, decodeRow); TranscodeWorkers::get().run(outBlocksY, encodeRow); }
            else { for (size_t y = 0; y < blocksY; ++y) decodeRow(y); for (size_t y = 0; y < outBlocksY; ++y) encodeRow(y); }
        }
        if (pixels.capacity() > (64u << 20)) std::vector<uint8_t>().swap(pixels);  // don't pin a 4K texture's worth
    }

    static bool fail(const char* what) {
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 16)
            __android_log_print(ANDROID_LOG_WARN, "Refract.Transcode", "%s; texture left as is", what);
        return false;
    }
    void report(uint64_t texels, bool astc, std::chrono::steady_clock::duration took, bool gpu) {
        std::lock_guard lock(statsMutex_);
        ++uploads_; (astc ? astcTexels_ : etcTexels_) += texels; busy_ += took; longest_ = std::max(longest_, took);
        if (gpu) gpuTexels_ += texels;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport_ < std::chrono::seconds(5)) return;
        lastReport_ = now;
        const double ms = std::chrono::duration<double, std::milli>(busy_).count();
        // The recording thread waits for each upload; "longest" is the worst stall since the last report.
        __android_log_print(ANDROID_LOG_INFO, "Refract.Transcode",
            "%llu uploads, %.1f Mpix ETC2/EAC + %.1f Mpix ASTC (%.1f on the GPU) transcoded in %.0f ms (%.0f Mpix/s), longest %.1f ms, staging %.0f MB",
            static_cast<unsigned long long>(uploads_), etcTexels_ / 1e6, astcTexels_ / 1e6, gpuTexels_ / 1e6, ms,
            ms > 0 ? (etcTexels_ + astcTexels_) / 1e3 / ms : 0.0,
            std::chrono::duration<double, std::milli>(longest_).count(), stagingBytes() / 1048576.0);
        longest_ = {};
    }
    VkDeviceSize stagingBytes() {
        std::lock_guard lock(stagingMutex_);
        VkDeviceSize total = 0; for (auto& c : chunks_) total += c->size; return total;
    }
    Chunk* createChunk(VkDeviceSize size, bool device) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | (gpuReady_ ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0);
        auto chunk = std::make_unique<Chunk>();
        chunk->size = size; chunk->device = device;
        if (createBuffer_(device_, &info, nullptr, &chunk->buffer) != VK_SUCCESS) return nullptr;
        VkMemoryRequirements requirements{};
        bufferRequirements_(device_, chunk->buffer, &requirements);
        // Host staging: host-visible and coherent, preferring cached system memory (never the device-local BAR heap).
        // GPU output: device-local, preferring memory that is not host-visible.
        int best = -1, bestScore = -1;
        for (uint32_t t = 0; t < memory_.memoryTypeCount; ++t) {
            const auto flags = memory_.memoryTypes[t].propertyFlags;
            if (!(requirements.memoryTypeBits & (1u << t))) continue;
            int score;
            if (device) {
                if (!(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
                score = flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? 0 : 1;
            } else {
                constexpr VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                if ((flags & need) != need) continue;
                score = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? 0 : 2) + (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? 1 : 0);
            }
            if (score > bestScore) { best = static_cast<int>(t); bestScore = score; }
        }
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = static_cast<uint32_t>(best);
        void* data = nullptr;
        if (best < 0 || allocate_(device_, &allocation, nullptr, &chunk->memory) != VK_SUCCESS) {
            destroyBuffer_(device_, chunk->buffer, nullptr); return nullptr;
        }
        if (bindBuffer_(device_, chunk->buffer, chunk->memory, 0) != VK_SUCCESS ||
            (!device && map_(device_, chunk->memory, 0, VK_WHOLE_SIZE, 0, &data) != VK_SUCCESS)) {
            destroyBuffer_(device_, chunk->buffer, nullptr); free_(device_, chunk->memory, nullptr); return nullptr;
        }
        chunk->data = static_cast<uint8_t*>(data);
        chunks_.push_back(std::move(chunk));
        return chunks_.back().get();
    }
    Chunk* allocate(VkCommandBuffer cmd, VkDeviceSize bytes, VkDeviceSize& offset, bool device = false) {
        std::lock_guard lock(stagingMutex_);
        Chunk*& current = current_[device];
        if (!current || current->used + bytes > current->size) {
            current = nullptr;
            for (auto& c : chunks_) if (c->device == device && !c->refs && c->size >= bytes) { c->used = 0; current = c.get(); break; }
            if (!current) current = createChunk(std::max(kChunkBytes, (bytes + kChunkBytes - 1) & ~(kChunkBytes - 1)), device);
            if (!current) return nullptr;
        }
        offset = current->used;
        current->used += bytes;
        auto& held = held_[cmd];
        if (held.empty()) holders_.fetch_add(1, std::memory_order_relaxed);
        if (std::find(held.begin(), held.end(), current) == held.end()) { held.push_back(current); ++current->refs; }
        return current;
    }
    void destroyChunk(Chunk& c) {
        if (c.data) unmap_(device_, c.memory);
        destroyBuffer_(device_, c.buffer, nullptr); free_(device_, c.memory, nullptr);
    }
    void releaseLocked(VkCommandBuffer cmd) {
        if (gpuReady_) releaseWorkLocked(cmd);
        auto it = held_.find(cmd);
        if (it == held_.end()) return;
        for (Chunk* c : it->second) if (!--c->refs) c->used = 0;
        held_.erase(it);
        holders_.fetch_sub(1, std::memory_order_relaxed);
        // Keep two idle host chunks and one idle device chunk for the next uploads; give the rest back.
        size_t idle[2] = {0, 0};
        for (auto c = chunks_.begin(); c != chunks_.end();) {
            Chunk* chunk = c->get();
            if (chunk->refs || chunk == current_[0] || chunk == current_[1] || ++idle[chunk->device] <= (chunk->device ? 1u : 2u)) { ++c; continue; }
            for (auto s = sets_.begin(); s != sets_.end();) {
                if (s->first.first != chunk && s->first.second != chunk) { ++s; continue; }
                freeSets_(device_, descriptorPool_, 1, &s->second); s = sets_.erase(s);
            }
            destroyChunk(*chunk);
            c = chunks_.erase(c);
        }
    }

    VkDevice device_;
    VkPhysicalDeviceMemoryProperties memory_;
    bool bcSupported_[VK_FORMAT_BC7_SRGB_BLOCK - VK_FORMAT_BC1_RGB_UNORM_BLOCK + 1] = {};
    bool astc_;
    GpuSetup gpuSetup_;
    bool gpuReady_ = false;
    static constexpr uint32_t kMaxSets = 128;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    PFN_vkDestroyDescriptorSetLayout destroySetLayout_ = nullptr;
    PFN_vkDestroyPipelineLayout destroyPipelineLayout_ = nullptr;
    PFN_vkDestroyPipeline destroyPipeline_ = nullptr;
    PFN_vkDestroyDescriptorPool destroyDescriptorPool_ = nullptr;
    PFN_vkAllocateDescriptorSets allocateSets_ = nullptr;
    PFN_vkFreeDescriptorSets freeSets_ = nullptr;
    PFN_vkUpdateDescriptorSets updateSets_ = nullptr;
    PFN_vkCreateCommandPool createCommandPool_ = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool_ = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommands_ = nullptr;
    PFN_vkBeginCommandBuffer beginCommands_ = nullptr;
    PFN_vkEndCommandBuffer endCommands_ = nullptr;
    PFN_vkCmdBindPipeline bindPipeline_ = nullptr;
    PFN_vkCmdBindDescriptorSets bindSets_ = nullptr;
    PFN_vkCmdPushConstants pushConstants_ = nullptr;
    PFN_vkCmdDispatch dispatch_ = nullptr;
    PFN_vkCmdPipelineBarrier barrier_ = nullptr;
    PFN_vkCreateBuffer createBuffer_ = nullptr;
    PFN_vkDestroyBuffer destroyBuffer_ = nullptr;
    PFN_vkGetBufferMemoryRequirements bufferRequirements_ = nullptr;
    PFN_vkAllocateMemory allocate_ = nullptr;
    PFN_vkFreeMemory free_ = nullptr;
    PFN_vkBindBufferMemory bindBuffer_ = nullptr;
    PFN_vkMapMemory map_ = nullptr;
    PFN_vkUnmapMemory unmap_ = nullptr;

    mutable std::shared_mutex mutex_;
    std::unordered_map<VkImage, Image> images_;
    std::unordered_map<VkBuffer, Buffer> buffers_;
    std::unordered_map<VkDeviceMemory, Memory> memories_;

    // Lock order: stagingMutex_ before gpuMutex_.
    std::mutex stagingMutex_;
    std::vector<std::unique_ptr<Chunk>> chunks_;
    Chunk* current_[2] = {nullptr, nullptr};  // host, device
    std::unordered_map<VkCommandBuffer, std::vector<Chunk*>> held_;
    std::unordered_map<VkCommandBuffer, Pool> pools_;
    std::unordered_map<VkCommandPool, uint32_t> poolFamilies_;
    std::map<std::pair<Chunk*, Chunk*>, VkDescriptorSet> sets_;
    std::atomic<size_t> holders_{0};

    std::mutex gpuMutex_;  // layer command pools and their command buffers
    std::unordered_map<VkCommandBuffer, Work> work_;
    std::unordered_map<uint32_t, OurPool> ourPools_;
    std::atomic<size_t> recording_{0};

    std::mutex statsMutex_;
    uint64_t uploads_ = 0, etcTexels_ = 0, astcTexels_ = 0, gpuTexels_ = 0;
    std::chrono::steady_clock::duration busy_{}, longest_{};
    std::chrono::steady_clock::time_point lastReport_{};
};

}  // namespace refract
