#pragma once

#include <cstdint>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include "pose_frame.h"

namespace refract::protocol {

// Legacy fallback when no host view recommendation is available.
constexpr uint32_t kTransportEyeDimension = 1024;

constexpr uint32_t kImageFrameMagic = 0x49585241; // AXRI, little-endian.
constexpr uint16_t kImageFrameVersion = 1;
constexpr uint16_t kProjectionImageFrameVersion = 2;
constexpr uint16_t kQuadImageFrameVersion = 4;
constexpr uint16_t kQuadGpuFrameVersion = 5;
constexpr uint32_t kMaxCompositionLayers = 16;
constexpr uint16_t kMixedProjectionGpuFrameVersion = 6;
constexpr uint16_t kMixedQuadGpuFrameVersion = 7;
inline bool mixed_gpu_version(uint16_t version) { return version == 6 || version == 7; }
inline bool valid_mixed_part(uint16_t version, uint32_t part) {
    const uint32_t count = part >> 16, index = part & 0xffff;
    return count >= 2 && count <= kMaxCompositionLayers && index < count &&
        (version == kMixedQuadGpuFrameVersion || (index == 0 && version == kMixedProjectionGpuFrameVersion));
}
constexpr uint16_t kImageFrameTypeRgba8 = 2;
constexpr uint32_t kImageFrameFormatRgba8 = 1;
// Versions 8/9 (scene/quads) carry one H.264 Annex-B access unit instead of pixels:
// both layers side by side in a (2*width) x height picture, BT.601 limited range,
// rows in the same order as the pixel stream. reserved bit 0 marks a key frame,
// which always starts with SPS/PPS so a viewer can join there.
constexpr uint16_t kVideoImageFrameVersion = 8;
constexpr uint16_t kQuadVideoImageFrameVersion = 9;
constexpr uint16_t kImageFrameTypeVideo = 4;
constexpr uint32_t kImageFrameFormatH264 = 2;
constexpr uint32_t kVideoFrameKey = 1;
constexpr uint64_t kMaxVideoPayload = 16ull * 1024ull * 1024ull;
inline bool video_version(uint16_t version) { return version == kVideoImageFrameVersion || version == kQuadVideoImageFrameVersion; }

struct ImageFrameHeader {
    uint32_t magic = kImageFrameMagic;
    uint16_t version = kImageFrameVersion;
    uint16_t type = kImageFrameTypeRgba8;
    uint32_t header_size = sizeof(ImageFrameHeader);
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layers = 0;
    uint32_t format = kImageFrameFormatRgba8;
    uint32_t bytes_per_pixel = 4;
    uint32_t reserved = 0;
    uint64_t sequence = 0;
    uint64_t monotonic_time_ns = 0;
    uint64_t payload_size = 0;
};

static_assert(sizeof(ImageFrameHeader) == 64);

struct ImageProjectionView {
    Pose pose; // The render camera in the host's LOCAL coordinate space, meters.
    float angle_left = 0, angle_right = 0, angle_up = 0, angle_down = 0;
};
struct ImageQuad {
    Pose pose;
    float width = 0, height = 0;
    uint32_t eye_visibility = 0, layer_flags = 0;
};
// Versions 4/5 carry one or two ordered quad layers in the same 96-byte
// metadata envelope. The high bit distinguishes them from stereo cameras.
constexpr uint32_t kQuadCompositionBit = 0x80000000u;
struct ImageProjection {
    uint32_t view_count = 0; // Zero for legacy frames; v2 requires two eyes.
    uint32_t layer_flags = 0; // OpenXR core composition flags (bits 0..2).
    union {
        ImageProjectionView views[2]{};
        ImageQuad quads[2];
    };
    uint32_t quad_count() const { return (view_count & kQuadCompositionBit) ? (view_count & ~kQuadCompositionBit) : 0; }
};
static_assert(sizeof(ImageQuad) == 44);
static_assert(sizeof(ImageProjectionView) == 44);
static_assert(sizeof(ImageProjection) == 96);

inline bool valid_projection(const ImageProjection& projection) {
    if (projection.view_count != 2 || (projection.layer_flags & ~7u) != 0) { return false; }
    for (const auto& view : projection.views) {
        const auto& p = view.pose;
        for (float value : {p.x, p.y, p.z, p.qx, p.qy, p.qz, p.qw,
                           view.angle_left, view.angle_right, view.angle_up, view.angle_down}) {
            if (!std::isfinite(value)) { return false; }
        }
        const float norm = p.qx*p.qx + p.qy*p.qy + p.qz*p.qz + p.qw*p.qw;
        if (std::fabs(norm - 1.0f) > 0.01f ||
            view.angle_left >= view.angle_right || view.angle_down >= view.angle_up ||
            view.angle_left <= -1.5707963f || view.angle_right >= 1.5707963f ||
            view.angle_down <= -1.5707963f || view.angle_up >= 1.5707963f) { return false; }
    }
    return true;
}

// ImageQuad::layer_flags bit beside the OpenXR core flags: the panel image is stored bottom-up
// (XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB), so the host flips it before display.
constexpr uint32_t kQuadLayerFlipped = 1u << 8;

inline bool valid_quad(const ImageQuad& q) {
    const auto& p = q.pose;
    for (float f : {p.x,p.y,p.z,p.qx,p.qy,p.qz,p.qw,q.width,q.height})
        if (!std::isfinite(f)) return false;
    const float norm = p.qx*p.qx+p.qy*p.qy+p.qz*p.qz+p.qw*p.qw;
    return std::fabs(norm-1.0f)<=0.01f && q.width>0 && q.height>0 &&
        q.eye_visibility<=2 && !(q.layer_flags & ~(7u | kQuadLayerFlipped));
}

inline bool valid_quads(const ImageProjection& composition) {
    const auto count = composition.quad_count();
    if (count < 1 || count > 2 || composition.layer_flags) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!valid_quad(composition.quads[i])) return false;
    return true;
}

// Version 10: a whole frame (scene and every quad panel) in one shared GPU texture pair used as
// an atlas. Texture 0 holds the left eye's scene and the panels both eyes (or the left) see,
// texture 1 the right eye's scene and right-only panels; the scene, if any, is at (0, 0).
// The ImageProjection carries both eye views (the scene's, else the head's) so the viewer
// can draw each panel where the app placed it. Payload: WindowsGpuFrame, CompositeHeader,
// then quad_count CompositeQuads in the app's layer order (back to front).
constexpr uint16_t kCompositeGpuFrameVersion = 10;
constexpr uint32_t kMaxCompositeQuads = 15;
struct CompositeHeader {
    uint32_t scene_width = 0, scene_height = 0;  // Zero: no scene layer (panels over black).
    uint32_t quad_count = 0;
    uint32_t reserved = 0;
};
// CompositeQuad::texture bit: the panel's rows are stored bottom-up (the app set
// XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB); the viewer flips it while sampling.
constexpr uint32_t kCompositeQuadFlipped = 2;
struct CompositeQuad {
    ImageQuad quad;
    uint32_t texture = 0;               // Atlas texture (bit 0: 0 or 1), plus kCompositeQuadFlipped.
    uint32_t x = 0, y = 0, width = 0, height = 0;  // Atlas rectangle of the panel image.
};
static_assert(sizeof(CompositeHeader) == 16);
static_assert(sizeof(CompositeQuad) == 64);

// `table` is the payload after its WindowsGpuFrame; the atlas is atlasWidth x atlasHeight.
inline bool valid_composite(const uint8_t* table, uint64_t size, uint32_t atlasWidth, uint32_t atlasHeight) {
    if (size < sizeof(CompositeHeader)) return false;
    CompositeHeader header;
    std::memcpy(&header, table, sizeof(header));
    if (header.quad_count > kMaxCompositeQuads || header.reserved ||
        size != sizeof(CompositeHeader) + uint64_t(header.quad_count) * sizeof(CompositeQuad) ||
        header.scene_width > atlasWidth || header.scene_height > atlasHeight ||
        (header.scene_width == 0) != (header.scene_height == 0)) return false;
    for (uint32_t i = 0; i < header.quad_count; ++i) {
        CompositeQuad q;
        std::memcpy(&q, table + sizeof(header) + i * sizeof(q), sizeof(q));
        if (!valid_quad(q.quad) || (q.texture & ~kCompositeQuadFlipped) > 1 || !q.width || !q.height ||
            uint64_t(q.x) + q.width > atlasWidth || uint64_t(q.y) + q.height > atlasHeight) return false;
    }
    return true;
}

} // namespace refract::protocol
