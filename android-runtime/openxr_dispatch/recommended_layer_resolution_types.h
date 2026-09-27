#pragma once
// Standard OpenXR extension ABI declarations (Khronos, Apache-2.0 OR MIT).
// XR_META_recommended_layer_resolution.
#include "openxr_minimal.h"
constexpr XrStructureType XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_META = static_cast<XrStructureType>(1000254000);
constexpr XrStructureType XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_GET_INFO_META = static_cast<XrStructureType>(1000254001);

struct XrRecommendedLayerResolutionMETA {
    XrStructureType type;
    void* next;
    XrExtent2Di recommendedImageDimensions;
    XrBool32 isValid;
};

struct XrRecommendedLayerResolutionGetInfoMETA {
    XrStructureType type;
    const void* next;
    const XrCompositionLayerBaseHeader* layer;
    XrTime predictedDisplayTime;
};
