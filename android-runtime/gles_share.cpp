#include "gles_share.h"
#if defined(__ANDROID__)
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <cstring>
#include <string>
#include <vector>

namespace refract::runtime {
namespace {
constexpr const char* kTag = "Refract.GLES";

PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC g_getNativeClientBuffer = nullptr;
PFNEGLCREATEIMAGEKHRPROC g_createImage = nullptr;
PFNEGLDESTROYIMAGEKHRPROC g_destroyImage = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC g_imageTargetTexture = nullptr;

bool property_is(const char* name, const char* value)
{
    char text[PROP_VALUE_MAX]{};
    __system_property_get(name, text);
    return std::strcmp(text, value) == 0;
}

// A triangle that covers the viewport; the fragment shader copies one texel per pixel.
constexpr const char* kVertexShader = R"(#version 300 es
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";

// base + (x, rowStep * y) addresses the source texel, so rowStep = -1 flips the rows: GL rows start
// at the bottom, while Vulkan (and the export) reads the buffer's first row as the top.
// Sampling an sRGB texture decodes it; encoding again keeps the bytes an sRGB swapchain holds.
constexpr const char* kFragmentShader = R"(
precision highp float;
precision highp int;
uniform ivec2 base;
uniform int rowStep;
uniform int layer;
uniform int srgb;
out vec4 color;
vec3 encode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 at = ivec2(base.x + p.x, base.y + rowStep * p.y);
    vec4 c = FETCH;
    color = vec4(srgb != 0 ? encode(c.rgb) : c.rgb, c.a);
})";

GLuint compile(GLenum type, const std::string& source)
{
    const GLuint shader = glCreateShader(type);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        __android_log_print(ANDROID_LOG_ERROR, kTag, "shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint link_program(bool array)
{
    const std::string fragment = std::string("#version 300 es\n") +
        (array ? "uniform highp sampler2DArray source;\n#define FETCH texelFetch(source, ivec3(at, layer), 0)\n"
               : "uniform highp sampler2D source;\n#define FETCH texelFetch(source, at, 0)\n") + kFragmentShader;
    const GLuint vertexShader = compile(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fragmentShader = compile(GL_FRAGMENT_SHADER, fragment);
    if (!vertexShader || !fragmentShader) {
        if (vertexShader) glDeleteShader(vertexShader);
        if (fragmentShader) glDeleteShader(fragmentShader);
        return 0;
    }
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512]{};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        __android_log_print(ANDROID_LOG_ERROR, kTag, "program link failed: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

// Restores whatever the app had current when it goes out of scope.
struct CurrentContext {
    EGLDisplay display = eglGetCurrentDisplay();
    EGLContext context = eglGetCurrentContext();
    EGLSurface draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface read = eglGetCurrentSurface(EGL_READ);
    EGLDisplay fallback;
    explicit CurrentContext(EGLDisplay ours) : fallback(ours) {}
    ~CurrentContext()
    {
        if (context != EGL_NO_CONTEXT) eglMakeCurrent(display, draw, read, context);
        else eglMakeCurrent(fallback, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
};
}

void GlesShare::configure(const XrGraphicsBindingOpenGLESAndroidKHR& binding, VulkanBackend& vulkan)
{
    shutdown();
    vulkan_ = &vulkan;
    display_ = binding.display;
    config_ = binding.config;
    appContext_ = binding.context;
    configured_ = display_ != EGL_NO_DISPLAY && appContext_ != EGL_NO_CONTEXT;
    __android_log_print(ANDROID_LOG_INFO, kTag, "GLES session: display=%p config=%p context=%p", display_, config_, appContext_);
}

bool GlesShare::setup()
{
    if (!vulkan_->active() && !vulkan_->initialize_private()) return false;
    g_getNativeClientBuffer = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    g_createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    g_destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    g_imageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!g_getNativeClientBuffer || !g_createImage || !g_destroyImage || !g_imageTargetTexture) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "missing EGL image entry points");
        return false;
    }
    if (!config_) {
        // The binding may omit the config (EGL_KHR_no_config_context); use the app context's.
        EGLint id = 0, count = 0;
        eglQueryContext(display_, appContext_, EGL_CONFIG_ID, &id);
        const EGLint attributes[] = {EGL_CONFIG_ID, id, EGL_NONE};
        eglChooseConfig(display_, attributes, &config_, 1, &count);
        if (!count) config_ = nullptr;
    }
    const char* extensions = eglQueryString(display_, EGL_EXTENSIONS);
    const bool surfaceless = extensions && std::strstr(extensions, "EGL_KHR_surfaceless_context");
    EGLConfig config = config_;
    if (!surfaceless) {
        const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                                     EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLint count = 0;
        if (!eglChooseConfig(display_, attributes, &config, 1, &count) || !count) return false;
        const EGLint size[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        surface_ = eglCreatePbufferSurface(display_, config, size);
        if (surface_ == EGL_NO_SURFACE) return false;
    }
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config, appContext_, contextAttributes);
    if (context_ == EGL_NO_CONTEXT) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "shared context failed: 0x%x", eglGetError());
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "shared context ready (%s)", surfaceless ? "surfaceless" : "pbuffer");
    return true;
}

bool GlesShare::ensure_mirror(Mirror& mirror, uint32_t width, uint32_t height)
{
    if (mirror.buffer && mirror.width == width && mirror.height == height) return true;
    release_mirror(mirror);
    AHardwareBuffer_Desc desc{};
    desc.width = width; desc.height = height; desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    if (AHardwareBuffer_allocate(&desc, &mirror.buffer) != 0) {
        mirror.buffer = nullptr;
        __android_log_print(ANDROID_LOG_WARN, kTag, "AHardwareBuffer_allocate %ux%u failed", width, height);
        return false;
    }
    const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    mirror.image = g_createImage(display_, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, g_getNativeClientBuffer(mirror.buffer), attributes);
    if (mirror.image == EGL_NO_IMAGE_KHR) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "eglCreateImageKHR failed: 0x%x", eglGetError());
        release_mirror(mirror);
        return false;
    }
    glGenTextures(1, &mirror.texture);
    glBindTexture(GL_TEXTURE_2D, mirror.texture);
    g_imageTargetTexture(GL_TEXTURE_2D, static_cast<GLeglImageOES>(mirror.image));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!vulkan_->import_hardware_buffer(mirror.buffer, width, height, VK_FORMAT_R8G8B8A8_UNORM, mirror.vkImage, mirror.memory)) {
        release_mirror(mirror);
        return false;
    }
    mirror.width = width; mirror.height = height;
    __android_log_print(ANDROID_LOG_INFO, kTag, "mirror %ux%u ready", width, height);
    return true;
}

void GlesShare::release_mirror(Mirror& mirror)
{
    if (mirror.vkImage || mirror.memory) vulkan_->destroy_image(mirror.vkImage, mirror.memory);
    if (mirror.texture) glDeleteTextures(1, &mirror.texture);
    if (mirror.image != EGL_NO_IMAGE_KHR && g_destroyImage) g_destroyImage(display_, mirror.image);
    if (mirror.buffer) AHardwareBuffer_release(mirror.buffer);
    mirror = {};
}

bool GlesShare::prepare(const Source sources[2], VulkanSwapchain out[2], XrSwapchainSubImage outSub[2])
{
    if (!enabled() || used_ + 2 > kSlots) return false;
    if (!ready_) {
        if (!setup()) {
            failed_ = true;
            __android_log_print(ANDROID_LOG_WARN, kTag, "GLES export unavailable; using pixel readback");
            return false;
        }
        ready_ = true;
    }
    // The app's frame must reach the host before our context samples it.
    glFlush();
    CurrentContext restore(display_);
    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "eglMakeCurrent failed: 0x%x", eglGetError());
        failed_ = true;
        return false;
    }
    if (!programs_[0]) {
        programs_[0] = link_program(false);
        programs_[1] = link_program(true);
        if (!programs_[0] || !programs_[1]) { failed_ = true; return false; }
        for (int i = 0; i < 2; ++i) {
            glUseProgram(programs_[i]);
            glUniform1i(glGetUniformLocation(programs_[i], "source"), 0);
            uniforms_[i][0] = glGetUniformLocation(programs_[i], "base");
            uniforms_[i][1] = glGetUniformLocation(programs_[i], "rowStep");
            uniforms_[i][2] = glGetUniformLocation(programs_[i], "layer");
            uniforms_[i][3] = glGetUniformLocation(programs_[i], "srgb");
        }
        glGenFramebuffers(1, &framebuffer_);
        glGenVertexArrays(1, &vertexArray_);
        // What the driver reports for MSAA (engines pick attachment sample counts from this).
        GLint maxSamples = 0, counts = 0, samples[8] = {};
        glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        glGetInternalformativ(GL_RENDERBUFFER, GL_SRGB8_ALPHA8, GL_NUM_SAMPLE_COUNTS, 1, &counts);
        glGetInternalformativ(GL_RENDERBUFFER, GL_SRGB8_ALPHA8, GL_SAMPLES, 8, samples);
        GLint colorSamples = 0, depthSamples = 0, integerSamples = 0;
        glGetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &colorSamples);
        glGetIntegerv(GL_MAX_DEPTH_TEXTURE_SAMPLES, &depthSamples);
        glGetIntegerv(GL_MAX_INTEGER_SAMPLES, &integerSamples);
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "MSAA caps: max_samples=%d srgb8_alpha8 counts=%d [%d %d %d %d] color_tex=%d depth_tex=%d integer=%d",
            maxSamples, counts, samples[0], samples[1], samples[2], samples[3], colorSamples, depthSamples, integerSamples);
    }
    // debug.refract.gles_flip=0 keeps GL row order (in case a driver stores AHardwareBuffer rows bottom-up).
    const bool flip = !property_is("debug.refract.gles_flip", "0");
    static uint64_t frames = 0;
    const bool probe = frames++ % 150 == 0 && property_is("debug.refract.gles_probe", "1");
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glBindVertexArray(vertexArray_);
    glActiveTexture(GL_TEXTURE0);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const auto& source = sources[eye];
        const uint32_t width = static_cast<uint32_t>(source.rect.extent.width);
        const uint32_t height = static_cast<uint32_t>(source.rect.extent.height);
        Mirror& mirror = mirrors_[ring_][used_ + eye];
        if (!ensure_mirror(mirror, width, height)) { failed_ = true; return false; }
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mirror.texture, 0);
        glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        const int program = source.array ? 1 : 0;
        const GLenum target = source.array ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
        glUseProgram(programs_[program]);
        glBindTexture(target, source.texture);
        glUniform2i(uniforms_[program][0], source.rect.offset.x,
                    source.rect.offset.y + (flip ? static_cast<GLint>(height) - 1 : 0));
        glUniform1i(uniforms_[program][1], flip ? -1 : 1);
        glUniform1i(uniforms_[program][2], static_cast<GLint>(source.layer));
        glUniform1i(uniforms_[program][3], source.srgb ? 1 : 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindTexture(target, 0);
        if (probe) {
            // debug.refract.gles_probe=1: log the centre texel of the app's image and of our copy.
            uint8_t copied[4]{}, original[4]{};
            glReadPixels(static_cast<GLint>(width / 2), static_cast<GLint>(height / 2), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, copied);
            if (!probeFramebuffer_) glGenFramebuffers(1, &probeFramebuffer_);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, probeFramebuffer_);
            if (source.array) glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, source.texture, 0, static_cast<GLint>(source.layer));
            else glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source.texture, 0);
            const GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
            glReadPixels(source.rect.offset.x + static_cast<GLint>(width / 2), source.rect.offset.y + static_cast<GLint>(height / 2),
                         1, 1, GL_RGBA, GL_UNSIGNED_BYTE, original);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
            // Whole copy: how much of the frame is not black, and its average colour.
            static std::vector<uint8_t> frame;
            frame.resize(static_cast<size_t>(width) * height * 4);
            glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, frame.data());
            uint64_t lit = 0, sum[4] = {};
            for (size_t i = 0; i < frame.size(); i += 4) {
                if (frame[i] | frame[i + 1] | frame[i + 2]) ++lit;
                for (int c = 0; c < 4; ++c) sum[c] += frame[i + c];
            }
            const uint64_t pixels = static_cast<uint64_t>(width) * height;
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "probe eye=%u tex=%u array=%d layer=%u srgb=%d rect=%d,%d %ux%u: app=%u,%u,%u,%u (fb 0x%x) copy=%u,%u,%u,%u "
                "lit=%.1f%% mean=%llu,%llu,%llu,%llu err=0x%x",
                eye, source.texture, source.array, source.layer, source.srgb, source.rect.offset.x, source.rect.offset.y, width, height,
                original[0], original[1], original[2], original[3], status, copied[0], copied[1], copied[2], copied[3],
                100.0 * static_cast<double>(lit) / static_cast<double>(pixels), (unsigned long long)(sum[0] / pixels),
                (unsigned long long)(sum[1] / pixels), (unsigned long long)(sum[2] / pixels), (unsigned long long)(sum[3] / pixels), glGetError());
        }

        out[eye] = {};
        out[eye].images[0] = mirror.vkImage;
        out[eye].format = VK_FORMAT_R8G8B8A8_UNORM;
        out[eye].layers = 1;
        out[eye].external = true;
        outSub[eye] = {};
        outSub[eye].imageRect.extent = source.rect.extent;
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    static uint32_t errorReports = 0;
    if (const GLenum error = glGetError(); error != GL_NO_ERROR && errorReports < 5) {
        ++errorReports;
        __android_log_print(ANDROID_LOG_WARN, kTag, "copy GL error 0x%x", error);
    }
    // Vulkan reads the buffers next; finishing guarantees gfxstream sees the finished copy.
    // debug.refract.gles_sync=flush only flushes (the host decodes GL and Vulkan in order).
    if (property_is("debug.refract.gles_sync", "flush")) glFlush();
    else glFinish();
    used_ += 2;
    return true;
}

void GlesShare::shutdown()
{
    if (context_ != EGL_NO_CONTEXT) {
        CurrentContext restore(display_);
        if (eglMakeCurrent(display_, surface_, surface_, context_)) {
            for (auto& frame : mirrors_) for (auto& mirror : frame) release_mirror(mirror);
            for (auto& program : programs_) if (program) glDeleteProgram(program);
            if (framebuffer_) glDeleteFramebuffers(1, &framebuffer_);
            if (probeFramebuffer_) glDeleteFramebuffers(1, &probeFramebuffer_);
            if (vertexArray_) glDeleteVertexArrays(1, &vertexArray_);
        }
    }
    if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
    if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
    *this = {};
}
}
#endif
