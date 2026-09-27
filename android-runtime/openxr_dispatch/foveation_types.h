#pragma once
// Standard OpenXR extension ABI declarations (Khronos, Apache-2.0 OR MIT).
// XR_FB_foveation, XR_FB_foveation_configuration, XR_FB_swapchain_update_state.
#include "openxr_minimal.h"
using XrFoveationProfileFB = struct XrFoveationProfileFB_T*;
using XrSwapchainStateFoveationFlagsFB = XrFlags64;
constexpr XrStructureType XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB = static_cast<XrStructureType>(1000114000);
constexpr XrStructureType XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB = static_cast<XrStructureType>(1000114001);
constexpr XrStructureType XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB = static_cast<XrStructureType>(1000114002);

struct XrFoveationProfileCreateInfoFB {
    XrStructureType type;
    void* next;
};

struct XrSwapchainStateBaseHeaderFB {
    XrStructureType type;
    void* next;
};

struct XrSwapchainStateFoveationFB {
    XrStructureType type;
    void* next;
    XrSwapchainStateFoveationFlagsFB flags;
    XrFoveationProfileFB profile;
};
