#pragma once
// Standard OpenXR extension ABI declarations (Khronos, Apache-2.0 OR MIT).
// XR_FB_composition_layer_image_layout.
#include "openxr_minimal.h"
using XrCompositionLayerImageLayoutFlagsFB = XrFlags64;
constexpr XrStructureType XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB = static_cast<XrStructureType>(1000040000);
constexpr XrCompositionLayerImageLayoutFlagsFB XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB = 0x00000001;

// Chained to a composition layer's header.
struct XrCompositionLayerImageLayoutFB {
    XrStructureType type;
    void* next;
    XrCompositionLayerImageLayoutFlagsFB flags;
};
