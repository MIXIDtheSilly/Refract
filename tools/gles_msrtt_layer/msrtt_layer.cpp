// Android GLES layer that gives the emulator's gfxstream GLES driver the extensions Quest engines
// need for XR eye buffers:
//
// - GL_EXT_multisampled_render_to_texture(2) and GL_OVR_multiview_multisampled_render_to_texture,
//   implemented by rendering without MSAA. Without them Unity creates single-sample eye textures
//   while its render passes request 4 samples, and every pass is rejected ("Attachment 0 was
//   created with 1 samples but 4 samples were requested").
// - GL_OVR_multiview / GL_OVR_multiview2, emulated. gfxstream passes GL_OVR_multiview2 through from
//   the host driver, but its guest encoder has no glFramebufferTextureMultiviewOVR (the call lands
//   in libEGL's "unimplemented" stub, the framebuffer stays empty and nothing is drawn). The layer
//   attaches one texture layer at a time, rewrites multiview shaders to read the view from a
//   uniform, and repeats draws and clears on multiview framebuffers once per view.
//
// Enable for one app (x86_64 GL stack, also for ARM apps under translation):
//   adb push librefract_gles_msrtt.so /data/local/debug/gles/
//   adb shell chcon u:object_r:apk_data_file:s0 /data/local/debug/gles/librefract_gles_msrtt.so
//   adb shell settings put global enable_gpu_debug_layers 1
//   adb shell settings put global gpu_debug_app <package>
//   adb shell settings put global gpu_debug_layers_gles librefract_gles_msrtt.so
// debug.refract.msrtt_trace=1 logs framebuffer setup and incomplete-framebuffer draws.
#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <new>
#include <cstring>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "Refract.MSRTT", __VA_ARGS__)

namespace {
typedef void* (*PFNNEXTPROC)(void* layer_id, const char* name);
void* g_layerId = nullptr;
PFNNEXTPROC g_getNext = nullptr;

constexpr GLenum kTextureSamplesExt = 0x8D6C;  // GL_TEXTURE_SAMPLES_EXT
constexpr GLenum kNumViews = 0x9630;           // GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_NUM_VIEWS_OVR
constexpr GLenum kMaxViews = 0x9631;           // GL_MAX_VIEWS_OVR
constexpr GLenum kBaseViewIndex = 0x9632;      // GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_BASE_VIEW_INDEX_OVR
constexpr GLint kSupportedViews = 2;
constexpr const char* kViewUniform = "refract_ViewID";

const char* const kExtensions[] = {
    "GL_OVR_multiview",
    "GL_OVR_multiview2",
    "GL_OVR_multiview_multisampled_render_to_texture",
    "GL_EXT_multisampled_render_to_texture",
    "GL_EXT_multisampled_render_to_texture2",
};
// Would need multisample array textures, which the emulator lacks.
const char* const kHidden[] = {"GL_EXT_multiview_texture_multisample"};

template <typename T> T next(const char* name) { return reinterpret_cast<T>(g_getNext(g_layerId, name)); }
// Next-layer entry points, looked up on first use (extensions resolve through eglGetProcAddress).
#define NEXT(type, name) static const auto next_##name = next<type>(#name)

bool tracing()
{
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX]{};
        __system_property_get("debug.refract.msrtt_trace", value);
        return std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

std::atomic<int> g_attachBudget{200}, g_drawBudget{6};

// debug.refract.msrtt_stats=1: per-second call counts and a periodic sample of the call sequence
// (buffer uploads vs draws), to see what the render thread spends its pipe round trips on.
bool stats()
{
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX]{};
        __system_property_get("debug.refract.msrtt_stats", value);
        return std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
struct Stats {
    std::mutex mutex;
    int64_t windowStart = 0;
    uint64_t subData = 0, subBytes = 0, draws = 0, maps = 0, binds = 0, isVao = 0, bufferData = 0, bufferDataBytes = 0;
    uint64_t sizes[8] = {};  // <=64, <=256, <=1K, <=4K, <=16K, <=64K, <=256K, more.
    std::map<GLenum, uint64_t> targets;
    std::string sequence;
    int sequenceLeft = 0;
    int64_t nextSequence = 0;
};
Stats g_stats;
int64_t now_ms()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}
// Records one event; `text` goes into the sampled sequence.
template <typename Update> void stat(Update update, const char* format = nullptr, ...)
{
    if (!stats()) return;
    std::lock_guard<std::mutex> lock(g_stats.mutex);
    update(g_stats);
    const int64_t t = now_ms();
    if (!g_stats.windowStart) g_stats.windowStart = t, g_stats.nextSequence = t + 3000;
    if (format && g_stats.sequenceLeft > 0) {
        char text[128];
        va_list args;
        va_start(args, format);
        vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        g_stats.sequence += text;
        g_stats.sequence += ' ';
        if (--g_stats.sequenceLeft == 0 || g_stats.sequence.size() > 900) {
            LOG("seq: %s", g_stats.sequence.c_str());
            g_stats.sequence.clear();
            if (g_stats.sequenceLeft == 0) g_stats.nextSequence = t + 10000;
        }
    }
    if (g_stats.sequenceLeft == 0 && t >= g_stats.nextSequence) g_stats.sequenceLeft = 600, g_stats.nextSequence = INT64_MAX;
    if (t - g_stats.windowStart >= 2000) {
        auto& s = g_stats;
        const double secs = (t - s.windowStart) / 1000.0;
        std::string targets;
        for (const auto& entry : s.targets) {
            char item[32];
            snprintf(item, sizeof(item), " 0x%x:%.0f", entry.first, entry.second / secs);
            targets += item;
        }
        LOG("per s: subdata=%.0f (%.0f KB) sizes<=64:%.0f <=256:%.0f <=1K:%.0f <=4K:%.0f <=16K:%.0f <=64K:%.0f <=256K:%.0f more:%.0f "
            "targets%s bufferdata=%.0f (%.0f KB) draws=%.0f maps=%.0f binds=%.0f isvao=%.0f",
            s.subData / secs, s.subBytes / 1024.0 / secs, s.sizes[0] / secs, s.sizes[1] / secs, s.sizes[2] / secs, s.sizes[3] / secs,
            s.sizes[4] / secs, s.sizes[5] / secs, s.sizes[6] / secs, s.sizes[7] / secs, targets.c_str(), s.bufferData / secs,
            s.bufferDataBytes / 1024.0 / secs, s.draws / secs, s.maps / secs, s.binds / secs, s.isVao / secs);
        const int left = s.sequenceLeft;
        const int64_t next = s.nextSequence;
        std::string sequence = std::move(s.sequence);
        s.~Stats();
        new (&s) Stats();
        s.windowStart = t, s.sequenceLeft = left, s.nextSequence = next, s.sequence = std::move(sequence);
    }
}

void log_once(const char* what, GLsizei value)
{
    static std::mutex mutex;
    static std::map<std::string, bool> seen;
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.emplace(what, true).second) LOG("%s (%d)", what, value);
}

// --- State. Framebuffers belong to one context; programs are shared, but their names are unique
// within the share group, which is all an app uses. ---
struct ViewAttachment {
    GLuint texture = 0;
    GLint level = 0, base = 0;
    GLsizei views = 0, samples = 0;
};
struct Framebuffer {
    std::map<GLenum, ViewAttachment> multiview;  // Attachment point -> emulated multiview texture.
    std::map<GLenum, GLsizei> samples;           // Pretended samples of multisampled-render-to-texture attachments.
    GLint attachedView = 0;                      // Which view's layers are attached right now.
};
struct ContextState {
    GLuint drawFramebuffer = 0, readFramebuffer = 0, program = 0;
    std::map<GLuint, Framebuffer> framebuffers;
};
std::mutex g_mutex;
std::map<EGLContext, ContextState> g_contexts;
std::map<GLuint, GLint> g_viewUniform;  // Program -> location of the view uniform.
std::map<GLuint, GLsizei> g_renderbufferSamples;

// Callers hold g_mutex.
ContextState& context() { return g_contexts[eglGetCurrentContext()]; }
GLuint bound(ContextState& state, GLenum target)
{
    return target == GL_READ_FRAMEBUFFER ? state.readFramebuffer : state.drawFramebuffer;
}
Framebuffer& framebuffer(GLenum target)
{
    auto& state = context();
    return state.framebuffers[bound(state, target)];
}

void trace_attach(const char* what, GLenum target, GLenum attachment, GLuint object, GLint a, GLint b, GLint c)
{
    if (!tracing() || g_attachBudget.fetch_sub(1) <= 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    LOG("%s fb=%u attachment=0x%x object=%u %d %d %d", what, bound(context(), target), attachment, object, a, b, c);
}

// A plain attach replaces whatever emulated state that attachment point had.
void forget(GLenum target, GLenum attachment)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& fb = framebuffer(target);
    fb.multiview.erase(attachment);
    fb.samples.erase(attachment);
    if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
        fb.multiview.erase(GL_DEPTH_ATTACHMENT);
        fb.multiview.erase(GL_STENCIL_ATTACHMENT);
    }
}

// --- Binding tracking ---
void GL_APIENTRY shim_glBindFramebuffer(GLenum target, GLuint name)
{
    NEXT(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer);
    next_glBindFramebuffer(target, name);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& state = context();
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) state.drawFramebuffer = name;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) state.readFramebuffer = name;
}

void GL_APIENTRY shim_glDeleteFramebuffers(GLsizei count, const GLuint* names)
{
    NEXT(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers);
    next_glDeleteFramebuffers(count, names);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& state = context();
    for (GLsizei i = 0; names && i < count; ++i) {
        state.framebuffers.erase(names[i]);
        if (state.drawFramebuffer == names[i]) state.drawFramebuffer = 0;
        if (state.readFramebuffer == names[i]) state.readFramebuffer = 0;
    }
}

void GL_APIENTRY shim_glUseProgram(GLuint program)
{
    NEXT(PFNGLUSEPROGRAMPROC, glUseProgram);
    next_glUseProgram(program);
    std::lock_guard<std::mutex> lock(g_mutex);
    context().program = program;
}

void remember_view_uniform(GLuint program)
{
    NEXT(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation);
    const GLint location = next_glGetUniformLocation(program, kViewUniform);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (location >= 0) g_viewUniform[program] = location;
    else g_viewUniform.erase(program);
}

void GL_APIENTRY shim_glLinkProgram(GLuint program)
{
    NEXT(PFNGLLINKPROGRAMPROC, glLinkProgram);
    next_glLinkProgram(program);
    remember_view_uniform(program);
}

void GL_APIENTRY shim_glProgramBinary(GLuint program, GLenum format, const void* binary, GLsizei length)
{
    NEXT(PFNGLPROGRAMBINARYPROC, glProgramBinary);
    next_glProgramBinary(program, format, binary, length);
    remember_view_uniform(program);
}

// --- Multiview shaders: the view index becomes a uniform the draw loop sets. ---
void GL_APIENTRY shim_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths)
{
    NEXT(PFNGLSHADERSOURCEPROC, glShaderSource);
    std::string source;
    for (GLsizei i = 0; strings && i < count; ++i) {
        if (!strings[i]) continue;
        source.append(strings[i], lengths && lengths[i] >= 0 ? static_cast<size_t>(lengths[i]) : std::strlen(strings[i]));
    }
    if (source.find("GL_OVR_multiview") == std::string::npos && source.find("gl_ViewID_OVR") == std::string::npos) {
        next_glShaderSource(shader, count, strings, lengths);
        return;
    }
    static const std::regex extension(R"(^[ \t]*#[ \t]*extension[ \t]+GL_OVR_multiview2?[ \t]*:[^\n]*)", std::regex::multiline);
    static const std::regex views(R"(layout[ \t]*\([ \t]*num_views[ \t]*=[ \t]*\d+[ \t]*\)[ \t]*in[ \t]*;)");
    std::string rewritten = std::regex_replace(std::regex_replace(source, extension, ""), views, "");
    // Declarations go after the last #version/#extension line: extension directives must come first.
    size_t insert = 0;
    static const std::regex directive(R"(^[ \t]*#[ \t]*(version|extension)[^\n]*\n?)", std::regex::multiline);
    for (auto it = std::sregex_iterator(rewritten.begin(), rewritten.end(), directive); it != std::sregex_iterator(); ++it)
        insert = static_cast<size_t>(it->position() + it->length());
    if (insert && rewritten[insert - 1] != '\n') rewritten.insert(insert++, "\n");
    rewritten.insert(insert, std::string("uniform highp uint ") + kViewUniform + ";\n#define gl_ViewID_OVR " + kViewUniform + "\n");
    log_once("rewrote a multiview shader (view index from a uniform), shader", static_cast<GLsizei>(shader));
    const GLchar* text = rewritten.c_str();
    next_glShaderSource(shader, 1, &text, nullptr);
}

// --- Attachments ---
void attach_multiview(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint base, GLsizei views, GLsizei samples)
{
    NEXT(PFNGLFRAMEBUFFERTEXTURELAYERPROC, glFramebufferTextureLayer);
    NEXT(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D);
    trace_attach("multiview attach", target, attachment, texture, level, base, views);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& fb = framebuffer(target);
    if (!texture) {
        next_glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, 0, 0);
        fb.multiview.erase(attachment);
        fb.samples.erase(attachment);
        return;
    }
    // Attach the first view's layer now; draws switch layers per view.
    next_glFramebufferTextureLayer(target, attachment, texture, level, base);
    fb.multiview[attachment] = {texture, level, base, views, samples};
    fb.attachedView = 0;
    if (samples > 0) fb.samples[attachment] = samples;
    else fb.samples.erase(attachment);
}

void GL_APIENTRY shim_glFramebufferTextureMultiviewOVR(GLenum target, GLenum attachment, GLuint texture, GLint level,
                                                       GLint baseViewIndex, GLsizei numViews)
{
    log_once("emulating glFramebufferTextureMultiviewOVR, views", numViews);
    attach_multiview(target, attachment, texture, level, baseViewIndex, numViews, 0);
}

void GL_APIENTRY shim_glFramebufferTextureMultisampleMultiviewOVR(GLenum target, GLenum attachment, GLuint texture,
                                                                  GLint level, GLsizei samples, GLint baseViewIndex,
                                                                  GLsizei numViews)
{
    log_once("emulating glFramebufferTextureMultisampleMultiviewOVR (single-sampled), samples", samples);
    attach_multiview(target, attachment, texture, level, baseViewIndex, numViews, samples);
}

void GL_APIENTRY shim_glFramebufferTexture2DMultisampleEXT(GLenum target, GLenum attachment, GLenum textarget,
                                                           GLuint texture, GLint level, GLsizei samples)
{
    NEXT(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D);
    log_once("glFramebufferTexture2DMultisampleEXT (single-sampled), samples", samples);
    trace_attach("glFramebufferTexture2DMultisampleEXT", target, attachment, texture, level, samples, 0);
    next_glFramebufferTexture2D(target, attachment, textarget, texture, level);
    forget(target, attachment);
    if (texture && samples > 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        framebuffer(target).samples[attachment] = samples;
    }
}

void GL_APIENTRY shim_glRenderbufferStorageMultisampleEXT(GLenum target, GLsizei samples, GLenum internalformat,
                                                          GLsizei width, GLsizei height)
{
    NEXT(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage);
    NEXT(PFNGLGETINTEGERVPROC, glGetIntegerv);
    log_once("glRenderbufferStorageMultisampleEXT (single-sampled), samples", samples);
    next_glRenderbufferStorage(target, internalformat, width, height);
    GLint renderbuffer = 0;
    next_glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (samples > 0) g_renderbufferSamples[static_cast<GLuint>(renderbuffer)] = samples;
    else g_renderbufferSamples.erase(static_cast<GLuint>(renderbuffer));
}

void GL_APIENTRY shim_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
    NEXT(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D);
    trace_attach("glFramebufferTexture2D", target, attachment, texture, level, 0, 0);
    next_glFramebufferTexture2D(target, attachment, textarget, texture, level);
    forget(target, attachment);
}

void GL_APIENTRY shim_glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer)
{
    NEXT(PFNGLFRAMEBUFFERTEXTURELAYERPROC, glFramebufferTextureLayer);
    trace_attach("glFramebufferTextureLayer", target, attachment, texture, level, layer, 0);
    next_glFramebufferTextureLayer(target, attachment, texture, level, layer);
    forget(target, attachment);
}

void GL_APIENTRY shim_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer)
{
    NEXT(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer);
    trace_attach("glFramebufferRenderbuffer", target, attachment, renderbuffer, 0, 0, 0);
    next_glFramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer);
    forget(target, attachment);
}

// --- Per-view replay of draws and clears on emulated multiview framebuffers ---
void dump_framebuffer(const char* why, GLenum status)
{
    NEXT(PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVPROC, glGetFramebufferAttachmentParameteriv);
    GLuint name = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        name = context().drawFramebuffer;
    }
    LOG("%s: framebuffer %u status 0x%x (context %p)", why, name, status, eglGetCurrentContext());
    const GLenum attachments[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
    for (GLenum attachment : attachments) {
        GLint type = 0, object = 0, layer = 0;
        next_glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        if (type == GL_NONE) continue;
        next_glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &object);
        if (type == GL_TEXTURE)
            next_glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER, &layer);
        LOG("  attachment 0x%x: %s %d layer=%d", attachment, type == GL_TEXTURE ? "texture" : "renderbuffer", object, layer);
    }
}

void check_draw(const char* what)
{
    if (!tracing() || g_drawBudget.load() <= 0) return;
    NEXT(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus);
    const GLenum status = next_glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE && g_drawBudget.fetch_sub(1) > 0) dump_framebuffer(what, status);
}

// Calls `draw` once per view when an emulated multiview framebuffer is bound (else once), starting
// with the view whose layers are attached so only the other view needs re-attaching.
template <typename Draw> void per_view(const char* what, Draw draw)
{
    NEXT(PFNGLFRAMEBUFFERTEXTURELAYERPROC, glFramebufferTextureLayer);
    NEXT(PFNGLUNIFORM1UIPROC, glUniform1ui);
    std::vector<std::pair<GLenum, ViewAttachment>> attachments;
    GLint views = 0, first = 0, location = -1;
    Framebuffer* fb = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& state = context();
        const auto found = state.framebuffers.find(state.drawFramebuffer);
        if (state.drawFramebuffer && found != state.framebuffers.end() && !found->second.multiview.empty()) {
            fb = &found->second;
            attachments.assign(fb->multiview.begin(), fb->multiview.end());
            for (const auto& entry : attachments) views = std::max<GLint>(views, entry.second.views);
            first = std::min(fb->attachedView, views - 1);
            const auto uniform = g_viewUniform.find(state.program);
            if (uniform != g_viewUniform.end()) location = uniform->second;
        }
    }
    stat([](Stats& s) { ++s.draws; }, "%c", what[2] == 'C' ? 'C' : 'D');
    if (views <= 0) {
        check_draw(what);
        draw();
        return;
    }
    static std::atomic<uint64_t> replayed{0};
    const uint64_t count = ++replayed;
    if ((count & (count - 1)) == 0 && count >= 64)  // 64, 128, 256, ...: proof the emulation is in the draw path.
        LOG("%llu draws/clears replayed per view (latest: %s, %d views, view uniform %s)", (unsigned long long)count, what,
            views, location >= 0 ? "set" : "absent");
    for (GLint i = 0; i < views; ++i) {
        const GLint view = (first + i) % views;
        if (i > 0)
            for (const auto& entry : attachments)
                next_glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, entry.first, entry.second.texture, entry.second.level,
                                               entry.second.base + std::min<GLint>(view, entry.second.views - 1));
        if (location >= 0) next_glUniform1ui(location, static_cast<GLuint>(view));
        if (i == 0) check_draw(what);
        draw();
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    fb->attachedView = (first + views - 1) % views;
}

void GL_APIENTRY shim_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    NEXT(PFNGLDRAWARRAYSPROC, glDrawArrays);
    per_view("glDrawArrays", [&] { next_glDrawArrays(mode, first, count); });
}
void GL_APIENTRY shim_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices)
{
    NEXT(PFNGLDRAWELEMENTSPROC, glDrawElements);
    per_view("glDrawElements", [&] { next_glDrawElements(mode, count, type, indices); });
}
void GL_APIENTRY shim_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances)
{
    NEXT(PFNGLDRAWARRAYSINSTANCEDPROC, glDrawArraysInstanced);
    per_view("glDrawArraysInstanced", [&] { next_glDrawArraysInstanced(mode, first, count, instances); });
}
void GL_APIENTRY shim_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances)
{
    NEXT(PFNGLDRAWELEMENTSINSTANCEDPROC, glDrawElementsInstanced);
    per_view("glDrawElementsInstanced", [&] { next_glDrawElementsInstanced(mode, count, type, indices, instances); });
}
void GL_APIENTRY shim_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices)
{
    NEXT(PFNGLDRAWRANGEELEMENTSPROC, glDrawRangeElements);
    per_view("glDrawRangeElements", [&] { next_glDrawRangeElements(mode, start, end, count, type, indices); });
}
void GL_APIENTRY shim_glDrawArraysIndirect(GLenum mode, const void* indirect)
{
    NEXT(PFNGLDRAWARRAYSINDIRECTPROC, glDrawArraysIndirect);
    per_view("glDrawArraysIndirect", [&] { next_glDrawArraysIndirect(mode, indirect); });
}
void GL_APIENTRY shim_glDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect)
{
    NEXT(PFNGLDRAWELEMENTSINDIRECTPROC, glDrawElementsIndirect);
    per_view("glDrawElementsIndirect", [&] { next_glDrawElementsIndirect(mode, type, indirect); });
}
void GL_APIENTRY shim_glClear(GLbitfield mask)
{
    NEXT(PFNGLCLEARPROC, glClear);
    per_view("glClear", [&] { next_glClear(mask); });
}
void GL_APIENTRY shim_glClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat* value)
{
    NEXT(PFNGLCLEARBUFFERFVPROC, glClearBufferfv);
    per_view("glClearBufferfv", [&] { next_glClearBufferfv(buffer, drawbuffer, value); });
}
void GL_APIENTRY shim_glClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint* value)
{
    NEXT(PFNGLCLEARBUFFERIVPROC, glClearBufferiv);
    per_view("glClearBufferiv", [&] { next_glClearBufferiv(buffer, drawbuffer, value); });
}
void GL_APIENTRY shim_glClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint* value)
{
    NEXT(PFNGLCLEARBUFFERUIVPROC, glClearBufferuiv);
    per_view("glClearBufferuiv", [&] { next_glClearBufferuiv(buffer, drawbuffer, value); });
}
void GL_APIENTRY shim_glClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil)
{
    NEXT(PFNGLCLEARBUFFERFIPROC, glClearBufferfi);
    per_view("glClearBufferfi", [&] { next_glClearBufferfi(buffer, drawbuffer, depth, stencil); });
}

// --- Buffers (statistics only) ---
void GL_APIENTRY shim_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data)
{
    NEXT(PFNGLBUFFERSUBDATAPROC, glBufferSubData);
    stat([&](Stats& s) {
        ++s.subData;
        s.subBytes += static_cast<uint64_t>(size);
        int bucket = 0;
        for (GLsizeiptr limit = 64; bucket < 7 && size > limit; limit *= 4) ++bucket;
        ++s.sizes[bucket];
        ++s.targets[target];
    }, "S%x@%ld+%ld", target & 0xff, static_cast<long>(offset), static_cast<long>(size));
    next_glBufferSubData(target, offset, size, data);
}
void GL_APIENTRY shim_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage)
{
    NEXT(PFNGLBUFFERDATAPROC, glBufferData);
    stat([&](Stats& s) { ++s.bufferData; s.bufferDataBytes += data ? static_cast<uint64_t>(size) : 0; },
         "BD%x+%ld%s", target & 0xff, static_cast<long>(size), data ? "" : "(null)");
    next_glBufferData(target, size, data, usage);
}
void GL_APIENTRY shim_glBindBuffer(GLenum target, GLuint buffer)
{
    NEXT(PFNGLBINDBUFFERPROC, glBindBuffer);
    stat([](Stats& s) { ++s.binds; }, "b%x=%u", target & 0xff, buffer);
    next_glBindBuffer(target, buffer);
}
void GL_APIENTRY shim_glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size)
{
    NEXT(PFNGLBINDBUFFERRANGEPROC, glBindBufferRange);
    stat([](Stats& s) { ++s.binds; }, "r%x.%u=%u@%ld+%ld", target & 0xff, index, buffer, static_cast<long>(offset), static_cast<long>(size));
    next_glBindBufferRange(target, index, buffer, offset, size);
}
void* GL_APIENTRY shim_glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access)
{
    NEXT(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange);
    stat([](Stats& s) { ++s.maps; }, "M%x@%ld+%ld/%x", target & 0xff, static_cast<long>(offset), static_cast<long>(length), access);
    return next_glMapBufferRange(target, offset, length, access);
}
GLboolean GL_APIENTRY shim_glIsVertexArray(GLuint array)
{
    NEXT(PFNGLISVERTEXARRAYPROC, glIsVertexArray);
    stat([](Stats& s) { ++s.isVao; }, "V");
    return next_glIsVertexArray(array);
}

// --- Queries ---
void GL_APIENTRY shim_glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint* params)
{
    NEXT(PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVPROC, glGetFramebufferAttachmentParameteriv);
    if (params && (pname == kTextureSamplesExt || pname == kNumViews || pname == kBaseViewIndex)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& fb = framebuffer(target);
        if (pname == kTextureSamplesExt) {
            const auto found = fb.samples.find(attachment);
            *params = found == fb.samples.end() ? 0 : found->second;
        } else {
            const auto found = fb.multiview.find(attachment);
            if (found == fb.multiview.end()) *params = pname == kNumViews ? 1 : 0;
            else *params = pname == kNumViews ? found->second.views : found->second.base;
        }
        return;
    }
    next_glGetFramebufferAttachmentParameteriv(target, attachment, pname, params);
}

void GL_APIENTRY shim_glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint* params)
{
    NEXT(PFNGLGETRENDERBUFFERPARAMETERIVPROC, glGetRenderbufferParameteriv);
    NEXT(PFNGLGETINTEGERVPROC, glGetIntegerv);
    next_glGetRenderbufferParameteriv(target, pname, params);
    if (pname == GL_RENDERBUFFER_SAMPLES && params) {
        GLint renderbuffer = 0;
        next_glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto found = g_renderbufferSamples.find(static_cast<GLuint>(renderbuffer));
        if (found != g_renderbufferSamples.end()) *params = found->second;
    }
}

bool hidden(const char* extension)
{
    for (const char* name : kHidden)
        if (std::strcmp(name, extension) == 0) return true;
    return false;
}

// The corrected extension list, built once from the driver's (all contexts share the driver).
struct ExtensionList {
    std::vector<std::string> names;
    std::string joined;
};
const ExtensionList& extensions()
{
    static std::mutex mutex;
    static ExtensionList list;
    std::lock_guard<std::mutex> lock(mutex);
    if (!list.names.empty()) return list;
    NEXT(PFNGLGETINTEGERVPROC, glGetIntegerv);
    NEXT(PFNGLGETSTRINGIPROC, glGetStringi);
    GLint count = 0;
    next_glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    std::string removed;
    for (GLint i = 0; i < count; ++i) {
        const auto* name = reinterpret_cast<const char*>(next_glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (!name) continue;
        if (hidden(name)) { removed += std::string(" ") + name; continue; }
        list.names.emplace_back(name);
    }
    if (list.names.empty()) return list;  // No context yet; try again later.
    std::string added;
    for (const char* name : kExtensions)
        if (std::find(list.names.begin(), list.names.end(), name) == list.names.end()) {
            list.names.emplace_back(name);
            added += std::string(" ") + name;
        }
    for (const auto& name : list.names) list.joined += (list.joined.empty() ? "" : " ") + name;
    LOG("extension list: %zu entries; added%s; hid%s", list.names.size(), added.empty() ? " nothing" : added.c_str(),
        removed.empty() ? " nothing" : removed.c_str());
    return list;
}

void GL_APIENTRY shim_glGetIntegerv(GLenum pname, GLint* data)
{
    NEXT(PFNGLGETINTEGERVPROC, glGetIntegerv);
    if (data && pname == GL_NUM_EXTENSIONS) {
        const auto& list = extensions();
        if (!list.names.empty()) { *data = static_cast<GLint>(list.names.size()); return; }
    }
    if (data && pname == kMaxViews) { *data = kSupportedViews; return; }
    next_glGetIntegerv(pname, data);
}

const GLubyte* GL_APIENTRY shim_glGetStringi(GLenum name, GLuint index)
{
    NEXT(PFNGLGETSTRINGIPROC, glGetStringi);
    if (name == GL_EXTENSIONS) {
        const auto& list = extensions();
        if (!list.names.empty())
            return index < list.names.size() ? reinterpret_cast<const GLubyte*>(list.names[index].c_str()) : nullptr;
    }
    return next_glGetStringi(name, index);
}

const GLubyte* GL_APIENTRY shim_glGetString(GLenum name)
{
    NEXT(PFNGLGETSTRINGPROC, glGetString);
    if (name == GL_EXTENSIONS) {
        const auto& list = extensions();
        if (!list.names.empty()) return reinterpret_cast<const GLubyte*>(list.joined.c_str());
    }
    return next_glGetString(name);
}

void* shim_for(const char* name)
{
    struct Entry { const char* name; void* function; };
#define ENTRY(fn) {#fn, reinterpret_cast<void*>(shim_##fn)}
    static const Entry entries[] = {
        ENTRY(glFramebufferTexture2DMultisampleEXT), ENTRY(glRenderbufferStorageMultisampleEXT),
        ENTRY(glFramebufferTextureMultisampleMultiviewOVR), ENTRY(glFramebufferTextureMultiviewOVR),
        ENTRY(glFramebufferTexture2D), ENTRY(glFramebufferTextureLayer), ENTRY(glFramebufferRenderbuffer),
        ENTRY(glBindFramebuffer), ENTRY(glDeleteFramebuffers), ENTRY(glUseProgram), ENTRY(glLinkProgram),
        ENTRY(glProgramBinary), ENTRY(glShaderSource),
        ENTRY(glDrawArrays), ENTRY(glDrawElements), ENTRY(glDrawArraysInstanced), ENTRY(glDrawElementsInstanced),
        ENTRY(glDrawRangeElements), ENTRY(glDrawArraysIndirect), ENTRY(glDrawElementsIndirect),
        ENTRY(glClear), ENTRY(glClearBufferfv), ENTRY(glClearBufferiv), ENTRY(glClearBufferuiv), ENTRY(glClearBufferfi),
        ENTRY(glGetFramebufferAttachmentParameteriv), ENTRY(glGetRenderbufferParameteriv),
        ENTRY(glGetIntegerv), ENTRY(glGetStringi), ENTRY(glGetString),
        ENTRY(glBufferSubData), ENTRY(glBufferData), ENTRY(glBindBuffer), ENTRY(glBindBufferRange), ENTRY(glMapBufferRange),
        ENTRY(glIsVertexArray),
    };
#undef ENTRY
    for (const auto& entry : entries)
        if (std::strcmp(entry.name, name) == 0) return entry.function;
    return nullptr;
}

// Extension functions unknown to the loader are looked up through eglGetProcAddress.
typedef __eglMustCastToProperFunctionPointerType (*PFNGPA)(const char*);
PFNGPA g_nextGetProcAddress = nullptr;
__eglMustCastToProperFunctionPointerType shim_eglGetProcAddress(const char* name)
{
    if (name) {
        if (tracing() && (std::strstr(name, "Framebuffer") || std::strstr(name, "Multiview") || std::strstr(name, "Multisample")))
            LOG("eglGetProcAddress(%s)", name);
        if (void* shim = shim_for(name)) return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(shim);
    }
    return g_nextGetProcAddress ? g_nextGetProcAddress(name) : nullptr;
}
}  // namespace

extern "C" __attribute__((visibility("default"))) void AndroidGLESLayer_Initialize(void* layer_id, PFNNEXTPROC get_next_layer_proc_address)
{
    g_layerId = layer_id;
    g_getNext = get_next_layer_proc_address;
    LOG("layer initialized");
}

extern "C" __attribute__((visibility("default"))) void* AndroidGLESLayer_GetProcAddress(const char* name, void* next)
{
    if (!name) return next;
    if (std::strcmp(name, "eglGetProcAddress") == 0) {
        g_nextGetProcAddress = reinterpret_cast<PFNGPA>(next);
        return reinterpret_cast<void*>(shim_eglGetProcAddress);
    }
    if (void* shim = shim_for(name)) return shim;
    return next;
}
