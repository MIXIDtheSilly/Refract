// Linux counterpart of viewer.cpp: a window on this PC showing the game's frames ("Play on PC"), drawn with
// OpenGL through SDL3. Receives on TCP 38491 (adb reverse) like viewer.cpp:
//  * GPU frames (AXRI v3/v5/v6/v7/v10): asks the emulator's Refract Vulkan layer for the shared eye textures
//    (protocol/linux_gpu_share.h), imports them (GL_EXT_memory_object_fd), copies them on the GPU and
//    acknowledges the frame. Composite frames (v10) draw the scene region and every UI panel where the app put it.
//  * Pixel frames (v1/v2/v4): uploads the RGBA payload (fallback path, debug.refract.gpu_share=0).
// H.264 frames (debug.refract.video) are not supported here.
// Copies run on a second GL context on the receive thread, so a frame is acknowledged (and the game goes on) as
// soon as its copy finishes, never waiting for the display.
// Mouse and keys drive pose_input_server.py over UDP, as on Windows: right-drag look, left button = right
// trigger, middle button = right grip, wheel = hand distance; the keys it maps (WASD walk, Space = A, ...) are
// sent as they are held. Home recenters, F1 toggles the performance overlay, F2 saves a screenshot, F3 both eyes, F5 flips the image, F6 scene or
// panels, F7 hides composited panels, F11 or Escape (when fullscreen) toggles fullscreen. SIGUSR1 saves a
// screenshot like F2 (for scripts). F1 shows the performance overlay (as viewer.cpp's): frame rates and times, a
// frame-time graph, and, with --adb/--serial/--package, Android's CPU use and the game's busiest threads.
#include "android_stats.h"
#include "image_transport.h"
#include "linux_gpu_share.h"
#include "windows_gpu_frame.h"

#include <SDL3/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include <SDL3/SDL_opengl.h>

#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace proto = refract::protocol;
using Clock = std::chrono::steady_clock;

namespace {

// OpenGL 4.5 core plus GL_EXT_memory_object(_fd), loaded through SDL: the prototypes only give the types, and
// nothing links libGL.
#define REFRACT_GL_FUNCTIONS(X) \
    X(GetString) X(GetError) X(Viewport) X(ClearColor) X(Clear) X(Enable) X(Disable) X(BlendFunc) X(ClipControl) \
    X(CreateTextures) X(DeleteTextures) X(TextureStorage3D) X(TextureSubImage3D) X(GetTextureSubImage) \
    X(BindTextureUnit) X(TextureStorage2D) X(TextureSubImage2D) X(CopyImageSubData) X(PixelStorei) X(CreateMemoryObjectsEXT) X(DeleteMemoryObjectsEXT) \
    X(MemoryObjectParameterivEXT) X(ImportMemoryFdEXT) X(TextureStorageMem2DEXT) X(FenceSync) X(ClientWaitSync) \
    X(DeleteSync) X(CreateShader) X(ShaderSource) X(CompileShader) X(GetShaderiv) X(GetShaderInfoLog) \
    X(DeleteShader) X(CreateProgram) X(AttachShader) X(LinkProgram) X(GetProgramiv) X(GetProgramInfoLog) \
    X(UseProgram) X(GetUniformLocation) X(ProgramUniform4fv) X(CreateVertexArrays) X(BindVertexArray) X(DrawArrays) \
    X(CreateSamplers) X(SamplerParameteri) X(BindSampler)
struct Gl {
#define REFRACT_GL_MEMBER(name) decltype(&::gl##name) name = nullptr;
    REFRACT_GL_FUNCTIONS(REFRACT_GL_MEMBER)
#undef REFRACT_GL_MEMBER
    bool load()
    {
        bool ok = true;
#define REFRACT_GL_LOAD(name) \
        name = reinterpret_cast<decltype(name)>(SDL_GL_GetProcAddress("gl" #name)); \
        if (!name) { std::fprintf(stderr, "viewer: OpenGL function gl" #name " is missing\n"); ok = false; }
        REFRACT_GL_FUNCTIONS(REFRACT_GL_LOAD)
#undef REFRACT_GL_LOAD
        return ok;
    }
} gl;

// A composite frame's layout (AXRI v10): the slot's layers are its two atlas textures.
struct Composite {
    proto::ImageProjection projection{};
    proto::CompositeHeader header{};
    std::vector<proto::CompositeQuad> quads;  // Back to front.
};

// One image of a kind (the game's eyes, or its UI panels), in a ring of texture arrays (one layer per eye): the
// receive thread fills one entry while the window shows another. Entries are replaced only on the receive thread.
struct Entry {
    GLuint texture = 0;
    GLsync drawn = nullptr;  // The window's last draw from this entry; a new copy waits for it.
};
struct Slot {
    std::array<Entry, 3> entries;
    uint32_t width = 0, height = 0, layers = 0;
    int latest = -1, shown = -1;  // Newest complete entry; entry the window draws.
    bool flip = false;            // Read-back eyes arrive bottom-up; shared textures and UI panels top-down.
    bool valid = false;
    uint64_t updates = 0;
    Clock::time_point at{};
    std::shared_ptr<const Composite> composite;  // Set: draw the scene region and panels (see draw_composite).
};

struct Shared {
    std::mutex mutex;
    Slot scene, panels;
    bool wantPanels = false;
    std::atomic<uint64_t> sceneFrames{0};
    std::atomic<bool> connected{false}, gpu{false};
    std::atomic<float> copyMs{0};
    Uint32 frameEvent = 0;
    // Milliseconds between scene frames for the F1 graph, ring buffer (guarded by mutex).
    std::array<float, 240> intervals{};
    size_t intervalCount = 0, intervalNext = 0;
    Clock::time_point lastScene{};
} g_shared;

volatile std::sig_atomic_t g_screenshotSignal = 0;

// Runs on the receive thread, with its own GL context (shared with the window's).
class Receiver {
public:
    bool on_frame(const proto::ImageFrameHeader& header, const proto::ImageProjection& projection, std::vector<uint8_t>&& payload)
    {
        g_shared.connected = true;
        const bool mixed = proto::mixed_gpu_version(header.version);
        const bool quads = header.version == proto::kQuadImageFrameVersion || header.version == proto::kQuadGpuFrameVersion ||
            header.version == proto::kMixedQuadGpuFrameVersion || header.version == proto::kQuadVideoImageFrameVersion;
        const bool composite = header.version == proto::kCompositeGpuFrameVersion;
        const bool gpu = header.version == proto::kWindowsGpuFrameVersion || header.version == proto::kQuadGpuFrameVersion || mixed || composite;
        if (proto::video_version(header.version)) return reject(header, "H.264 frames are not supported here; unset debug.refract.video");
        g_shared.gpu = gpu;
        if (!quads) ++g_shared.sceneFrames;

        if (mixed && (header.reserved & 0xffff) == 0) batchHasScene_ = !quads;
        if (quads) {
            // Only one panel is shown: the first quad of a batch (after the scene, if the batch has one).
            if (mixed && (header.reserved & 0xffff) != (batchHasScene_ ? 1u : 0u)) return true;
            std::lock_guard lock(g_shared.mutex);
            const bool sceneStale = !g_shared.scene.valid || Clock::now() - g_shared.scene.at > std::chrono::seconds(1);
            if (!g_shared.wantPanels && !sceneStale) return true;  // Not displayed: nothing to read.
        }

        const auto copyStart = Clock::now();
        Slot& slot = quads ? g_shared.panels : g_shared.scene;
        std::shared_ptr<Composite> layout;
        const Opened* textures = nullptr;
        if (gpu) {
            proto::WindowsGpuFrame frame{};
            std::memcpy(&frame, payload.data(), sizeof(frame));
            if (composite) {  // The transport validated the table.
                layout = std::make_shared<Composite>();
                layout->projection = projection;
                std::memcpy(&layout->header, payload.data() + sizeof(frame), sizeof(layout->header));
                layout->quads.resize(layout->header.quad_count);
                if (!layout->quads.empty())
                    std::memcpy(layout->quads.data(), payload.data() + sizeof(frame) + sizeof(layout->header),
                                layout->quads.size() * sizeof(proto::CompositeQuad));
            }
            textures = open_shared(frame.session);
            if (!textures) return reject(header, "shared textures unavailable");
            if (textures->width != header.width || textures->height != header.height)
                return reject(header, "shared texture is %ux%u", textures->width, textures->height);
        }

        const uint32_t layers = gpu ? 2 : std::min<uint32_t>(header.layers, 2);
        int index = -1;
        GLsync drawn = nullptr;
        {
            std::lock_guard lock(g_shared.mutex);
            if (!ensure_ring(slot, header.width, header.height)) return reject(header, "cannot allocate %ux%u textures", header.width, header.height);
            for (int i = 0; i < int(slot.entries.size()) && index < 0; ++i)
                if (i != slot.latest && i != slot.shown) index = i;
            std::swap(drawn, slot.entries[index].drawn);
        }
        if (drawn) {  // The window's last draw from this entry must be done before it is overwritten.
            gl.ClientWaitSync(drawn, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            gl.DeleteSync(drawn);
        }
        const GLuint target = slot.entries[index].texture;
        if (gpu) {
            for (GLint eye = 0; eye < 2; ++eye)
                gl.CopyImageSubData(textures->texture[eye], GL_TEXTURE_2D, 0, 0, 0, 0, target, GL_TEXTURE_2D_ARRAY, 0, 0, 0, eye,
                                    GLsizei(header.width), GLsizei(header.height), 1);
        } else {
            gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
            gl.TextureSubImage3D(target, 0, 0, 0, 0, GLsizei(header.width), GLsizei(header.height), GLsizei(layers),
                                 GL_RGBA, GL_UNSIGNED_BYTE, payload.data());
        }
        // The game may overwrite its shared textures once the frame is acknowledged, so the copy must be finished.
        // Under the runtime's 3 s acknowledgment timeout, but past ordinary hitches (a failed copy drops the stream).
        const GLsync copied = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        const GLenum waited = gl.ClientWaitSync(copied, GL_SYNC_FLUSH_COMMANDS_BIT, 2500000000ull);
        gl.DeleteSync(copied);
        if (waited != GL_ALREADY_SIGNALED && waited != GL_CONDITION_SATISFIED)
            return reject(header, "GPU copy did not complete within 2.5 s");

        {
            std::lock_guard lock(g_shared.mutex);
            slot.latest = index;
            slot.layers = layers;
            slot.flip = !gpu && !quads;
            slot.composite = std::move(layout);
            slot.valid = true;
            slot.at = Clock::now();
            ++slot.updates;
            if (!quads) {
                if (g_shared.lastScene != Clock::time_point{}) {
                    g_shared.intervals[g_shared.intervalNext] = std::chrono::duration<float, std::milli>(slot.at - g_shared.lastScene).count();
                    g_shared.intervalNext = (g_shared.intervalNext + 1) % g_shared.intervals.size();
                    g_shared.intervalCount = std::min(g_shared.intervalCount + 1, g_shared.intervals.size());
                }
                g_shared.lastScene = slot.at;
                const float ms = std::chrono::duration<float, std::milli>(slot.at - copyStart).count();
                const float average = g_shared.copyMs;
                g_shared.copyMs = average ? average * 0.95f + ms * 0.05f : ms;
            }
        }
        SDL_Event wake{};
        wake.type = g_shared.frameEvent;
        SDL_PushEvent(&wake);
        return true;
    }

private:
    // Rejected GPU frames are acknowledged as UINT64_MAX, which makes the runtime drop the connection.
    template <typename... Args>
    static bool reject(const proto::ImageFrameHeader& header, const char* reason, Args... args)
    {
        static std::atomic<int> reported{0};
        if (reported++ < 20) {
            std::fprintf(stderr, "viewer: rejected frame seq=%llu %ux%u version=%u part=%u: ",
                static_cast<unsigned long long>(header.sequence), header.width, header.height, header.version, header.reserved);
            std::fprintf(stderr, reason, args...);
            std::fputc('\n', stderr);
        }
        return false;
    }

    // Called with g_shared.mutex held.
    static bool ensure_ring(Slot& slot, uint32_t width, uint32_t height)
    {
        if (slot.entries[0].texture && slot.width == width && slot.height == height) return true;
        for (auto& entry : slot.entries) {
            if (entry.drawn) gl.DeleteSync(entry.drawn);
            if (entry.texture) gl.DeleteTextures(1, &entry.texture);
            entry = {};
        }
        slot.latest = slot.shown = -1;
        slot.valid = false;
        slot.width = width;
        slot.height = height;
        for (auto& entry : slot.entries) {
            gl.CreateTextures(GL_TEXTURE_2D_ARRAY, 1, &entry.texture);
            gl.TextureStorage3D(entry.texture, 1, GL_RGBA8, GLsizei(width), GLsizei(height), 2);
        }
        return gl.GetError() == GL_NO_ERROR;
    }

    struct Opened {
        GLuint memory[2]{}, texture[2]{};
        uint32_t width = 0, height = 0;
        Clock::time_point used{};
    };
    static void close_shared(Opened& opened)
    {
        gl.DeleteTextures(2, opened.texture);
        gl.DeleteMemoryObjectsEXT(2, opened.memory);
    }

    // Each id is one slot of the runtime's export ring. The GPU layer frees a pair after 10 s unused and may later
    // create a new one under the same id, so pairs unused for 5 s are dropped here and opened again when needed.
    const Opened* open_shared(uint64_t session)
    {
        const auto now = Clock::now();
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (it->first != session && now - it->second.used > std::chrono::seconds(5)) {
                close_shared(it->second);
                it = sessions_.erase(it);
            } else ++it;
        }
        auto found = sessions_.find(session);
        if (found != sessions_.end()) { found->second.used = now; return &found->second; }
        Opened opened;
        opened.used = now;
        gl.CreateMemoryObjectsEXT(2, opened.memory);
        gl.CreateTextures(GL_TEXTURE_2D, 2, opened.texture);
        for (uint32_t eye = 0; eye < 2; ++eye) {
            proto::LinuxGpuShareReply reply;
            const int fd = proto::request_linux_gpu_share(session, eye, reply);
            if (fd < 0) {
                std::fprintf(stderr, "viewer: the emulator's GPU layer did not share texture %016llx/%u (status %d); "
                             "was the emulator started with it (tools/linux_android_emulator.sh)?\n",
                             static_cast<unsigned long long>(session), eye, reply.status);
                close_shared(opened);
                return nullptr;
            }
            const GLint dedicated = GL_TRUE;  // The layer allocates each image on its own.
            gl.MemoryObjectParameterivEXT(opened.memory[eye], GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
            gl.ImportMemoryFdEXT(opened.memory[eye], reply.size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);  // GL owns fd now.
            // VK_FORMAT_R8G8B8A8_SRGB (43) or _UNORM (37). Copies into the ring keep the bytes as they are.
            gl.TextureStorageMem2DEXT(opened.texture[eye], 1, reply.format == 43 ? GL_SRGB8_ALPHA8 : GL_RGBA8,
                                      GLsizei(reply.width), GLsizei(reply.height), opened.memory[eye], 0);
            opened.width = reply.width;
            opened.height = reply.height;
        }
        if (const GLenum error = gl.GetError(); error != GL_NO_ERROR) {
            std::fprintf(stderr, "viewer: importing shared textures failed (GL error 0x%x)\n", error);
            close_shared(opened);
            return nullptr;
        }
        if (sessions_.size() >= 32) {
            for (auto& [id, old] : sessions_) close_shared(old);
            sessions_.clear();
        }
        std::fprintf(stderr, "viewer: opened shared textures for session %016llx (%ux%u)\n",
                     static_cast<unsigned long long>(session), opened.width, opened.height);
        return &sessions_.emplace(session, opened).first->second;
    }

    std::map<uint64_t, Opened> sessions_;
    bool batchHasScene_ = false;
};

const char kSceneVertex[] = R"(#version 450 core
out vec2 uv;
void main() {
    uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(uv * vec2(2, -2) + vec2(-1, 1), 0, 1);
}
)";
// uv (0, 0) is the window's top left, and texture row 0 is the image's top (as on Direct3D).
const char kSceneFragment[] = R"(#version 450 core
layout(binding = 0) uniform sampler2DArray image;
uniform vec4 params;  // slice, flip, uv scale
in vec2 uv;
out vec4 color;
void main() {
    vec2 t = uv;
    if (params.y > 0.5) t.y = 1 - t.y;
    color = vec4(texture(image, vec3(t * params.zw, params.x)).rgb, 1);
}
)";
// A composite frame's UI panel: one quad whose corners arrive in clip space, textured from its atlas rectangle,
// blended as OpenXR composites quad layers (premultiplied by default).
const char kPanelVertex[] = R"(#version 450 core
uniform vec4 corners[4];
uniform vec4 uvRect;
out vec2 uv;
void main() {
    gl_Position = corners[gl_VertexID];
    uv = mix(uvRect.xy, uvRect.zw, vec2(gl_VertexID & 1, gl_VertexID >> 1));
}
)";
const char kPanelFragment[] = R"(#version 450 core
layout(binding = 0) uniform sampler2DArray image;
uniform vec4 uvClamp;
uniform vec4 flags;  // slice, blend, unpremultiplied
in vec2 uv;
out vec4 color;
void main() {
    vec4 c = texture(image, vec3(clamp(uv, uvClamp.xy, uvClamp.zw), flags.x));
    if (flags.y < 0.5) c.a = 1;
    else if (flags.z > 0.5) c.rgb *= c.a;
    color = c;
}
)";

// The F1 overlay: an RGBA image drawn at a pixel rectangle of the window (top-left origin), alpha-blended.
const char kOverlayVertex[] = R"(#version 450 core
uniform vec4 rect;  // x, y, width, height in clip space, y up
out vec2 uv;
void main() {
    vec2 corner = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    gl_Position = vec4(rect.xy + corner * rect.zw, 0, 1);
    uv = vec2(corner.x, 1 - corner.y);
}
)";
const char kOverlayFragment[] = R"(#version 450 core
layout(binding = 0) uniform sampler2D image;
in vec2 uv;
out vec4 color;
void main() { color = texture(image, uv); }
)";

GLuint compile_program(const char* vertex, const char* fragment)
{
    const GLuint program = gl.CreateProgram();
    for (const auto& [type, source] : {std::pair{GL_VERTEX_SHADER, vertex}, std::pair{GL_FRAGMENT_SHADER, fragment}}) {
        const GLuint shader = gl.CreateShader(type);
        gl.ShaderSource(shader, 1, &source, nullptr);
        gl.CompileShader(shader);
        GLint ok = 0;
        gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024]{};
            gl.GetShaderInfoLog(shader, sizeof(log), nullptr, log);
            std::fprintf(stderr, "viewer: shader: %s\n", log);
            return 0;
        }
        gl.AttachShader(program, shader);
        gl.DeleteShader(shader);
    }
    gl.LinkProgram(program);
    GLint ok = 0;
    gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]{};
        gl.GetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "viewer: program: %s\n", log);
        return 0;
    }
    return program;
}

struct Renderer {
    GLuint scene = 0, panel = 0, overlay = 0, vertexArray = 0, sampler = 0;
    GLint sceneParams = -1, panelCorners = -1, panelUvRect = -1, panelUvClamp = -1, panelFlags = -1, overlayRect = -1;
    bool hidePanels = false;  // F7: composite frames show the scene alone.
    bool create()
    {
        scene = compile_program(kSceneVertex, kSceneFragment);
        panel = compile_program(kPanelVertex, kPanelFragment);
        overlay = compile_program(kOverlayVertex, kOverlayFragment);
        if (!scene || !panel || !overlay) return false;
        overlayRect = gl.GetUniformLocation(overlay, "rect");
        sceneParams = gl.GetUniformLocation(scene, "params");
        panelCorners = gl.GetUniformLocation(panel, "corners");
        panelUvRect = gl.GetUniformLocation(panel, "uvRect");
        panelUvClamp = gl.GetUniformLocation(panel, "uvClamp");
        panelFlags = gl.GetUniformLocation(panel, "flags");
        gl.CreateVertexArrays(1, &vertexArray);
        gl.CreateSamplers(1, &sampler);
        gl.SamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.SamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.SamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.SamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Panel corners are computed as for Direct3D (depth 0..1), like viewer.cpp.
        gl.ClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        return gl.GetError() == GL_NO_ERROR;
    }
};

struct Vec3 { float x, y, z; };
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
// Rotates v by the unit quaternion (qx, qy, qz, qw); `inverse` rotates by its conjugate.
Vec3 rotate(const proto::Pose& q, Vec3 v, bool inverse = false)
{
    const Vec3 u{inverse ? -q.qx : q.qx, inverse ? -q.qy : q.qy, inverse ? -q.qz : q.qz};
    return v + cross(u, cross(u, v) + v * q.qw) * 2.0f;
}

// Width / height of an eye's field of view.
float view_aspect(const proto::ImageProjectionView& view)
{
    return (std::tan(view.angle_right) - std::tan(view.angle_left)) / (std::tan(view.angle_up) - std::tan(view.angle_down));
}

// Draws one eye of a composite frame into the current viewport: the scene region of the atlas, then every panel
// that eye sees, projected with the eye's pose and field of view. The scene program is bound on entry and return.
void draw_composite(Renderer& r, const Slot& slot, const Composite& composite, uint32_t eye)
{
    const auto& header = composite.header;
    if (header.scene_width) {
        const float params[4] = {float(eye), 0, float(header.scene_width) / slot.width, float(header.scene_height) / slot.height};
        gl.ProgramUniform4fv(r.scene, r.sceneParams, 1, params);
        gl.DrawArrays(GL_TRIANGLES, 0, 3);
    }
    if (composite.quads.empty() || r.hidePanels) return;
    const auto& view = composite.projection.views[eye];
    const Vec3 eyePosition{view.pose.x, view.pose.y, view.pose.z};
    const float left = std::tan(view.angle_left), right = std::tan(view.angle_right);
    const float up = std::tan(view.angle_up), down = std::tan(view.angle_down);
    constexpr float kNear = 0.01f, kFar = 1000.0f;
    gl.UseProgram(r.panel);
    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    for (const auto& panel : composite.quads) {
        const auto& quad = panel.quad;
        if (quad.eye_visibility && quad.eye_visibility != eye + 1) continue;  // 1 = left only, 2 = right only.
        float corners[4][4]{};
        const Vec3 center{quad.pose.x, quad.pose.y, quad.pose.z};
        bool inFront = false;
        for (int corner = 0; corner < 4; ++corner) {
            // Top-left, top-right, bottom-left, bottom-right: image row 0 is the panel's top (+Y).
            const Vec3 local{(corner & 1 ? 0.5f : -0.5f) * quad.width, (corner & 2 ? -0.5f : 0.5f) * quad.height, 0};
            const Vec3 world = center + rotate(quad.pose, local);
            const Vec3 e = rotate(view.pose, world - eyePosition, true);  // OpenXR eye space: -Z forward.
            const float w = -e.z;
            inFront = inFront || w > kNear;
            float* out = corners[corner];
            out[0] = (2 * e.x - w * (right + left)) / (right - left);
            out[1] = (2 * e.y - w * (up + down)) / (up - down);
            out[2] = (w - kNear) * kFar / (kFar - kNear);
            out[3] = w;
        }
        if (!inFront) continue;
        const float texelU = 1.0f / slot.width, texelV = 1.0f / slot.height;
        float uvRect[4] = {panel.x * texelU, panel.y * texelV, (panel.x + panel.width) * texelU, (panel.y + panel.height) * texelV};
        // Half a texel in, so filtering never reads a neighbouring panel.
        const float uvClamp[4] = {uvRect[0] + texelU / 2, uvRect[1] + texelV / 2, uvRect[2] - texelU / 2, uvRect[3] - texelV / 2};
        if (panel.texture & proto::kCompositeQuadFlipped) std::swap(uvRect[1], uvRect[3]);  // Rows bottom-up.
        const float flags[4] = {float(panel.texture & 1),
                                (quad.layer_flags & 2) ? 1.0f : 0.0f,   // XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
                                (quad.layer_flags & 4) ? 1.0f : 0.0f,   // XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT
                                0};
        gl.ProgramUniform4fv(r.panel, r.panelCorners, 4, &corners[0][0]);
        gl.ProgramUniform4fv(r.panel, r.panelUvRect, 1, uvRect);
        gl.ProgramUniform4fv(r.panel, r.panelUvClamp, 1, uvClamp);
        gl.ProgramUniform4fv(r.panel, r.panelFlags, 1, flags);
        gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    gl.Disable(GL_BLEND);
    gl.UseProgram(r.scene);
}

// The image's place in a cell of the window (top-left origin), keeping its aspect ratio.
struct Rect { float x, y, width, height; };
Rect fit(float x, float width, float height, float aspect)
{
    const float w = std::min(width, height * aspect), h = w / aspect;
    return {x + (width - w) / 2, (height - h) / 2, w, h};
}

// Saves the shown entry's first layer (the scene region of a composite frame) as a BMP.
void save_screenshot(const Slot& slot, GLuint texture, const std::string& folder, bool flipOverride)
{
    if (folder.empty() || !texture) return;
    uint32_t width = slot.width, height = slot.height;
    if (slot.composite && slot.composite->header.scene_width) {
        width = slot.composite->header.scene_width;
        height = slot.composite->header.scene_height;
    }
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.GetTextureSubImage(texture, 0, 0, 0, 0, GLsizei(width), GLsizei(height), 1, GL_RGBA, GL_UNSIGNED_BYTE,
                          GLsizei(pixels.size()), pixels.data());
    if (slot.flip != flipOverride)
        for (uint32_t y = 0; y < height / 2; ++y)
            std::swap_ranges(pixels.begin() + size_t(y) * width * 4, pixels.begin() + size_t(y + 1) * width * 4,
                             pixels.begin() + size_t(height - 1 - y) * width * 4);
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    SDL_Surface* surface = SDL_CreateSurfaceFrom(int(width), int(height), SDL_PIXELFORMAT_RGBA32, pixels.data(), int(width * 4));
    if (!surface) return;
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    char name[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(name, sizeof(name), "refract-%Y%m%d-%H%M%S.bmp", std::localtime(&now));
    const std::string path = (std::filesystem::path(folder) / name).string();
    if (SDL_SaveBMP(surface, path.c_str())) std::fprintf(stderr, "viewer: saved %s\n", path.c_str());
    else std::fprintf(stderr, "viewer: screenshot failed: %s\n", SDL_GetError());
    SDL_DestroySurface(surface);
}

// The F1 performance overlay, as viewer.cpp draws it: drawn on the CPU with SDL's built-in 8x8 font into an RGBA
// surface (at most 30 times a second), then shown over the image as one blended quad, pixel for pixel.
class Overlay {
public:
    bool visible = true;
    float gameFps = 0, shownFps = 0;
    AndroidStats* android = nullptr;

    // Call with g_shared.mutex held: reads the frame intervals and the shown slot.
    void update(const Slot& slot, int outputHeight)
    {
        const int scale = outputHeight >= 1000 ? 2 : 1;
        const auto now = Clock::now();
        if (scale == scale_ && surface_ && now - updatedAt_ < std::chrono::milliseconds(33)) return;
        updatedAt_ = now;
        if (scale != scale_ || !surface_) {
            if (cpu_) SDL_DestroyRenderer(cpu_);
            if (surface_) SDL_DestroySurface(surface_);
            if (texture_) gl.DeleteTextures(1, &texture_);
            scale_ = scale;
            surface_ = SDL_CreateSurface(kWidth * scale, kHeight * scale, SDL_PIXELFORMAT_RGBA32);
            cpu_ = surface_ ? SDL_CreateSoftwareRenderer(surface_) : nullptr;
            if (!cpu_) return;
            SDL_SetRenderScale(cpu_, float(scale), float(scale));
            gl.CreateTextures(GL_TEXTURE_2D, 1, &texture_);
            gl.TextureStorage2D(texture_, 1, GL_RGBA8, kWidth * scale, kHeight * scale);
        }
        if (!cpu_) return;

        std::vector<float> frames;  // Oldest first.
        for (size_t i = 0; i < g_shared.intervalCount; ++i)
            frames.push_back(g_shared.intervals[(g_shared.intervalNext + g_shared.intervals.size() - g_shared.intervalCount + i) % g_shared.intervals.size()]);
        float average = 0, worst = 0, low = 0;
        if (!frames.empty()) {
            std::vector<float> sorted = frames;
            std::sort(sorted.begin(), sorted.end());
            for (float f : frames) average += f;
            average /= frames.size();
            worst = sorted.back();
            low = sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)];  // 99th percentile frame time.
        }
        std::vector<std::string> lines;
        char line[256];
        std::snprintf(line, sizeof(line), "Game     %5.0f fps   %5.1f ms avg   %5.1f ms worst   1%% low %3.0f fps",
                      gameFps, average, worst, low > 0 ? 1000.0f / low : 0.0f);
        lines.push_back(line);
        std::snprintf(line, sizeof(line), "Viewer   %5.0f fps shown   copy %.2f ms   %s", shownFps, g_shared.copyMs.load(),
                      !g_shared.connected ? "waiting for game" : g_shared.gpu ? "GPU shared textures" : "pixel stream");
        lines.push_back(line);
        if (slot.composite)
            std::snprintf(line, sizeof(line), "Render   %u x %u per eye + %zu UI panel(s)", slot.composite->header.scene_width,
                          slot.composite->header.scene_height, slot.composite->quads.size());
        else
            std::snprintf(line, sizeof(line), "Render   %u x %u per eye%s", slot.width, slot.height, &slot == &g_shared.panels ? " (UI panel)" : "");
        lines.push_back(line);
        const AndroidSnapshot stats = android ? android->snapshot() : AndroidSnapshot{};
        if (stats.valid) {
            // Android's /proc/stat: in Waydroid, which shares this PC's kernel, that is the whole PC.
            std::snprintf(line, sizeof(line), "System   CPU %3.0f%% of %d cores (%.1f cores busy)", stats.cpuPercent, stats.cores,
                          stats.cpuPercent * stats.cores / 100.0);
            lines.push_back(line);
            std::string threads = "Threads ";
            for (size_t i = 0; i < std::min<size_t>(4, stats.threads.size()); ++i) {
                std::snprintf(line, sizeof(line), " %s %.0f%%", stats.threads[i].name.c_str(), stats.threads[i].percent);
                threads += line;
            }
            lines.push_back(threads);
        } else {
            lines.push_back("Android  stats unavailable (adb)");
            lines.push_back("");
        }
        lines.push_back("(thread % = share of one core)   F1 hide");

        SDL_SetRenderDrawBlendMode(cpu_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(cpu_, 0, 0, 0, 0);
        SDL_RenderClear(cpu_);
        SDL_SetRenderDrawColor(cpu_, 0, 0, 0, 153);
        SDL_RenderFillRect(cpu_, nullptr);
        SDL_SetRenderDrawBlendMode(cpu_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(cpu_, 255, 255, 255, 255);
        for (size_t i = 0; i < lines.size(); ++i)
            SDL_RenderDebugText(cpu_, kPad, kPad + float(i) * kLine, lines[i].c_str());
        // Frame-time graph, newest on the right; 0-33 ms tall, lines at 90 and 60 fps.
        const float graphTop = kPad + kTextLines * kLine + 6, graphBottom = graphTop + kGraph, scale30 = kGraph / 33.3f;
        const float barWidth = (kWidth - 2 * kPad) / float(g_shared.intervals.size());
        for (size_t i = 0; i < frames.size(); ++i) {
            const float ms = frames[i];
            if (ms <= 11.2f) SDL_SetRenderDrawColor(cpu_, 77, 217, 102, 255);
            else if (ms <= 16.7f) SDL_SetRenderDrawColor(cpu_, 242, 204, 51, 255);
            else SDL_SetRenderDrawColor(cpu_, 242, 77, 64, 255);
            const float left = kPad + (g_shared.intervals.size() - frames.size() + i) * barWidth;
            const float top = std::max(graphTop, graphBottom - ms * scale30);
            const SDL_FRect bar{left, top, barWidth, graphBottom - top};
            SDL_RenderFillRect(cpu_, &bar);
        }
        SDL_SetRenderDrawColor(cpu_, 255, 255, 255, 89);
        for (float ms : {11.1f, 16.7f})
            SDL_RenderLine(cpu_, kPad, graphBottom - ms * scale30, kWidth - kPad, graphBottom - ms * scale30);
        SDL_FlushRenderer(cpu_);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, surface_->pitch / 4);
        gl.TextureSubImage2D(texture_, 0, 0, 0, surface_->w, surface_->h, GL_RGBA, GL_UNSIGNED_BYTE, surface_->pixels);
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    // Draws over the whole window's viewport, 12 pixels from the top-left corner.
    void draw(Renderer& r, int outputWidth, int outputHeight) const
    {
        if (!texture_ || !surface_) return;
        gl.Viewport(0, 0, outputWidth, outputHeight);
        const float width = 2.0f * surface_->w / outputWidth, height = 2.0f * surface_->h / outputHeight;
        const float rect[4] = {-1 + 2.0f * 12 / outputWidth, 1 - 2.0f * 12 / outputHeight - height, width, height};
        gl.UseProgram(r.overlay);
        gl.BindVertexArray(r.vertexArray);
        gl.BindTextureUnit(0, texture_);
        gl.BindSampler(0, 0);
        gl.ProgramUniform4fv(r.overlay, r.overlayRect, 1, rect);
        gl.Enable(GL_BLEND);
        gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        gl.Disable(GL_BLEND);
    }

private:
    static constexpr int kTextLines = 6, kLine = 11, kPad = 8, kGraph = 70;
    static constexpr int kWidth = 74 * 8 + 2 * kPad, kHeight = kPad + kTextLines * kLine + 6 + kGraph + kPad;
    SDL_Surface* surface_ = nullptr;
    SDL_Renderer* cpu_ = nullptr;
    GLuint texture_ = 0;
    int scale_ = 0;
    Clock::time_point updatedAt_{};
};

struct Mouse {
    float yaw = 0, pitch = 0, reach = 0.4f;
    int trigger = 0, grip = 0;
    bool dragging = false;
};

// The keys pose_input_server.py maps, by the names it expects.
const char* key_name(SDL_Scancode code)
{
    switch (code) {
    case SDL_SCANCODE_W: return "w";
    case SDL_SCANCODE_A: return "a";
    case SDL_SCANCODE_S: return "s";
    case SDL_SCANCODE_D: return "d";
    case SDL_SCANCODE_Z: return "z";
    case SDL_SCANCODE_X: return "x";
    case SDL_SCANCODE_M: return "m";
    case SDL_SCANCODE_E: return "e";
    case SDL_SCANCODE_Q: return "q";
    case SDL_SCANCODE_R: return "r";
    case SDL_SCANCODE_F: return "f";
    case SDL_SCANCODE_T: return "t";
    case SDL_SCANCODE_C: return "c";
    case SDL_SCANCODE_SPACE: return "space";
    case SDL_SCANCODE_RETURN: return "enter";
    case SDL_SCANCODE_BACKSPACE: return "backspace";
    case SDL_SCANCODE_TAB: return "tab";
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return "shift";
    case SDL_SCANCODE_HOME: return "home";
    case SDL_SCANCODE_LEFT: return "left";
    case SDL_SCANCODE_RIGHT: return "right";
    case SDL_SCANCODE_UP: return "up";
    case SDL_SCANCODE_DOWN: return "down";
    default: return nullptr;
    }
}

std::string held_keys()
{
    int count = 0;
    const bool* state = SDL_GetKeyboardState(&count);
    std::string keys = "keys";
    for (int code = 0; code < count; ++code) {
        const char* name = state[code] ? key_name(static_cast<SDL_Scancode>(code)) : nullptr;
        if (name && keys.find(std::string(" ") + name) == std::string::npos) keys += std::string(" ") + name;
    }
    return keys;
}

}  // namespace

int main(int argc, char** argv)
{
    int port = 38491, controlPort = 38495;
    float sensitivity = 0.15f;
    std::string title = "Refract Viewer", shots, adb, serial, package;
    bool stats = true;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port") port = std::atoi(argv[++i]);
        else if (arg == "--control-port") controlPort = std::atoi(argv[++i]);
        else if (arg == "--shots") shots = argv[++i];
        else if (arg == "--sensitivity") sensitivity = float(std::atof(argv[++i]));
        else if (arg == "--title") title = std::string(argv[++i]) + " \xe2\x80\x93 Refract Viewer";
        else if (arg == "--adb") adb = argv[++i];
        else if (arg == "--serial") serial = argv[++i];
        else if (arg == "--package") package = argv[++i];
        else if (arg == "--stats") stats = std::atoi(argv[++i]) != 0;  // 0: start with the F1 overlay hidden.
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "viewer: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window* window = SDL_CreateWindow(title.c_str(), 1280, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_OPENGL);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : nullptr;
    // The receive thread's context shares textures and fences with the window's.
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    SDL_GLContext copyContext = context ? SDL_GL_CreateContext(window) : nullptr;
    // The receive thread draws into its own hidden window. A context made current without a surface does nothing
    // with Nvidia on Wayland (SDL reports success, EGL has no current context), and the visible window's surface
    // cannot be current in two threads.
    SDL_Window* copyWindow = copyContext ? SDL_CreateWindow("Refract Viewer copy", 1, 1, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN) : nullptr;
    if (!copyWindow || !SDL_GL_MakeCurrent(window, context) || !gl.load()) {
        std::fprintf(stderr, "viewer: cannot open an OpenGL 4.5 window: %s\n", SDL_GetError());
        return 1;
    }
    Renderer renderer;
    if (!renderer.create()) return 1;
    SDL_GL_SetSwapInterval(0);  // Game frames are shown as soon as they arrive (viewer.cpp presents with interval 0).
    std::fprintf(stderr, "viewer: OpenGL %s on %s\n", gl.GetString(GL_VERSION), gl.GetString(GL_RENDERER));
    g_shared.frameEvent = SDL_RegisterEvents(1);

    std::signal(SIGUSR1, [](int) { g_screenshotSignal = 1; });
    std::thread([port, copyWindow, copyContext] {
        if (!SDL_GL_MakeCurrent(copyWindow, copyContext) || !gl.GetString(GL_RENDERER)) {
            std::fprintf(stderr, "viewer: cannot use the copy context: %s\n", SDL_GetError());
            return;
        }
        Receiver receiver;
        proto::TcpImageServer server;
        server.serve_with_callback(static_cast<uint16_t>(port), 0,
            [&](const proto::ImageFrameHeader& header, const proto::ImageProjection& projection, std::vector<uint8_t>&& payload) {
                return receiver.on_frame(header, projection, std::move(payload));
            });
        std::fprintf(stderr, "viewer: image server stopped (port %d busy?)\n", port);
    }).detach();

    const int control = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in controlAddress{};
    controlAddress.sin_family = AF_INET;
    controlAddress.sin_port = htons(static_cast<uint16_t>(controlPort));
    inet_pton(AF_INET, "127.0.0.1", &controlAddress.sin_addr);
    const auto send_control = [&](const std::string& text) {
        if (control >= 0) sendto(control, text.data(), text.size(), 0, reinterpret_cast<sockaddr*>(&controlAddress), sizeof(controlAddress));
    };

    Overlay overlay;
    overlay.visible = stats;
    std::unique_ptr<AndroidStats> android;
    if (!adb.empty() && !serial.empty() && !package.empty()) {
        android = std::make_unique<AndroidStats>(adb, serial, package);
        android->enabled = overlay.visible;
        android->start();
        overlay.android = android.get();
    }

    Mouse mouse;
    bool both = false, flipOverride = false, closed = false, fullscreen = false, saveRequested = false, redraw = true;
    uint64_t drawnUpdates = UINT64_MAX;
    std::string sentView, sentKeys;
    auto sentAt = Clock::now(), statsAt = Clock::now(), loggedAt = Clock::now(), overlayDrawnAt = Clock::now();
    uint32_t presented = 0, presentedLogged = 0;
    uint64_t sceneLogged = 0;

    while (!closed) {
        SDL_Event event;
        bool any = SDL_WaitEventTimeout(&event, 100);  // New frames wake it (g_shared.frameEvent).
        while (any) {
            switch (event.type) {
            // The hidden copy window stays open, so SDL would not send QUIT for the visible one closing.
            case SDL_EVENT_QUIT: case SDL_EVENT_WINDOW_CLOSE_REQUESTED: closed = true; break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: case SDL_EVENT_WINDOW_EXPOSED: redraw = true; break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                if (event.button.button == SDL_BUTTON_LEFT) mouse.trigger = down;
                if (event.button.button == SDL_BUTTON_MIDDLE) mouse.grip = down;
                if (event.button.button == SDL_BUTTON_RIGHT) {
                    mouse.dragging = down;
                    SDL_SetWindowRelativeMouseMode(window, down);
                }
                break;
            }
            case SDL_EVENT_MOUSE_MOTION:
                if (mouse.dragging) {
                    mouse.yaw = std::fmod(mouse.yaw - event.motion.xrel * sensitivity + 540.0f, 360.0f) - 180.0f;
                    mouse.pitch = std::clamp(mouse.pitch - event.motion.yrel * sensitivity, -80.0f, 80.0f);
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:  // Scroll up pushes the hands away, down pulls them in.
                mouse.reach = std::clamp(mouse.reach + event.wheel.y * 0.05f, 0.1f, 1.0f);
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                mouse.trigger = mouse.grip = 0;
                mouse.dragging = false;
                SDL_SetWindowRelativeMouseMode(window, false);
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.repeat) break;
                switch (event.key.scancode) {
                case SDL_SCANCODE_F1:
                    overlay.visible = !overlay.visible;
                    if (android) android->enabled = overlay.visible;
                    break;
                case SDL_SCANCODE_F2: saveRequested = true; break;
                case SDL_SCANCODE_F3: both = !both; break;
                case SDL_SCANCODE_F5: flipOverride = !flipOverride; break;
                case SDL_SCANCODE_F6: { std::lock_guard lock(g_shared.mutex); g_shared.wantPanels = !g_shared.wantPanels; } break;
                case SDL_SCANCODE_F7: renderer.hidePanels = !renderer.hidePanels; break;
                case SDL_SCANCODE_HOME: mouse.yaw = mouse.pitch = 0; break;
                case SDL_SCANCODE_ESCAPE: if (!fullscreen) break; [[fallthrough]];
                case SDL_SCANCODE_F11: fullscreen = !fullscreen; SDL_SetWindowFullscreen(window, fullscreen); break;
                default: break;
                }
                redraw = true;
                break;
            default: break;
            }
            any = SDL_PollEvent(&event);
        }

        // Mouse and keys to pose_input_server.py; resent twice a second so a restarted server picks them up.
        const auto now = Clock::now();
        char view[96];
        std::snprintf(view, sizeof(view), "view %.2f %.2f %d %d %.2f", mouse.yaw, mouse.pitch, mouse.trigger, mouse.grip, mouse.reach);
        const std::string keys = (SDL_GetKeyboardFocus() == window) ? held_keys() : "keys";
        const bool periodic = now - sentAt > std::chrono::milliseconds(500);
        if (sentView != view || periodic) send_control(sentView = view);
        if (sentKeys != keys || periodic) send_control(sentKeys = keys);
        if (periodic) sentAt = now;
        if (g_screenshotSignal) {
            g_screenshotSignal = 0;
            saveRequested = redraw = true;
        }

        // Draws the newest image. A scene that stopped (a menu shown only as panels) gives way to the panels after
        // a second; F6 prefers the panels.
        {
            std::lock_guard lock(g_shared.mutex);
            const bool sceneStale = !g_shared.scene.valid || now - g_shared.scene.at > std::chrono::seconds(1);
            Slot& slot = (g_shared.wantPanels || sceneStale) && g_shared.panels.valid ? g_shared.panels : g_shared.scene;
            const uint64_t updates = g_shared.scene.updates + g_shared.panels.updates;
            // The overlay's numbers move without new frames too (waiting, Android's CPU).
            if (overlay.visible && now - overlayDrawnAt > std::chrono::milliseconds(250)) redraw = true;
            if (redraw || updates != drawnUpdates) {
                int outputWidth = 0, outputHeight = 0;
                SDL_GetWindowSizeInPixels(window, &outputWidth, &outputHeight);
                gl.Viewport(0, 0, outputWidth, outputHeight);
                gl.ClearColor(0.05f, 0.05f, 0.06f, 1);
                gl.Clear(GL_COLOR_BUFFER_BIT);
                if (slot.valid && slot.latest >= 0) {
                    slot.shown = slot.latest;
                    Entry& entry = slot.entries[slot.shown];
                    gl.UseProgram(renderer.scene);
                    gl.BindVertexArray(renderer.vertexArray);
                    gl.BindTextureUnit(0, entry.texture);
                    gl.BindSampler(0, renderer.sampler);
                    const uint32_t count = both && slot.layers >= 2 ? 2 : 1;
                    const Composite* composite = slot.composite.get();
                    const float aspect = !composite ? float(slot.width) / float(slot.height) :
                        composite->header.scene_width ? float(composite->header.scene_width) / float(composite->header.scene_height) :
                        view_aspect(composite->projection.views[0]);
                    for (uint32_t i = 0; i < count; ++i) {
                        const float cellWidth = float(outputWidth) / count;
                        const Rect cell = fit(cellWidth * i, cellWidth, float(outputHeight), aspect);
                        gl.Viewport(GLint(cell.x), GLint(float(outputHeight) - cell.y - cell.height), GLsizei(cell.width), GLsizei(cell.height));
                        if (composite) {
                            draw_composite(renderer, slot, *composite, i);
                            continue;
                        }
                        const float params[4] = {float(i), (slot.flip != flipOverride) ? 1.0f : 0.0f, 1, 1};
                        gl.ProgramUniform4fv(renderer.scene, renderer.sceneParams, 1, params);
                        gl.DrawArrays(GL_TRIANGLES, 0, 3);
                    }
                    if (entry.drawn) gl.DeleteSync(entry.drawn);
                    entry.drawn = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                    if (saveRequested) save_screenshot(slot, entry.texture, shots, flipOverride);
                    ++presented;
                    ++presentedLogged;
                }
                if (overlay.visible) {
                    overlay.update(slot, outputHeight);
                    overlay.draw(renderer, outputWidth, outputHeight);
                    overlayDrawnAt = now;
                }
                saveRequested = redraw = false;
                drawnUpdates = updates;
                SDL_GL_SwapWindow(window);
            }
        }

        // The same numbers in the log every 5 s, for runs nobody watches.
        if (const double seconds = std::chrono::duration<double>(now - loggedAt).count(); seconds >= 5.0) {
            uint64_t sceneUpdates = 0;
            uint32_t width = 0, height = 0, layers = 0;
            {
                std::lock_guard lock(g_shared.mutex);
                sceneUpdates = g_shared.scene.updates;
                width = g_shared.scene.width, height = g_shared.scene.height, layers = g_shared.scene.layers;
            }
            std::fprintf(stderr, "viewer: game %.1f fps (%ux%u x%u, %s, copy %.2f ms), shown %.1f fps\n",
                (sceneUpdates - sceneLogged) / seconds, width, height, layers, g_shared.gpu ? "GPU shared" : "pixels",
                g_shared.copyMs.load(), presentedLogged / seconds);
            sceneLogged = sceneUpdates;
            presentedLogged = 0;
            loggedAt = now;
        }
        const double elapsed = std::chrono::duration<double>(now - statsAt).count();
        if (elapsed >= 1.0) {
            overlay.gameFps = float(g_shared.sceneFrames.exchange(0) / elapsed);
            overlay.shownFps = float(presented / elapsed);
            char text[1024];
            std::snprintf(text, sizeof(text), "%s  |  game %.0f fps, shown %.0f fps (%s)  |  right-drag look, click trigger, "
                "WASD walk, wheel hand distance  |  F1 stats, F2 screenshot, F3 both eyes, F6 scene/panels, F11 fullscreen, "
                "Home recenter", title.c_str(), overlay.gameFps, overlay.shownFps,
                !g_shared.connected ? "waiting for game" : g_shared.gpu ? "GPU shared" : "pixel stream");
            SDL_SetWindowTitle(window, text);
            presented = 0;
            statsAt = now;
        }
    }
    send_control("keys");
    send_control("view 0 0 0 0 0.4");
    if (control >= 0) close(control);
    // The receive thread may still be inside GL with its context current, and SDL_Quit unloads the EGL driver
    // under it (Nvidia then crashes in its exit handlers). Nothing needs tearing down: leave without them.
    std::fflush(nullptr);
    std::_Exit(0);
}
