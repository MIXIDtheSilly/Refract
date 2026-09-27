#pragma once
#if defined(__ANDROID__)
#include "openxr_dispatch/openxr_minimal.h"
#include "vulkan_backend.h"
#include "image_frame.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <array>

struct XrGraphicsBindingOpenGLESAndroidKHR {
    XrStructureType type; const void* next;
    EGLDisplay display; EGLConfig config; EGLContext context;
};

namespace refract::runtime {
// Gives GLES apps the Vulkan export path. Each frame the eye images are drawn (on our own
// context, shared with the app's) into AHardwareBuffers that a private Vulkan device has
// imported; VulkanBackend then exports those like a Vulkan app's swapchain images.
class GlesShare {
public:
    struct Source {
        GLuint texture = 0;
        bool array = false;  // GL_TEXTURE_2D_ARRAY (multiview) rather than GL_TEXTURE_2D.
        bool srgb = false;   // GL_SRGB8_ALPHA8: sampling decodes, so the copy re-encodes.
        uint32_t layer = 0;
        XrRect2Di rect{};
    };
    // Called at session creation; the GL work itself starts lazily on the render thread.
    void configure(const XrGraphicsBindingOpenGLESAndroidKHR& binding, VulkanBackend& vulkan);
    bool enabled() const { return configured_ && !failed_; }
    void begin_frame() { ring_ = (ring_ + 1) % kRing; used_ = 0; }
    // Copies both eyes (with the app's context current) and describes the copies as 1-layer
    // external Vulkan images (image index 0, full-image rects). False: use pixel readback.
    bool prepare(const Source sources[2], VulkanSwapchain out[2], XrSwapchainSubImage outSub[2]);
    void shutdown();

private:
    // A frame's Vulkan copy has finished by the time the frame after next is copied
    // (the pipelined export waits for each previous frame), so three frames never collide.
    static constexpr uint32_t kRing = 3;
    static constexpr uint32_t kSlots = 2 * refract::protocol::kMaxCompositionLayers;  // Eye images per frame.
    struct Mirror {
        AHardwareBuffer* buffer = nullptr;
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        GLuint texture = 0;
        VkImage vkImage = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
    };
    bool setup();
    bool ensure_mirror(Mirror& mirror, uint32_t width, uint32_t height);
    void release_mirror(Mirror& mirror);
    VulkanBackend* vulkan_ = nullptr;
    bool configured_ = false, failed_ = false, ready_ = false;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext appContext_ = EGL_NO_CONTEXT, context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    GLuint programs_[2] = {};  // 2D, 2D array.
    GLint uniforms_[2][4] = {};  // offset, height, layer, srgb.
    GLuint framebuffer_ = 0, vertexArray_ = 0, probeFramebuffer_ = 0;
    std::array<std::array<Mirror, kSlots>, kRing> mirrors_{};
    uint32_t ring_ = 0, used_ = 0;
};
}
#endif
