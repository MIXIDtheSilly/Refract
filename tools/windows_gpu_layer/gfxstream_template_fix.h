#pragma once
// gfxstream (the emulator's host Vulkan decoder) re-linearizes descriptor update
// templates: all image infos, then buffer infos, then buffer views, packed by
// descriptorCount. Its per-entry offsets, however, advance by one element per
// entry instead of by descriptorCount (calcLinearizedDescriptorUpdateTemplateInfo,
// `++imageInfoCount`), so an entry that follows a multi-descriptor entry of the
// same kind reads the previous entry's data. Animal Company's {2 sampled images,
// 2 samplers} template made NVIDIA's driver dereference a null sampler and take
// qemu down. The packed data itself is correct, so recomputing offsets fixes it.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <iterator>
#include <vector>

namespace refract {
enum class TemplateKind { Image, Buffer, View, Inline, Other };
inline TemplateKind template_kind(VkDescriptorType type) {
    switch (type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER: case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return TemplateKind::Image;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return TemplateKind::Buffer;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return TemplateKind::View;
    case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK: return TemplateKind::Inline;
    default: return TemplateKind::Other;
    }
}
// Rewrites the offsets of a gfxstream-linearized template. Returns false (and
// leaves the entries alone) unless every entry has exactly gfxstream's buggy
// layout, so templates from anything else pass through untouched.
inline bool repair_gfxstream_template_offsets(std::vector<VkDescriptorUpdateTemplateEntry>& entries) {
    size_t totals[3]{};
    for (auto& e : entries) {
        auto kind = template_kind(e.descriptorType);
        if (kind == TemplateKind::Other) return false;
        if (kind != TemplateKind::Inline) totals[static_cast<int>(kind)] += e.descriptorCount;
    }
    const size_t sizes[3]{sizeof(VkDescriptorImageInfo), sizeof(VkDescriptorBufferInfo), sizeof(VkBufferView)};
    const size_t starts[3]{0, totals[0] * sizes[0], totals[0] * sizes[0] + totals[1] * sizes[1]};
    size_t buggy[3]{}, packed[3]{};
    bool changed = false;
    for (auto& e : entries) {
        auto kind = static_cast<int>(template_kind(e.descriptorType));
        if (kind == static_cast<int>(TemplateKind::Inline)) continue;
        if (e.stride != sizes[kind] || e.offset != starts[kind] + buggy[kind] * sizes[kind]) return false;
        ++buggy[kind]; packed[kind] += e.descriptorCount;
    }
    std::fill(std::begin(packed), std::end(packed), 0);
    for (auto& e : entries) {
        auto kind = static_cast<int>(template_kind(e.descriptorType));
        if (kind == static_cast<int>(TemplateKind::Inline)) continue;
        size_t offset = starts[kind] + packed[kind] * sizes[kind];
        changed |= e.offset != offset; e.offset = offset; packed[kind] += e.descriptorCount;
    }
    return changed;
}
}  // namespace refract
