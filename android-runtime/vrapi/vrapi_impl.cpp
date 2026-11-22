// libvrapiimpl.so: Meta's VrApi (the Oculus Mobile SDK's API, used by Quest games built before OpenXR) on
// top of Refract's OpenXR runtime.
//
// A VrApi game ships Meta's loader, libvrapi.so. Its vrapi_Initialize creates a package context for
// com.oculus.systemdriver, loads com.oculus.systemdriver.DriverLoader there and calls load64[Ext](). On a
// Quest that class loads the system's libvrapiimpl.so and returns a function accessor,
// void* accessor(const char* name), through which the loader resolves every vrapi_* entry point. Refract's
// systemdriver stand-in does the same with this library (DriverLoader.java).
//
// Every call here becomes OpenXR calls into libopenxr_runtime.so from the same package, so VrApi games use
// the same pose stream, GPU image sharing, viewer and host bridge as OpenXR games:
// - vrapi_EnterVrMode/swapchain creation create one OpenXR session on the game's EGL context (or Vulkan
//   device) that lives until vrapi_Shutdown.
// - A VrApi swapchain is an OpenXR swapchain. VrApi games pick the image index themselves; the runtime hands
//   images out in a fixed round-robin order, so at submit each swapchain is cycled (acquire, wait, release,
//   with no rendering) until the image the game submits is the released one. No copies.
// - vrapi_GetPredictedDisplayTime/vrapi_WaitFrame wait on xrWaitFrame (one wait per frame); vrapi_SubmitFrame2
//   becomes xrBeginFrame/xrEndFrame. A projection layer's per-eye FOV comes from its TexCoordsFromTanAngles
//   matrix and its eye poses from the layer's head pose plus the runtime's eye offsets.
// - Cylinder layers (curved menus and HUDs) become flat quads on the middle of their arc.
// - Touch controllers are the runtime's /interaction_profiles/oculus/touch_controller actions.
// - Native engines built on Meta's samples get an EGL config they accept on the emulator (see
//   apply_egl_config_compat).
// Not supported yet: cube, equirect, fisheye and loading-icon layers (skipped), quads drawn through
// unit-square tan-angle matrices, Android surface swapchains (video), hand tracking. Vulkan games are
// implemented but untested.
#include "vrapi_abi.h"

#include "openxr_minimal.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <vector>

#define VRAPI_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

constexpr char kTag[] = "Refract.VrApi";
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kTag, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, kTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kTag, __VA_ARGS__)

// OpenXR structures the runtime's minimal header leaves out.
constexpr auto XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR = static_cast<XrStructureType>(1000008000);
constexpr auto XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR = static_cast<XrStructureType>(1000089000);
constexpr auto XR_TYPE_SPACE_VELOCITY = static_cast<XrStructureType>(43);
constexpr auto XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR = static_cast<XrReferenceSpaceType>(1000426000);
struct XrInstanceCreateInfoAndroidKHR { XrStructureType type; const void* next; void* applicationVM; void* applicationActivity; };
struct XrLoaderInitInfoAndroidKHR { XrStructureType type; const void* next; void* applicationVM; void* applicationContext; };
struct XrGraphicsBindingOpenGLESAndroidKHR { XrStructureType type; const void* next; EGLDisplay display; EGLConfig config; EGLContext context; };
struct XrGraphicsBindingVulkanKHR {
    XrStructureType type; const void* next; VkInstance instance; VkPhysicalDevice physicalDevice; VkDevice device;
    uint32_t queueFamilyIndex; uint32_t queueIndex;
};
struct XrVector3f { float x, y, z; };
struct XrSpaceVelocity { XrStructureType type; void* next; XrFlags64 velocityFlags; XrVector3f linearVelocity; XrVector3f angularVelocity; };

// ---- OpenXR entry points, resolved from the runtime ------------------------------------------------------

#define REFRACT_XR_FUNCTIONS(X) \
    X(xrCreateInstance) X(xrDestroyInstance) X(xrGetSystem) X(xrPollEvent) X(xrCreateSession) X(xrDestroySession) \
    X(xrBeginSession) X(xrEndSession) X(xrRequestExitSession) X(xrEnumerateViewConfigurationViews) X(xrCreateReferenceSpace) \
    X(xrDestroySpace) X(xrLocateSpace) X(xrLocateViews) X(xrCreateSwapchain) X(xrDestroySwapchain) \
    X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) \
    X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrStringToPath) X(xrCreateActionSet) X(xrCreateAction) \
    X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) X(xrCreateActionSpace) X(xrSyncActions) \
    X(xrGetActionStateBoolean) X(xrGetActionStateFloat) X(xrGetActionStateVector2f) X(xrApplyHapticFeedback) \
    X(xrStopHapticFeedback) X(xrGetOpenGLESGraphicsRequirementsKHR) X(xrGetVulkanGraphicsRequirementsKHR) \
    X(xrEnumerateDisplayRefreshRatesFB) X(xrGetDisplayRefreshRateFB) X(xrRequestDisplayRefreshRateFB)

struct XrFunctions {
#define REFRACT_DECLARE(name) PFN_xrVoidFunction name = nullptr;
    REFRACT_XR_FUNCTIONS(REFRACT_DECLARE)
#undef REFRACT_DECLARE
};

template <typename Signature, typename... Args>
XrResult call(PFN_xrVoidFunction function, Args... args) {
    if (!function) return XR_ERROR_FUNCTION_UNSUPPORTED;
    return reinterpret_cast<Signature>(function)(args...);
}

// ---- Math (VrApi matrices are row-major with column vectors) ----------------------------------------------

XrPosef identity_pose() { XrPosef p{}; p.orientation.w = 1.0f; return p; }

XrPosef multiply(const XrPosef& a, const XrPosef& b) {  // a * b: b expressed in a's frame
    const auto& q = a.orientation; const auto& r = b.orientation;
    XrPosef out;
    out.orientation = {q.w * r.x + q.x * r.w + q.y * r.z - q.z * r.y, q.w * r.y - q.x * r.z + q.y * r.w + q.z * r.x,
                       q.w * r.z + q.x * r.y - q.y * r.x + q.z * r.w, q.w * r.w - q.x * r.x - q.y * r.y - q.z * r.z};
    const float vx = b.position.x, vy = b.position.y, vz = b.position.z;
    const float tx = 2 * (q.y * vz - q.z * vy), ty = 2 * (q.z * vx - q.x * vz), tz = 2 * (q.x * vy - q.y * vx);
    out.position = {vx + q.w * tx + (q.y * tz - q.z * ty) + a.position.x, vy + q.w * ty + (q.z * tx - q.x * tz) + a.position.y,
                    vz + q.w * tz + (q.x * ty - q.y * tx) + a.position.z};
    return out;
}

XrPosef inverse(const XrPosef& p) {
    XrPosef rotation = identity_pose();
    rotation.orientation = {-p.orientation.x, -p.orientation.y, -p.orientation.z, p.orientation.w};
    XrPosef translation = identity_pose();
    translation.position = {-p.position.x, -p.position.y, -p.position.z};
    return multiply(rotation, translation);
}

ovrPosef to_ovr(const XrPosef& p) {
    return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, {p.position.x, p.position.y, p.position.z}};
}

XrPosef to_xr(const ovrPosef& p) {
    XrPosef out;
    out.orientation = {p.Orientation.x, p.Orientation.y, p.Orientation.z, p.Orientation.w};
    out.position = {p.Position.x, p.Position.y, p.Position.z};
    const float length = std::sqrt(p.Orientation.x * p.Orientation.x + p.Orientation.y * p.Orientation.y +
                                   p.Orientation.z * p.Orientation.z + p.Orientation.w * p.Orientation.w);
    if (!(length > 1e-4f)) return identity_pose();
    out.orientation = {out.orientation.x / length, out.orientation.y / length, out.orientation.z / length, out.orientation.w / length};
    return out;
}

ovrMatrix4f matrix_from_pose(const XrPosef& p) {
    const auto& q = p.orientation;
    const float ww = q.w * q.w, xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    ovrMatrix4f m{};
    m.M[0][0] = ww + xx - yy - zz; m.M[0][1] = 2 * (q.x * q.y - q.w * q.z); m.M[0][2] = 2 * (q.x * q.z + q.w * q.y);
    m.M[1][0] = 2 * (q.x * q.y + q.w * q.z); m.M[1][1] = ww - xx + yy - zz; m.M[1][2] = 2 * (q.y * q.z - q.w * q.x);
    m.M[2][0] = 2 * (q.x * q.z - q.w * q.y); m.M[2][1] = 2 * (q.y * q.z + q.w * q.x); m.M[2][2] = ww - xx - yy + zz;
    m.M[0][3] = p.position.x; m.M[1][3] = p.position.y; m.M[2][3] = p.position.z; m.M[3][3] = 1.0f;
    return m;
}

ovrMatrix4f multiply(const ovrMatrix4f& a, const ovrMatrix4f& b) {
    ovrMatrix4f out{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) out.M[i][j] += a.M[i][k] * b.M[k][j];
    return out;
}

// General 4x4 inverse (cofactors); false for a singular matrix.
bool invert(const ovrMatrix4f& matrix, ovrMatrix4f& out) {
    const float* m = &matrix.M[0][0];
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float determinant = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!(std::fabs(determinant) > 1e-12f)) return false;
    for (int i = 0; i < 16; ++i) (&out.M[0][0])[i] = inv[i] / determinant;
    return true;
}

// The rotation of a transform whose columns are scaled by `scale`.
XrPosef rotation_of(const ovrMatrix4f& m, const float (&scale)[3]) {
    float r[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = m.M[i][j] / scale[j];
    XrPosef pose = identity_pose();
    auto& q = pose.orientation;
    const float trace = r[0][0] + r[1][1] + r[2][2];
    if (trace > 0) {
        const float s = 0.5f / std::sqrt(trace + 1.0f);
        q = {(r[2][1] - r[1][2]) * s, (r[0][2] - r[2][0]) * s, (r[1][0] - r[0][1]) * s, 0.25f / s};
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const float s = 2.0f * std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]);
        q = {0.25f * s, (r[0][1] + r[1][0]) / s, (r[0][2] + r[2][0]) / s, (r[2][1] - r[1][2]) / s};
    } else if (r[1][1] > r[2][2]) {
        const float s = 2.0f * std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]);
        q = {(r[0][1] + r[1][0]) / s, 0.25f * s, (r[1][2] + r[2][1]) / s, (r[0][2] - r[2][0]) / s};
    } else {
        const float s = 2.0f * std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]);
        q = {(r[0][2] + r[2][0]) / s, (r[1][2] + r[2][1]) / s, 0.25f * s, (r[1][0] - r[0][1]) / s};
    }
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (length > 1e-6f) q = {q.x / length, q.y / length, q.z / length, q.w / length};
    return pose;
}

// VrApi's own eye projection: near plane 0.1 m, far plane at infinity.
ovrMatrix4f projection_from_fov(const XrFovf& fov) {
    constexpr float nearZ = 0.1f;
    const float minX = nearZ * std::tan(fov.angleLeft), maxX = nearZ * std::tan(fov.angleRight);
    const float minY = nearZ * std::tan(fov.angleDown), maxY = nearZ * std::tan(fov.angleUp);
    ovrMatrix4f m{};
    m.M[0][0] = 2 * nearZ / (maxX - minX); m.M[0][2] = (maxX + minX) / (maxX - minX);
    m.M[1][1] = 2 * nearZ / (maxY - minY); m.M[1][2] = (maxY + minY) / (maxY - minY);
    m.M[2][2] = -1.0f; m.M[2][3] = -2 * nearZ;
    m.M[3][2] = -1.0f;
    return m;
}

// A projection layer's TexCoordsFromTanAngles maps (tanX, tanY, -1, 1) to texture coordinates. Invert it for
// the tangents at the edges of the submitted rectangle. False for matrices that are not a projection (quads
// placed with a unit-square matrix, rotated mappings).
bool fov_from_tan_angles(const ovrMatrix4f& t, const ovrRectf& rect, XrFovf& fov) {
    constexpr float e = 1e-3f;
    if (std::fabs(t.M[0][0]) < e || std::fabs(t.M[1][1]) < e || std::fabs(t.M[0][1]) > e || std::fabs(t.M[1][0]) > e ||
        std::fabs(t.M[2][0]) > e || std::fabs(t.M[2][1]) > e || std::fabs(t.M[2][2] + 1.0f) > e) return false;
    const auto tangent = [](float coordinate, float scale, float offset, float bias) { return (coordinate + offset - bias) / scale; };
    fov.angleLeft = std::atan(tangent(rect.x, t.M[0][0], t.M[0][2], t.M[0][3]));
    fov.angleRight = std::atan(tangent(rect.x + rect.width, t.M[0][0], t.M[0][2], t.M[0][3]));
    fov.angleDown = std::atan(tangent(rect.y, t.M[1][1], t.M[1][2], t.M[1][3]));
    fov.angleUp = std::atan(tangent(rect.y + rect.height, t.M[1][1], t.M[1][2], t.M[1][3]));
    // Vulkan games map up to v = 0 (negative M[1][1]): the image's first row is the top, as OpenXR's Vulkan
    // swapchains expect, so only the edge order changes (Resident Evil 4 / UE4).
    if (fov.angleUp < fov.angleDown) std::swap(fov.angleUp, fov.angleDown);
    return fov.angleRight > fov.angleLeft && fov.angleUp > fov.angleDown;
}

double now_seconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// The runtime's XrTime is CLOCK_MONOTONIC in nanoseconds, VrApi's time is the same clock in seconds.
XrTime to_xr_time(double seconds) { return static_cast<XrTime>(std::llround(seconds * 1e9)); }
double to_seconds(XrTime time) { return time * 1e-9; }

// ---- State -------------------------------------------------------------------------------------------------

}  // namespace

struct ovrTextureSwapChain {
    XrSwapchain xr = nullptr;  // Null for images the compositor never shows (depth, cube maps): app-owned GL textures.
    ovrTextureType type = VRAPI_TEXTURE_TYPE_2D;
    int64_t format = 0;
    int width = 0, height = 0, arraySize = 1, levels = 1;
    uint32_t length = 0;
    std::vector<uint32_t> gl;
    std::vector<uint64_t> vk;
    uint64_t selectedFrame = 0;
    int selectedIndex = -1;
    ovrTextureSamplerState sampler{};
};

struct ovrMobile {
    int unused = 0;
};

namespace {

enum Hand { kLeft = 0, kRight = 1 };
constexpr ovrDeviceID kDeviceIds[2] = {0x100, 0x101};  // Left and right Touch controller.

struct Actions {
    XrActionSet set = nullptr;
    XrPath hands[2] = {};
    XrAction trigger = nullptr, squeeze = nullptr, stick = nullptr, stickClick = nullptr, stickTouch = nullptr,
             triggerTouch = nullptr, primary = nullptr, secondary = nullptr, primaryTouch = nullptr,
             secondaryTouch = nullptr, menu = nullptr, thumbrest = nullptr, aim = nullptr, haptic = nullptr;
    XrSpace aimSpaces[2] = {};
    double lastSync = 0;
};

struct State {
    std::mutex mutex;
    bool initialized = false;
    ovrJava java{};
    jobject activity = nullptr;  // Global reference.
    int32_t graphicsApi = VRAPI_GRAPHICS_API_TYPE_OPENGL_ES;
    void* runtimeLibrary = nullptr;
    PFN_xrGetInstanceProcAddr getProcAddr = nullptr;
    XrFunctions xr;
    XrInstance instance = nullptr;
    XrSystemId system = 0;
    uint32_t eyeWidth = 1440, eyeHeight = 1584;
    float halfFovX = 0.0f, halfFovY = 0.0f;

    // Session.
    XrSession session = nullptr;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    EGLContext sessionContext = EGL_NO_CONTEXT;
    ovrSystemCreateInfoVulkan vulkan{};
    XrSpace viewSpace = nullptr;
    XrSpace spaces[8] = {};  // Indexed by ovrTrackingSpace.
    ovrTrackingSpace trackingSpace = VRAPI_TRACKING_SPACE_LOCAL;
    Actions actions;
    std::vector<ovrTextureSwapChain*> swapchains;

    // VR mode and frames.
    ovrMobile mobile;
    bool inVrMode = false;
    uint32_t modeFlags = 0;
    std::deque<ovrEventType> events;
    std::atomic<int> pendingWaits{0};
    std::array<std::pair<long long, double>, 8> predicted{};  // frameIndex -> predicted display time.
    size_t predictedNext = 0;
    double lastPredicted = 0, framePeriod = 1.0 / 72;
    uint64_t submittedFrames = 0;
    double fpsWindowStart = 0;
    uint64_t fpsWindowFrames = 0;
    float appFps = 0;
    XrPosef eyeOffsets[2] = {identity_pose(), identity_pose()};
};

State& state() {
    static State s;
    return s;
}

bool ok(XrResult result, const char* what) {
    if (result >= 0) return true;
    LOGE("%s failed: %d", what, result);
    return false;
}

#define XR_CALL(name, ...) call<name##_fn>(state().xr.name, __VA_ARGS__)

// Signatures for XR_CALL.
using xrCreateInstance_fn = XrResult(XRAPI_PTR*)(const XrInstanceCreateInfo*, XrInstance*);
using xrDestroyInstance_fn = XrResult(XRAPI_PTR*)(XrInstance);
using xrGetSystem_fn = XrResult(XRAPI_PTR*)(XrInstance, const XrSystemGetInfo*, XrSystemId*);
using xrPollEvent_fn = XrResult(XRAPI_PTR*)(XrInstance, XrEventDataBuffer*);
using xrCreateSession_fn = XrResult(XRAPI_PTR*)(XrInstance, const XrSessionCreateInfo*, XrSession*);
using xrDestroySession_fn = XrResult(XRAPI_PTR*)(XrSession);
using xrBeginSession_fn = XrResult(XRAPI_PTR*)(XrSession, const XrSessionBeginInfo*);
using xrEndSession_fn = XrResult(XRAPI_PTR*)(XrSession);
using xrRequestExitSession_fn = XrResult(XRAPI_PTR*)(XrSession);
using xrEnumerateViewConfigurationViews_fn = XrResult(XRAPI_PTR*)(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t, uint32_t*, XrViewConfigurationView*);
using xrCreateReferenceSpace_fn = XrResult(XRAPI_PTR*)(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace*);
using xrDestroySpace_fn = XrResult(XRAPI_PTR*)(XrSpace);
using xrLocateSpace_fn = XrResult(XRAPI_PTR*)(XrSpace, XrSpace, XrTime, XrSpaceLocation*);
using xrLocateViews_fn = XrResult(XRAPI_PTR*)(XrSession, const XrViewLocateInfo*, XrViewState*, uint32_t, uint32_t*, XrView*);
using xrCreateSwapchain_fn = XrResult(XRAPI_PTR*)(XrSession, const XrSwapchainCreateInfo*, XrSwapchain*);
using xrDestroySwapchain_fn = XrResult(XRAPI_PTR*)(XrSwapchain);
using xrEnumerateSwapchainImages_fn = XrResult(XRAPI_PTR*)(XrSwapchain, uint32_t, uint32_t*, XrSwapchainImageBaseHeader*);
using xrAcquireSwapchainImage_fn = XrResult(XRAPI_PTR*)(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t*);
using xrWaitSwapchainImage_fn = XrResult(XRAPI_PTR*)(XrSwapchain, const XrSwapchainImageWaitInfo*);
using xrReleaseSwapchainImage_fn = XrResult(XRAPI_PTR*)(XrSwapchain, const XrSwapchainImageReleaseInfo*);
using xrWaitFrame_fn = XrResult(XRAPI_PTR*)(XrSession, const XrFrameWaitInfo*, XrFrameState*);
using xrBeginFrame_fn = XrResult(XRAPI_PTR*)(XrSession, const XrFrameBeginInfo*);
using xrEndFrame_fn = XrResult(XRAPI_PTR*)(XrSession, const XrFrameEndInfo*);
using xrStringToPath_fn = XrResult(XRAPI_PTR*)(XrInstance, const char*, XrPath*);
using xrCreateActionSet_fn = XrResult(XRAPI_PTR*)(XrInstance, const XrActionSetCreateInfo*, XrActionSet*);
using xrCreateAction_fn = XrResult(XRAPI_PTR*)(XrActionSet, const XrActionCreateInfo*, XrAction*);
using xrSuggestInteractionProfileBindings_fn = XrResult(XRAPI_PTR*)(XrInstance, const XrInteractionProfileSuggestedBinding*);
using xrAttachSessionActionSets_fn = XrResult(XRAPI_PTR*)(XrSession, const XrSessionActionSetsAttachInfo*);
using xrCreateActionSpace_fn = XrResult(XRAPI_PTR*)(XrSession, const XrActionSpaceCreateInfo*, XrSpace*);
using xrSyncActions_fn = XrResult(XRAPI_PTR*)(XrSession, const XrActionsSyncInfo*);
using xrGetActionStateBoolean_fn = XrResult(XRAPI_PTR*)(XrSession, const XrActionStateGetInfo*, XrActionStateBoolean*);
using xrGetActionStateFloat_fn = XrResult(XRAPI_PTR*)(XrSession, const XrActionStateGetInfo*, XrActionStateFloat*);
using xrGetActionStateVector2f_fn = XrResult(XRAPI_PTR*)(XrSession, const XrActionStateGetInfo*, XrActionStateVector2f*);
using xrApplyHapticFeedback_fn = XrResult(XRAPI_PTR*)(XrSession, const XrHapticActionInfo*, const XrHapticBaseHeader*);
using xrStopHapticFeedback_fn = XrResult(XRAPI_PTR*)(XrSession, const XrHapticActionInfo*);
using xrGetOpenGLESGraphicsRequirementsKHR_fn = XrResult(XRAPI_PTR*)(XrInstance, XrSystemId, XrGraphicsRequirementsOpenGLESKHR*);
using xrGetVulkanGraphicsRequirementsKHR_fn = XrResult(XRAPI_PTR*)(XrInstance, XrSystemId, XrGraphicsRequirementsVulkanKHR*);
using xrEnumerateDisplayRefreshRatesFB_fn = XrResult(XRAPI_PTR*)(XrSession, uint32_t, uint32_t*, float*);
using xrGetDisplayRefreshRateFB_fn = XrResult(XRAPI_PTR*)(XrSession, float*);
using xrRequestDisplayRefreshRateFB_fn = XrResult(XRAPI_PTR*)(XrSession, float);

// ---- The emulator's EGL configs ------------------------------------------------------------------------------
//
// Meta's native samples (VrCubeWorld), and the games built on them, create their context on the first EGL
// config that is RGBA8 with no depth, stencil or multisampling. The emulator's RGBA8 configs all have
// depth 24 and stencil 8, so those games stop at "eglChooseConfig() failed". They only make a 16x16
// pbuffer with that config and render into their own framebuffers, so for libraries that link libvrapi.so
// themselves (native engines, not Unity), eglGetConfigAttrib reports no depth or stencil.
// debug.refract.vrapi_egl_compat=0 turns this off.

EGLBoolean compat_get_config_attrib(EGLDisplay display, EGLConfig config, EGLint attribute, EGLint* value) {
    const EGLBoolean result = eglGetConfigAttrib(display, config, attribute, value);
    if (result && value && (attribute == EGL_DEPTH_SIZE || attribute == EGL_STENCIL_SIZE)) *value = 0;
    return result;
}

#if defined(__aarch64__)
constexpr uint32_t kJumpSlot = R_AARCH64_JUMP_SLOT, kGlobalData = R_AARCH64_GLOB_DAT;
#else
constexpr uint32_t kJumpSlot = R_X86_64_JUMP_SLOT, kGlobalData = R_X86_64_GLOB_DAT;
#endif

// Points `library`'s imports of `symbol` (its GOT slots) at `replacement`. True if any slot changed.
bool redirect_import(const dl_phdr_info* info, const char* symbol, void* replacement) {
    const ElfW(Dyn)* dynamic = nullptr;
    for (int i = 0; i < info->dlpi_phnum; ++i)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) dynamic = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    if (!dynamic) return false;
    // Bionic leaves the dynamic section as linked: its addresses are relative to the load bias.
    const auto at = [&](ElfW(Addr) address) { return address < info->dlpi_addr ? info->dlpi_addr + address : address; };
    const ElfW(Sym)* symbols = nullptr;
    const char* strings = nullptr;
    const ElfW(Rela)* tables[2] = {};
    size_t sizes[2] = {};
    for (auto* entry = dynamic; entry->d_tag != DT_NULL; ++entry) {
        switch (entry->d_tag) {
        case DT_SYMTAB: symbols = reinterpret_cast<const ElfW(Sym)*>(at(entry->d_un.d_ptr)); break;
        case DT_STRTAB: strings = reinterpret_cast<const char*>(at(entry->d_un.d_ptr)); break;
        case DT_JMPREL: tables[0] = reinterpret_cast<const ElfW(Rela)*>(at(entry->d_un.d_ptr)); break;
        case DT_PLTRELSZ: sizes[0] = entry->d_un.d_val; break;
        case DT_RELA: tables[1] = reinterpret_cast<const ElfW(Rela)*>(at(entry->d_un.d_ptr)); break;
        case DT_RELASZ: sizes[1] = entry->d_un.d_val; break;
        }
    }
    if (!symbols || !strings) return false;
    bool changed = false;
    const long page = sysconf(_SC_PAGESIZE);
    for (int t = 0; t < 2; ++t) {
        for (size_t i = 0; tables[t] && i < sizes[t] / sizeof(ElfW(Rela)); ++i) {
            const auto& relocation = tables[t][i];
            const uint32_t type = ELF64_R_TYPE(relocation.r_info);
            if ((type != kJumpSlot && type != kGlobalData) ||
                std::strcmp(strings + symbols[ELF64_R_SYM(relocation.r_info)].st_name, symbol) != 0) continue;
            auto* slot = reinterpret_cast<void**>(info->dlpi_addr + relocation.r_offset);
            auto* start = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(slot) & ~static_cast<uintptr_t>(page - 1));
            // The GOT is read-only after relocation (RELRO).
            if (mprotect(start, static_cast<size_t>(page), PROT_READ | PROT_WRITE) != 0) continue;
            *slot = replacement;
            mprotect(start, static_cast<size_t>(page), PROT_READ);
            changed = true;
        }
    }
    return changed;
}

bool links_vrapi_loader(const dl_phdr_info* info) {
    const ElfW(Dyn)* dynamic = nullptr;
    for (int i = 0; i < info->dlpi_phnum; ++i)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) dynamic = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    if (!dynamic) return false;
    const char* strings = nullptr;
    for (auto* entry = dynamic; entry->d_tag != DT_NULL; ++entry)
        if (entry->d_tag == DT_STRTAB)
            strings = reinterpret_cast<const char*>(entry->d_un.d_ptr < info->dlpi_addr ? info->dlpi_addr + entry->d_un.d_ptr : entry->d_un.d_ptr);
    if (!strings) return false;
    for (auto* entry = dynamic; entry->d_tag != DT_NULL; ++entry)
        if (entry->d_tag == DT_NEEDED && std::strcmp(strings + entry->d_un.d_val, "libvrapi.so") == 0) return true;
    return false;
}

void apply_egl_config_compat() {
    char value[PROP_VALUE_MAX]{};
    __system_property_get("debug.refract.vrapi_egl_compat", value);
    if (std::strcmp(value, "0") == 0) return;
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void*) {
        const char* path = info->dlpi_name;
        if (!path || !std::strstr(path, "/data/app/") || !links_vrapi_loader(info)) return 0;
        if (redirect_import(info, "eglGetConfigAttrib", reinterpret_cast<void*>(&compat_get_config_attrib)))
            LOGI("EGL config compatibility for %s", std::strrchr(path, '/') ? std::strrchr(path, '/') + 1 : path);
        return 0;
    }, nullptr);
}

// ---- Runtime and instance -----------------------------------------------------------------------------------

bool load_runtime() {
    auto& s = state();
    if (s.getProcAddr) return true;
    s.runtimeLibrary = dlopen("libopenxr_runtime.so", RTLD_NOW | RTLD_LOCAL);
    if (!s.runtimeLibrary) { LOGE("cannot load libopenxr_runtime.so: %s", dlerror()); return false; }
    auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(dlsym(s.runtimeLibrary, "xrNegotiateLoaderRuntimeInterface"));
    if (!negotiate) { LOGE("runtime has no xrNegotiateLoaderRuntimeInterface"); return false; }
    XrNegotiateLoaderInfo info{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(XrNegotiateLoaderInfo),
                               1, XR_CURRENT_LOADER_RUNTIME_VERSION, XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 0x3ff, 0xfff)};
    XrNegotiateRuntimeRequest request{XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof(XrNegotiateRuntimeRequest)};
    if (!ok(negotiate(&info, &request), "xrNegotiateLoaderRuntimeInterface") || !request.getInstanceProcAddr) return false;
    s.getProcAddr = request.getInstanceProcAddr;
    return true;
}

void resolve_functions() {
    auto& s = state();
#define REFRACT_RESOLVE(name) s.getProcAddr(s.instance, #name, &s.xr.name);
    REFRACT_XR_FUNCTIONS(REFRACT_RESOLVE)
#undef REFRACT_RESOLVE
}

bool create_instance() {
    auto& s = state();
    PFN_xrVoidFunction initializeLoader = nullptr;
    s.getProcAddr(nullptr, "xrInitializeLoaderKHR", &initializeLoader);
    if (initializeLoader) {
        XrLoaderInitInfoAndroidKHR loaderInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR, nullptr, s.java.Vm, s.activity};
        reinterpret_cast<XrResult(XRAPI_PTR*)(const void*)>(initializeLoader)(&loaderInfo);
    }
    s.getProcAddr(nullptr, "xrCreateInstance", &s.xr.xrCreateInstance);
    const bool vulkan = (s.graphicsApi & kVrApiGraphicsTypeMask) == VRAPI_GRAPHICS_API_TYPE_VULKAN;
    const char* extensions[] = {"XR_KHR_android_create_instance", vulkan ? "XR_KHR_vulkan_enable" : "XR_KHR_opengl_es_enable",
                                "XR_FB_display_refresh_rate"};
    XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR, nullptr, s.java.Vm, s.activity};
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO, &android};
    std::snprintf(info.applicationInfo.applicationName, sizeof(info.applicationInfo.applicationName), "VrApi game");
    std::snprintf(info.applicationInfo.engineName, sizeof(info.applicationInfo.engineName), "Refract VrApi");
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
    info.enabledExtensionNames = extensions;
    if (!ok(XR_CALL(xrCreateInstance, &info, &s.instance), "xrCreateInstance")) return false;
    resolve_functions();
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    if (!ok(XR_CALL(xrGetSystem, s.instance, &systemInfo, &s.system), "xrGetSystem")) return false;
    XrViewConfigurationView views[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t count = 0;
    if (XR_CALL(xrEnumerateViewConfigurationViews, s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2u, &count, views) == XR_SUCCESS && count) {
        s.eyeWidth = views[0].recommendedImageRectWidth;
        s.eyeHeight = views[0].recommendedImageRectHeight;
    }
    LOGI("OpenXR instance ready (%s), suggested eye %ux%u", vulkan ? "Vulkan" : "GLES", s.eyeWidth, s.eyeHeight);
    return true;
}

// ---- Session ----------------------------------------------------------------------------------------------

void queue_event(ovrEventType type) {
    auto& s = state();
    if (s.events.size() < 32) s.events.push_back(type);
}

// Drains the runtime's events and follows its session state (READY -> begin the session).
void pump_events() {
    auto& s = state();
    if (!s.instance) return;
    for (int i = 0; i < 16; ++i) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        if (XR_CALL(xrPollEvent, s.instance, &event) != XR_SUCCESS) break;
        if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) continue;
        const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
        s.sessionState = changed->state;
        if (s.sessionState == XR_SESSION_STATE_READY && s.session) {
            XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
            ok(XR_CALL(xrBeginSession, s.session, &begin), "xrBeginSession");
        }
    }
}

bool session_running() {
    pump_events();
    return state().session && state().sessionState == XR_SESSION_STATE_FOCUSED;
}

XrPath path(const char* text) {
    XrPath result = XR_NULL_PATH;
    XR_CALL(xrStringToPath, state().instance, text, &result);
    return result;
}

XrAction make_action(const char* name, XrActionType type) {
    auto& a = state().actions;
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    std::snprintf(info.actionName, sizeof(info.actionName), "%s", name);
    std::snprintf(info.localizedActionName, sizeof(info.localizedActionName), "%s", name);
    info.actionType = type;
    info.countSubactionPaths = 2;
    info.subactionPaths = a.hands;
    XrAction action = nullptr;
    ok(XR_CALL(xrCreateAction, a.set, &info, &action), name);
    return action;
}

void create_actions() {
    auto& s = state();
    auto& a = s.actions;
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(setInfo.actionSetName, sizeof(setInfo.actionSetName), "vrapi");
    std::snprintf(setInfo.localizedActionSetName, sizeof(setInfo.localizedActionSetName), "VrApi");
    if (!ok(XR_CALL(xrCreateActionSet, s.instance, &setInfo, &a.set), "xrCreateActionSet")) return;
    a.hands[kLeft] = path("/user/hand/left");
    a.hands[kRight] = path("/user/hand/right");
    a.trigger = make_action("trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    a.squeeze = make_action("squeeze", XR_ACTION_TYPE_FLOAT_INPUT);
    a.stick = make_action("stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    a.stickClick = make_action("stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a.stickTouch = make_action("stick_touch", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a.triggerTouch = make_action("trigger_touch", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a.primary = make_action("primary", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a.secondary = make_action("secondary", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a.primaryTouch = make_action("primary_touch", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a.secondaryTouch = make_action("secondary_touch", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a.menu = make_action("menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a.thumbrest = make_action("thumbrest", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a.aim = make_action("aim", XR_ACTION_TYPE_POSE_INPUT);
    a.haptic = make_action("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT);
    const struct { XrAction action; const char* binding; } bindings[] = {
        {a.trigger, "/user/hand/left/input/trigger/value"}, {a.trigger, "/user/hand/right/input/trigger/value"},
        {a.squeeze, "/user/hand/left/input/squeeze/value"}, {a.squeeze, "/user/hand/right/input/squeeze/value"},
        {a.stick, "/user/hand/left/input/thumbstick"}, {a.stick, "/user/hand/right/input/thumbstick"},
        {a.stickClick, "/user/hand/left/input/thumbstick/click"}, {a.stickClick, "/user/hand/right/input/thumbstick/click"},
        {a.stickTouch, "/user/hand/left/input/thumbstick/touch"}, {a.stickTouch, "/user/hand/right/input/thumbstick/touch"},
        {a.triggerTouch, "/user/hand/left/input/trigger/touch"}, {a.triggerTouch, "/user/hand/right/input/trigger/touch"},
        {a.primary, "/user/hand/left/input/x/click"}, {a.primary, "/user/hand/right/input/a/click"},
        {a.secondary, "/user/hand/left/input/y/click"}, {a.secondary, "/user/hand/right/input/b/click"},
        {a.primaryTouch, "/user/hand/left/input/x/touch"}, {a.primaryTouch, "/user/hand/right/input/a/touch"},
        {a.secondaryTouch, "/user/hand/left/input/y/touch"}, {a.secondaryTouch, "/user/hand/right/input/b/touch"},
        {a.menu, "/user/hand/left/input/menu/click"},
        {a.thumbrest, "/user/hand/left/input/thumbrest/touch"}, {a.thumbrest, "/user/hand/right/input/thumbrest/touch"},
        {a.aim, "/user/hand/left/input/aim/pose"}, {a.aim, "/user/hand/right/input/aim/pose"},
        {a.haptic, "/user/hand/left/output/haptic"}, {a.haptic, "/user/hand/right/output/haptic"},
    };
    std::vector<XrActionSuggestedBinding> suggested;
    for (const auto& binding : bindings) if (binding.action) suggested.push_back({binding.action, path(binding.binding)});
    XrInteractionProfileSuggestedBinding profile{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr,
                                                 path("/interaction_profiles/oculus/touch_controller"),
                                                 static_cast<uint32_t>(suggested.size()), suggested.data()};
    ok(XR_CALL(xrSuggestInteractionProfileBindings, s.instance, &profile), "xrSuggestInteractionProfileBindings");
}

void attach_actions() {
    auto& s = state();
    auto& a = s.actions;
    if (!a.set) return;
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &a.set};
    ok(XR_CALL(xrAttachSessionActionSets, s.session, &attach), "xrAttachSessionActionSets");
    for (int hand = 0; hand < 2; ++hand) {
        XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO, nullptr, a.aim, a.hands[hand], identity_pose()};
        ok(XR_CALL(xrCreateActionSpace, s.session, &info, &a.aimSpaces[hand]), "xrCreateActionSpace");
    }
}

XrSpace reference_space(XrReferenceSpaceType type) {
    XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, type, identity_pose()};
    XrSpace space = nullptr;
    ok(XR_CALL(xrCreateReferenceSpace, state().session, &info, &space), "xrCreateReferenceSpace");
    return space;
}

// Creates the one OpenXR session on the game's graphics context the first time it is needed (a swapchain is
// created or VR mode is entered), and begins it.
bool ensure_session(EGLDisplay display, EGLContext context) {
    auto& s = state();
    if (s.session) return true;
    if (!s.instance) return false;
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO, nullptr, 0, s.system};
    XrGraphicsBindingOpenGLESAndroidKHR gles{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    XrGraphicsBindingVulkanKHR vulkan{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    if ((s.graphicsApi & kVrApiGraphicsTypeMask) == VRAPI_GRAPHICS_API_TYPE_VULKAN) {
        if (!s.vulkan.Device) { LOGE("Vulkan session before vrapi_CreateSystemVulkan"); return false; }
        XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
        XR_CALL(xrGetVulkanGraphicsRequirementsKHR, s.instance, s.system, &requirements);
        // VrApi never names the queue family; engines render on the first graphics family.
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(s.vulkan.PhysicalDevice, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(s.vulkan.PhysicalDevice, &count, families.data());
        for (uint32_t i = 0; i < count; ++i) if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { vulkan.queueFamilyIndex = i; break; }
        vulkan.instance = s.vulkan.Instance;
        vulkan.physicalDevice = s.vulkan.PhysicalDevice;
        vulkan.device = s.vulkan.Device;
        info.next = &vulkan;
    } else {
        if (display == EGL_NO_DISPLAY) display = eglGetCurrentDisplay();
        if (context == EGL_NO_CONTEXT) context = eglGetCurrentContext();
        if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT) { LOGE("no EGL context for the session"); return false; }
        XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        XR_CALL(xrGetOpenGLESGraphicsRequirementsKHR, s.instance, s.system, &requirements);
        EGLint configId = 0, configCount = 0;
        EGLConfig config = nullptr;
        if (eglQueryContext(display, context, EGL_CONFIG_ID, &configId)) {
            const EGLint attributes[] = {EGL_CONFIG_ID, configId, EGL_NONE};
            eglChooseConfig(display, attributes, &config, 1, &configCount);
        }
        gles.display = display;
        gles.config = configCount ? config : nullptr;
        gles.context = context;
        s.sessionContext = context;
        info.next = &gles;
    }
    if (!ok(XR_CALL(xrCreateSession, s.instance, &info, &s.session), "xrCreateSession")) { s.session = nullptr; return false; }
    s.viewSpace = reference_space(XR_REFERENCE_SPACE_TYPE_VIEW);
    s.spaces[VRAPI_TRACKING_SPACE_LOCAL] = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL);
    s.spaces[VRAPI_TRACKING_SPACE_LOCAL_FLOOR] = reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR);
    s.spaces[VRAPI_TRACKING_SPACE_STAGE] = reference_space(XR_REFERENCE_SPACE_TYPE_STAGE);
    // Tilted and fixed-yaw local spaces are plain LOCAL here.
    s.spaces[VRAPI_TRACKING_SPACE_LOCAL_TILTED] = s.spaces[VRAPI_TRACKING_SPACE_LOCAL];
    s.spaces[VRAPI_TRACKING_SPACE_LOCAL_FIXED_YAW] = s.spaces[VRAPI_TRACKING_SPACE_LOCAL];
    if (!s.actions.set) create_actions();
    attach_actions();
    pump_events();  // IDLE, READY (begins the session)
    pump_events();  // SYNCHRONIZED, VISIBLE, FOCUSED
    LOGI("session created (%s), state %d", info.next == &gles ? "GLES" : "Vulkan", s.sessionState);
    return true;
}

void destroy_session() {
    auto& s = state();
    if (!s.session) return;
    for (auto* chain : s.swapchains) {
        if (chain->xr) XR_CALL(xrDestroySwapchain, chain->xr);
        chain->xr = nullptr;
    }
    if (s.sessionState == XR_SESSION_STATE_FOCUSED || s.sessionState == XR_SESSION_STATE_VISIBLE ||
        s.sessionState == XR_SESSION_STATE_SYNCHRONIZED) {
        XR_CALL(xrRequestExitSession, s.session);
        XR_CALL(xrEndSession, s.session);
    }
    XR_CALL(xrDestroySession, s.session);
    s.session = nullptr;
    s.sessionState = XR_SESSION_STATE_UNKNOWN;
    s.sessionContext = EGL_NO_CONTEXT;
    s.viewSpace = nullptr;
    std::fill(std::begin(s.spaces), std::end(s.spaces), nullptr);
    s.actions.aimSpaces[0] = s.actions.aimSpaces[1] = nullptr;
}

XrSpace tracking_space() {
    auto& s = state();
    XrSpace space = s.spaces[s.trackingSpace];
    return space ? space : s.spaces[VRAPI_TRACKING_SPACE_LOCAL];
}

// ---- Frame timing ------------------------------------------------------------------------------------------

double wait_frame() {
    auto& s = state();
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo info{XR_TYPE_FRAME_WAIT_INFO};
    if (!session_running() || XR_CALL(xrWaitFrame, s.session, &info, &frame) != XR_SUCCESS) {
        return now_seconds() + s.framePeriod;
    }
    if (frame.predictedDisplayPeriod > 0) s.framePeriod = to_seconds(frame.predictedDisplayPeriod);
    s.lastPredicted = to_seconds(frame.predictedDisplayTime);
    return s.lastPredicted;
}

double predicted_time_for_frame(long long frameIndex) {
    auto& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        for (const auto& [index, time] : s.predicted) if (index == frameIndex && time > 0) return time;
    }
    if (!s.inVrMode) return now_seconds() + s.framePeriod;
    const double time = wait_frame();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.predicted[s.predictedNext++ % s.predicted.size()] = {frameIndex, time};
    ++s.pendingWaits;
    return time;
}

// ---- Tracking ------------------------------------------------------------------------------------------------

uint32_t tracking_status(XrSpaceLocationFlags flags) {
    uint32_t status = VRAPI_TRACKING_STATUS_HMD_CONNECTED;
    if (flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) status |= VRAPI_TRACKING_STATUS_ORIENTATION_VALID;
    if (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) status |= VRAPI_TRACKING_STATUS_POSITION_VALID;
    if (flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) status |= VRAPI_TRACKING_STATUS_ORIENTATION_TRACKED;
    if (flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) status |= VRAPI_TRACKING_STATUS_POSITION_TRACKED;
    return status;
}

// Locates `space` in the current tracking space at `seconds` as a VrApi rigid body pose.
bool locate(XrSpace space, double seconds, ovrRigidBodyPosef& out, XrSpaceLocationFlags& flags) {
    out = {};
    out.Pose.Orientation.w = 1.0f;
    out.TimeInSeconds = seconds;
    out.PredictionInSeconds = std::max(0.0, seconds - now_seconds());
    flags = 0;
    XrSpace base = tracking_space();
    if (!space || !base) return false;
    XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION, &velocity};
    if (XR_CALL(xrLocateSpace, space, base, to_xr_time(seconds), &location) != XR_SUCCESS) return false;
    flags = location.locationFlags;
    out.Pose = to_ovr(location.pose);
    if (velocity.velocityFlags & 1) out.LinearVelocity = {velocity.linearVelocity.x, velocity.linearVelocity.y, velocity.linearVelocity.z};
    if (velocity.velocityFlags & 2) out.AngularVelocity = {velocity.angularVelocity.x, velocity.angularVelocity.y, velocity.angularVelocity.z};
    return true;
}

ovrTracking2 predicted_tracking2(double seconds) {
    auto& s = state();
    ovrTracking2 tracking{};
    tracking.HeadPose.Pose.Orientation.w = 1.0f;
    tracking.HeadPose.TimeInSeconds = seconds;
    XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrPosef head = identity_pose();
    XrSpaceLocationFlags flags = 0;
    if (session_running() && locate(s.viewSpace, seconds, tracking.HeadPose, flags)) {
        tracking.Status = tracking_status(flags);
        head = to_xr(tracking.HeadPose.Pose);
        // Also records which host pose this display time was rendered with (the runtime's frame sync).
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, to_xr_time(seconds), tracking_space()};
        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t count = 0;
        if (XR_CALL(xrLocateViews, s.session, &info, &viewState, 2u, &count, views) == XR_SUCCESS && count == 2) {
            for (int eye = 0; eye < 2; ++eye) s.eyeOffsets[eye] = multiply(inverse(head), views[eye].pose);
            s.halfFovX = std::max(-views[0].fov.angleLeft, views[0].fov.angleRight);
            s.halfFovY = std::max(views[0].fov.angleUp, -views[0].fov.angleDown);
        }
    }
    for (int eye = 0; eye < 2; ++eye) {
        XrFovf fov = views[eye].fov;
        if (!(fov.angleRight > fov.angleLeft)) {
            const float x = s.halfFovX > 0 ? s.halfFovX : 0.785398f, y = s.halfFovY > 0 ? s.halfFovY : 0.785398f;
            fov = {-x, x, y, -y};
        }
        tracking.Eye[eye].ProjectionMatrix = projection_from_fov(fov);
        tracking.Eye[eye].ViewMatrix = matrix_from_pose(inverse(multiply(head, s.eyeOffsets[eye])));
    }
    return tracking;
}

// ---- Swapchains ------------------------------------------------------------------------------------------

bool depth_format(int64_t format) {
    switch (format) {
    case 0x81A5: case 0x81A6: case 0x81A7: case 0x8CAC: case 0x88F0: case 0x8CAD:  // GL depth/stencil formats
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_S8_UINT:
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return true;
    default:
        return false;
    }
}

int64_t gl_format(ovrTextureFormat format) {
    switch (format) {
    case VRAPI_TEXTURE_FORMAT_565: return 0x8D62;              // GL_RGB565
    case VRAPI_TEXTURE_FORMAT_5551: return 0x8057;             // GL_RGB5_A1
    case VRAPI_TEXTURE_FORMAT_4444: return 0x8056;             // GL_RGBA4
    case VRAPI_TEXTURE_FORMAT_8888_sRGB: return 0x8C43;        // GL_SRGB8_ALPHA8
    case VRAPI_TEXTURE_FORMAT_RGBA16F: return 0x881A;          // GL_RGBA16F
    case VRAPI_TEXTURE_FORMAT_DEPTH_16: return 0x81A5;         // GL_DEPTH_COMPONENT16
    case VRAPI_TEXTURE_FORMAT_DEPTH_24: return 0x81A6;         // GL_DEPTH_COMPONENT24
    case VRAPI_TEXTURE_FORMAT_DEPTH_24_STENCIL_8: return 0x88F0;  // GL_DEPTH24_STENCIL8
    case VRAPI_TEXTURE_FORMAT_RG16: return 0x822C;             // GL_RG16 (GL_RG16F on ES)
    default: return 0x8058;                                    // GL_RGBA8
    }
}

void create_local_gl_images(ovrTextureSwapChain& chain) {
    const GLenum target = chain.type == VRAPI_TEXTURE_TYPE_CUBE ? GL_TEXTURE_CUBE_MAP
                          : chain.arraySize > 1                ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    chain.gl.resize(chain.length);
    glGenTextures(static_cast<GLsizei>(chain.length), chain.gl.data());
    for (uint32_t texture : chain.gl) {
        glBindTexture(target, texture);
        if (target == GL_TEXTURE_2D_ARRAY)
            glTexStorage3D(target, chain.levels, static_cast<GLenum>(chain.format), chain.width, chain.height, chain.arraySize);
        else
            glTexStorage2D(target, chain.levels, static_cast<GLenum>(chain.format), chain.width, chain.height);
    }
    glBindTexture(target, 0);
}

ovrTextureSwapChain* create_swapchain(ovrTextureType type, int64_t format, int width, int height, int levels, int bufferCount, int arraySize) {
    auto& s = state();
    if (width <= 0 || height <= 0) return nullptr;
    auto* chain = new ovrTextureSwapChain;
    chain->type = type;
    chain->format = format;
    chain->width = width;
    chain->height = height;
    chain->levels = std::max(1, levels);
    chain->arraySize = type == VRAPI_TEXTURE_TYPE_2D_ARRAY ? std::max(2, arraySize) : 1;
    // The runtime keeps three images; a game asking for one (a static image) renders only index 0.
    chain->length = static_cast<uint32_t>(std::clamp(bufferCount, 1, 3));
    const bool vulkan = (s.graphicsApi & kVrApiGraphicsTypeMask) == VRAPI_GRAPHICS_API_TYPE_VULKAN;
    const bool displayable = type != VRAPI_TEXTURE_TYPE_CUBE && !depth_format(format);
    if (displayable && ensure_session(EGL_NO_DISPLAY, EGL_NO_CONTEXT)) {
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = format;
        info.sampleCount = 1;
        info.width = static_cast<uint32_t>(width);
        info.height = static_cast<uint32_t>(height);
        info.faceCount = 1;
        info.arraySize = static_cast<uint32_t>(chain->arraySize);
        info.mipCount = static_cast<uint32_t>(chain->levels);
        if (ok(XR_CALL(xrCreateSwapchain, s.session, &info, &chain->xr), "xrCreateSwapchain")) {
            uint32_t count = 0;
            XR_CALL(xrEnumerateSwapchainImages, chain->xr, 0u, &count, nullptr);
            if (vulkan) {
                std::vector<XrSwapchainImageVulkanKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
                XR_CALL(xrEnumerateSwapchainImages, chain->xr, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
                for (const auto& image : images) chain->vk.push_back(image.image);
            } else {
                std::vector<XrSwapchainImageOpenGLESKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
                XR_CALL(xrEnumerateSwapchainImages, chain->xr, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
                for (const auto& image : images) chain->gl.push_back(image.image);
            }
            chain->length = std::min<uint32_t>(chain->length, count);
            if (bufferCount >= 3) chain->length = count;
        } else {
            chain->xr = nullptr;
        }
    }
    if (!chain->xr) {
        if (vulkan) {
            LOGW("swapchain type=%d format=%lld %dx%d is not supported with Vulkan", type, static_cast<long long>(format), width, height);
            delete chain;
            return nullptr;
        }
        create_local_gl_images(*chain);
    }
    LOGI("swapchain %p type=%d format=0x%llx %dx%d layers=%d levels=%d images=%u%s", static_cast<void*>(chain), type,
         static_cast<unsigned long long>(format), width, height, chain->arraySize, chain->levels, chain->length,
         chain->xr ? "" : " (not displayed)");
    std::lock_guard<std::mutex> lock(s.mutex);
    s.swapchains.push_back(chain);
    return chain;
}

bool real_swapchain(const ovrTextureSwapChain* chain) {
    return reinterpret_cast<uintptr_t>(chain) > VRAPI_DEFAULT_TEXTURE_SWAPCHAIN_LOADING_ICON;
}

// Makes the runtime's released image the one the game rendered: cycle acquire/wait/release (no rendering
// in between) until the round-robin index comes up.
bool select_image(ovrTextureSwapChain& chain, int index, uint64_t frame) {
    if (!chain.xr) return false;
    if (chain.selectedFrame == frame) return chain.selectedIndex == index;
    chain.selectedFrame = frame;
    chain.selectedIndex = -1;
    const uint32_t target = static_cast<uint32_t>(index) % std::max<uint32_t>(1, chain.length);
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, INT64_MAX};
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    for (int attempt = 0; attempt < 8; ++attempt) {
        uint32_t acquired = 0;
        if (XR_CALL(xrAcquireSwapchainImage, chain.xr, &acquire, &acquired) != XR_SUCCESS) return false;
        XR_CALL(xrWaitSwapchainImage, chain.xr, &wait);
        XR_CALL(xrReleaseSwapchainImage, chain.xr, &release);
        if (acquired == target) {
            chain.selectedIndex = index;
            return true;
        }
    }
    return false;
}

// ---- Frame submission ----------------------------------------------------------------------------------------

struct EyeImage {
    ovrTextureSwapChain* chain;
    int index;
    const ovrMatrix4f* texCoordsFromTanAngles;
    ovrRectf rect;
};

struct XrCompositionLayerImageLayoutFB { XrStructureType type; void* next; XrFlags64 flags; };
constexpr auto XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB = static_cast<XrStructureType>(1000040000);
constexpr XrFlags64 XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB = 1;

struct FrameLayers {
    std::vector<XrCompositionLayerProjection> projections;
    std::vector<std::array<XrCompositionLayerProjectionView, 2>> views;
    std::vector<XrCompositionLayerQuad> quads;
    std::vector<XrCompositionLayerImageLayoutFB> layouts;  // One per quad.
    std::vector<const void*> pointers;
    FrameLayers() { quads.reserve(16); layouts.reserve(16); }
};

void report_skipped_layer(int type, const char* why) {
    static uint32_t reported = 0;
    if (!(reported & (1u << (type & 31)))) LOGW("skipping layer type %d: %s", type, why);
    reported |= 1u << (type & 31);
}

// Converts one VrApi projection layer (either submit path) into an OpenXR projection layer.
bool add_projection(FrameLayers& layers, const ovrRigidBodyPosef& headPose, const EyeImage (&eyes)[2], uint32_t layerFlags, uint64_t frame, int type) {
    auto& s = state();
    std::array<XrCompositionLayerProjectionView, 2> views{};
    const XrPosef head = to_xr(headPose.Pose);
    for (int eye = 0; eye < 2; ++eye) {
        const auto& image = eyes[eye];
        if (!real_swapchain(image.chain)) return false;  // The runtime's black/loading placeholder images.
        auto& chain = *image.chain;
        if (!chain.xr) { report_skipped_layer(type, "image is not displayable"); return false; }
        if (!select_image(chain, image.index, frame)) { report_skipped_layer(type, "cannot select the submitted image"); return false; }
        ovrRectf rect = image.rect;
        if (!(rect.width > 0 && rect.height > 0)) rect = {0, 0, 1, 1};
        XrFovf fov{};
        if (!fov_from_tan_angles(*image.texCoordsFromTanAngles, rect, fov)) { report_skipped_layer(type, "not a projection (quad or rotated mapping)"); return false; }
        auto& view = views[eye];
        view.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        view.fov = fov;
        view.pose = (layerFlags & VRAPI_FRAME_LAYER_FLAG_FIXED_TO_VIEW) ? s.eyeOffsets[eye] : multiply(head, s.eyeOffsets[eye]);
        view.subImage.swapchain = chain.xr;
        const int x = static_cast<int>(std::lround(rect.x * chain.width)), y = static_cast<int>(std::lround(rect.y * chain.height));
        view.subImage.imageRect = {{x, y}, {std::min(chain.width - x, static_cast<int>(std::lround(rect.width * chain.width))),
                                            std::min(chain.height - y, static_cast<int>(std::lround(rect.height * chain.height)))}};
        view.subImage.imageArrayIndex = chain.arraySize > 1 ? static_cast<uint32_t>(eye) : 0u;
    }
    layers.views.push_back(views);
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space = (layerFlags & VRAPI_FRAME_LAYER_FLAG_FIXED_TO_VIEW) ? s.viewSpace : tracking_space();
    projection.viewCount = 2;
    layers.projections.push_back(projection);
    return true;
}

// A projection layer whose TexCoordsFromTanAngles is not a projection: a flat image placed with
// ovrMatrix4f_TanAngleMatrixFromUnitSquare (OVRPlugin/UE4 stereo layers, splash screens), as one OpenXR quad.
// Per eye the matrix maps (tanX, tanY, 1) to (u, v, 1) homogeneously, so eye-space points of the image are
// k * (g.x, g.y, -g.z) with g = inverse * (u, v, 1); k (the distance) comes from where the two eyes' rays to
// the image centre meet.
bool add_unit_square(FrameLayers& layers, const ovrRigidBodyPosef& headPose, const EyeImage (&eyes)[2], uint32_t layerFlags, uint64_t frame, int type) {
    auto& s = state();
    const auto& image = eyes[0];
    if (!real_swapchain(image.chain)) return false;
    auto& chain = *image.chain;
    if (!chain.xr) { report_skipped_layer(type, "image is not displayable"); return false; }
    if (layers.quads.size() >= layers.quads.capacity()) return false;
    if (!select_image(chain, image.index, frame)) { report_skipped_layer(type, "cannot select the submitted image"); return false; }
    ovrRectf rect = image.rect;
    if (!(rect.width > 0 && rect.height > 0)) rect = {0, 0, 1, 1};
    float center[2][3], axisU[3], axisV[3], origin[2][3];
    for (int eye = 0; eye < 2; ++eye) {
        const auto& t = eyes[eye].texCoordsFromTanAngles->M;
        ovrMatrix4f h{}, g{};
        h.M[3][3] = 1.0f;
        for (int r = 0; r < 3; ++r) { h.M[r][0] = t[r][0]; h.M[r][1] = t[r][1]; h.M[r][2] = t[r][3] - t[r][2]; }
        if (!invert(h, g)) { report_skipped_layer(type, "singular quad mapping"); return false; }
        const ovrMatrix4f rotation = matrix_from_pose(s.eyeOffsets[eye]);
        const auto toHead = [&](float u, float v, float w, float (&out)[3]) {
            const float p[3] = {g.M[0][0] * u + g.M[0][1] * v + g.M[0][2] * w, g.M[1][0] * u + g.M[1][1] * v + g.M[1][2] * w,
                                -(g.M[2][0] * u + g.M[2][1] * v + g.M[2][2] * w)};
            for (int i = 0; i < 3; ++i) out[i] = rotation.M[i][0] * p[0] + rotation.M[i][1] * p[1] + rotation.M[i][2] * p[2];
        };
        toHead(rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f, 1.0f, center[eye]);
        if (eye == 0) { toHead(rect.width, 0.0f, 0.0f, axisU); toHead(0.0f, rect.height, 0.0f, axisV); }
        origin[eye][0] = s.eyeOffsets[eye].position.x; origin[eye][1] = s.eyeOffsets[eye].position.y; origin[eye][2] = s.eyeOffsets[eye].position.z;
    }
    // Least squares for origin0 + k0 * a = origin1 + k1 * b.
    const auto dot = [](const float (&x)[3], const float (&y)[3]) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
    const auto& a = center[0]; const auto& b = center[1];
    const float d[3] = {origin[1][0] - origin[0][0], origin[1][1] - origin[0][1], origin[1][2] - origin[0][2]};
    const float aa = dot(a, a), ab = dot(a, b), bb = dot(b, b), ad = dot(a, d), bd = dot(b, d);
    const float determinant = ab * ab - aa * bb;
    float k = std::fabs(determinant) > 1e-9f * aa * bb ? (ab * bd - bb * ad) / determinant : 0.0f;
    const float distance = std::fabs(k) * std::sqrt(aa);
    if (!(std::isfinite(k) && -k * a[2] > 0 && distance > 0.05f && distance < 100.0f))
        k = (a[2] > 0 ? -2.0f : 2.0f) / std::sqrt(aa);  // Parallel rays (far away) or no stereo: 2 m in front.
    float x[3], y[3], z[3];
    for (int i = 0; i < 3; ++i) { x[i] = k * axisU[i]; y[i] = -k * axisV[i]; }  // +v runs down the image.
    const float width = std::sqrt(dot(x, x)), height = std::sqrt(dot(y, y));
    if (!(width > 1e-4f && height > 1e-4f)) return false;
    for (int i = 0; i < 3; ++i) { x[i] /= width; y[i] /= height; }
    z[0] = x[1] * y[2] - x[2] * y[1]; z[1] = x[2] * y[0] - x[0] * y[2]; z[2] = x[0] * y[1] - x[1] * y[0];
    const float zLength = std::sqrt(dot(z, z));
    if (!(zLength > 1e-4f)) return false;
    for (float& c : z) c /= zLength;
    y[0] = z[1] * x[2] - z[2] * x[1]; y[1] = z[2] * x[0] - z[0] * x[2]; y[2] = z[0] * x[1] - z[1] * x[0];
    ovrMatrix4f frame3{};
    for (int i = 0; i < 3; ++i) { frame3.M[i][0] = x[i]; frame3.M[i][1] = y[i]; frame3.M[i][2] = z[i]; }
    const float unit[3] = {1.0f, 1.0f, 1.0f};
    XrPosef pose = rotation_of(frame3, unit);
    pose.position = {origin[0][0] + k * a[0], origin[0][1] + k * a[1], origin[0][2] + k * a[2]};
    const bool fixedToView = (layerFlags & VRAPI_FRAME_LAYER_FLAG_FIXED_TO_VIEW) != 0;
    if (!fixedToView) pose = multiply(to_xr(headPose.Pose), pose);
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.space = fixedToView ? s.viewSpace : tracking_space();
    quad.eyeVisibility = 0;  // Both eyes.
    quad.pose = pose;
    quad.size = {width, height};
    const int px = static_cast<int>(std::lround(rect.x * chain.width)), py = static_cast<int>(std::lround(rect.y * chain.height));
    quad.subImage = {chain.xr, {{px, py}, {std::min(chain.width - px, static_cast<int>(std::lround(rect.width * chain.width))),
                                           std::min(chain.height - py, static_cast<int>(std::lround(rect.height * chain.height)))}}, 0};
    layers.layouts.push_back({XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB, nullptr, 0});  // v = 0 is the top row.
    quad.next = &layers.layouts.back();
    layers.quads.push_back(quad);
    return true;
}

// A VrApi cylinder layer (a curved panel: menus, HUDs) as a flat OpenXR quad on the middle of its arc; the
// runtime composites quads but not cylinders. TexCoordsFromTanAngles is inverse(eye view * cylinder
// transform), where the transform places a unit cylinder (radius 1, axis +Y, height -1..1) whose 180 degrees
// in front of -Z map to u 0..1; TextureMatrix then maps (u, y) to the image.
bool add_cylinder(FrameLayers& layers, const ovrLayerCylinder2& layer, uint64_t frame) {
    auto& s = state();
    const auto& eye = layer.Textures[0];
    if (!real_swapchain(eye.ColorSwapChain)) return false;
    auto& chain = *eye.ColorSwapChain;
    if (!chain.xr) { report_skipped_layer(layer.Header.Type, "image is not displayable"); return false; }
    if (layers.quads.size() >= layers.quads.capacity()) return false;
    if (!select_image(chain, eye.SwapChainIndex, frame)) { report_skipped_layer(layer.Header.Type, "cannot select the submitted image"); return false; }
    // The cylinder in tracking space (or head space): eye pose * inverse(TexCoordsFromTanAngles).
    const bool fixedToView = (layer.Header.Flags & VRAPI_FRAME_LAYER_FLAG_FIXED_TO_VIEW) != 0;
    const XrPosef eyePose = fixedToView ? s.eyeOffsets[0] : multiply(to_xr(layer.HeadPose.Pose), s.eyeOffsets[0]);
    ovrMatrix4f cylinder{};
    if (!invert(eye.TexCoordsFromTanAngles, cylinder)) return false;
    cylinder = multiply(matrix_from_pose(eyePose), cylinder);
    float scale[3]{};
    for (int axis = 0; axis < 3; ++axis)
        scale[axis] = std::sqrt(cylinder.M[0][axis] * cylinder.M[0][axis] + cylinder.M[1][axis] * cylinder.M[1][axis] + cylinder.M[2][axis] * cylinder.M[2][axis]);
    if (!(scale[0] > 1e-4f && scale[1] > 1e-4f && scale[2] > 1e-4f)) return false;
    // The part of the hemicylinder the image covers (TextureMatrix maps it to 0..1).
    const auto& tm = eye.TextureMatrix;
    if (std::fabs(tm.M[0][0]) < 1e-6f || std::fabs(tm.M[1][1]) < 1e-6f) return false;
    const float u0 = (0.0f - tm.M[0][2]) / tm.M[0][0], u1 = (1.0f - tm.M[0][2]) / tm.M[0][0];
    const float y0 = (0.0f - tm.M[1][2]) / tm.M[1][1], y1 = (1.0f - tm.M[1][2]) / tm.M[1][1];
    const float arc = std::fabs(u1 - u0) * 3.14159265f, middleAngle = ((u0 + u1) * 0.5f - 0.5f) * 3.14159265f;
    const float middleY = (y0 + y1) * 0.5f;
    // Middle of the arc in the cylinder's frame, then in tracking space.
    const float local[3] = {std::sin(middleAngle), middleY, -std::cos(middleAngle)};
    XrPosef pose = identity_pose();
    pose.position = {cylinder.M[0][0] * local[0] + cylinder.M[0][1] * local[1] + cylinder.M[0][2] * local[2] + cylinder.M[0][3],
                     cylinder.M[1][0] * local[0] + cylinder.M[1][1] * local[1] + cylinder.M[1][2] * local[2] + cylinder.M[1][3],
                     cylinder.M[2][0] * local[0] + cylinder.M[2][1] * local[1] + cylinder.M[2][2] * local[2] + cylinder.M[2][3]};
    // Facing the cylinder's axis: its rotation, turned to the middle of the arc.
    XrPosef turn = identity_pose();
    turn.orientation = {0.0f, std::sin(-middleAngle * 0.5f), 0.0f, std::cos(-middleAngle * 0.5f)};
    pose.orientation = multiply(rotation_of(cylinder, scale), turn).orientation;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.space = fixedToView ? s.viewSpace : tracking_space();
    quad.eyeVisibility = 0;  // Both eyes.
    quad.pose = pose;
    quad.size = {scale[0] * arc, scale[1] * std::fabs(y1 - y0)};
    ovrRectf rect = eye.TextureRect;
    if (!(rect.width > 0 && rect.height > 0)) rect = {0, 0, 1, 1};
    const int x = static_cast<int>(std::lround(rect.x * chain.width)), y = static_cast<int>(std::lround(rect.y * chain.height));
    quad.subImage = {chain.xr, {{x, y}, {std::min(chain.width - x, static_cast<int>(std::lround(rect.width * chain.width))),
                                         std::min(chain.height - y, static_cast<int>(std::lround(rect.height * chain.height)))}}, 0};
    // Meta's samples map the cylinder's height to the image top-down (negative y scale); the other way
    // round the image is stored bottom-up.
    layers.layouts.push_back({XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB, nullptr,
                              tm.M[1][1] > 0 ? XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB : 0});
    quad.next = &layers.layouts.back();
    layers.quads.push_back(quad);
    return true;
}

ovrResult end_frame(FrameLayers& layers, double displayTime) {
    auto& s = state();
    // The runtime takes one projection: the last one the game submitted is its scene (earlier ones are
    // usually black or loading backgrounds).
    XrCompositionLayerProjection* scene = nullptr;
    if (!layers.projections.empty()) {
        scene = &layers.projections.back();
        scene->views = layers.views.back().data();
        layers.pointers.push_back(scene);
    }
    // Panels go on top of the scene, in the game's order.
    for (const auto& quad : layers.quads) layers.pointers.push_back(&quad);
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    XR_CALL(xrBeginFrame, s.session, &begin);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = to_xr_time(displayTime > 0 ? displayTime : (s.lastPredicted > 0 ? s.lastPredicted : now_seconds()));
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = static_cast<uint32_t>(layers.pointers.size());
    end.layers = layers.pointers.data();
    const XrResult result = XR_CALL(xrEndFrame, s.session, &end);
    if (result < 0) {
        static int reports = 0;
        if (reports++ < 8) LOGE("xrEndFrame failed: %d", result);
    }
    const double now = now_seconds();
    ++s.fpsWindowFrames;
    if (s.fpsWindowStart == 0) s.fpsWindowStart = now;
    if (now - s.fpsWindowStart >= 1.0) {
        s.appFps = static_cast<float>(s.fpsWindowFrames / (now - s.fpsWindowStart));
        s.fpsWindowStart = now;
        s.fpsWindowFrames = 0;
    }
    if (s.submittedFrames++ % 900 == 0) LOGI("frame %llu: %u layer(s), %.1f fps", static_cast<unsigned long long>(s.submittedFrames), end.layerCount, s.appFps);
    return result < 0 ? ovrError_InvalidOperation : ovrSuccess;
}

// Every submitted frame consumes one xrWaitFrame: the one vrapi_GetPredictedDisplayTime/vrapi_WaitFrame did,
// or one here for games that submit without asking for a display time.
bool begin_submit() {
    auto& s = state();
    if (!s.inVrMode || !session_running()) return false;
    int pending = s.pendingWaits.load();
    while (pending > 0 && !s.pendingWaits.compare_exchange_weak(pending, pending - 1)) {}
    if (pending <= 0) wait_frame();
    return true;
}

// ---- Input ---------------------------------------------------------------------------------------------------

int hand_for_device(ovrDeviceID id) {
    return id == kDeviceIds[kLeft] ? kLeft : id == kDeviceIds[kRight] ? kRight : -1;
}

void sync_actions() {
    auto& s = state();
    auto& a = s.actions;
    if (!a.set || !session_running()) return;
    const double now = now_seconds();
    if (now - a.lastSync < 0.002) return;
    a.lastSync = now;
    XrActiveActionSet active{a.set, XR_NULL_PATH};
    XrActionsSyncInfo info{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
    XR_CALL(xrSyncActions, s.session, &info);
}

bool boolean(XrAction action, int hand) {
    if (!action) return false;
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action, state().actions.hands[hand]};
    XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_CALL(xrGetActionStateBoolean, state().session, &info, &value) == XR_SUCCESS && value.isActive && value.currentState;
}

float scalar(XrAction action, int hand) {
    if (!action) return 0.0f;
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action, state().actions.hands[hand]};
    XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_CALL(xrGetActionStateFloat, state().session, &info, &value) == XR_SUCCESS && value.isActive ? value.currentState : 0.0f;
}

XrVector2f vector2(XrAction action, int hand) {
    if (!action) return {};
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action, state().actions.hands[hand]};
    XrActionStateVector2f value{XR_TYPE_ACTION_STATE_VECTOR2F};
    return XR_CALL(xrGetActionStateVector2f, state().session, &info, &value) == XR_SUCCESS && value.isActive ? value.currentState : XrVector2f{};
}

void haptic(int hand, float amplitude, XrDuration duration) {
    auto& s = state();
    if (!s.actions.haptic || !session_running()) return;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, s.actions.haptic, s.actions.hands[hand]};
    if (amplitude <= 0.0f) {
        XR_CALL(xrStopHapticFeedback, s.session, &info);
        return;
    }
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION, nullptr, duration, 0.0f, std::min(amplitude, 1.0f)};
    XR_CALL(xrApplyHapticFeedback, s.session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

// ---- VrApi entry points ----------------------------------------------------------------------------------

const char* GetVersionString() { return "1.1.56.0-0-0 Refract"; }

double GetTimeInSeconds() { return now_seconds(); }

ovrInitializeStatus Initialize(const ovrInitParms* parms) {
    auto& s = state();
    if (!parms || !parms->Java.Vm) return VRAPI_INITIALIZE_UNKNOWN_ERROR;
    if (s.initialized) return VRAPI_INITIALIZE_SUCCESS;
    LOGI("vrapi_Initialize: API %d.%d.%d.%d graphics=0x%x", parms->ProductVersion, parms->MajorVersion, parms->MinorVersion,
         parms->PatchVersion, parms->GraphicsAPI);
    s.java = parms->Java;
    s.graphicsApi = parms->GraphicsAPI;
    JNIEnv* env = nullptr;
    if (s.java.Vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) s.java.Vm->AttachCurrentThread(&env, nullptr);
    if (env && parms->Java.ActivityObject) s.activity = env->NewGlobalRef(parms->Java.ActivityObject);
    apply_egl_config_compat();  // Games create their EGL context after vrapi_Initialize.
    if (!load_runtime() || !create_instance()) return VRAPI_INITIALIZE_UNKNOWN_ERROR;
    s.initialized = true;
    return VRAPI_INITIALIZE_SUCCESS;
}

void Shutdown() {
    auto& s = state();
    LOGI("vrapi_Shutdown");
    destroy_session();
    if (s.instance) XR_CALL(xrDestroyInstance, s.instance);
    s.instance = nullptr;
    s.actions = {};
    s.initialized = false;
}

void SetPropertyInt(const ovrJava*, ovrProperty, int) {}
void SetPropertyFloat(const ovrJava*, ovrProperty, float) {}

bool GetPropertyInt(const ovrJava*, ovrProperty property, int* value) {
    if (!value) return false;
    switch (property) {
    case VRAPI_ACTIVE_INPUT_DEVICE_ID: *value = static_cast<int>(kDeviceIds[kRight]); return true;
    case VRAPI_DEVICE_EMULATION_MODE: *value = 0; return true;
    case VRAPI_FOVEATION_LEVEL: *value = 0; return true;
    default: return false;
    }
}

float refresh_rate() {
    auto& s = state();
    float rate = 0.0f;
    if (s.session && XR_CALL(xrGetDisplayRefreshRateFB, s.session, &rate) == XR_SUCCESS && rate > 0) return rate;
    return s.framePeriod > 0 ? static_cast<float>(1.0 / s.framePeriod) : 72.0f;
}

int GetSystemPropertyInt(const ovrJava*, ovrSystemProperty property) {
    auto& s = state();
    switch (property) {
    case VRAPI_SYS_PROP_DEVICE_TYPE: return VRAPI_DEVICE_TYPE_OCULUSQUEST2;
    case VRAPI_SYS_PROP_MAX_FULLSPEED_FRAMEBUFFER_SAMPLES: return 4;
    case VRAPI_SYS_PROP_DISPLAY_PIXELS_WIDE: return 3664;
    case VRAPI_SYS_PROP_DISPLAY_PIXELS_HIGH: return 1920;
    case VRAPI_SYS_PROP_DISPLAY_REFRESH_RATE: return static_cast<int>(std::lround(refresh_rate()));
    case VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_WIDTH: return static_cast<int>(s.eyeWidth);
    case VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_HEIGHT: return static_cast<int>(s.eyeHeight);
    case VRAPI_SYS_PROP_DEVICE_REGION: return 0;
    case VRAPI_SYS_PROP_DOMINANT_HAND: return VRAPI_HAND_RIGHT;
    case VRAPI_SYS_PROP_HAS_ORIENTATION_TRACKING:
    case VRAPI_SYS_PROP_HAS_POSITION_TRACKING:
    case VRAPI_SYS_PROP_MULTIVIEW_AVAILABLE:
    case VRAPI_SYS_PROP_SRGB_LAYER_SOURCE_AVAILABLE:
        return 1;
    case VRAPI_SYS_PROP_FOVEATION_AVAILABLE: return 0;
    case VRAPI_SYS_PROP_NUM_SUPPORTED_DISPLAY_REFRESH_RATES: return 1;
    case VRAPI_SYS_PROP_NUM_SUPPORTED_SWAPCHAIN_FORMATS: return 2;
    default: return 0;
    }
}

float suggested_fov_degrees(bool horizontal) {
    auto& s = state();
    const float half = horizontal ? s.halfFovX : s.halfFovY;
    if (half > 0) return 2.0f * half * 57.29578f;
    if (s.session) {
        predicted_tracking2(now_seconds());
        const float measured = horizontal ? s.halfFovX : s.halfFovY;
        if (measured > 0) return 2.0f * measured * 57.29578f;
    }
    return 90.0f;
}

float GetSystemPropertyFloat(const ovrJava*, ovrSystemProperty property) {
    switch (property) {
    case VRAPI_SYS_PROP_DISPLAY_REFRESH_RATE: return refresh_rate();
    case VRAPI_SYS_PROP_SUGGESTED_EYE_FOV_DEGREES_X: return suggested_fov_degrees(true);
    case VRAPI_SYS_PROP_SUGGESTED_EYE_FOV_DEGREES_Y: return suggested_fov_degrees(false);
    default: return 0.0f;
    }
}

int GetSystemPropertyFloatArray(const ovrJava*, ovrSystemProperty property, float* values, int count) {
    if (property != VRAPI_SYS_PROP_SUPPORTED_DISPLAY_REFRESH_RATES) return 0;
    if (values && count > 0) values[0] = refresh_rate();
    return 1;
}

int GetSystemPropertyInt64Array(const ovrJava*, ovrSystemProperty property, int64_t* values, int count) {
    if (property != VRAPI_SYS_PROP_SUPPORTED_SWAPCHAIN_FORMATS) return 0;
    const bool vulkan = (state().graphicsApi & kVrApiGraphicsTypeMask) == VRAPI_GRAPHICS_API_TYPE_VULKAN;
    const int64_t formats[2] = {vulkan ? VK_FORMAT_R8G8B8A8_SRGB : 0x8C43, vulkan ? VK_FORMAT_R8G8B8A8_UNORM : 0x8058};
    const int written = std::clamp(count, 0, 2);
    if (values) std::copy(formats, formats + written, values);
    return values ? written : 2;
}

const char* GetSystemPropertyString(const ovrJava*, ovrSystemProperty) { return ""; }

int GetSystemStatusInt(const ovrJava*, ovrSystemStatus status) {
    auto& s = state();
    switch (status) {
    case VRAPI_SYS_STATUS_MOUNTED: return 1;
    case VRAPI_SYS_STATUS_FRONT_BUFFER_SRGB: return (s.modeFlags & VRAPI_MODE_FLAG_FRONT_BUFFER_SRGB) ? 1 : 0;
    case VRAPI_SYS_STATUS_APP_FRAMES_PER_SECOND: return static_cast<int>(s.appFps);
    default: return 0;
    }
}

float GetSystemStatusFloat(const ovrJava*, ovrSystemStatus status) {
    return status == VRAPI_SYS_STATUS_APP_FRAMES_PER_SECOND ? state().appFps : 0.0f;
}

ovrMobile* EnterVrMode(const ovrModeParms* parms) {
    auto& s = state();
    if (!parms || !s.initialized) return nullptr;
    LOGI("vrapi_EnterVrMode flags=0x%x display=0x%llx context=0x%llx", parms->Flags, parms->Display, parms->ShareContext);
    s.modeFlags = parms->Flags;
    const bool vulkan = (s.graphicsApi & kVrApiGraphicsTypeMask) == VRAPI_GRAPHICS_API_TYPE_VULKAN;
    const auto context = reinterpret_cast<EGLContext>(static_cast<uintptr_t>(parms->ShareContext));
    // A game that dropped its EGL context (and all its swapchains) while paused gets a session on the new one.
    if (!vulkan && s.session && context != EGL_NO_CONTEXT && context != s.sessionContext && s.swapchains.empty()) destroy_session();
    if (!ensure_session(reinterpret_cast<EGLDisplay>(static_cast<uintptr_t>(parms->Display)), context)) return nullptr;
    s.inVrMode = true;
    s.pendingWaits = 0;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.predicted.fill({});
    queue_event(VRAPI_EVENT_VISIBILITY_GAINED);
    queue_event(VRAPI_EVENT_FOCUS_GAINED);
    return &s.mobile;
}

void LeaveVrMode(ovrMobile*) {
    auto& s = state();
    LOGI("vrapi_LeaveVrMode");
    s.inVrMode = false;
    std::lock_guard<std::mutex> lock(s.mutex);
    queue_event(VRAPI_EVENT_FOCUS_LOST);
    queue_event(VRAPI_EVENT_VISIBILITY_LOST);
}

double GetPredictedDisplayTime(ovrMobile*, long long frameIndex) { return predicted_time_for_frame(frameIndex); }

ovrTracking2 GetPredictedTracking2(ovrMobile*, double seconds) { return predicted_tracking2(seconds); }

ovrTracking GetPredictedTracking(ovrMobile*, double seconds) {
    const ovrTracking2 full = predicted_tracking2(seconds);
    ovrTracking tracking{};
    tracking.Status = full.Status;
    tracking.HeadPose = full.HeadPose;
    return tracking;
}

void RecenterPose(ovrMobile*) {}

ovrTrackingSpace GetTrackingSpace(ovrMobile*) { return state().trackingSpace; }

ovrResult SetTrackingSpace(ovrMobile*, ovrTrackingSpace space) {
    if (space < 0 || space >= 8 || (space > VRAPI_TRACKING_SPACE_STAGE && space != VRAPI_TRACKING_SPACE_LOCAL_FIXED_YAW)) return ovrError_InvalidParameter;
    if (state().trackingSpace != space) LOGI("tracking space %d", space);
    state().trackingSpace = space;
    return ovrSuccess;
}

ovrPosef LocateTrackingSpace(ovrMobile*, ovrTrackingSpace target) {
    auto& s = state();
    ovrRigidBodyPosef pose{};
    XrSpaceLocationFlags flags = 0;
    if (target >= 0 && target < 8 && session_running()) locate(s.spaces[target], now_seconds(), pose, flags);
    else pose.Pose.Orientation.w = 1.0f;
    return pose.Pose;
}

ovrPosef GetTrackingTransform(ovrMobile* ovr, ovrTrackingTransform which) {
    switch (which) {
    case VRAPI_TRACKING_TRANSFORM_SYSTEM_CENTER_EYE_LEVEL: return LocateTrackingSpace(ovr, VRAPI_TRACKING_SPACE_LOCAL);
    case VRAPI_TRACKING_TRANSFORM_SYSTEM_CENTER_FLOOR_LEVEL: return LocateTrackingSpace(ovr, VRAPI_TRACKING_SPACE_LOCAL_FLOOR);
    default: return to_ovr(identity_pose());
    }
}

void SetTrackingTransform(ovrMobile*, ovrPosef) {}

ovrResult GetBoundaryGeometry(ovrMobile*, uint32_t, uint32_t* count, ovrVector3f*) {
    if (count) *count = 0;
    return ovrSuccess_BoundaryInvalid;
}

ovrResult GetBoundaryOrientedBoundingBox(ovrMobile*, ovrPosef* pose, ovrVector3f* scale) {
    if (pose) *pose = to_ovr(identity_pose());
    if (scale) *scale = {1.5f, 1.0f, 1.5f};
    return ovrSuccess_BoundaryInvalid;
}

ovrResult TestPointIsInBoundary(ovrMobile*, ovrVector3f, bool* inside, ovrBoundaryTriggerResult* result) {
    if (inside) *inside = true;
    if (result) *result = {{0, 0, 0}, {0, 0, 1}, 10.0f, false};
    return ovrSuccess_BoundaryInvalid;
}

ovrResult GetBoundaryTriggerState(ovrMobile*, ovrTrackedDeviceTypeId, ovrBoundaryTriggerResult* result) {
    if (result) *result = {{0, 0, 0}, {0, 0, 1}, 10.0f, false};
    return ovrSuccess_BoundaryInvalid;
}

ovrResult RequestBoundaryVisible(ovrMobile*, bool) { return ovrSuccess; }

ovrResult GetBoundaryVisible(ovrMobile*, bool* visible) {
    if (visible) *visible = false;
    return ovrSuccess;
}

ovrTextureSwapChain* CreateTextureSwapChain4(const ovrSwapChainCreateInfo* info) {
    if (!info) return nullptr;
    const ovrTextureType type = info->FaceCount == 6 ? VRAPI_TEXTURE_TYPE_CUBE : info->ArraySize > 1 ? VRAPI_TEXTURE_TYPE_2D_ARRAY : VRAPI_TEXTURE_TYPE_2D;
    return create_swapchain(type, info->Format, info->Width, info->Height, info->Levels, info->BufferCount, info->ArraySize);
}

ovrTextureSwapChain* CreateTextureSwapChain3(ovrTextureType type, int64_t format, int width, int height, int levels, int bufferCount) {
    return create_swapchain(type, format, width, height, levels, bufferCount, 2);
}

ovrTextureSwapChain* CreateTextureSwapChain2(ovrTextureType type, ovrTextureFormat format, int width, int height, int levels, int bufferCount) {
    return create_swapchain(type, gl_format(format), width, height, levels, bufferCount, 2);
}

ovrTextureSwapChain* CreateTextureSwapChain(ovrTextureType type, ovrTextureFormat format, int width, int height, int levels, bool buffered) {
    return create_swapchain(type, gl_format(format), width, height, levels, buffered ? 3 : 1, 2);
}

void DestroyTextureSwapChain(ovrTextureSwapChain* chain) {
    auto& s = state();
    if (!real_swapchain(chain)) return;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.swapchains.erase(std::remove(s.swapchains.begin(), s.swapchains.end(), chain), s.swapchains.end());
    }
    if (chain->xr) XR_CALL(xrDestroySwapchain, chain->xr);
    else if (!chain->gl.empty() && eglGetCurrentContext() != EGL_NO_CONTEXT) glDeleteTextures(static_cast<GLsizei>(chain->gl.size()), chain->gl.data());
    delete chain;
}

int GetTextureSwapChainLength(ovrTextureSwapChain* chain) {
    return real_swapchain(chain) ? static_cast<int>(chain->length) : 1;
}

unsigned int GetTextureSwapChainHandle(ovrTextureSwapChain* chain, int index) {
    if (!real_swapchain(chain) || chain->gl.empty() || index < 0) return 0;
    return chain->gl[static_cast<size_t>(index) % chain->length];
}

uint64_t GetTextureSwapChainBufferVulkan(ovrTextureSwapChain* chain, int index) {
    if (!real_swapchain(chain) || chain->vk.empty() || index < 0) return 0;
    return chain->vk[static_cast<size_t>(index) % chain->length];
}

ovrResult GetTextureSwapChainBufferFoveationVulkan(ovrTextureSwapChain*, int, uint64_t* image, uint32_t* width, uint32_t* height) {
    if (image) *image = 0;
    if (width) *width = 0;
    if (height) *height = 0;
    return ovrError_Unavailable;
}

ovrResult SetTextureSwapChainSamplerState(ovrTextureSwapChain* chain, const ovrTextureSamplerState* sampler) {
    if (!real_swapchain(chain) || !sampler) return ovrError_InvalidParameter;
    chain->sampler = *sampler;
    return ovrSuccess;
}

ovrResult GetTextureSwapChainSamplerState(ovrTextureSwapChain* chain, ovrTextureSamplerState* sampler) {
    if (!real_swapchain(chain) || !sampler) return ovrError_InvalidParameter;
    *sampler = chain->sampler;
    return ovrSuccess;
}

ovrResult SetTextureSwapChainHandle(ovrTextureSwapChain*, int, unsigned int) { return ovrError_NotImplemented; }

ovrResult WaitFrame(ovrMobile*, uint64_t frameIndex) {
    predicted_time_for_frame(static_cast<long long>(frameIndex));
    return ovrSuccess;
}

ovrResult BeginFrame(ovrMobile*, uint64_t) { return ovrSuccess; }

ovrResult SubmitFrame2(ovrMobile*, const ovrSubmitFrameDescription2* description) {
    auto& s = state();
    if (!description) return ovrError_InvalidParameter;
    if (!begin_submit()) return ovrSuccess;
    FrameLayers layers;
    double displayTime = description->DisplayTime;
    const uint64_t frame = s.submittedFrames + 1;
    std::vector<std::pair<uint32_t, size_t>> unitSquares;  // Layer index, index in layers.quads.
    int sceneLayer = -1;
    bool sceneOpaque = false;
    // debug.refract.vrapi_layer_log=1 (read once): every 300th frame, what the game submitted per layer.
    static const bool layerLog = [] { char value[PROP_VALUE_MAX]{}; __system_property_get("debug.refract.vrapi_layer_log", value); return std::strcmp(value, "1") == 0; }();
    if (layerLog && frame % 300 == 1) {
        LOGI("frame %llu: %u layer(s)", static_cast<unsigned long long>(frame), description->LayerCount);
        for (uint32_t i = 0; i < description->LayerCount && description->Layers; ++i) {
            const ovrLayerHeader2* header = description->Layers[i];
            if (!header) continue;
            LOGI("  layer %u type %d flags 0x%x blend %d/%d", i, header->Type, header->Flags, header->SrcBlend, header->DstBlend);
            if (header->Type != VRAPI_LAYER_TYPE_PROJECTION2) continue;
            const auto* p = reinterpret_cast<const ovrLayerProjection2*>(header);
            for (int eye = 0; eye < 2; ++eye) {
                const auto& t = p->Textures[eye];
                const auto& m = t.TexCoordsFromTanAngles.M;
                LOGI("    eye %d chain %p%s index %d rect %.3f,%.3f %.3fx%.3f m [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
                     eye, static_cast<void*>(t.ColorSwapChain), real_swapchain(t.ColorSwapChain) ? (t.ColorSwapChain->xr ? "" : " (not displayable)") : " (placeholder)",
                     t.SwapChainIndex, t.TextureRect.x, t.TextureRect.y, t.TextureRect.width, t.TextureRect.height,
                     m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3], m[2][0], m[2][1], m[2][2], m[2][3], m[3][0], m[3][1], m[3][2], m[3][3]);
            }
        }
    }
    for (uint32_t i = 0; i < description->LayerCount && description->Layers; ++i) {
        const ovrLayerHeader2* header = description->Layers[i];
        if (!header) continue;
        if (header->Type == VRAPI_LAYER_TYPE_CYLINDER2) {
            add_cylinder(layers, *reinterpret_cast<const ovrLayerCylinder2*>(header), frame);
            continue;
        }
        if (header->Type != VRAPI_LAYER_TYPE_PROJECTION2) {
            report_skipped_layer(header->Type, "layer type not supported yet");
            continue;
        }
        const auto* projection = reinterpret_cast<const ovrLayerProjection2*>(header);
        const EyeImage eyes[2] = {
            {projection->Textures[0].ColorSwapChain, projection->Textures[0].SwapChainIndex, &projection->Textures[0].TexCoordsFromTanAngles, projection->Textures[0].TextureRect},
            {projection->Textures[1].ColorSwapChain, projection->Textures[1].SwapChainIndex, &projection->Textures[1].TexCoordsFromTanAngles, projection->Textures[1].TextureRect}};
        ovrRectf rect = eyes[0].rect;
        if (!(rect.width > 0 && rect.height > 0)) rect = {0, 0, 1, 1};
        XrFovf fov{};
        if (!fov_from_tan_angles(*eyes[0].texCoordsFromTanAngles, rect, fov)) {
            if (add_unit_square(layers, projection->HeadPose, eyes, header->Flags, frame, header->Type)) unitSquares.push_back({i, layers.quads.size() - 1});
            continue;
        }
        if (add_projection(layers, projection->HeadPose, eyes, header->Flags, frame, header->Type)) {
            sceneLayer = static_cast<int>(i);
            sceneOpaque = header->SrcBlend == kVrApiBlendOne && header->DstBlend == kVrApiBlendZero;
            if (projection->HeadPose.TimeInSeconds > 0)
                displayTime = projection->HeadPose.TimeInSeconds;  // The time its poses (and the runtime's pose tag) were sampled for.
        }
    }
    // Quads go on top of the scene; one the game put under an opaque scene would have been hidden by it.
    if (sceneOpaque)
        for (size_t q = unitSquares.size(); q-- > 0;)
            if (static_cast<int>(unitSquares[q].first) < sceneLayer) layers.quads.erase(layers.quads.begin() + static_cast<long>(unitSquares[q].second));
    return end_frame(layers, displayTime);
}

void SubmitFrame(ovrMobile*, const ovrFrameParms* parms) {
    auto& s = state();
    if (!parms || !begin_submit()) return;
    FrameLayers layers;
    double displayTime = 0;
    const uint64_t frame = s.submittedFrames + 1;
    for (int i = 0; i < std::min(parms->LayerCount, kVrApiLegacyMaxLayers); ++i) {
        const auto& layer = parms->Layers[i];
        const EyeImage eyes[2] = {
            {layer.Textures[0].ColorTextureSwapChain, layer.Textures[0].TextureSwapChainIndex, &layer.Textures[0].TexCoordsFromTanAngles, layer.Textures[0].TextureRect},
            {layer.Textures[1].ColorTextureSwapChain, layer.Textures[1].TextureSwapChainIndex, &layer.Textures[1].TexCoordsFromTanAngles, layer.Textures[1].TextureRect}};
        if (add_projection(layers, layer.Textures[0].HeadPose, eyes, static_cast<uint32_t>(layer.Flags), frame, VRAPI_LAYER_TYPE_PROJECTION2) &&
            layer.Textures[0].HeadPose.TimeInSeconds > 0)
            displayTime = layer.Textures[0].HeadPose.TimeInSeconds;
    }
    end_frame(layers, displayTime);
}

ovrResult SetClockLevels(ovrMobile*, int32_t, int32_t) { return ovrSuccess; }
ovrResult SetPerfThread(ovrMobile*, ovrPerfThreadType, uint32_t) { return ovrSuccess; }
ovrResult SetExtraLatencyMode(ovrMobile*, ovrExtraLatencyMode) { return ovrSuccess; }

ovrHmdColorDesc GetHmdColorDesc(ovrMobile*) { return {VRAPI_COLORSPACE_REC_2020, 0}; }
ovrResult SetClientColorDesc(ovrMobile*, const ovrHmdColorDesc*) { return ovrSuccess; }

ovrResult SetDisplayRefreshRate(ovrMobile*, float rate) {
    auto& s = state();
    if (s.session) XR_CALL(xrRequestDisplayRefreshRateFB, s.session, rate);
    return ovrSuccess;
}

ovrResult PollEvent(ovrEventHeader* event) {
    auto& s = state();
    if (!event) return ovrError_InvalidParameter;
    pump_events();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.events.empty()) {
        event->EventType = VRAPI_EVENT_NONE;
        return ovrSuccess_EventUnavailable;
    }
    event->EventType = s.events.front();
    s.events.pop_front();
    return ovrSuccess;
}

ovrResult EnumerateInputDevices(ovrMobile*, uint32_t index, ovrInputCapabilityHeader* header) {
    if (!header) return ovrError_InvalidParameter;
    if (index >= 2) return ovrError_NoDevice;
    header->Type = ovrControllerType_TrackedRemote;
    header->DeviceID = kDeviceIds[index];
    return ovrSuccess;
}

ovrResult GetInputDeviceCapabilities(ovrMobile*, ovrInputCapabilityHeader* header) {
    if (!header) return ovrError_InvalidParameter;
    const int hand = hand_for_device(header->DeviceID);
    if (hand < 0 || header->Type != ovrControllerType_TrackedRemote) return ovrError_NoDevice;
    auto* caps = reinterpret_cast<ovrInputTrackedRemoteCapabilities*>(header);
    caps->ControllerCapabilities = ovrControllerCaps_HasOrientationTracking | ovrControllerCaps_HasPositionTracking |
                                   (hand == kLeft ? ovrControllerCaps_LeftHand : ovrControllerCaps_RightHand) |
                                   ovrControllerCaps_HasAnalogIndexTrigger | ovrControllerCaps_HasAnalogGripTrigger |
                                   ovrControllerCaps_HasSimpleHapticVibration | ovrControllerCaps_HasJoystick |
                                   ovrControllerCaps_ModelOculusTouch;
    caps->ButtonCapabilities = (hand == kLeft ? ovrButton_X | ovrButton_Y | ovrButton_LThumb | ovrButton_Enter
                                              : ovrButton_A | ovrButton_B | ovrButton_RThumb) |
                               ovrButton_GripTrigger | ovrButton_Trigger | ovrButton_Joystick;
    caps->TrackpadMaxX = caps->TrackpadMaxY = 0;
    caps->TrackpadSizeX = caps->TrackpadSizeY = 0.0f;
    caps->HapticSamplesMax = 0;
    caps->HapticSampleDurationMS = 0;
    caps->TouchCapabilities = (hand == kLeft ? ovrTouch_X | ovrTouch_Y | ovrTouch_LThumb | ovrTouch_LThumbRest
                                             : ovrTouch_A | ovrTouch_B | ovrTouch_RThumb | ovrTouch_RThumbRest) |
                              ovrTouch_Joystick | ovrTouch_IndexTrigger | ovrTouch_ThumbRest | ovrTouch_ThumbUp | ovrTouch_IndexPointing;
    caps->Reserved4 = caps->Reserved5 = 0;
    return ovrSuccess;
}

ovrResult SetHapticVibrationSimple(ovrMobile*, ovrDeviceID device, float intensity) {
    const int hand = hand_for_device(device);
    if (hand < 0) return ovrError_NoDevice;
    // VrApi holds a simple vibration until the game changes it; games refresh it every frame.
    haptic(hand, intensity, 100'000'000);
    return ovrSuccess;
}

ovrResult SetHapticVibrationBuffer(ovrMobile*, ovrDeviceID device, const ovrHapticBuffer* buffer) {
    const int hand = hand_for_device(device);
    if (hand < 0) return ovrError_NoDevice;
    if (!buffer || !buffer->HapticBuffer || !buffer->NumSamples) return ovrError_InvalidParameter;
    uint32_t peak = 0;
    for (uint32_t i = 0; i < buffer->NumSamples; ++i) peak = std::max<uint32_t>(peak, buffer->HapticBuffer[i]);
    haptic(hand, peak / 255.0f, static_cast<XrDuration>(buffer->NumSamples) * 2'000'000);
    return ovrSuccess;
}

ovrResult GetCurrentInputState(ovrMobile*, ovrDeviceID device, ovrInputStateHeader* header) {
    if (!header) return ovrError_InvalidParameter;
    const int hand = hand_for_device(device);
    if (hand < 0 || header->ControllerType != ovrControllerType_TrackedRemote) return ovrError_NoDevice;
    sync_actions();
    auto& a = state().actions;
    auto* input = reinterpret_cast<ovrInputStateTrackedRemote*>(header);
    const bool left = hand == kLeft;
    input->Header.TimeInSeconds = now_seconds();
    input->IndexTrigger = scalar(a.trigger, hand);
    input->GripTrigger = scalar(a.squeeze, hand);
    const XrVector2f stick = vector2(a.stick, hand);
    input->JoystickNoDeadZone = {stick.x, stick.y};
    const bool outsideDeadZone = stick.x * stick.x + stick.y * stick.y > 0.1f * 0.1f;
    input->Joystick = outsideDeadZone ? ovrVector2f{stick.x, stick.y} : ovrVector2f{0, 0};
    uint32_t buttons = 0, touches = 0;
    if (boolean(a.primary, hand)) buttons |= left ? ovrButton_X : ovrButton_A;
    if (boolean(a.secondary, hand)) buttons |= left ? ovrButton_Y : ovrButton_B;
    if (boolean(a.stickClick, hand)) buttons |= (left ? ovrButton_LThumb : ovrButton_RThumb) | ovrButton_Joystick;
    if (left && boolean(a.menu, hand)) buttons |= ovrButton_Enter;
    if (input->IndexTrigger > 0.5f) buttons |= ovrButton_Trigger;
    if (input->GripTrigger > 0.5f) buttons |= ovrButton_GripTrigger;
    if (boolean(a.primaryTouch, hand)) touches |= left ? ovrTouch_X : ovrTouch_A;
    if (boolean(a.secondaryTouch, hand)) touches |= left ? ovrTouch_Y : ovrTouch_B;
    if (boolean(a.stickTouch, hand)) touches |= (left ? ovrTouch_LThumb : ovrTouch_RThumb) | ovrTouch_Joystick;
    if (boolean(a.thumbrest, hand)) touches |= (left ? ovrTouch_LThumbRest : ovrTouch_RThumbRest) | ovrTouch_ThumbRest;
    const bool triggerTouched = boolean(a.triggerTouch, hand) || input->IndexTrigger > 0.05f;
    if (triggerTouched) touches |= ovrTouch_IndexTrigger;
    else touches |= ovrTouch_IndexPointing;
    if (!(touches & (ovrTouch_A | ovrTouch_B | ovrTouch_X | ovrTouch_Y | ovrTouch_Joystick | ovrTouch_ThumbRest))) touches |= ovrTouch_ThumbUp;
    input->Buttons = buttons;
    input->Touches = touches;
    input->TrackpadStatus = 0;
    input->TrackpadPosition = {0, 0};
    input->BatteryPercentRemaining = 100;
    input->RecenterCount = 0;
    input->Reserved = 0;
    input->Reserved5a = 0;
    return ovrSuccess;
}

ovrResult GetInputTrackingState(ovrMobile*, ovrDeviceID device, double seconds, ovrTracking* tracking) {
    if (!tracking) return ovrError_InvalidParameter;
    const int hand = hand_for_device(device);
    if (hand < 0) return ovrError_NoDevice;
    *tracking = {};
    tracking->HeadPose.Pose.Orientation.w = 1.0f;
    XrSpaceLocationFlags flags = 0;
    if (session_running()) {
        sync_actions();
        locate(state().actions.aimSpaces[hand], seconds, tracking->HeadPose, flags);
    }
    tracking->Status = tracking_status(flags) & ~VRAPI_TRACKING_STATUS_HMD_CONNECTED;
    return ovrSuccess;
}

void RecenterInputPose(ovrMobile*, ovrDeviceID) {}

ovrResult GetHandPose(ovrMobile*, ovrDeviceID, double, ovrHandPoseHeader*) { return ovrError_NoDevice; }
ovrResult GetHandSkeleton(ovrMobile*, ovrHandedness, ovrHandSkeletonHeader*) { return ovrError_NoDevice; }
ovrResult GetHandMesh(ovrMobile*, ovrHandedness, ovrHandMeshHeader*) { return ovrError_NoDevice; }

// Vulkan.
ovrResult GetInstanceExtensionsVulkan(char* names, uint32_t* size) {
    if (!size) return ovrError_InvalidParameter;
    if (names && *size > 0) names[0] = '\0';  // The runtime needs no Vulkan extensions.
    *size = 1;
    return ovrSuccess;
}

ovrResult GetDeviceExtensionsVulkan(char* names, uint32_t* size) { return GetInstanceExtensionsVulkan(names, size); }

ovrResult CreateSystemVulkan(ovrSystemCreateInfoVulkan* info) {
    if (!info || !info->Device) return ovrError_InvalidParameter;
    state().vulkan = *info;
    LOGI("vrapi_CreateSystemVulkan device=%p", static_cast<void*>(info->Device));
    return ovrSuccess;
}

void DestroySystemVulkan() { state().vulkan = {}; }

bool ShowSystemUI(const ovrJava*, ovrSystemUIType) { return false; }

void ShowFatalError(const ovrJava*, const char* title, const char* message, const char* file, unsigned int line) {
    LOGE("fatal error from the game: %s: %s (%s:%u)", title ? title : "", message ? message : "", file ? file : "", line);
}

ovrTextureSwapChain* CreateAndroidSurfaceSwapChain(int width, int height) {
    LOGW("Android surface swapchain %dx%d is not supported", width, height);
    return nullptr;
}
ovrTextureSwapChain* CreateAndroidSurfaceSwapChain2(int width, int height, bool) { return CreateAndroidSurfaceSwapChain(width, height); }
ovrTextureSwapChain* CreateAndroidSurfaceSwapChain3(int width, int height, uint64_t) { return CreateAndroidSurfaceSwapChain(width, height); }
jobject GetTextureSwapChainAndroidSurface(ovrTextureSwapChain*) { return nullptr; }

const char* GetLocalPreferenceValueForKey(const char*, const char* defaultValue) { return defaultValue; }
void SetLocalPreferenceValueForKey(const char*, const char*) {}

// Entry points the loader resolves that games do not use on a PC: internal, deprecated or Meta-only.
ovrResult NotImplemented() { return ovrError_NotImplemented; }
void* NullResult() { return nullptr; }

struct Entry {
    const char* name;
    void* function;
};

#define VRAPI_ENTRY(name) {"vrapi_" #name, reinterpret_cast<void*>(&name)}
#define VRAPI_UNSUPPORTED(name) {"vrapi_" #name, reinterpret_cast<void*>(&NotImplemented)}
const Entry kEntries[] = {
    VRAPI_ENTRY(GetVersionString), VRAPI_ENTRY(GetTimeInSeconds), VRAPI_ENTRY(Initialize), VRAPI_ENTRY(Shutdown),
    VRAPI_ENTRY(SetPropertyInt), VRAPI_ENTRY(SetPropertyFloat), VRAPI_ENTRY(GetPropertyInt),
    VRAPI_ENTRY(GetSystemPropertyInt), VRAPI_ENTRY(GetSystemPropertyFloat), VRAPI_ENTRY(GetSystemPropertyFloatArray),
    VRAPI_ENTRY(GetSystemPropertyInt64Array), VRAPI_ENTRY(GetSystemPropertyString), VRAPI_ENTRY(GetSystemStatusInt),
    VRAPI_ENTRY(GetSystemStatusFloat), VRAPI_ENTRY(EnterVrMode), VRAPI_ENTRY(LeaveVrMode),
    VRAPI_ENTRY(GetPredictedDisplayTime), VRAPI_ENTRY(GetPredictedTracking2), VRAPI_ENTRY(GetPredictedTracking),
    VRAPI_ENTRY(RecenterPose), VRAPI_ENTRY(GetTrackingSpace), VRAPI_ENTRY(SetTrackingSpace),
    VRAPI_ENTRY(LocateTrackingSpace), VRAPI_ENTRY(GetTrackingTransform), VRAPI_ENTRY(SetTrackingTransform),
    VRAPI_ENTRY(GetBoundaryGeometry), VRAPI_ENTRY(GetBoundaryOrientedBoundingBox), VRAPI_ENTRY(TestPointIsInBoundary),
    VRAPI_ENTRY(GetBoundaryTriggerState), VRAPI_ENTRY(RequestBoundaryVisible), VRAPI_ENTRY(GetBoundaryVisible),
    VRAPI_ENTRY(CreateTextureSwapChain4), VRAPI_ENTRY(CreateTextureSwapChain3), VRAPI_ENTRY(CreateTextureSwapChain2),
    VRAPI_ENTRY(CreateTextureSwapChain), VRAPI_ENTRY(DestroyTextureSwapChain), VRAPI_ENTRY(GetTextureSwapChainLength),
    VRAPI_ENTRY(GetTextureSwapChainHandle), VRAPI_ENTRY(GetTextureSwapChainBufferVulkan),
    VRAPI_ENTRY(GetTextureSwapChainBufferFoveationVulkan), VRAPI_ENTRY(SetTextureSwapChainSamplerState),
    VRAPI_ENTRY(GetTextureSwapChainSamplerState), VRAPI_ENTRY(SetTextureSwapChainHandle), VRAPI_ENTRY(WaitFrame),
    VRAPI_ENTRY(BeginFrame), VRAPI_ENTRY(SubmitFrame), VRAPI_ENTRY(SubmitFrame2), VRAPI_ENTRY(SetClockLevels),
    VRAPI_ENTRY(SetPerfThread), VRAPI_ENTRY(SetExtraLatencyMode), VRAPI_ENTRY(GetHmdColorDesc),
    VRAPI_ENTRY(SetClientColorDesc), VRAPI_ENTRY(SetDisplayRefreshRate), VRAPI_ENTRY(PollEvent),
    VRAPI_ENTRY(EnumerateInputDevices), VRAPI_ENTRY(GetInputDeviceCapabilities), VRAPI_ENTRY(SetHapticVibrationSimple),
    VRAPI_ENTRY(SetHapticVibrationBuffer), VRAPI_ENTRY(GetCurrentInputState), VRAPI_ENTRY(GetInputTrackingState),
    VRAPI_ENTRY(RecenterInputPose), VRAPI_ENTRY(GetHandPose), VRAPI_ENTRY(GetHandSkeleton), VRAPI_ENTRY(GetHandMesh),
    VRAPI_ENTRY(GetInstanceExtensionsVulkan), VRAPI_ENTRY(GetDeviceExtensionsVulkan), VRAPI_ENTRY(CreateSystemVulkan),
    VRAPI_ENTRY(DestroySystemVulkan), VRAPI_ENTRY(ShowSystemUI), VRAPI_ENTRY(ShowFatalError),
    VRAPI_ENTRY(CreateAndroidSurfaceSwapChain), VRAPI_ENTRY(CreateAndroidSurfaceSwapChain2),
    VRAPI_ENTRY(CreateAndroidSurfaceSwapChain3), VRAPI_ENTRY(GetTextureSwapChainAndroidSurface),
    {"vrapi_CreateTextureSwapChainCrossProcess", reinterpret_cast<void*>(&NullResult)},
    {"vrapi_SetAndroidSurfaceParameters", reinterpret_cast<void*>(&NotImplemented)},
    {"vrapi_ShowSystemUIWithExtra", reinterpret_cast<void*>(&ShowSystemUI)},
    {"ovr_GetLocalPreferenceValueForKey", reinterpret_cast<void*>(&GetLocalPreferenceValueForKey)},
    {"ovr_SetLocalPreferenceValueForKey", reinterpret_cast<void*>(&SetLocalPreferenceValueForKey)},
    VRAPI_UNSUPPORTED(GetCurrentInputState2), VRAPI_UNSUPPORTED(GetPredictedTracking3), VRAPI_UNSUPPORTED(UpdateFoveation),
    VRAPI_UNSUPPORTED(ShowKeyboardOverlay), VRAPI_UNSUPPORTED(CloseKeyboardOverlay), VRAPI_UNSUPPORTED(AnchorKeyboardOverlay),
    VRAPI_UNSUPPORTED(BeginCaptureSection), VRAPI_UNSUPPORTED(EndCaptureSection), VRAPI_UNSUPPORTED(CaptureScalar),
    VRAPI_UNSUPPORTED(ReturnToHome), VRAPI_UNSUPPORTED(GetFrameMetric), VRAPI_UNSUPPORTED(SetAADTLutData),
    VRAPI_UNSUPPORTED(UpdateHmdInfo), VRAPI_UNSUPPORTED(SetRemoteEmulation), VRAPI_UNSUPPORTED(SendTrexTestCommand),
    VRAPI_UNSUPPORTED(EnumerateSupportedTrackedObjectTypes), VRAPI_UNSUPPORTED(EnumerateSupportedTrackedObjectTypes_V1),
    VRAPI_UNSUPPORTED(StartTrackingObjectType), VRAPI_UNSUPPORTED(StartTrackingObjectType_V1),
    VRAPI_UNSUPPORTED(StopTrackingObjectType), VRAPI_UNSUPPORTED(StopTrackingObjectType_V1),
    VRAPI_UNSUPPORTED(BodyConfigBodyTracking), VRAPI_UNSUPPORTED(BodyGetSkeleton), VRAPI_UNSUPPORTED(BodyGetPose),
    VRAPI_UNSUPPORTED(BodyGetState), VRAPI_UNSUPPORTED(BodyOverridePoseInputs), VRAPI_UNSUPPORTED(BodySetOffsetPoseInputs),
};
#undef VRAPI_ENTRY
#undef VRAPI_UNSUPPORTED

// The function accessor handed to the loader.
void* get_api_function(const char* name) {
    if (!name) return nullptr;
    for (const auto& entry : kEntries) if (std::strcmp(entry.name, name) == 0) return entry.function;
    static int unknown = 0;
    if (unknown++ < 32) LOGW("unknown VrApi function %s", name);
    return reinterpret_cast<void*>(&NotImplemented);
}

}  // namespace

// Native half of com.oculus.systemdriver.DriverLoader.load32/load64[Ext] (DriverLoader.java).
VRAPI_EXPORT jlong Java_com_oculus_systemdriver_DriverLoader_nativeGetVrApiAccessor(JNIEnv*, jclass) {
    LOGI("VrApi driver loaded (Refract)");
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(&get_api_function));
}

// Same accessor under the name Meta's driver exports, for loaders that look it up directly.
VRAPI_EXPORT void* vrapi_GetAPIFunction(const char* name) { return get_api_function(name); }
