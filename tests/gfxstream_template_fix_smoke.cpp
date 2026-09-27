#include "../tools/windows_gpu_layer/gfxstream_template_fix.h"
#include <cstdio>
int main() {
    constexpr size_t I = sizeof(VkDescriptorImageInfo), B = sizeof(VkDescriptorBufferInfo);
    // Animal Company's template as gfxstream created it (buffers start after 4 image infos).
    std::vector<VkDescriptorUpdateTemplateEntry> entries{
        {0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * I + 0 * B, B},
        {1, 0, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 0 * I, I},
        {3, 0, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * I + 1 * B, B},
        {6, 0, 2, VK_DESCRIPTOR_TYPE_SAMPLER, 1 * I, I}};
    if (!refract::repair_gfxstream_template_offsets(entries)) return 1;
    if (entries[0].offset != 4 * I || entries[1].offset != 0 || entries[2].offset != 4 * I + B || entries[3].offset != 2 * I) return 2;
    // Already-correct layouts (or ones gfxstream did not produce) are left alone.
    auto before = entries;
    if (refract::repair_gfxstream_template_offsets(entries)) return 3;
    for (size_t i = 0; i < entries.size(); ++i) if (entries[i].offset != before[i].offset) return 4;
    std::vector<VkDescriptorUpdateTemplateEntry> app{{0, 0, 2, VK_DESCRIPTOR_TYPE_SAMPLER, 8, 16}, {1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, 40, 16}};
    if (refract::repair_gfxstream_template_offsets(app) || app[1].offset != 40) return 5;
    std::puts("gfxstream template offset repair passed");
}
