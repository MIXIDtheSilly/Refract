#include "session.h"

#include "image_frame.h"
#include "openxr_dispatch/openxr_minimal.h"
#include "pose_client.h"
#include "host_address.h"
#include "space_velocity.h"
#include "video_encoder.h"
#include "perf_stats.h"
#include "vulkan_backend.h"
#include "gles_share.h"
#include "controller_input.h"
#include "openxr_dispatch/hand_tracking_types.h"
#include "openxr_dispatch/foveation_types.h"
#include "openxr_dispatch/recommended_layer_resolution_types.h"
#include "game_patches.h"
#include "openxr_dispatch/image_layout_types.h"
#include "windows_gpu_frame.h"

#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <ctime>
#include <limits>

#if defined(__ANDROID__)
#include <android/log.h>
#include <GLES3/gl3.h>
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/system_properties.h>
#include <unistd.h>
#endif

namespace refract::runtime {

namespace {

constexpr XrSystemId kSystemId = 1;
constexpr float kEyeHalfIpdMeters = 0.0315f;
constexpr float kProjectionHalfFovRadians = 0.95f;

#if defined(__ANDROID__)
struct AndroidLoaderInitInfo {
    XrStructureType type;
    const void* next;
    void* applicationVM;
    void* applicationContext;
};
#endif

struct RuntimeHandle {
    uint64_t magic;
};

RuntimeHandle g_instanceHandle{0xAABBCCDD00000001ULL};
RuntimeHandle g_sessionHandle{0xAABBCCDD00000002ULL};
RuntimeHandle g_spaceHandle{0xAABBCCDD00000003ULL};
RuntimeHandle g_actionSetHandle{0xAABBCCDD00000005ULL};
struct ActionSample {
    refract::protocol::InputValue value;
    bool changed = false;
    XrTime changedAt = 0;
};
// A binding path split into hand and input component once, not on every xrSyncActions.
struct ResolvedBinding {
    uint8_t hand = 0;
    bool aim = false;  // .../input/aim/pose
    refract::protocol::InputComponent component = refract::protocol::InputComponent::None;
};
struct ActionRecord {
    uint64_t magic;
    XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::vector<XrPath> bindings;
    std::vector<ResolvedBinding> resolved;  // Rebuilt when `bindings` changes.
    size_t resolvedFrom = SIZE_MAX;         // bindings.size() that `resolved` was built from.
    std::array<ActionSample, 3> samples{}; // Left, right, aggregate.
};
std::deque<ActionRecord> g_actionHandles;
std::unordered_map<XrAction, uint32_t> g_actionIndex;  // Handle -> index in g_actionHandles.
struct HandTrackerRecord { bool alive = true; uint32_t hand = 0; uint32_t sources = 3; };
std::deque<HandTrackerRecord> g_handTrackers;
XrSessionState g_sessionState = XR_SESSION_STATE_UNKNOWN;
std::deque<XrSessionState> g_pendingSessionEvents;
bool g_pendingInteractionProfileEvent = false;

enum class SpaceKind {
    Reference,
    Local,
    View,
    LeftHand,
    RightHand,
    LeftAim,
    RightAim,
};

struct SpaceRecord {
    RuntimeHandle handle{};
    SpaceKind kind = SpaceKind::Reference;
    XrPosef offsetInParent{};
};

struct PathRecord {
    XrPath path = XR_NULL_PATH;
    std::string text;
};

std::deque<SpaceRecord> g_spaces;
uint32_t g_spaceCount = 0;
std::deque<PathRecord> g_paths;
uint32_t g_pathCount = 0;
struct SwapchainRecord {
#if defined(__ANDROID__)
    VulkanSwapchain vulkan{};
#endif
    bool created = false;
    bool acquired = false;
    bool waited = false;
    bool hasReleasedImage = false;
    uint32_t imageCount = 3;
    uint32_t nextImage = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t arraySize = 0;
    int64_t format = 0;
    uint32_t textures[3] = {};
    uint32_t currentImage = 0;
    uint32_t releasedImage = 0;
};
// Menus can hold dozens of panel swapchains at once (AC Nexus creates ~20).
std::array<SwapchainRecord, 64> g_swapchains{};
SwapchainRecord* g_lastReleasedSwapchain = nullptr;
uint32_t g_actionCount = 0;
uint64_t g_nextPath = 1;
std::atomic<uint64_t> g_imageFrameSequence{0};  // Bumped by xrEndFrame and the export publisher thread.
XrTime g_nextFrameStart = 0;
uint32_t g_renderWidth = 1024, g_renderHeight = 1024;
// Half-angles of each eye's field of view. The horizontal one comes from
// debug.refract.hfov (degrees, full width); the vertical one follows the
// recommended eye aspect, so wide render sizes give an undistorted wide view.
void view_half_angles(float& horizontal, float& vertical)
{
    static float configured = -1.0f;
    if (configured < 0.0f) {
        configured = kProjectionHalfFovRadians;
#if defined(__ANDROID__)
        char value[PROP_VALUE_MAX]{};
        if (__system_property_get("debug.refract.hfov", value) > 0) {
            const float degrees = std::strtof(value, nullptr);
            if (degrees >= 40.0f && degrees <= 170.0f) configured = degrees * 3.14159265f / 360.0f;
        }
#endif
    }
    horizontal = configured;
    vertical = std::atan(std::tan(horizontal) * static_cast<float>(g_renderHeight) / static_cast<float>(g_renderWidth));
}
bool g_renderExtentQueried = false;
void query_render_extent() {
    if (g_renderExtentQueried) return;
    g_renderExtentQueried = true;
    // View enumeration precedes the first frame. Allow the initial nonblocking
    // pose connection to deliver the host configuration before the app allocates.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        const auto frame = pose_client().latest_pose_frame();
        if (refract::protocol::valid_render_extent(frame.render_width, frame.render_height)) {
            g_renderWidth = frame.render_width; g_renderHeight = frame.render_height;
            break;
        }
#if defined(__ANDROID__)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
#else
        break;
#endif
    } while (std::chrono::steady_clock::now() < deadline);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "Refract.GPU", "recommended eye extent %ux%u", g_renderWidth, g_renderHeight);
#endif
}
refract::protocol::PoseFrame g_lastViewPoseFrame{};

#if defined(__ANDROID__)
int int_property(const char* name, int fallback, int low, int high);
#endif

// debug.refract.frame_sync=1 (the SteamVR host bridge): see xrWaitFrame.
bool frame_sync_enabled()
{
#if defined(__ANDROID__)
    static const bool enabled = int_property("debug.refract.frame_sync", 0, 0, 1) != 0;
    return enabled;
#else
    return false;
#endif
}

// The pose stream sequence each frame's views came from, by the frame's display time: xrLocateViews
// records it on the game's main thread, xrEndFrame looks it up on its render thread. Frames sent to
// the SteamVR host bridge carry it, so the bridge can show every frame a fixed number of display frames
// after the pose it was rendered with, instead of whichever frame happens to be newest.
struct ViewPoseRecord { XrTime displayTime = 0; uint64_t sequence = 0; };
std::array<ViewPoseRecord, 8> g_viewPoses{};
size_t g_viewPoseNext = 0;
std::mutex g_viewPoseMutex;

void record_view_pose(XrTime displayTime, uint64_t sequence)
{
    std::lock_guard<std::mutex> lock(g_viewPoseMutex);
    auto& last = g_viewPoses[(g_viewPoseNext + g_viewPoses.size() - 1) % g_viewPoses.size()];
    if (last.displayTime == displayTime) { last.sequence = sequence; return; }
    g_viewPoses[g_viewPoseNext++ % g_viewPoses.size()] = {displayTime, sequence};
}

// The tag for a frame's image header: pose sequence + 1, or 0 when unknown.
uint32_t view_pose_tag(XrTime displayTime)
{
    std::lock_guard<std::mutex> lock(g_viewPoseMutex);
    for (const auto& record : g_viewPoses) {
        if (record.displayTime == displayTime && displayTime) return static_cast<uint32_t>(record.sequence) + 1;
    }
    return 0;
}
#if defined(__ANDROID__)
VulkanBackend g_vulkan;
// GLES sessions may also run g_vulkan (a private device for GPU export), so "the app renders
// with Vulkan" is tracked separately.
bool g_vulkanSession = false;
GlesShare g_glesShare;
bool g_vulkanRequirementsQueried = false;
VkInstance g_vulkanInstance = VK_NULL_HANDLE;
#endif

void log_call(const char* name)
{
#if defined(__ANDROID__) && defined(NDEBUG)
    // Keep release frame loops free of per-entry-point log traffic unless debug.refract.log_calls=1.
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        return __system_property_get("debug.refract.log_calls", value) > 0 && value[0] == '1';
    }();
    if (enabled) {
        __android_log_print(ANDROID_LOG_INFO, "Refract.Runtime", "%s", name);
    }
#elif defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "Refract.Runtime", "%s", name);
#else
    std::fprintf(stderr, "Refract.Runtime: %s\n", name);
#endif
}

void log_proc_request(const char* name)
{
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "Refract.Runtime", "xrGetInstanceProcAddr(%s)", name);
#else
    std::fprintf(stderr, "Refract.Runtime: xrGetInstanceProcAddr(%s)\n", name);
#endif
}

XrInstance fake_instance()
{
    return reinterpret_cast<XrInstance>(&g_instanceHandle);
}

XrSession fake_session()
{
    return reinterpret_cast<XrSession>(&g_sessionHandle);
}

XrSpace fake_space()
{
    return reinterpret_cast<XrSpace>(&g_spaceHandle);
}

XrPosef identity_pose()
{
    XrPosef pose{};
    pose.orientation.w = 1.0f;
    return pose;
}

XrSpace make_space(SpaceKind kind, const XrPosef& offsetInParent = identity_pose())
{
    g_spaces.emplace_back();
    SpaceRecord& record = g_spaces[g_spaceCount];
    record.handle.magic = 0xAABBCCDD00000200ULL + g_spaceCount;
    record.kind = kind;
    record.offsetInParent = offsetInParent;
    ++g_spaceCount;
    return reinterpret_cast<XrSpace>(&record.handle);
}

SpaceRecord* find_space(XrSpace space)
{
    for (uint32_t i = 0; i < g_spaceCount; ++i) {
        if (space == reinterpret_cast<XrSpace>(&g_spaces[i].handle) && g_spaces[i].handle.magic != 0) {
            return &g_spaces[i];
        }
    }
    if (space == fake_space()) {
        static SpaceRecord legacy{};
        legacy.handle = g_spaceHandle;
        legacy.kind = SpaceKind::Reference;
        legacy.offsetInParent = identity_pose();
        return &legacy;
    }
    return nullptr;
}

XrActionSet fake_action_set()
{
    return reinterpret_cast<XrActionSet>(&g_actionSetHandle);
}

XrAction fake_action(uint32_t index)
{
    return reinterpret_cast<XrAction>(&g_actionHandles[index]);
}

bool is_valid_instance(XrInstance instance)
{
    return instance == fake_instance();
}

bool is_valid_session(XrSession session)
{
    return session == fake_session();
}

SwapchainRecord* find_swapchain(XrSwapchain handle)
{
    for (auto& record : g_swapchains) {
        if (record.created && handle == reinterpret_cast<XrSwapchain>(&record)) { return &record; }
    }
    return nullptr;
}

bool is_valid_action_set(XrActionSet actionSet)
{
    return actionSet == fake_action_set();
}

bool is_valid_action(XrAction action)
{
    const auto found = g_actionIndex.find(action);
    return found != g_actionIndex.end() && g_actionHandles[found->second].magic != 0;
}

const char* path_text(XrPath path)
{
    // xrStringToPath hands out 1, 2, 3... in g_paths order.
    if (path >= 1 && path <= g_pathCount && g_paths[path - 1].path == path) return g_paths[path - 1].text.c_str();
    for (uint32_t i = 0; i < g_pathCount; ++i) {
        if (g_paths[i].path == path) {
            return g_paths[i].text.c_str();
        }
    }
    return "";
}

SpaceKind action_space_kind(XrPath subactionPath)
{
    const std::string_view text{path_text(subactionPath)};
    if (text == "/user/hand/left") {
        return SpaceKind::LeftHand;
    }
    if (text == "/user/hand/right") {
        return SpaceKind::RightHand;
    }
    return SpaceKind::Reference;
}

XrPosef protocol_pose_to_xr(const refract::protocol::Pose& pose)
{
    XrPosef out{};
    out.orientation.x = pose.qx;
    out.orientation.y = pose.qy;
    out.orientation.z = pose.qz;
    out.orientation.w = pose.qw;
    out.position.x = pose.x;
    out.position.y = pose.y;
    out.position.z = pose.z;
    return out;
}

XrPosef normalize_pose(XrPosef pose)
{
    const float length = std::sqrt(
        pose.orientation.x * pose.orientation.x +
        pose.orientation.y * pose.orientation.y +
        pose.orientation.z * pose.orientation.z +
        pose.orientation.w * pose.orientation.w);
    if (length > 0.00001f) {
        pose.orientation.x /= length;
        pose.orientation.y /= length;
        pose.orientation.z /= length;
        pose.orientation.w /= length;
    } else {
        pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    }
    return pose;
}

XrPosef multiply_pose(const XrPosef& a, const XrPosef& b)
{
    XrPosef out{};
    const float ax = a.orientation.x;
    const float ay = a.orientation.y;
    const float az = a.orientation.z;
    const float aw = a.orientation.w;
    const float bx = b.orientation.x;
    const float by = b.orientation.y;
    const float bz = b.orientation.z;
    const float bw = b.orientation.w;

    out.orientation.x = aw * bx + ax * bw + ay * bz - az * by;
    out.orientation.y = aw * by - ax * bz + ay * bw + az * bx;
    out.orientation.z = aw * bz + ax * by - ay * bx + az * bw;
    out.orientation.w = aw * bw - ax * bx - ay * by - az * bz;

    const float vx = b.position.x;
    const float vy = b.position.y;
    const float vz = b.position.z;
    const float tx = 2.0f * (ay * vz - az * vy);
    const float ty = 2.0f * (az * vx - ax * vz);
    const float tz = 2.0f * (ax * vy - ay * vx);
    const float rx = vx + aw * tx + (ay * tz - az * ty);
    const float ry = vy + aw * ty + (az * tx - ax * tz);
    const float rz = vz + aw * tz + (ax * ty - ay * tx);

    out.position.x = a.position.x + rx;
    out.position.y = a.position.y + ry;
    out.position.z = a.position.z + rz;
    return normalize_pose(out);
}

XrPosef inverse_pose(const XrPosef& pose)
{
    XrPosef inverse{};
    inverse.orientation.x = -pose.orientation.x;
    inverse.orientation.y = -pose.orientation.y;
    inverse.orientation.z = -pose.orientation.z;
    inverse.orientation.w = pose.orientation.w;

    const float px = -pose.position.x;
    const float py = -pose.position.y;
    const float pz = -pose.position.z;
    const float qx = inverse.orientation.x;
    const float qy = inverse.orientation.y;
    const float qz = inverse.orientation.z;
    const float qw = inverse.orientation.w;
    const float tx = 2.0f * (qy * pz - qz * py);
    const float ty = 2.0f * (qz * px - qx * pz);
    const float tz = 2.0f * (qx * py - qy * px);
    inverse.position.x = px + qw * tx + (qy * tz - qz * ty);
    inverse.position.y = py + qw * ty + (qz * tx - qx * tz);
    inverse.position.z = pz + qw * tz + (qx * ty - qy * tx);
    return normalize_pose(inverse);
}

// Hosts before protocol v5 send no LOCAL origin. Leaving LOCAL at the floor-level origin puts the
// eye-level space on the floor, and games that read their eye height as "eye-level origin relative
// to floor-level origin" get 0 (Batman: Arkham Shadow divides by it and its camera rig goes NaN).
// Do what a Quest does after a recenter instead: LOCAL is the head's position and yaw, taken from
// the first real pose frame (sequence 0 is the default, before any host frame arrived).
XrPosef implicit_local_origin(const refract::protocol::PoseFrame& poseFrame)
{
    static std::mutex mutex;
    static bool captured = false;
    static XrPosef origin = identity_pose();
    std::lock_guard lock(mutex);
    if (!captured && poseFrame.sequence != 0) {
        const XrPosef hmd = protocol_pose_to_xr(poseFrame.hmd);
        origin.position = hmd.position;
        origin.orientation = {0.0f, hmd.orientation.y, 0.0f, hmd.orientation.w};  // Twist about +Y.
        origin = normalize_pose(origin);
        captured = true;
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_INFO, "Refract.Space", "implicit LOCAL origin at (%.3f %.3f %.3f)",
            origin.position.x, origin.position.y, origin.position.z);
#endif
    }
    return origin;
}

XrPosef world_pose_for_space(const SpaceRecord& record, const refract::protocol::PoseFrame& poseFrame)
{
    XrPosef base = identity_pose();
    switch (record.kind) {
    case SpaceKind::Local:
        if (poseFrame.version >= 5 && (poseFrame.local_origin_flags & 3) == 3)
            base = protocol_pose_to_xr(poseFrame.local_origin);
        else if (poseFrame.version < 5)
            base = implicit_local_origin(poseFrame);
        break;
    case SpaceKind::View:
        base = protocol_pose_to_xr(poseFrame.hmd);
        break;
    case SpaceKind::LeftHand:
        base = protocol_pose_to_xr(poseFrame.left_controller);
        break;
    case SpaceKind::RightHand:
        base = protocol_pose_to_xr(poseFrame.right_controller);
        break;
    case SpaceKind::LeftAim:
        base = protocol_pose_to_xr(poseFrame.version >= 3 ? poseFrame.aim[0] : poseFrame.left_controller);
        break;
    case SpaceKind::RightAim:
        base = protocol_pose_to_xr(poseFrame.version >= 3 ? poseFrame.aim[1] : poseFrame.right_controller);
        break;
    case SpaceKind::Reference:
    default:
        base = identity_pose();
        break;
    }
    return multiply_pose(base, record.offsetInParent);
}

refract::protocol::Pose xr_pose_to_protocol(const XrPosef& p)
{
    return {p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
}

// The space's velocity in the tracking world (v6 frames). Reference spaces are fixed in that world;
// an offset space moves with its tracked parent, plus the parent's rotation swinging the offset around.
refract::protocol::SpaceVelocity world_velocity_for_space(const SpaceRecord& record, const refract::protocol::PoseFrame& poseFrame)
{
    refract::protocol::SpaceVelocity velocity{};
    switch (record.kind) {
    case SpaceKind::Reference: velocity.flags = 3; break;
    case SpaceKind::Local:
        // LOCAL made up in the runtime (older hosts) never moves.
        velocity = poseFrame.version >= 5 && (poseFrame.local_origin_flags & 3) == 3 ? poseFrame.local_origin_velocity
                                                                                        : refract::protocol::SpaceVelocity{3};
        break;
    case SpaceKind::View: velocity = poseFrame.hmd_velocity; break;
    case SpaceKind::LeftHand: velocity = poseFrame.grip_velocity[0]; break;
    case SpaceKind::RightHand: velocity = poseFrame.grip_velocity[1]; break;
    case SpaceKind::LeftAim: velocity = poseFrame.aim_velocity[0]; break;
    case SpaceKind::RightAim: velocity = poseFrame.aim_velocity[1]; break;
    default: break;
    }
    SpaceRecord parent = record;
    parent.offsetInParent = identity_pose();
    const auto parentPose = xr_pose_to_protocol(world_pose_for_space(parent, poseFrame));
    const auto& offset = record.offsetInParent.position;
    return refract::protocol::offset_velocity(velocity, refract::protocol::rotate(parentPose, {offset.x, offset.y, offset.z}));
}

XrTime monotonic_time_ns()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

#if defined(__ANDROID__) || defined(REFRACT_INPUT_FIXTURE)
// Android's steady_clock and this runtime's XrTime both use CLOCK_MONOTONIC.
XrResult XRAPI_CALL xrConvertTimespecTimeToTimeKHR_impl(XrInstance instance, const timespec* source, XrTime* time)
{
    if (!is_valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!source || !time) return XR_ERROR_VALIDATION_FAILURE;
    constexpr int64_t billion = 1000000000;
    if (source->tv_sec < 0 || source->tv_nsec < 0 || source->tv_nsec >= billion ||
        source->tv_sec > (std::numeric_limits<XrTime>::max() - source->tv_nsec) / billion)
        return XR_ERROR_TIME_INVALID;
    const XrTime value = static_cast<XrTime>(source->tv_sec) * billion + source->tv_nsec;
    if (value <= 0) return XR_ERROR_TIME_INVALID;
    *time = value;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrConvertTimeToTimespecTimeKHR_impl(XrInstance instance, XrTime time, timespec* target)
{
    if (!is_valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!target) return XR_ERROR_VALIDATION_FAILURE;
    if (time <= 0 || time / 1000000000 > std::numeric_limits<decltype(target->tv_sec)>::max())
        return XR_ERROR_TIME_INVALID;
    target->tv_sec = static_cast<decltype(target->tv_sec)>(time / 1000000000);
    target->tv_nsec = static_cast<decltype(target->tv_nsec)>(time % 1000000000);
    return XR_SUCCESS;
}
#endif

void queue_session_state(XrSessionState state)
{
    g_sessionState = state;
    g_pendingSessionEvents.push_back(state);
}

void destroy_swapchain_images(SwapchainRecord& sc)
{
#if defined(__ANDROID__)
    if (sc.vulkan.images[0]) { g_vulkan.destroy(sc.vulkan); }
    if (sc.textures[0] != 0 || sc.textures[1] != 0 || sc.textures[2] != 0) {
        glDeleteTextures(3, sc.textures);
    }
#endif
    sc.textures[0] = 0;
    sc.textures[1] = 0;
    sc.textures[2] = 0;
}

void create_opengles_swapchain_images(SwapchainRecord& sc, const XrSwapchainCreateInfo& createInfo)
{
    destroy_swapchain_images(sc);
#if defined(__ANDROID__)
    const auto* renderer = glGetString(GL_RENDERER);
    const auto* vendor = glGetString(GL_VENDOR);
    __android_log_print(ANDROID_LOG_INFO, "Refract.GPU", "swapchain GLES vendor=%s renderer=%s",
                        vendor ? reinterpret_cast<const char*>(vendor) : "unknown",
                        renderer ? reinterpret_cast<const char*>(renderer) : "unknown");
    GLenum internalFormat = static_cast<GLenum>(createInfo.format);
    if (internalFormat != GL_RGBA8 && internalFormat != GL_SRGB8_ALPHA8) {
        internalFormat = GL_RGBA8;
    }

    const GLenum target = createInfo.arraySize > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    glGenTextures(sc.imageCount, sc.textures);
    for (uint32_t i = 0; i < sc.imageCount; ++i) {
        glBindTexture(target, sc.textures[i]);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (target == GL_TEXTURE_2D_ARRAY) {
            glTexStorage3D(
                target,
                static_cast<GLsizei>(createInfo.mipCount),
                internalFormat,
                static_cast<GLsizei>(createInfo.width),
                static_cast<GLsizei>(createInfo.height),
                static_cast<GLsizei>(createInfo.arraySize));
        } else {
            glTexStorage2D(
                target,
                static_cast<GLsizei>(createInfo.mipCount),
                internalFormat,
                static_cast<GLsizei>(createInfo.width),
                static_cast<GLsizei>(createInfo.height));
        }
    }
    glBindTexture(target, 0);
#else
    (void)createInfo;
#endif
}

#if defined(__ANDROID__)
// The emulator, or a real device with debug.refract.direct_host=1, reaches the Windows host
// directly through `adb reverse` (poses on :38490, images on :38491). Waydroid reaches the Linux
// host at debug.refract.host_addr instead (see direct_host_address).
bool direct_to_host()
{
    static const bool direct = [] {
        if (direct_host_address()) return true;
        char value[PROP_VALUE_MAX]{};
        __system_property_get("ro.hardware", value);
        if (std::strcmp(value, "ranchu") == 0 || std::strcmp(value, "goldfish") == 0) return true;
        __system_property_get("debug.refract.direct_host", value);
        return std::strcmp(value, "1") == 0;
    }();
    return direct;
}

class ImageTransportClient {
public:
    ~ImageTransportClient()
    {
        close_socket();
    }

    bool send_frame(
        uint64_t sequence,
        uint32_t width,
        uint32_t height,
        uint32_t layers,
        const uint8_t* payload,
        uint64_t payloadSize,
        const refract::protocol::ImageProjection* projection = nullptr, bool gpu = false, uint32_t batchPart = 0,
        bool waitAck = true, uint32_t poseTag = 0)
    {
        static refract::protocol::PerfStats stats("image-send");
        // The async sender thread and synchronous GPU messages share the socket.
        std::lock_guard<std::mutex> lock(mutex_);
        refract::protocol::PerfScope scope(stats);
        if (!ensure_connected()) {
            if (!reportedSendSkip_) {
                __android_log_print(ANDROID_LOG_INFO, "Refract.Image", "send skipped: no TCP connection");
                reportedSendSkip_ = true;
            }
            return false;
        }

        refract::protocol::ImageFrameHeader header{};
        header.width = width;
        header.height = height;
        header.layers = layers;
        header.sequence = sequence;
        header.monotonic_time_ns = static_cast<uint64_t>(monotonic_time_ns());
        header.payload_size = payloadSize;
        if (projection && directWindows_) {
            header.version = gpu ? refract::protocol::kWindowsGpuFrameVersion : refract::protocol::kProjectionImageFrameVersion;
            if (projection->quad_count()) header.version = gpu ? refract::protocol::kQuadGpuFrameVersion : refract::protocol::kQuadImageFrameVersion;
            if (batchPart) {
                header.version = projection->quad_count() ? refract::protocol::kMixedQuadGpuFrameVersion : refract::protocol::kMixedProjectionGpuFrameVersion;
                header.reserved = batchPart;
            }
            if (gpu) header.type = refract::protocol::kWindowsGpuFrameType;
            if (gpu && !batchPart) header.reserved = poseTag;
            header.header_size += sizeof(*projection);
        }

        // GPU messages are small: one write avoids three emulator/ADB wakeups.
        bool sent = false;
        if (gpu && projection && directWindows_ && payloadSize == sizeof(refract::protocol::WindowsGpuFrame)) {
            std::array<uint8_t, sizeof(header) + sizeof(*projection) + sizeof(refract::protocol::WindowsGpuFrame)> message{};
            std::memcpy(message.data(), &header, sizeof(header));
            std::memcpy(message.data() + sizeof(header), projection, sizeof(*projection));
            std::memcpy(message.data() + sizeof(header) + sizeof(*projection), payload, static_cast<size_t>(payloadSize));
            sent = send_all(message.data(), message.size());
        } else {
            sent = send_all(&header, sizeof(header)) &&
                (!projection || !directWindows_ || send_all(projection, sizeof(*projection))) &&
                send_all(payload, static_cast<size_t>(payloadSize));
        }
        if (!sent) {
            if (!reportedSendFailure_) {
                __android_log_print(ANDROID_LOG_INFO, "Refract.Image", "send failed");
                reportedSendFailure_ = true;
            }
            close_socket();
            return false;
        }
        // The viewer acknowledges each GPU frame (in order) once it has copied the shared textures.
        if (gpu) {
            unacked_.push_back(sequence);
            if (waitAck) return await_acks_locked(0);
        }
        return true;
    }

    // A composite GPU frame (v10): an atlas pair plus its panel table. Acknowledged like the other
    // GPU frames; the caller waits for that with await_acks.
    bool send_composite_frame(uint64_t sequence, uint32_t width, uint32_t height,
        const refract::protocol::ImageProjection& projection, const refract::protocol::WindowsGpuFrame& gpu,
        const std::vector<uint8_t>& table, uint32_t poseTag = 0)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensure_connected() || !directWindows_) return false;
        refract::protocol::ImageFrameHeader header{};
        header.version = refract::protocol::kCompositeGpuFrameVersion;
        header.type = refract::protocol::kWindowsGpuFrameType;
        header.reserved = poseTag;
        header.header_size += sizeof(projection);
        header.width = width;
        header.height = height;
        header.layers = 2;
        header.sequence = sequence;
        header.monotonic_time_ns = static_cast<uint64_t>(monotonic_time_ns());
        header.payload_size = sizeof(gpu) + table.size();
        std::vector<uint8_t> message(sizeof(header) + sizeof(projection) + header.payload_size);
        std::memcpy(message.data(), &header, sizeof(header));
        std::memcpy(message.data() + sizeof(header), &projection, sizeof(projection));
        std::memcpy(message.data() + sizeof(header) + sizeof(projection), &gpu, sizeof(gpu));
        std::memcpy(message.data() + sizeof(header) + sizeof(projection) + sizeof(gpu), table.data(), table.size());
        if (!send_all(message.data(), message.size())) {
            __android_log_print(ANDROID_LOG_INFO, "Refract.Image", "composite send failed");
            close_socket();
            return false;
        }
        unacked_.push_back(sequence);
        return true;
    }

    // Waits until at most `outstanding` GPU frames are unacknowledged; false (and disconnected)
    // if the viewer went away or acknowledged something else.
    bool await_acks(size_t outstanding)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return await_acks_locked(outstanding);
    }

    // Tries to (re)connect, at most every two seconds.
    bool connected()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return ensure_connected() && directWindows_;
    }

    // One H.264 access unit (AXRI v8/v9): both eyes side by side, eye size width x height.
    bool send_video_frame(uint64_t sequence, uint32_t width, uint32_t height, bool key,
        const uint8_t* data, size_t size, const refract::protocol::ImageProjection& projection)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensure_connected() || !directWindows_) return false;
        refract::protocol::ImageFrameHeader header{};
        header.version = projection.quad_count() ? refract::protocol::kQuadVideoImageFrameVersion : refract::protocol::kVideoImageFrameVersion;
        header.type = refract::protocol::kImageFrameTypeVideo;
        header.format = refract::protocol::kImageFrameFormatH264;
        header.bytes_per_pixel = 0;
        header.header_size += sizeof(projection);
        header.width = width;
        header.height = height;
        header.layers = 2;
        header.reserved = key ? refract::protocol::kVideoFrameKey : 0;
        header.sequence = sequence;
        header.monotonic_time_ns = static_cast<uint64_t>(monotonic_time_ns());
        header.payload_size = size;
        if (send_all(&header, sizeof(header)) && send_all(&projection, sizeof(projection)) && send_all(data, size)) return true;
        __android_log_print(ANDROID_LOG_INFO, "Refract.Image", "video send failed; reconnecting");
        close_socket();
        return false;
    }

    // Successful connections so far; a change means a (new) consumer is listening.
    uint64_t connections() const { return connections_.load(); }

private:
    bool ensure_connected()
    {
        if (socket_ >= 0) {
            return true;
        }

        const XrTime now = monotonic_time_ns();
        if (now - lastConnectAttemptNs_ < 2'000'000'000LL) {
            return false;
        }
        lastConnectAttemptNs_ = now;

        if (direct_to_host()) {
            // Stock Android SELinux separates the app and broker's Unix sockets.
            // Use adb reverse's native emulator pipe rather than its slow NAT.
            int candidate = ::socket(AF_INET, SOCK_STREAM, 0);
            if (candidate >= 0) {
                int noDelay = 1;
                setsockopt(candidate, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
                int sendBuffer = 4 * 1024 * 1024;
                setsockopt(candidate, SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof(sendBuffer));
                timeval timeout{3, 0};
                setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_port = htons(38491);
                const char* host = direct_host_address();
                if (inet_pton(AF_INET, host ? host : "127.0.0.1", &address.sin_addr) == 1 &&
                    ::connect(candidate, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
                    socket_ = candidate;
                    directWindows_ = true;
                    ++connections_;
                    __android_log_print(ANDROID_LOG_INFO, "Refract.Image", "connected to host image stream at %s:38491",
                                        host ? host : "127.0.0.1 (adb reverse)");
                    return true;
                }
            }
            const int tcpError = errno;
            if (candidate >= 0) ::close(candidate);
            if (!reportedConnectFailure_) {
                __android_log_print(ANDROID_LOG_INFO, "Refract.Image",
                                    "direct TCP unavailable errno=%d; trying provider FD", tcpError);
                reportedConnectFailure_ = true;
            }
            const int providerFd = pose_client().open_image_transport_fd();
            if (providerFd >= 0) {
                int sendBuffer = 4 * 1024 * 1024;
                setsockopt(providerFd, SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof(sendBuffer));
                // A stalled peer must fail the frame and reconnect, not block xrEndFrame forever.
                timeval timeout{3, 0};
                setsockopt(providerFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                setsockopt(providerFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                socket_ = providerFd;
                directWindows_ = true;
                ++connections_;
                __android_log_print(ANDROID_LOG_INFO, "Refract.Image", "connected to Windows image stream through provider FD");
                return true;
            }
            return false;
        }

        int candidate = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (candidate < 0) {
            lastError_ = errno;
            return false;
        }

        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        constexpr const char* kSocketName = "refract_image_proxy";
        address.sun_path[0] = '\0';
        std::strncpy(address.sun_path + 1, kSocketName, sizeof(address.sun_path) - 2);
        const socklen_t addressLength =
            static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + std::strlen(kSocketName));

        if (::connect(candidate, reinterpret_cast<sockaddr*>(&address), addressLength) == 0) {
            int bufferSize = 1024 * 1024;
            setsockopt(candidate, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize));
            socket_ = candidate;
            ++connections_;
            __android_log_print(ANDROID_LOG_INFO, "Refract.Image", "connected to local image proxy");
            return true;
        }
        lastError_ = errno;
        __android_log_print(ANDROID_LOG_INFO, "Refract.Image", "local image proxy connect failed errno=%d", lastError_);
        ::close(candidate);

        if (!reportedConnectFailure_) {
            __android_log_print(ANDROID_LOG_INFO, "Refract.Image", "connect failed: errno=%d", lastError_);
            reportedConnectFailure_ = true;
        }
        return false;
    }

    bool send_all(const void* data, size_t size)
    {
        const uint8_t* cursor = static_cast<const uint8_t*>(data);
        size_t remaining = size;
        while (remaining > 0) {
            const ssize_t sent = ::send(socket_, cursor, remaining, MSG_NOSIGNAL);
            if (sent <= 0) {
                if (sent < 0 && errno == EINTR) { continue; }
                return false;
            }
            cursor += sent;
            remaining -= static_cast<size_t>(sent);
        }
        return true;
    }

    bool await_acks_locked(size_t outstanding)
    {
        static refract::protocol::PerfStats ackStats("image-ack-wait");
        refract::protocol::PerfScope ackScope(ackStats);
        while (unacked_.size() > outstanding) {
            if (socket_ < 0) return false;
            uint64_t acknowledgment = UINT64_MAX;
            auto* bytes = reinterpret_cast<uint8_t*>(&acknowledgment);
            size_t done = 0;
            while (done < sizeof(acknowledgment)) {
                ssize_t n = ::recv(socket_, bytes + done, sizeof(acknowledgment) - done, 0);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                done += n;
            }
            if (done != sizeof(acknowledgment) || acknowledgment != unacked_.front()) { close_socket(); return false; }
            unacked_.pop_front();
        }
        return true;
    }

    void close_socket()
    {
        if (socket_ >= 0) {
            ::close(socket_);
            socket_ = -1;
        }
        unacked_.clear();
    }

    std::deque<uint64_t> unacked_;  // GPU frame sequences sent but not yet acknowledged, oldest first.
    std::mutex mutex_;
    std::atomic<uint64_t> connections_{0};
    int socket_ = -1;
    bool directWindows_ = false;
    int lastError_ = 0;
    XrTime lastConnectAttemptNs_ = 0;
    bool reportedConnectFailure_ = false;
    bool reportedSendSkip_ = false;
    bool reportedSendFailure_ = false;
};

ImageTransportClient& image_transport_client()
{
    static ImageTransportClient client;
    return client;
}

// Sends readback frames on its own thread so xrEndFrame never waits on the GPU copy,
// the encoder or the adb reverse pipe. Frames are handled in order; up to two may wait,
// and when a third arrives the oldest waiting one is dropped.
class AsyncFrameSender {
public:
    void submit(uint64_t sequence, uint32_t width, uint32_t height, uint32_t layers,
                const std::vector<uint8_t>* const images, size_t imageBytes,
                const refract::protocol::ImageProjection& projection)
    {
        Frame frame;
        frame.payload.resize(imageBytes * layers);
        for (uint32_t i = 0; i < layers; ++i) {
            std::memcpy(frame.payload.data() + i * imageBytes, images[i].data(), imageBytes);
        }
        frame.sequence = sequence;
        frame.width = width;
        frame.height = height;
        frame.layers = layers;
        frame.projection = projection;
        push(std::move(frame));
    }

    // Pixels still on their way from the GPU (VulkanBackend::readback_async); the
    // sender waits for them, reads the staging memory in place and releases the slot.
    void submit_readback(int slot, bool nv12, uint64_t sequence, uint32_t width, uint32_t height, uint32_t layers,
                         const refract::protocol::ImageProjection& projection)
    {
        Frame frame;
        frame.slot = slot;
        frame.nv12 = nv12;
        frame.sequence = sequence;
        frame.width = width;
        frame.height = height;
        frame.layers = layers;
        frame.projection = projection;
        push(std::move(frame));
    }

private:
    static constexpr size_t kMaxQueued = 2;

    struct Frame {
        std::vector<uint8_t> payload;
        int slot = -1;  // >= 0: the pixels are in this readback slot, not in payload.
        bool nv12 = false;  // The slot holds a GPU-converted NV12 picture (video only).
        refract::protocol::ImageProjection projection{};
        uint64_t sequence = 0;
        uint32_t width = 0, height = 0, layers = 0;
    };

    void push(Frame&& frame)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!started_) {
                std::thread([this] { run(); }).detach();
                started_ = true;
            }
            if (queue_.size() >= kMaxQueued) {
                ++dropped_;
                if (queue_.front().slot >= 0) g_vulkan.readback_release(queue_.front().slot);
                queue_.pop_front();
            }
            queue_.push_back(std::move(frame));
        }
        ready_.notify_one();
    }

    void run()
    {
        H264StreamEncoder video;
        const H264StreamEncoder::Sink sink = [](const H264StreamEncoder::Output& out) {
            const bool sent = image_transport_client().send_video_frame(out.sequence, out.eyeWidth, out.eyeHeight, out.key,
                out.data, out.size, *out.projection);
            if (sent && (out.sequence % 450 == 0 || out.key)) {
                __android_log_print(ANDROID_LOG_INFO, "Refract.Video", "sent seq=%llu %ux%u x2 %s %zu bytes",
                    static_cast<unsigned long long>(out.sequence), out.eyeWidth, out.eyeHeight, out.key ? "key" : "delta", out.size);
            }
            return sent;
        };
        for (;;) {
            Frame frame;
            uint64_t dropped = 0;
            bool have = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // While the encoder holds frames, drain() below does the waiting.
                if (!video.in_flight()) ready_.wait(lock, [this] { return !queue_.empty(); });
                if (!queue_.empty()) {
                    frame = std::move(queue_.front());
                    queue_.pop_front();
                    have = true;
                }
                dropped = dropped_;
            }
            if (!have) {
                video.drain(sink, 2000);
                continue;
            }
            // debug.refract.video=h264 sends hardware-encoded H.264 instead of raw pixels.
            char mode[PROP_VALUE_MAX]{};
            __system_property_get("debug.refract.video", mode);
            const bool encoded = std::strcmp(mode, "h264") == 0 && frame.layers == 2;
            if (!encoded) video.close();
            const uint8_t* pixels = frame.payload.data();
            const size_t bytes = static_cast<size_t>(frame.width) * frame.height * 4 * frame.layers;
            if (frame.slot >= 0) {
                // Send finished video frames while the GPU completes this one.
                while (video.in_flight() && !g_vulkan.readback_ready(frame.slot)) video.drain(sink, 1000);
                pixels = g_vulkan.readback_wait(frame.slot);
                if (!pixels) have = false;
            }
            if (have && encoded && video.encode(frame.sequence, frame.width, frame.height, pixels,
                    frame.projection, image_transport_client().connected(), frame.nv12)) {
                // Queued (or skipped with no viewer); drain() sends it.
            } else if (have && !frame.nv12 && image_transport_client().send_frame(frame.sequence, frame.width, frame.height, frame.layers,
                    pixels, bytes, &frame.projection) && frame.sequence % 450 == 0) {
                __android_log_print(ANDROID_LOG_INFO, "Refract.Stereo", "sent seq=%llu %ux%u layers=%u (async, %llu dropped so far)",
                    static_cast<unsigned long long>(frame.sequence), frame.width, frame.height, frame.layers,
                    static_cast<unsigned long long>(dropped));
            }
            if (frame.slot >= 0) g_vulkan.readback_release(frame.slot);  // Encoded or sent: done with the pixels.
            if (video.in_flight()) video.drain(sink, 0);
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Frame> queue_;
    uint64_t dropped_ = 0;
    bool started_ = false;
};

AsyncFrameSender& async_frame_sender()
{
    static auto* sender = new AsyncFrameSender;  // Never destroyed: its thread is detached.
    return *sender;
}

int int_property(const char* name, int fallback, int low, int high)
{
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get(name, value) <= 0) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value) return fallback;
    return static_cast<int>(std::clamp<long>(parsed, low, high));
}

void maybe_send_swapchain_image(const SwapchainRecord& sc,
                               const XrSwapchainSubImage* subImage = nullptr,
                               std::vector<uint8_t>* readback = nullptr)
{
    if (readback) { readback->clear(); }
    const auto readbackStart = std::chrono::steady_clock::now();
    static bool reportedEntry = false;
    static bool reportedMissingSwapchain = false;
    static bool reportedOversize = false;
    static bool reportedFramebufferFailure = false;
    static bool reportedReadFailure = false;
    static bool reportedReadback = false;
    if (!reportedEntry) {
        __android_log_print(ANDROID_LOG_INFO, "Refract.Image", "image transport hook entered");
        reportedEntry = true;
    }
    if (!sc.created || sc.width == 0 || sc.height == 0 || sc.arraySize == 0) {
        if (!reportedMissingSwapchain) {
            __android_log_print(ANDROID_LOG_INFO, "Refract.Image", "skip: no swapchain image available");
            reportedMissingSwapchain = true;
        }
        return;
    }
    if (sc.textures[sc.releasedImage] == 0) {
        if (!reportedMissingSwapchain) {
            __android_log_print(ANDROID_LOG_INFO, "Refract.Image", "skip: swapchain texture is zero");
            reportedMissingSwapchain = true;
        }
        return;
    }

    const uint64_t sequence = readback ? 0 : g_imageFrameSequence++;
    if (sc.width > 16384 || sc.height > 16384 || sc.arraySize > 4) {
        if (!reportedOversize) {
            __android_log_print(
                ANDROID_LOG_INFO,
                "Refract.Image",
                "skip: oversized image %ux%u layers=%u",
                sc.width,
                sc.height,
                sc.arraySize);
            reportedOversize = true;
        }
        return;
    }

    const uint32_t sourceWidth = subImage ? subImage->imageRect.extent.width : sc.width;
    const uint32_t sourceHeight = subImage ? subImage->imageRect.extent.height : sc.height;
    const GLint sourceX = subImage ? subImage->imageRect.offset.x : 0;
    const GLint sourceY = subImage ? subImage->imageRect.offset.y : 0;
    const uint32_t layerCount = subImage ? 1 : sc.arraySize;
    const uint32_t transportWidth =
        std::min(sourceWidth, g_renderWidth);
    const uint32_t transportHeight =
        std::min(sourceHeight, g_renderHeight);
    const uint64_t layerBytes = static_cast<uint64_t>(transportWidth) * transportHeight * 4;
    const uint64_t payloadBytes = layerBytes * layerCount;
    if (payloadBytes > 128ull * 1024ull * 1024ull) {
        return;
    }

    GLint previousReadFramebuffer = 0;
    GLint previousDrawFramebuffer = 0;
    GLint previousPackAlignment = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);

    static GLuint sourceFramebuffer = 0;
    static GLuint transportFramebuffer = 0;
    static GLuint transportTexture = 0;
    static uint32_t transportTextureWidth = 0;
    static uint32_t transportTextureHeight = 0;
    static std::vector<uint8_t> payload;

    GLint previousTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    if (sourceFramebuffer == 0) {
        glGenFramebuffers(1, &sourceFramebuffer);
    }
    if (transportFramebuffer == 0) {
        glGenFramebuffers(1, &transportFramebuffer);
    }
    if (transportTexture == 0 ||
        transportTextureWidth != transportWidth ||
        transportTextureHeight != transportHeight) {
        if (transportTexture != 0) {
            glDeleteTextures(1, &transportTexture);
            transportTexture = 0;
        }
        glGenTextures(1, &transportTexture);
        glBindTexture(GL_TEXTURE_2D, transportTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA8,
            static_cast<GLsizei>(transportWidth),
            static_cast<GLsizei>(transportHeight),
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            nullptr);
        transportTextureWidth = transportWidth;
        transportTextureHeight = transportHeight;
    }
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));

    if (sourceFramebuffer == 0 || transportFramebuffer == 0 || transportTexture == 0) {
        return;
    }

    payload.resize(static_cast<size_t>(payloadBytes));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    bool ok = true;
    for (uint32_t layer = 0; layer < layerCount; ++layer) {
        while (glGetError() != GL_NO_ERROR) {
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFramebuffer);
        if (sc.arraySize > 1) {
            glFramebufferTextureLayer(
                GL_READ_FRAMEBUFFER,
                GL_COLOR_ATTACHMENT0,
                sc.textures[sc.releasedImage],
                0,
                static_cast<GLint>(subImage ? subImage->imageArrayIndex : layer));
        } else {
            glFramebufferTexture2D(
                GL_READ_FRAMEBUFFER,
                GL_COLOR_ATTACHMENT0,
                GL_TEXTURE_2D,
                sc.textures[sc.releasedImage],
                0);
        }

        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            if (!reportedFramebufferFailure) {
                __android_log_print(
                    ANDROID_LOG_INFO,
                    "Refract.Image",
                    "skip: framebuffer incomplete for tex=%u layer=%u status=0x%x",
                    sc.textures[sc.releasedImage],
                    layer,
                    glCheckFramebufferStatus(GL_READ_FRAMEBUFFER));
                reportedFramebufferFailure = true;
            }
            ok = false;
            break;
        }

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, transportFramebuffer);
        glFramebufferTexture2D(
            GL_DRAW_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D,
            transportTexture,
            0);
        if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            ok = false;
            break;
        }
        glBlitFramebuffer(
            sourceX,
            sourceY,
            sourceX + static_cast<GLint>(sourceWidth),
            sourceY + static_cast<GLint>(sourceHeight),
            0,
            0,
            static_cast<GLint>(transportWidth),
            static_cast<GLint>(transportHeight),
            GL_COLOR_BUFFER_BIT,
            GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, transportFramebuffer);
        glReadPixels(
            0,
            0,
            static_cast<GLsizei>(transportWidth),
            static_cast<GLsizei>(transportHeight),
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            payload.data() + static_cast<size_t>(layerBytes * layer));
        const GLenum readError = glGetError();
        if (readError != GL_NO_ERROR) {
            if (!reportedReadFailure) {
                __android_log_print(
                    ANDROID_LOG_INFO,
                    "Refract.Image",
                    "skip: glReadPixels failed err=0x%x tex=%u layer=%u size=%ux%u",
                    readError,
                    sc.textures[sc.releasedImage],
                    layer,
                    transportWidth,
                    transportHeight);
                reportedReadFailure = true;
            }
            ok = false;
            break;
        }
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
    glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);

    if (!ok) {
        return;
    }
    if (readback) {
        *readback = payload;
        static refract::protocol::PerfStats stats("eye-readback");
        stats.record(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readbackStart).count());
        return;
    }

    if (!reportedReadback) {
        __android_log_print(
            ANDROID_LOG_INFO,
            "Refract.Image",
            "readback ok seq=%llu %ux%u layers=%u bytes=%llu",
            static_cast<unsigned long long>(sequence),
            transportWidth,
            transportHeight,
            sc.arraySize,
            static_cast<unsigned long long>(payloadBytes));
        reportedReadback = true;
    }

    static refract::protocol::PerfStats readbackStats("image-readback");
    readbackStats.record(std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - readbackStart).count());
    if (image_transport_client().send_frame(
            sequence,
            transportWidth,
            transportHeight,
            sc.arraySize,
            payload.data(),
            payloadBytes) && sequence % 150 == 0) {
        __android_log_print(
            ANDROID_LOG_INFO,
            "Refract.Image",
            "sent frame seq=%llu %ux%u layers=%u bytes=%llu",
            static_cast<unsigned long long>(sequence),
            transportWidth,
            transportHeight,
            sc.arraySize,
            static_cast<unsigned long long>(payloadBytes));
    }
}
#else
void maybe_send_swapchain_image(const SwapchainRecord&) {}
#endif

// XR_FB_composition_layer_image_layout: OVRPlugin marks layers whose texture has a bottom-left
// origin (e.g. some of AC Nexus's UI panels); without the flag they show upside down.
bool layer_vertically_flipped(const XrCompositionLayerBaseHeader* layer)
{
    for (auto* next = static_cast<const XrCompositionLayerBaseHeader*>(layer->next); next;
         next = static_cast<const XrCompositionLayerBaseHeader*>(next->next)) {
        if (next->type == XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB)
            return (reinterpret_cast<const XrCompositionLayerImageLayoutFB*>(next)->flags &
                    XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB) != 0;
    }
    return false;
}

#if defined(__ANDROID__)
// Pipelined shared export: xrEndFrame submits the frame's copy into the next ring pair and returns,
// and the publisher thread sends the frame as soon as that copy completes. The app's render thread
// never waits for the GPU copy or the consumer's acknowledgment (~5 ms per frame, and the app's main
// thread often waits on this thread). Publishing the previous frame from the next xrEndFrame instead
// held every frame back a whole app frame (~11 ms of extra head and controller latency in VR).
// Scene-only (v3) and composite (v10) frames share this one ordered pipeline.
struct PendingExport {
    int slot = -1;
    uint32_t width = 0, height = 0;
    refract::protocol::ImageProjection projection{};
    std::vector<uint8_t> composite;  // Composite table (after the WindowsGpuFrame); empty for v3.
    uint32_t poseTag = 0;  // view_pose_tag of the frame.
};

bool publish_export(const PendingExport& pending)
{
    refract::protocol::WindowsGpuMarker marker{};
    if (!g_vulkan.export_wait(pending.slot, &marker)) return false;
    refract::protocol::WindowsGpuFrame gpu{marker.session, {marker.formats[0], marker.formats[1]}};
    const uint64_t sequence = g_imageFrameSequence++;
    const bool sent = pending.composite.empty()
        ? image_transport_client().send_frame(sequence, pending.width, pending.height, 2,
              reinterpret_cast<const uint8_t*>(&gpu), sizeof(gpu), &pending.projection, true, 0, false, pending.poseTag)
        : image_transport_client().send_composite_frame(sequence, pending.width, pending.height, pending.projection, gpu,
              pending.composite, pending.poseTag);
    if (!sent) return false;
    if (sequence % 90 == 0) __android_log_print(ANDROID_LOG_INFO, "Refract.GPU", "shared GPU %s seq=%llu %ux%u; published on copy completion",
        pending.composite.empty() ? "eyes" : "composite", static_cast<unsigned long long>(sequence), pending.width, pending.height);
    return true;
}

class ExportPublisher {
public:
    // Queues a submitted export; frames are published in order.
    void push(PendingExport&& frame)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!started_) {
                std::thread([this] { run(); }).detach();
                started_ = true;
            }
            queue_.push_back(std::move(frame));
        }
        ready_.notify_one();
    }

    // Waits until at most `unpublished` queued exports are not yet sent. False once a publication
    // failed (the consumer went away); the exports after it were dropped.
    bool wait(size_t unpublished)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait(lock, [&] { return queue_.size() + (busy_ ? 1 : 0) <= unpublished; });
        return !failed_;
    }

    // Drops the unsent exports and forgets a failure, once the one being sent is done.
    void drop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        queue_.clear();
        idle_.wait(lock, [&] { return !busy_; });
        failed_ = false;
    }

private:
    void run()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            ready_.wait(lock, [this] { return !queue_.empty(); });
            PendingExport frame = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
            lock.unlock();
            const bool published = publish_export(frame);
            lock.lock();
            busy_ = false;
            if (!published) {
                failed_ = true;
                queue_.clear();
            }
            idle_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_, idle_;
    std::deque<PendingExport> queue_;
    bool busy_ = false, failed_ = false, started_ = false;
};

ExportPublisher& export_publisher()
{
    static auto* publisher = new ExportPublisher;  // Never destroyed: its thread is detached.
    return *publisher;
}

// Before an export refills a ring pair, the frame that used it kExportRing exports ago must be
// published and acknowledged: with at most the newest export unpublished, at most kExportRing - 2
// sent frames may be unacknowledged. (The copies finish within a frame, so this rarely waits.)
bool export_ring_pair_free()
{
    return export_publisher().wait(1) && image_transport_client().await_acks(VulkanBackend::kExportRing - 2);
}

// Shared export stops when its consumer goes away (e.g. the viewer restarts);
// resume it once the image stream has connected again.
bool g_gpuConsumerLost = false;
uint64_t g_gpuLostAtConnection = 0;

void resume_gpu_export_if_reconnected()
{
    if (g_gpuConsumerLost && g_vulkan.active() && image_transport_client().connections() != g_gpuLostAtConnection) {
        g_gpuConsumerLost = false;
        if (g_vulkan.reenable_gpu_export())
            __android_log_print(ANDROID_LOG_INFO, "Refract.GPU", "image stream reconnected; resuming shared GPU export");
    }
}

void gpu_consumer_lost()
{
    // Never reuse shared images after an uncertain consumer completion.
    export_publisher().drop();
    g_vulkan.disable_gpu_export();
    g_gpuConsumerLost = true;
    g_gpuLostAtConnection = image_transport_client().connections();
    __android_log_print(ANDROID_LOG_WARN, "Refract.GPU", "GPU consumer unavailable; disabling shared export for this session");
}

bool valid_subimage(const XrSwapchainSubImage& sub, SwapchainRecord*& sc)
{
    sc = find_swapchain(sub.swapchain);
    const auto& rect = sub.imageRect;
    return sc && sc->hasReleasedImage && sc->vulkan.images[sc->releasedImage] && rect.offset.x >= 0 && rect.offset.y >= 0 &&
        rect.extent.width > 0 && rect.extent.height > 0 &&
        static_cast<uint64_t>(rect.offset.x) + rect.extent.width <= sc->width &&
        static_cast<uint64_t>(rect.offset.y) + rect.extent.height <= sc->height && sub.imageArrayIndex < sc->arraySize;
}

// Frames with UI panels (quad layers), optionally over a projection scene, go out as one
// pipelined composite frame (AXRI v10): every layer is blitted into a shared atlas pair and
// the viewer draws each panel at its pose. Returns false, having sent nothing, when the frame
// cannot take this path (no panels, no shared GPU export, other layer types, bad input);
// the other paths then handle it (and report errors).
bool submit_composite_frame(const XrFrameEndInfo& info, XrResult& result)
{
    namespace proto = refract::protocol;
    if (!g_vulkanSession || !direct_to_host() || !info.layers) return false;
    // debug.refract.composite=0 sends the mixed GPU batch instead: one synchronous copy and
    // acknowledgment per layer inside xrEndFrame (~5 ms each), kept for consumers without v10.
    static const bool compositeEnabled = int_property("debug.refract.composite", 1, 0, 1) != 0;
    if (!compositeEnabled) return false;
    // After a lost consumer nothing else reconnects for panel frames (the batch path needs export
    // on), so try here; a new connection turns shared export back on.
    if (g_gpuConsumerLost && image_transport_client().connected()) resume_gpu_export_if_reconnected();
    if (!g_vulkan.gpu_export_enabled()) return false;
    const XrCompositionLayerProjection* scene = nullptr;
    std::vector<const XrCompositionLayerQuad*> quads;
    for (uint32_t i = 0; i < info.layerCount; ++i) {
        const auto* base = static_cast<const XrCompositionLayerBaseHeader*>(info.layers[i]);
        if (!base) return false;
        if (i == 0 && base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            scene = reinterpret_cast<const XrCompositionLayerProjection*>(base);
        else if (base->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
            quads.push_back(reinterpret_cast<const XrCompositionLayerQuad*>(base));
        else
            return false;
    }
    if (quads.empty()) return false;

    proto::ImageProjection projection{};
    projection.view_count = 2;
    std::vector<VulkanBackend::AtlasBlit> blits;
    uint32_t sceneWidth = 0, sceneHeight = 0;
    if (scene) {
        if (scene->viewCount != 2 || !scene->views || (scene->layerFlags & ~uint64_t{7})) return false;
        const auto* space = find_space(scene->space);
        if (!space) return false;
        const XrPosef spaceWorld = world_pose_for_space(*space, g_lastViewPoseFrame);
        projection.layer_flags = static_cast<uint32_t>(scene->layerFlags);
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const auto& view = scene->views[eye];
            SwapchainRecord* sc = nullptr;
            if (view.type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW || !valid_subimage(view.subImage, sc)) return false;
            // Scaled to the recommended size, like the scene-only path.
            const uint32_t w = std::min<uint32_t>(g_renderWidth, view.subImage.imageRect.extent.width);
            const uint32_t h = std::min<uint32_t>(g_renderHeight, view.subImage.imageRect.extent.height);
            if (eye == 0) { sceneWidth = w; sceneHeight = h; }
            if (w != sceneWidth || h != sceneHeight) return false;
            blits.push_back({&sc->vulkan, sc->releasedImage, view.subImage, eye, 0, 0, int32_t(w), int32_t(h)});
            const auto pose = multiply_pose(spaceWorld, view.pose);
            projection.views[eye] = {{pose.position.x, pose.position.y, pose.position.z,
                pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w},
                view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp, view.fov.angleDown};
        }
    } else {
        // Panels only (menus, loading screens): place them with the views the app was given.
        const XrPosef head = protocol_pose_to_xr(g_lastViewPoseFrame.hmd);
        float halfWidth = 0, halfHeight = 0;
        view_half_angles(halfWidth, halfHeight);
        for (uint32_t eye = 0; eye < 2; ++eye) {
            XrPosef offset = identity_pose();
            offset.position.x = eye == 0 ? -kEyeHalfIpdMeters : kEyeHalfIpdMeters;
            const auto pose = multiply_pose(head, offset);
            projection.views[eye] = {{pose.position.x, pose.position.y, pose.position.z,
                pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w},
                -halfWidth, halfWidth, halfHeight, -halfHeight};
        }
    }
    if (!proto::valid_projection(projection)) return false;

    // Atlas: the scene at (0, 0) in both textures, panels packed to its right and below it.
    constexpr uint32_t kPanelArea = 2048, kPad = 2, kMaxPanelSide = 2048;
    const uint32_t atlasWidth = sceneWidth + kPanelArea;
    const uint32_t atlasHeight = std::max(sceneHeight, kPanelArea);
    struct Panel {
        const XrCompositionLayerQuad* layer;
        SwapchainRecord* sc;
        proto::CompositeQuad entry;
        bool placed;
    };
    std::vector<Panel> panels;
    for (const auto* quad : quads) {
        if (panels.size() == proto::kMaxCompositeQuads) break;
        SwapchainRecord* sc = nullptr;
        const auto* quadSpace = find_space(quad->space);
        if (!quadSpace || !valid_subimage(quad->subImage, sc)) continue;
        const auto pose = multiply_pose(world_pose_for_space(*quadSpace, g_lastViewPoseFrame), quad->pose);
        Panel panel{quad, sc, {}, false};
        panel.entry.quad = {{pose.position.x, pose.position.y, pose.position.z,
            pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w},
            quad->size.width, quad->size.height, static_cast<uint32_t>(quad->eyeVisibility), static_cast<uint32_t>(quad->layerFlags & 7)};
        if (!proto::valid_quad(panel.entry.quad)) continue;
        panel.entry.texture = quad->eyeVisibility == 2 ? 1 : 0;  // XR_EYE_VISIBILITY_RIGHT
        panels.push_back(panel);
    }
    // Shelf packing per texture; shrink every panel until they fit (a few tries), then drop the rest.
    struct Region { uint32_t left, top, right, bottom; };
    const Region regions[] = {{sceneWidth ? sceneWidth + kPad : 0, 0, atlasWidth, atlasHeight},
                              {0, sceneHeight ? sceneHeight + kPad : atlasHeight, sceneWidth, atlasHeight}};
    std::vector<size_t> order(panels.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return panels[a].layer->subImage.imageRect.extent.height > panels[b].layer->subImage.imageRect.extent.height;
    });
    float scale = 1.0f;
    for (int attempt = 0; attempt < 6; ++attempt, scale *= 0.75f) {
        bool all = true;
        for (auto& panel : panels) panel.placed = false;
        for (uint32_t texture = 0; texture < 2; ++texture) {
            size_t region = 0;
            uint32_t x = regions[0].left, y = regions[0].top, shelf = 0;
            for (size_t i : order) {
                auto& panel = panels[i];
                if (panel.entry.texture != texture) continue;
                const auto& extent = panel.layer->subImage.imageRect.extent;
                const float fit = std::min(1.0f, float(kMaxPanelSide) / float(std::max(extent.width, extent.height)));
                const uint32_t w = std::max<uint32_t>(1, uint32_t(extent.width * fit * scale));
                const uint32_t h = std::max<uint32_t>(1, uint32_t(extent.height * fit * scale));
                panel.placed = false;
                while (region < 2) {
                    const auto& r = regions[region];
                    if (x + w > r.right) { x = r.left; y += shelf + kPad; shelf = 0; }
                    if (x + w <= r.right && y + h <= r.bottom) break;
                    if (++region < 2) { x = regions[region].left; y = regions[region].top; shelf = 0; }
                }
                if (region >= 2) { all = false; break; }
                panel.entry.x = x; panel.entry.y = y; panel.entry.width = w; panel.entry.height = h;
                panel.placed = true;
                x += w + kPad;
                shelf = std::max(shelf, h);
            }
        }
        if (all) break;
    }

    proto::CompositeHeader table{sceneWidth, sceneHeight, 0, 0};
    std::vector<proto::CompositeQuad> entries;
    for (const auto& panel : panels) {  // Back to front, as the app ordered them.
        if (!panel.placed) continue;
        entries.push_back(panel.entry);
        // Flipped panels are copied as stored and flipped by the viewer while sampling: a blit with
        // reversed destination rows made the host GPU copies several times slower.
        if (layer_vertically_flipped(reinterpret_cast<const XrCompositionLayerBaseHeader*>(panel.layer)))
            entries.back().texture |= proto::kCompositeQuadFlipped;
        blits.push_back({&panel.sc->vulkan, panel.sc->releasedImage, panel.layer->subImage, panel.entry.texture,
                         int32_t(panel.entry.x), int32_t(panel.entry.y), int32_t(panel.entry.width), int32_t(panel.entry.height)});
    }
    static size_t loggedFlipped = 0;
    const size_t flipped = static_cast<size_t>(std::count_if(entries.begin(), entries.end(),
        [](const proto::CompositeQuad& q) { return (q.texture & proto::kCompositeQuadFlipped) != 0; }));
    if (flipped != loggedFlipped) {
        loggedFlipped = flipped;
        __android_log_print(ANDROID_LOG_INFO, "Refract.GPU", "composite: %zu of %zu panels stored bottom-up", flipped, entries.size());
    }
    table.quad_count = static_cast<uint32_t>(entries.size());
    std::vector<uint8_t> tableBytes(sizeof(table) + entries.size() * sizeof(proto::CompositeQuad));
    std::memcpy(tableBytes.data(), &table, sizeof(table));
    if (!entries.empty()) std::memcpy(tableBytes.data() + sizeof(table), entries.data(), entries.size() * sizeof(proto::CompositeQuad));

    resume_gpu_export_if_reconnected();
    if (!export_ring_pair_free()) { gpu_consumer_lost(); result = XR_SUCCESS; return true; }
    const int slot = g_vulkan.export_atlas_async(blits.data(), static_cast<uint32_t>(blits.size()), atlasWidth, atlasHeight);
    if (slot < 0) return false;
    static bool reported = false;
    if (!reported) {
        __android_log_print(ANDROID_LOG_INFO, "Refract.Layer", "composite: scene %ux%u + %zu panel(s) in %ux%u atlas",
                            sceneWidth, sceneHeight, entries.size(), atlasWidth, atlasHeight);
        reported = true;
    }
    export_publisher().push({slot, atlasWidth, atlasHeight, projection, std::move(tableBytes),
                             frame_sync_enabled() ? view_pose_tag(info.displayTime) : 0});
    result = XR_SUCCESS;
    return true;
}
#endif

XrResult submit_projection_frame(const XrFrameEndInfo& info, uint32_t batchPart = 0, bool validateOnly = false)
{
    auto invalid = [&](const char* reason) {
#if defined(__ANDROID__)
        static uint32_t reports = 0;
        if (reports++ < 12) {
            __android_log_print(ANDROID_LOG_ERROR, "Refract.Layer", "reject: %s layers=%u", reason, info.layerCount);
            if (info.layers) for (uint32_t i = 0; i < std::min<uint32_t>(info.layerCount, 16); ++i) {
                const auto* item = static_cast<const XrCompositionLayerBaseHeader*>(info.layers[i]);
                if (item) __android_log_print(ANDROID_LOG_ERROR, "Refract.Layer", "layer[%u] type=%d flags=%llu", i,
                    item->type, static_cast<unsigned long long>(item->layerFlags));
            }
        }
#endif
        return XR_ERROR_LAYER_INVALID;
    };
    static bool reportedLayers = false;
    if (!reportedLayers && info.layerCount && info.layers && info.layers[0]) {
        const auto* first = static_cast<const XrCompositionLayerProjection*>(info.layers[0]);
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_INFO, "Refract.Layer", "layers=%u type=%d flags=%llu views=%u", info.layerCount,
            first->type, static_cast<unsigned long long>(first->layerFlags), first->viewCount);
#else
        std::fprintf(stderr, "Refract projection: layers=%u type=%d flags=%llu\n", info.layerCount,
            first->type, static_cast<unsigned long long>(first->layerFlags));
#endif
        reportedLayers = true;
    }
    if (info.layerCount == 0) { return XR_SUCCESS; }
    // Some Oculus builds submit equirect backgrounds even when the runtime did
    // not advertise XR_KHR_composition_layer_equirect2. Preserve the scene and
    // panels that this transport can display, in their original layer order.
    if (!batchPart && info.layerCount <= refract::protocol::kMaxCompositionLayers && info.layers) {
        const void* supported[refract::protocol::kMaxCompositionLayers]{};
        uint32_t supportedCount = 0;
        for (uint32_t i = 0; i < info.layerCount; ++i) {
            const auto* layer = static_cast<const XrCompositionLayerBaseHeader*>(info.layers[i]);
            if (layer && layer->type == XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR) continue;
            supported[supportedCount++] = info.layers[i];
        }
        if (supportedCount != info.layerCount && supportedCount > 0) {
#if defined(__ANDROID__)
            static bool reportedEquirectFallback = false;
            if (!reportedEquirectFallback) {
                __android_log_print(ANDROID_LOG_WARN, "Refract.Layer",
                                    "skipping %u unsupported equirect background layers",
                                    info.layerCount - supportedCount);
                reportedEquirectFallback = true;
            }
#endif
            auto filtered = info;
            filtered.layerCount = supportedCount;
            filtered.layers = supported;
            return submit_projection_frame(filtered);
        }
    }
#if defined(__ANDROID__)
    if (!batchPart && !validateOnly) {
        XrResult result = XR_SUCCESS;
        if (submit_composite_frame(info, result)) return result;
    }
#endif
    // Mixed scene/panel frames are transferred as an atomic GPU batch. Validate
    // every member before publishing any part; preserve application layer order.
    if (!batchPart && info.layerCount >= 2 && info.layerCount <= refract::protocol::kMaxCompositionLayers && info.layers && info.layers[0] &&
        (info.layerCount > 2 || static_cast<const XrCompositionLayerBaseHeader*>(info.layers[0])->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)) {
        for (uint32_t i = 0; i < info.layerCount; ++i) {
            if (!info.layers[i] || (static_cast<const XrCompositionLayerBaseHeader*>(info.layers[i])->type != XR_TYPE_COMPOSITION_LAYER_QUAD && (i || static_cast<const XrCompositionLayerBaseHeader*>(info.layers[i])->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)))
                return invalid("mixed layer type");
            auto part = info; part.layerCount = 1; part.layers = &info.layers[i];
            const auto result = submit_projection_frame(part, (info.layerCount << 16) | i, true);
            if (result != XR_SUCCESS) return result;
        }
#if defined(__ANDROID__)
        if (!(g_vulkanSession || g_glesShare.enabled()) || g_vulkan.gpu_marker().status != 1) {
            // Pixel readback carries one image pair per frame. Keep the scene
            // projection (or the first two panels) rather than failing the frame.
            static bool reportedDrop = false;
            if (!reportedDrop) {
                __android_log_print(ANDROID_LOG_WARN, "Refract.Layer", "readback path: dropping %u extra layer(s)", info.layerCount - 1);
                reportedDrop = true;
            }
            auto part = info;
            const bool sceneFirst = static_cast<const XrCompositionLayerBaseHeader*>(info.layers[0])->type ==
                XR_TYPE_COMPOSITION_LAYER_PROJECTION;
            char mode[PROP_VALUE_MAX]{};
            __system_property_get("debug.refract.readback_layers", mode);
            if (sceneFirst && std::strcmp(mode, "panels") == 0) {
                // Debug view of the UI panels (quads) instead of the scene.
                part.layers = &info.layers[1];
                part.layerCount = std::min<uint32_t>(info.layerCount - 1, 2);
                if (part.layerCount == 2 && submit_projection_frame(part, 0, true) != XR_SUCCESS) part.layerCount = 1;
                return submit_projection_frame(part);
            }
            part.layerCount = sceneFirst ? 1 : 2;
            return submit_projection_frame(part);
        }
#endif
        for (uint32_t i = 0; i < info.layerCount; ++i) {
            auto part = info; part.layerCount = 1; part.layers = &info.layers[i];
            const auto result = submit_projection_frame(part, (info.layerCount << 16) | i);
            if (result != XR_SUCCESS) return result;
        }
        return XR_SUCCESS;
    }
    if (info.layerCount > 2 || info.layers == nullptr || info.layers[0] == nullptr) { return invalid("layer count/pointer"); }
    const auto* layer = static_cast<const XrCompositionLayerProjection*>(info.layers[0]);
    const bool quads = layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD;
    if (!quads && (info.layerCount != 1 || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION || layer->viewCount != 2 || layer->views == nullptr ||
        (layer->layerFlags & ~uint64_t{7}) != 0)) { return invalid("projection type/views/flags"); }
    const auto* space = find_space(layer->space);
    if (!space) { return XR_ERROR_HANDLE_INVALID; }
    const XrPosef spaceWorld = world_pose_for_space(*space, g_lastViewPoseFrame);
    refract::protocol::ImageProjection projection{};
    projection.view_count = 2;
    projection.layer_flags = static_cast<uint32_t>(layer->layerFlags);
    std::array<SwapchainRecord*, 2> swapchains{};
    const XrSwapchainSubImage* subimages[2]{};
    if (quads) {
        projection.view_count = refract::protocol::kQuadCompositionBit | info.layerCount;
        projection.layer_flags = 0;
        for (uint32_t i = 0; i < info.layerCount; ++i) {
            const auto* quad = static_cast<const XrCompositionLayerQuad*>(info.layers[i]);
            if (!quad || quad->type != XR_TYPE_COMPOSITION_LAYER_QUAD || (quad->layerFlags & ~uint64_t{7})) return invalid("quad type/flags");
            const auto* quadSpace = find_space(quad->space);
            if (!quadSpace) return XR_ERROR_HANDLE_INVALID;
            const auto pose = multiply_pose(world_pose_for_space(*quadSpace, g_lastViewPoseFrame), quad->pose);
            projection.quads[i] = {{pose.position.x, pose.position.y, pose.position.z,
                pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w},
                quad->size.width, quad->size.height, static_cast<uint32_t>(quad->eyeVisibility), static_cast<uint32_t>(quad->layerFlags)};
            // Copied as stored; the host flips it (a flipping blit is slow under gfxstream, see the composite path).
            if (layer_vertically_flipped(reinterpret_cast<const XrCompositionLayerBaseHeader*>(quad)))
                projection.quads[i].layer_flags |= refract::protocol::kQuadLayerFlipped;
            subimages[i] = &quad->subImage;
        }
        if (!refract::protocol::valid_quads(projection)) return invalid("quad pose/size/visibility");
        // The existing shared texture pair carries two ordered panel images;
        // a one-panel frame duplicates its image but submits only one layer.
        if (info.layerCount == 1) subimages[1] = subimages[0];
    } else {
        subimages[0] = &layer->views[0].subImage;
        subimages[1] = &layer->views[1].subImage;
    }
    uint32_t width = 0, height = 0;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const auto& sub = *subimages[eye];
        auto* sc = find_swapchain(sub.swapchain);
        if (!sc) { return XR_ERROR_HANDLE_INVALID; }
        if (!sc->hasReleasedImage) { return XR_ERROR_CALL_ORDER_INVALID; }
        const auto& rect = sub.imageRect;
        if (rect.offset.x < 0 || rect.offset.y < 0 || rect.extent.width <= 0 || rect.extent.height <= 0 ||
            static_cast<uint64_t>(rect.offset.x) + rect.extent.width > sc->width ||
            static_cast<uint64_t>(rect.offset.y) + rect.extent.height > sc->height ||
            sub.imageArrayIndex >= sc->arraySize) { return XR_ERROR_SWAPCHAIN_RECT_INVALID; }
        const uint32_t eyeWidth = std::min<uint32_t>(g_renderWidth, rect.extent.width);
        const uint32_t eyeHeight = std::min<uint32_t>(g_renderHeight, rect.extent.height);
        if (eye == 0) { width = eyeWidth; height = eyeHeight; }
        if (width != eyeWidth || height != eyeHeight) { return invalid("unequal eye dimensions"); }
        swapchains[eye] = sc;
        if (quads) continue;
        const auto& view = layer->views[eye];
        if (view.type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW) { return invalid("view type"); }
        const auto pose = multiply_pose(spaceWorld, view.pose);
        auto& out = projection.views[eye];
        out.pose = {pose.position.x, pose.position.y, pose.position.z,
                    pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
        out.angle_left = view.fov.angleLeft;
        out.angle_right = view.fov.angleRight;
        out.angle_up = view.fov.angleUp;
        out.angle_down = view.fov.angleDown;
    }
    if (!quads && !refract::protocol::valid_projection(projection)) {
#if defined(__ANDROID__)
        static bool reportedInvalidPose = false;
        if (!reportedInvalidPose) for (const auto& eye : projection.views)
            __android_log_print(ANDROID_LOG_ERROR, "Refract.Layer", "pose q=(%f %f %f %f) fov=(%f %f %f %f)",
                eye.pose.qx, eye.pose.qy, eye.pose.qz, eye.pose.qw, eye.angle_left, eye.angle_right, eye.angle_up, eye.angle_down);
        reportedInvalidPose = true;
#endif
        return invalid("render pose/FOV");
    }
    if (validateOnly) return XR_SUCCESS;
#if defined(__ANDROID__)
    if (!direct_to_host()) {
        // The legacy Java proxy understands only v1. Preserve that development path.
        if (g_lastReleasedSwapchain) { maybe_send_swapchain_image(*g_lastReleasedSwapchain); }
        return XR_SUCCESS;
    }
    // GLES: draw the eyes into buffers the private Vulkan device imported, so they take the
    // Vulkan paths below (shared GPU export, or asynchronous pixel readback).
    bool useVulkan = g_vulkanSession;
    VulkanSwapchain glesMirrors[2];
    XrSwapchainSubImage glesSubimages[2];
    if (!g_vulkanSession && g_glesShare.enabled()) {
        GlesShare::Source sources[2];
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const auto& sc = *swapchains[eye];
            sources[eye] = {sc.textures[sc.releasedImage], sc.arraySize > 1, sc.format == 0x8C43 /* GL_SRGB8_ALPHA8 */,
                            subimages[eye]->imageArrayIndex, subimages[eye]->imageRect};
        }
        if (g_glesShare.prepare(sources, glesMirrors, glesSubimages)) {
            useVulkan = true;
            subimages[0] = &glesSubimages[0];
            subimages[1] = &glesSubimages[1];
        }
    }
    if (batchPart && !useVulkan) return XR_ERROR_RUNTIME_FAILURE;
    // debug.refract.stream_scale (percent) shrinks the streamed image; the GPU blit
    // does the scaling, so it cuts readback and transfer without touching the app's rendering.
    // (The GL readback below always copies at full size.)
    const int streamScale = int_property("debug.refract.stream_scale", 100, 10, 100);
    if (streamScale < 100 && !batchPart && useVulkan) {
        width = std::max<uint32_t>(16, width * streamScale / 100 & ~1u);
        height = std::max<uint32_t>(16, height * streamScale / 100 & ~1u);
    }
    static std::vector<uint8_t> eyes[2];
    const size_t eyeBytes = static_cast<size_t>(width) * height * 4;
    resume_gpu_export_if_reconnected();
    if (useVulkan) {
        const VulkanSwapchain* vkSwapchains[] = {g_vulkanSession ? &swapchains[0]->vulkan : &glesMirrors[0],
                                                 g_vulkanSession ? &swapchains[1]->vulkan : &glesMirrors[1]};
        const uint32_t indices[] = {g_vulkanSession ? swapchains[0]->releasedImage : 0u,
                                    g_vulkanSession ? swapchains[1]->releasedImage : 0u};
        if (!g_vulkan.gpu_export_enabled()) {
            // Submit the copy and return; the sender thread waits for the GPU. Waiting here
            // would serialize the app's CPU and GPU work (it halved the frame rate on a phone).
            if (batchPart) return XR_ERROR_RUNTIME_FAILURE;
            const uint32_t streamLayers = quads ? 2 : static_cast<uint32_t>(int_property("debug.refract.stream_eyes", 2, 1, 2));
            // For the H.264 stream the GPU also converts to the encoder's NV12.
            char mode[PROP_VALUE_MAX]{};
            __system_property_get("debug.refract.video", mode);
            bool nv12 = streamLayers == 2 && std::strcmp(mode, "h264") == 0;
            const int slot = g_vulkan.readback_async(vkSwapchains, indices, subimages, width, height, &nv12);
            if (slot >= 0) async_frame_sender().submit_readback(slot, nv12, g_imageFrameSequence++, width, height, streamLayers, projection);
            return XR_SUCCESS;
        }
        const auto consumerLost = gpu_consumer_lost;
        if (!batchPart) {
            if (!export_ring_pair_free()) { consumerLost(); return XR_SUCCESS; }
            const int slot = g_vulkan.export_async(vkSwapchains, indices, subimages, width, height);
            if (slot >= 0) {
                export_publisher().push({slot, width, height, projection, {}, frame_sync_enabled() ? view_pose_tag(info.displayTime) : 0});
                return XR_SUCCESS;
            }
            // No ring for this configuration: the synchronous export below still works.
        }
        // Keep frames in order: a mixed-layer batch goes out after every queued frame.
        if (!export_publisher().wait(0)) {
            consumerLost();
            return batchPart ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS;
        }
        if (!g_vulkan.readback(vkSwapchains, indices, subimages, width, height, eyes)) return XR_ERROR_RUNTIME_FAILURE;
        if (g_vulkan.gpu_marker().status == 1) {
            const auto& marker = g_vulkan.gpu_marker();
            refract::protocol::WindowsGpuFrame gpu{marker.session, {marker.formats[0], marker.formats[1]}};
            uint64_t sequence = g_imageFrameSequence++;
            if (image_transport_client().send_frame(sequence, width, height, 2,
                    reinterpret_cast<const uint8_t*>(&gpu), sizeof(gpu), &projection, true, batchPart)) {
                if (sequence % 90 == 0) __android_log_print(ANDROID_LOG_INFO, "Refract.GPU", "shared GPU eyes seq=%llu %ux%u; no pixel readback", static_cast<unsigned long long>(sequence), width, height);
                return XR_SUCCESS;
            }
            // Never reuse shared images after an uncertain consumer completion.
            g_vulkan.disable_gpu_export();
            g_gpuConsumerLost = true;
            g_gpuLostAtConnection = image_transport_client().connections();
            if (batchPart) return XR_ERROR_RUNTIME_FAILURE;
            __android_log_print(ANDROID_LOG_WARN, "Refract.GPU", "GPU consumer unavailable; disabling shared export for this session");
            if (!g_vulkan.readback(vkSwapchains, indices, subimages, width, height, eyes)) return XR_ERROR_RUNTIME_FAILURE;
        }
    }
    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (!useVulkan) {
            maybe_send_swapchain_image(*swapchains[eye], subimages[eye], &eyes[eye]);
        }
        if (eyes[eye].size() != eyeBytes) { return XR_ERROR_RUNTIME_FAILURE; }
    }
    if (batchPart) return XR_ERROR_RUNTIME_FAILURE;
    // debug.refract.stream_eyes=1 streams only the left eye (quad frames keep both panels).
    const uint32_t streamLayers = quads ? 2 : static_cast<uint32_t>(int_property("debug.refract.stream_eyes", 2, 1, 2));
    async_frame_sender().submit(g_imageFrameSequence++, width, height, streamLayers, eyes, eyeBytes, projection);
#endif
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrCreateInstance_impl(const XrInstanceCreateInfo* createInfo, XrInstance* instance)
{
    log_call("xrCreateInstance");
    refract::runtime::apply_game_patches();
    if (createInfo == nullptr || instance == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (createInfo->applicationInfo.apiVersion > XR_MAKE_VERSION(1, 1, 0)) {
        return XR_ERROR_API_VERSION_UNSUPPORTED;
    }

    *instance = fake_instance();
    g_sessionState = XR_SESSION_STATE_UNKNOWN;
    g_pendingSessionEvents.clear();
    g_pendingInteractionProfileEvent = false;
    g_spaceCount = 0;
    g_pathCount = 0;
    g_spaces.clear();
    g_paths.clear();
    g_actionHandles.clear();
    g_actionIndex.clear();
    g_actionCount = 0;
    g_nextPath = 1;
    for (auto& sc : g_swapchains) {
        destroy_swapchain_images(sc);
        sc = {};
    }
    g_lastReleasedSwapchain = nullptr;
    g_imageFrameSequence = 0;
    g_nextFrameStart = 0;
    g_renderWidth = g_renderHeight = 1024;
    g_renderExtentQueried = false;
#if defined(__ANDROID__)
    g_glesShare.shutdown();
    g_vulkan.shutdown();
    g_vulkanSession = false;
    g_vulkanRequirementsQueried = false;
    g_vulkanInstance = VK_NULL_HANDLE;
#endif
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrInitializeLoaderKHR_impl(const void* loaderInitInfo)
{
    log_call("xrInitializeLoaderKHR");
    if (loaderInitInfo == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
#if defined(__ANDROID__)
    const auto* initInfo = static_cast<const AndroidLoaderInitInfo*>(loaderInitInfo);
    if (initInfo->applicationVM != nullptr && initInfo->applicationContext != nullptr) {
        pose_client().set_android_context(
            static_cast<JavaVM*>(initInfo->applicationVM),
            static_cast<jobject>(initInfo->applicationContext));
    }
#endif
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrDestroyInstance_impl(XrInstance instance)
{
    log_call("xrDestroyInstance");
    if (!is_valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    for (auto& sc : g_swapchains) { destroy_swapchain_images(sc); sc = {}; }
    g_lastReleasedSwapchain = nullptr;
    for (auto& tracker : g_handTrackers) tracker.alive = false;
#if defined(__ANDROID__)
    g_glesShare.shutdown();
    g_vulkan.shutdown();
    g_vulkanSession = false;
#endif
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties_impl(
    const char* layerName,
    uint32_t propertyCapacityInput,
    uint32_t* propertyCountOutput,
    XrExtensionProperties* properties)
{
    log_call("xrEnumerateInstanceExtensionProperties");
    if (layerName != nullptr && layerName[0] != '\0') {
        return XR_ERROR_API_LAYER_NOT_PRESENT;
    }
    if (propertyCountOutput == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    constexpr const char* kExtensions[] = {
        "XR_KHR_android_create_instance",
        "XR_KHR_opengl_es_enable",
        "XR_KHR_vulkan_enable",
        "XR_FB_display_refresh_rate",
        "XR_META_recommended_layer_resolution",
        "XR_EXT_hand_tracking",
        "XR_EXT_hand_tracking_data_source",
#if defined(__ANDROID__) || defined(REFRACT_INPUT_FIXTURE)
        "XR_KHR_convert_timespec_time",
#endif
#if defined(__ANDROID__)
        "XR_KHR_vulkan_enable2",
        // OVRPlugin stops its eye layer after the first frame when fixed
        // foveation cannot be applied. Accept the profile calls; the host
        // bridge renders full density regardless.
        "XR_FB_foveation",
        "XR_FB_foveation_configuration",
        "XR_FB_swapchain_update_state",
        "XR_FB_composition_layer_image_layout",  // Honored for quad layers by the composite path.
#endif
    };

    *propertyCountOutput = static_cast<uint32_t>(sizeof(kExtensions) / sizeof(kExtensions[0]));
    if (propertyCapacityInput == 0 || properties == nullptr) {
        return XR_SUCCESS;
    }

    const uint32_t count = propertyCapacityInput < *propertyCountOutput
        ? propertyCapacityInput
        : *propertyCountOutput;
    for (uint32_t i = 0; i < count; ++i) {
        if (properties[i].type != XR_TYPE_EXTENSION_PROPERTIES) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        std::strncpy(properties[i].extensionName, kExtensions[i], XR_MAX_EXTENSION_NAME_SIZE - 1);
        properties[i].extensionName[XR_MAX_EXTENSION_NAME_SIZE - 1] = '\0';
        properties[i].extensionVersion = 1;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetInstanceProperties_impl(
    XrInstance instance,
    XrInstanceProperties* instanceProperties)
{
    log_call("xrGetInstanceProperties");
    if (!is_valid_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (instanceProperties == nullptr || instanceProperties->type != XR_TYPE_INSTANCE_PROPERTIES) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    instanceProperties->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    std::strncpy(instanceProperties->runtimeName, "Android XR Bridge Runtime", XR_MAX_RUNTIME_NAME_SIZE - 1);
#if defined(__ANDROID__)
    // Some vendor plugins refuse runtimes by name; allow an emulator-level identity override.
    char runtimeName[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.refract.runtime_name", runtimeName) > 0) {
        std::strncpy(instanceProperties->runtimeName, runtimeName, XR_MAX_RUNTIME_NAME_SIZE - 1);
    }
    char runtimeVersion[PROP_VALUE_MAX] = {};
    unsigned major = 0, minor = 0, patch = 0;
    if (__system_property_get("debug.refract.runtime_version", runtimeVersion) > 0 &&
        std::sscanf(runtimeVersion, "%u.%u.%u", &major, &minor, &patch) == 3) {
        instanceProperties->runtimeVersion = XR_MAKE_VERSION(major, minor, patch);
    }
#endif
    instanceProperties->runtimeName[XR_MAX_RUNTIME_NAME_SIZE - 1] = '\0';
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetSystem_impl(
    XrInstance instance,
    const XrSystemGetInfo* getInfo,
    XrSystemId* systemId)
{
    log_call("xrGetSystem");
    if (!is_valid_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (getInfo == nullptr || systemId == nullptr || getInfo->type != XR_TYPE_SYSTEM_GET_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (getInfo->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
        return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    }

    *systemId = kSystemId;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetSystemProperties_impl(
    XrInstance instance,
    XrSystemId systemId,
    XrSystemProperties* properties)
{
    log_call("xrGetSystemProperties");
    if (!is_valid_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != kSystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (properties == nullptr || properties->type != XR_TYPE_SYSTEM_PROPERTIES) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    properties->systemId = kSystemId;
    properties->vendorId = 0;
    std::strncpy(properties->systemName, "Refract Fake HMD", XR_MAX_SYSTEM_NAME_SIZE - 1);
    properties->systemName[XR_MAX_SYSTEM_NAME_SIZE - 1] = '\0';
    properties->graphicsProperties.maxSwapchainImageHeight = 16384;
    properties->graphicsProperties.maxSwapchainImageWidth = 16384;
    properties->graphicsProperties.maxLayerCount = refract::protocol::kMaxCompositionLayers;
    properties->trackingProperties.orientationTracking = 1;
    properties->trackingProperties.positionTracking = 1;
    struct OutputHeader { XrStructureType type; void* next; };
    for (auto* next = static_cast<OutputHeader*>(properties->next); next; next = static_cast<OutputHeader*>(next->next)) {
        if (next->type == XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT)
            // Capability is independent of whether the first asynchronous host
            // packet has arrived or controllers are currently awake.
            reinterpret_cast<XrSystemHandTrackingPropertiesEXT*>(next)->supportsHandTracking = 1;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetOpenGLESGraphicsRequirementsKHR_impl(
    XrInstance instance,
    XrSystemId systemId,
    XrGraphicsRequirementsOpenGLESKHR* graphicsRequirements)
{
    log_call("xrGetOpenGLESGraphicsRequirementsKHR");
    if (!is_valid_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != kSystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (graphicsRequirements == nullptr ||
        graphicsRequirements->type != XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    graphicsRequirements->minApiVersionSupported = XR_MAKE_VERSION(3, 0, 0);
    graphicsRequirements->maxApiVersionSupported = XR_MAKE_VERSION(3, 2, 0);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetVulkanGraphicsRequirementsKHR_impl(
    XrInstance instance,
    XrSystemId systemId,
    XrGraphicsRequirementsVulkanKHR* graphicsRequirements)
{
    log_call("xrGetVulkanGraphicsRequirementsKHR");
    if (!is_valid_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != kSystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (graphicsRequirements == nullptr ||
        graphicsRequirements->type != XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    graphicsRequirements->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    graphicsRequirements->maxApiVersionSupported = XR_MAKE_VERSION(1, 1, 0);
#if defined(__ANDROID__)
    g_vulkanRequirementsQueried = true;
#endif
    return XR_SUCCESS;
}

#if defined(__ANDROID__)
XrResult XRAPI_CALL xrGetVulkanExtensionsKHR_impl(XrInstance instance, XrSystemId systemId,
                                                uint32_t capacity, uint32_t* count, char* buffer) {
    if (!is_valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (systemId != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (!count || (capacity && !buffer)) return XR_ERROR_VALIDATION_FAILURE;
    *count = 1; // No runtime-specific Vulkan extensions are needed for CPU readback.
    if (capacity) buffer[0] = '\0';
    return XR_SUCCESS;
}
XrResult XRAPI_CALL xrGetVulkanGraphicsDeviceKHR_impl(XrInstance instance, XrSystemId systemId,
                                                    VkInstance vkInstance, VkPhysicalDevice* device) {
    if (!is_valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (systemId != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (!vkInstance || !device) return XR_ERROR_VALIDATION_FAILURE;
    *device = VulkanBackend::choose_device(vkInstance);
    g_vulkanInstance = vkInstance;
    return *device ? XR_SUCCESS : XR_ERROR_GRAPHICS_DEVICE_INVALID;
}
XrResult XRAPI_CALL xrGetVulkanGraphicsDevice2KHR_impl(XrInstance instance,
        const XrVulkanGraphicsDeviceGetInfoKHR* info, VkPhysicalDevice* device) {
    if (!info || info->type != XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR) return XR_ERROR_VALIDATION_FAILURE;
    return xrGetVulkanGraphicsDeviceKHR_impl(instance, info->systemId, info->vulkanInstance, device);
}
XrResult XRAPI_CALL xrCreateVulkanInstanceKHR_impl(XrInstance instance, const XrVulkanInstanceCreateInfoKHR* info,
                                                VkInstance* vkInstance, VkResult* result) {
    if (!is_valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR || !vkInstance || !result ||
        !info->vulkanCreateInfo || !info->pfnGetInstanceProcAddr || info->createFlags) return XR_ERROR_VALIDATION_FAILURE;
    if (info->systemId != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(info->pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) return XR_ERROR_RUNTIME_FAILURE;
    *result = create(info->vulkanCreateInfo, info->vulkanAllocator, vkInstance);
    if (*result == VK_SUCCESS) g_vulkanInstance = *vkInstance;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL xrCreateVulkanDeviceKHR_impl(XrInstance instance, const XrVulkanDeviceCreateInfoKHR* info,
                                              VkDevice* device, VkResult* result) {
    if (!is_valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR || !device || !result ||
        !info->vulkanCreateInfo || !info->pfnGetInstanceProcAddr || info->createFlags) return XR_ERROR_VALIDATION_FAILURE;
    if (info->systemId != kSystemId) return XR_ERROR_SYSTEM_INVALID;
    if (!g_vulkanInstance || info->vulkanPhysicalDevice != VulkanBackend::choose_device(g_vulkanInstance)) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    auto create = reinterpret_cast<PFN_vkCreateDevice>(info->pfnGetInstanceProcAddr(g_vulkanInstance, "vkCreateDevice"));
    if (!create) return XR_ERROR_RUNTIME_FAILURE;
    *result = create(info->vulkanPhysicalDevice, info->vulkanCreateInfo, info->vulkanAllocator, device);
    return XR_SUCCESS;
}
#endif

XrResult XRAPI_CALL xrCreateSession_impl(
    XrInstance instance,
    const XrSessionCreateInfo* createInfo,
    XrSession* session)
{
    log_call("xrCreateSession");
    refract::runtime::apply_game_patches();  // In case the game code loaded after the instance.
    if (!is_valid_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo == nullptr || session == nullptr || createInfo->type != XR_TYPE_SESSION_CREATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (createInfo->systemId != kSystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }

#if defined(__ANDROID__)
    struct Base { XrStructureType type; const Base* next; };
    for (auto* next = static_cast<const Base*>(createInfo->next); next; next = next->next) {
        if (next->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
            if (!g_vulkanRequirementsQueried) return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
            if (!g_vulkan.initialize(*reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(next))) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            g_vulkanSession = true;
            break;
        }
        if (next->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR) {
            // debug.refract.gles_vulkan=0 keeps GLES frames on the GL pixel readback.
            char mode[PROP_VALUE_MAX]{};
            __system_property_get("debug.refract.gles_vulkan", mode);
            if (std::strcmp(mode, "0") != 0)
                g_glesShare.configure(*reinterpret_cast<const XrGraphicsBindingOpenGLESAndroidKHR*>(next), g_vulkan);
            break;
        }
    }
#endif
    *session = fake_session();
    g_pendingSessionEvents.clear();
    g_pendingInteractionProfileEvent = false;
    queue_session_state(XR_SESSION_STATE_IDLE);
    queue_session_state(XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrDestroySession_impl(XrSession session)
{
    log_call("xrDestroySession");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    g_sessionState = XR_SESSION_STATE_UNKNOWN;
    g_pendingSessionEvents.clear();
    g_pendingInteractionProfileEvent = false;
    for (auto& sc : g_swapchains) {
        destroy_swapchain_images(sc);
        sc = {};
    }
    g_lastReleasedSwapchain = nullptr;
    for (auto& tracker : g_handTrackers) tracker.alive = false;
#if defined(__ANDROID__)
    g_glesShare.shutdown();
    g_vulkan.shutdown();
    g_vulkanSession = false;
#endif
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrBeginSession_impl(XrSession session, const XrSessionBeginInfo* beginInfo)
{
    log_call("xrBeginSession");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (beginInfo == nullptr || beginInfo->type != XR_TYPE_SESSION_BEGIN_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (beginInfo->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    if (g_sessionState != XR_SESSION_STATE_READY) {
        return XR_ERROR_SESSION_NOT_READY;
    }

    queue_session_state(XR_SESSION_STATE_SYNCHRONIZED);
    queue_session_state(XR_SESSION_STATE_VISIBLE);
    queue_session_state(XR_SESSION_STATE_FOCUSED);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrEndSession_impl(XrSession session)
{
    log_call("xrEndSession");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (g_sessionState != XR_SESSION_STATE_FOCUSED &&
        g_sessionState != XR_SESSION_STATE_VISIBLE &&
        g_sessionState != XR_SESSION_STATE_SYNCHRONIZED) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }

    queue_session_state(XR_SESSION_STATE_STOPPING);
    queue_session_state(XR_SESSION_STATE_IDLE);
    queue_session_state(XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrPollEvent_impl(XrInstance instance, XrEventDataBuffer* eventData)
{
    log_call("xrPollEvent");
    refract::runtime::apply_unity_quality();
    if (!is_valid_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (eventData == nullptr || eventData->type != XR_TYPE_EVENT_DATA_BUFFER) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (g_pendingSessionEvents.empty()) {
        if (g_pendingInteractionProfileEvent) {
            g_pendingInteractionProfileEvent = false;
            auto* changed = reinterpret_cast<XrEventDataInteractionProfileChanged*>(eventData);
            changed->type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
            changed->next = nullptr;
            changed->session = fake_session();
            return XR_SUCCESS;
        }
        return XR_EVENT_UNAVAILABLE;
    }

    const XrSessionState state = g_pendingSessionEvents.front();
    g_pendingSessionEvents.pop_front();

    auto* stateChanged = reinterpret_cast<XrEventDataSessionStateChanged*>(eventData);
    stateChanged->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    stateChanged->next = nullptr;
    stateChanged->session = fake_session();
    stateChanged->state = state;
    stateChanged->time = monotonic_time_ns();
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrEnumerateViewConfigurations_impl(
    XrInstance instance,
    XrSystemId systemId,
    uint32_t viewConfigurationTypeCapacityInput,
    uint32_t* viewConfigurationTypeCountOutput,
    XrViewConfigurationType* viewConfigurationTypes)
{
    log_call("xrEnumerateViewConfigurations");
    if (!is_valid_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != kSystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (viewConfigurationTypeCountOutput == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *viewConfigurationTypeCountOutput = 1;
    if (viewConfigurationTypeCapacityInput > 0 && viewConfigurationTypes != nullptr) {
        viewConfigurationTypes[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrEnumerateEnvironmentBlendModes_impl(
    XrInstance instance,
    XrSystemId systemId,
    XrViewConfigurationType viewConfigurationType,
    uint32_t environmentBlendModeCapacityInput,
    uint32_t* environmentBlendModeCountOutput,
    XrEnvironmentBlendMode* environmentBlendModes)
{
    log_call("xrEnumerateEnvironmentBlendModes");
    if (!is_valid_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != kSystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    if (environmentBlendModeCountOutput == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *environmentBlendModeCountOutput = 1;
    if (environmentBlendModeCapacityInput > 0 && environmentBlendModes != nullptr) {
        environmentBlendModes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetViewConfigurationProperties_impl(
    XrInstance instance,
    XrSystemId systemId,
    XrViewConfigurationType viewConfigurationType,
    XrViewConfigurationProperties* configurationProperties)
{
    log_call("xrGetViewConfigurationProperties");
    if (!is_valid_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != kSystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    if (configurationProperties == nullptr || configurationProperties->type != XR_TYPE_VIEW_CONFIGURATION_PROPERTIES) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    configurationProperties->viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    configurationProperties->fovMutable = 0;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrEnumerateSwapchainFormats_impl(
    XrSession session,
    uint32_t formatCapacityInput,
    uint32_t* formatCountOutput,
    int64_t* formats)
{
    log_call("xrEnumerateSwapchainFormats");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (formatCountOutput == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    const int64_t kFormats[] = {
#if defined(__ANDROID__)
        g_vulkanSession ? 43 : 0x8058,
        g_vulkanSession ? 37 : 0x8C43,
#else
        0x8058, // GL_RGBA8
        0x8C43, // GL_SRGB8_ALPHA8
#endif
    };

    *formatCountOutput = static_cast<uint32_t>(sizeof(kFormats) / sizeof(kFormats[0]));
    if (formatCapacityInput > 0 && formats != nullptr) {
        const uint32_t count = formatCapacityInput < *formatCountOutput ? formatCapacityInput : *formatCountOutput;
        for (uint32_t i = 0; i < count; ++i) {
            formats[i] = kFormats[i];
        }
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrCreateSwapchain_impl(
    XrSession session,
    const XrSwapchainCreateInfo* createInfo,
    XrSwapchain* swapchain)
{
    log_call("xrCreateSwapchain");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo == nullptr || swapchain == nullptr || createInfo->type != XR_TYPE_SWAPCHAIN_CREATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (createInfo->width == 0 || createInfo->height == 0 ||
        createInfo->sampleCount == 0 || createInfo->faceCount == 0 ||
        createInfo->arraySize == 0 || createInfo->mipCount == 0) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (createInfo->createFlags & ~XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) return XR_ERROR_FEATURE_UNSUPPORTED;
    SwapchainRecord* available = nullptr;
    for (auto& record : g_swapchains) {
        if (!record.created) { available = &record; break; }
    }
    if (!available) { return XR_ERROR_RUNTIME_FAILURE; }
    auto& sc = *available;
    sc = {};
    sc.created = true;
    sc.acquired = false;
    sc.waited = false;
    sc.imageCount = (createInfo->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ? 1 : 3;
    sc.nextImage = 0;
    sc.currentImage = 0;
    sc.releasedImage = 0;
    sc.width = createInfo->width;
    sc.height = createInfo->height;
    sc.arraySize = createInfo->arraySize;
    sc.format = createInfo->format;
#if defined(__ANDROID__)
    if (g_vulkanSession) {
        const XrResult result = g_vulkan.create(sc.vulkan, *createInfo);
        if (result != XR_SUCCESS) { sc = {}; return result; }
    } else
#endif
    { create_opengles_swapchain_images(sc, *createInfo); }
    *swapchain = reinterpret_cast<XrSwapchain>(&sc);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrDestroySwapchain_impl(XrSwapchain swapchain)
{
    log_call("xrDestroySwapchain");
    auto* record = find_swapchain(swapchain);
    if (!record) { return XR_ERROR_HANDLE_INVALID; }
    auto& sc = *record;
    if (g_lastReleasedSwapchain == &sc) { g_lastReleasedSwapchain = nullptr; }
    sc.created = false;
    sc.acquired = false;
    sc.waited = false;
    destroy_swapchain_images(sc);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrEnumerateSwapchainImages_impl(
    XrSwapchain swapchain,
    uint32_t imageCapacityInput,
    uint32_t* imageCountOutput,
    XrSwapchainImageBaseHeader* images)
{
    log_call("xrEnumerateSwapchainImages");
    auto* record = find_swapchain(swapchain);
    if (!record) { return XR_ERROR_HANDLE_INVALID; }
    auto& sc = *record;
    if (imageCountOutput == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *imageCountOutput = sc.imageCount;
    if (imageCapacityInput > 0 && images != nullptr) {
        const uint32_t count = imageCapacityInput < *imageCountOutput ? imageCapacityInput : *imageCountOutput;
        if (images[0].type == XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR) {
#if defined(__ANDROID__)
            if (g_vulkanSession) return XR_ERROR_VALIDATION_FAILURE;
#endif
            auto* glImages = reinterpret_cast<XrSwapchainImageOpenGLESKHR*>(images);
            for (uint32_t i = 0; i < count; ++i) {
                if (glImages[i].type != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR) {
                    return XR_ERROR_VALIDATION_FAILURE;
                }
                glImages[i].image = sc.textures[i];
            }
        } else if (images[0].type == XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) {
            auto* vkImages = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
            for (uint32_t i = 0; i < count; ++i) {
                if (vkImages[i].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) {
                    return XR_ERROR_VALIDATION_FAILURE;
                }
#if defined(__ANDROID__)
                if (!g_vulkanSession) return XR_ERROR_VALIDATION_FAILURE;
                vkImages[i].image = reinterpret_cast<uint64_t>(sc.vulkan.images[i]);
#else
                return XR_ERROR_FEATURE_UNSUPPORTED;
#endif
            }
        } else {
            return XR_ERROR_VALIDATION_FAILURE;
        }
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrAcquireSwapchainImage_impl(
    XrSwapchain swapchain,
    const XrSwapchainImageAcquireInfo* acquireInfo,
    uint32_t* index)
{
    log_call("xrAcquireSwapchainImage");
    auto* record = find_swapchain(swapchain);
    if (!record) { return XR_ERROR_HANDLE_INVALID; }
    auto& sc = *record;
    if (index == nullptr || (acquireInfo != nullptr && acquireInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO)) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (sc.acquired || (sc.imageCount == 1 && sc.hasReleasedImage)) {
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    *index = sc.nextImage;
    sc.currentImage = *index;
    sc.nextImage = (sc.nextImage + 1) % sc.imageCount;
    sc.acquired = true;
    sc.waited = false;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrWaitSwapchainImage_impl(
    XrSwapchain swapchain,
    const XrSwapchainImageWaitInfo* waitInfo)
{
    log_call("xrWaitSwapchainImage");
    auto* record = find_swapchain(swapchain);
    if (!record) { return XR_ERROR_HANDLE_INVALID; }
    auto& sc = *record;
    if (waitInfo == nullptr || waitInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (!sc.acquired) {
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    sc.waited = true;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrReleaseSwapchainImage_impl(
    XrSwapchain swapchain,
    const XrSwapchainImageReleaseInfo* releaseInfo)
{
    log_call("xrReleaseSwapchainImage");
    auto* record = find_swapchain(swapchain);
    if (!record) { return XR_ERROR_HANDLE_INVALID; }
    auto& sc = *record;
    if (releaseInfo != nullptr && releaseInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (!sc.acquired || !sc.waited) {
        return XR_ERROR_CALL_ORDER_INVALID;
    }

    sc.acquired = false;
    sc.waited = false;
    sc.releasedImage = sc.currentImage;
    sc.hasReleasedImage = true;
    g_lastReleasedSwapchain = &sc;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrEnumerateViewConfigurationViews_impl(
    XrInstance instance,
    XrSystemId systemId,
    XrViewConfigurationType viewConfigurationType,
    uint32_t viewCapacityInput,
    uint32_t* viewCountOutput,
    XrViewConfigurationView* views)
{
    log_call("xrEnumerateViewConfigurationViews");
    if (!is_valid_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (systemId != kSystemId) {
        return XR_ERROR_SYSTEM_INVALID;
    }
    if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    if (viewCountOutput == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    *viewCountOutput = 2;
    query_render_extent();
    if (viewCapacityInput > 0 && views != nullptr) {
        const uint32_t count = viewCapacityInput < 2 ? viewCapacityInput : 2;
        for (uint32_t i = 0; i < count; ++i) {
            if (views[i].type != XR_TYPE_VIEW_CONFIGURATION_VIEW) {
                return XR_ERROR_VALIDATION_FAILURE;
            }
            views[i].recommendedImageRectWidth = g_renderWidth;
            views[i].maxImageRectWidth = g_renderWidth;
            views[i].recommendedImageRectHeight = g_renderHeight;
            views[i].maxImageRectHeight = g_renderHeight;
            views[i].recommendedSwapchainSampleCount = 1;
            views[i].maxSwapchainSampleCount = 1;
        }
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrEnumerateReferenceSpaces_impl(
    XrSession session,
    uint32_t spaceCapacityInput,
    uint32_t* spaceCountOutput,
    XrReferenceSpaceType* spaces)
{
    log_call("xrEnumerateReferenceSpaces");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (spaceCountOutput == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    constexpr XrReferenceSpaceType kSpaces[] = {
        XR_REFERENCE_SPACE_TYPE_VIEW,
        XR_REFERENCE_SPACE_TYPE_LOCAL,
        XR_REFERENCE_SPACE_TYPE_STAGE,
        static_cast<XrReferenceSpaceType>(1000426000), // LOCAL_FLOOR
    };

    *spaceCountOutput = static_cast<uint32_t>(sizeof(kSpaces) / sizeof(kSpaces[0]));
    if (spaceCapacityInput > 0 && spaces != nullptr) {
        const uint32_t count = spaceCapacityInput < *spaceCountOutput ? spaceCapacityInput : *spaceCountOutput;
        for (uint32_t i = 0; i < count; ++i) {
            spaces[i] = kSpaces[i];
        }
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetReferenceSpaceBoundsRect_impl(
    XrSession session, XrReferenceSpaceType type, XrExtent2Df* bounds)
{
    if (!is_valid_session(session)) return XR_ERROR_HANDLE_INVALID;
    if (!bounds) return XR_ERROR_VALIDATION_FAILURE;
    if (type != XR_REFERENCE_SPACE_TYPE_VIEW && type != XR_REFERENCE_SPACE_TYPE_LOCAL &&
        type != XR_REFERENCE_SPACE_TYPE_STAGE && type != static_cast<XrReferenceSpaceType>(1000426000))
        return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    *bounds = {};
    // The pose bridge does not yet carry the host's calibrated play-area bounds.
    return XR_SPACE_BOUNDS_UNAVAILABLE;
}

XrResult XRAPI_CALL xrCreateReferenceSpace_impl(
    XrSession session,
    const XrReferenceSpaceCreateInfo* createInfo,
    XrSpace* space)
{
    log_call("xrCreateReferenceSpace");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo == nullptr || space == nullptr || createInfo->type != XR_TYPE_REFERENCE_SPACE_CREATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    // LOCAL_FLOOR (OpenXR 1.1 / XR_EXT_local_floor) shares STAGE's floor-level origin here.
    // OVRPlugin also requests Meta's eye-level/floor-level app spaces (1000043000/1000043001).
    constexpr auto kLocalFloor = static_cast<XrReferenceSpaceType>(1000426000);
    constexpr auto kOculusEyeLevel = static_cast<XrReferenceSpaceType>(1000043000);
    constexpr auto kOculusFloorLevel = static_cast<XrReferenceSpaceType>(1000043001);
    const XrReferenceSpaceType type = createInfo->referenceSpaceType;
    if (type != XR_REFERENCE_SPACE_TYPE_VIEW && type != XR_REFERENCE_SPACE_TYPE_LOCAL &&
        type != XR_REFERENCE_SPACE_TYPE_STAGE && type != kLocalFloor &&
        type != kOculusEyeLevel && type != kOculusFloorLevel) {
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_WARN, "Refract.Space", "unsupported reference space type=%d", type);
#endif
        return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    }

    *space = make_space(
        type == XR_REFERENCE_SPACE_TYPE_VIEW ? SpaceKind::View :
        type == XR_REFERENCE_SPACE_TYPE_LOCAL || type == kOculusEyeLevel ? SpaceKind::Local : SpaceKind::Reference,
        createInfo->poseInReferenceSpace);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "Refract.Space", "create type=%d offset=(%.3f %.3f %.3f)",
        createInfo->referenceSpaceType, createInfo->poseInReferenceSpace.position.x,
        createInfo->poseInReferenceSpace.position.y, createInfo->poseInReferenceSpace.position.z);
#endif
    if (*space == nullptr) {
        return XR_ERROR_OUT_OF_MEMORY;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrCreateActionSet_impl(
    XrInstance instance,
    const XrActionSetCreateInfo* createInfo,
    XrActionSet* actionSet)
{
    log_call("xrCreateActionSet");
    if (!is_valid_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo == nullptr || createInfo->type != XR_TYPE_ACTION_SET_CREATE_INFO || actionSet == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *actionSet = fake_action_set();
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrDestroyActionSet_impl(XrActionSet actionSet)
{
    log_call("xrDestroyActionSet");
    return is_valid_action_set(actionSet) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

XrResult XRAPI_CALL xrCreateAction_impl(
    XrActionSet actionSet,
    const XrActionCreateInfo* createInfo,
    XrAction* action)
{
    log_call("xrCreateAction");
    if (!is_valid_action_set(actionSet)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo == nullptr || createInfo->type != XR_TYPE_ACTION_CREATE_INFO || action == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    g_actionHandles.push_back({0xAABBCCDD00000100ULL + g_actionCount});
    g_actionHandles.back().type = createInfo->actionType;
    g_actionIndex[fake_action(g_actionCount)] = g_actionCount;
    *action = fake_action(g_actionCount++);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrDestroyAction_impl(XrAction action)
{
    log_call("xrDestroyAction");
    if (!is_valid_action(action)) return XR_ERROR_HANDLE_INVALID;
    reinterpret_cast<RuntimeHandle*>(action)->magic = 0;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrStringToPath_impl(XrInstance instance, const char* pathString, XrPath* path)
{
    log_call("xrStringToPath");
    if (!is_valid_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (pathString == nullptr || path == nullptr || pathString[0] != '/') {
        return XR_ERROR_PATH_FORMAT_INVALID;
    }
    const std::string_view requested{pathString};
    if (requested.size() >= 256) return XR_ERROR_PATH_FORMAT_INVALID;
    for (uint32_t i = 0; i < g_pathCount; ++i) {
        if (requested == g_paths[i].text) {
            *path = g_paths[i].path;
            return XR_SUCCESS;
        }
    }
    g_paths.emplace_back();
    PathRecord& record = g_paths[g_pathCount++];
    record.path = g_nextPath++;
    record.text = requested;
    *path = record.path;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrPathToString_impl(
    XrInstance instance,
    XrPath path,
    uint32_t bufferCapacityInput,
    uint32_t* bufferCountOutput,
    char* buffer)
{
    log_call("xrPathToString");
    if (!is_valid_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    const char* text = path_text(path);
    if (text[0] == '\0') {
        return XR_ERROR_PATH_INVALID;
    }
    const uint32_t pathSize = static_cast<uint32_t>(std::strlen(text) + 1);
    if (!bufferCountOutput) return XR_ERROR_VALIDATION_FAILURE;
    *bufferCountOutput = pathSize;
    if (!bufferCapacityInput) return XR_SUCCESS;
    if (bufferCapacityInput < pathSize) return XR_ERROR_SIZE_INSUFFICIENT;
    if (!buffer) return XR_ERROR_VALIDATION_FAILURE;
    std::memcpy(buffer, text, pathSize);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrSuggestInteractionProfileBindings_impl(
    XrInstance instance,
    const XrInteractionProfileSuggestedBinding* suggestedBindings)
{
    log_call("xrSuggestInteractionProfileBindings");
    if (!is_valid_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (suggestedBindings == nullptr ||
        suggestedBindings->type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    // Present a Touch-compatible logical controller to Android. SteamVR maps
    // the user's physical controller to these semantic inputs on Windows.
    if (std::string_view(path_text(suggestedBindings->interactionProfile)) == "/interaction_profiles/oculus/touch_controller") {
        if (suggestedBindings->countSuggestedBindings && !suggestedBindings->suggestedBindings) return XR_ERROR_VALIDATION_FAILURE;
        for (uint32_t i = 0; i < suggestedBindings->countSuggestedBindings; ++i) {
            const auto& binding = suggestedBindings->suggestedBindings[i];
            if (!is_valid_action(binding.action)) return XR_ERROR_HANDLE_INVALID;
#if defined(__ANDROID__)
            __android_log_print(ANDROID_LOG_INFO, "Refract.Input", "binding action=%p type=%d path=%s",
                reinterpret_cast<void*>(binding.action), reinterpret_cast<ActionRecord*>(binding.action)->type, path_text(binding.binding));
#endif
            auto& paths = reinterpret_cast<ActionRecord*>(binding.action)->bindings;
            if (std::find(paths.begin(), paths.end(), binding.binding) == paths.end()) paths.push_back(binding.binding);
        }
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrCreateActionSpace_impl(
    XrSession session,
    const XrActionSpaceCreateInfo* createInfo,
    XrSpace* space)
{
    log_call("xrCreateActionSpace");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo == nullptr || createInfo->type != XR_TYPE_ACTION_SPACE_CREATE_INFO || space == nullptr ||
        !is_valid_action(createInfo->action)) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    auto kind = action_space_kind(createInfo->subactionPath);
    if (createInfo->subactionPath == XR_NULL_PATH) {
        for (auto path : reinterpret_cast<ActionRecord*>(createInfo->action)->bindings) {
            const std::string_view binding{path_text(path)};
            if (binding.starts_with("/user/hand/left/")) { kind = SpaceKind::LeftHand; break; }
            if (binding.starts_with("/user/hand/right/")) { kind = SpaceKind::RightHand; break; }
        }
    }
    for (auto path : reinterpret_cast<ActionRecord*>(createInfo->action)->bindings) {
        const std::string_view binding{path_text(path)};
        if (binding == "/user/hand/left/input/aim/pose" && kind == SpaceKind::LeftHand) kind = SpaceKind::LeftAim;
        if (binding == "/user/hand/right/input/aim/pose" && kind == SpaceKind::RightHand) kind = SpaceKind::RightAim;
    }
    *space = make_space(kind, createInfo->poseInActionSpace);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "Refract.Input", "space action=%p hand=%s kind=%d offset=(%.3f %.3f %.3f) q=(%.3f %.3f %.3f %.3f)",
        reinterpret_cast<void*>(createInfo->action), path_text(createInfo->subactionPath), static_cast<int>(kind),
        createInfo->poseInActionSpace.position.x, createInfo->poseInActionSpace.position.y, createInfo->poseInActionSpace.position.z,
        createInfo->poseInActionSpace.orientation.x, createInfo->poseInActionSpace.orientation.y,
        createInfo->poseInActionSpace.orientation.z, createInfo->poseInActionSpace.orientation.w);
#endif
    if (*space == nullptr) {
        return XR_ERROR_OUT_OF_MEMORY;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrLocateSpace_impl(
    XrSpace space,
    XrSpace baseSpace,
    XrTime time,
    XrSpaceLocation* location)
{
    log_call("xrLocateSpace");
    SpaceRecord* spaceRecord = find_space(space);
    SpaceRecord* baseRecord = find_space(baseSpace);
    if (spaceRecord == nullptr || baseRecord == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (location == nullptr || location->type != XR_TYPE_SPACE_LOCATION) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    (void)time;
    location->locationFlags =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
        XR_SPACE_LOCATION_POSITION_VALID_BIT |
        XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
        XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    const refract::protocol::PoseFrame& poseFrame = pose_client().latest_pose_frame();
    auto tracking_flags = [&](const SpaceRecord& record) -> XrSpaceLocationFlags {
        if (poseFrame.version >= 5 && record.kind == SpaceKind::Local) return poseFrame.local_origin_flags;
        if (poseFrame.version >= 5 && record.kind == SpaceKind::View) return poseFrame.hmd_flags;
        const bool aim = record.kind == SpaceKind::LeftAim || record.kind == SpaceKind::RightAim;
        const bool left = record.kind == SpaceKind::LeftHand || record.kind == SpaceKind::LeftAim;
        const bool right = record.kind == SpaceKind::RightHand || record.kind == SpaceKind::RightAim;
        if (!left && !right) return 15;
        const size_t hand = right ? 1 : 0;
        if (poseFrame.version < 3) return poseFrame.controllers[hand].active ? 15 : 0;
        if (!(aim ? poseFrame.aim_active[hand] : poseFrame.controllers[hand].active)) return 0;
        return aim ? poseFrame.aim_flags[hand] : poseFrame.grip_flags[hand];
    };
    location->locationFlags = tracking_flags(*spaceRecord) & tracking_flags(*baseRecord);
    const XrPosef spaceWorld = world_pose_for_space(*spaceRecord, poseFrame);
    const XrPosef baseWorld = world_pose_for_space(*baseRecord, poseFrame);
    location->pose = multiply_pose(inverse_pose(baseWorld), spaceWorld);
    // OVRPlugin chains XrSpaceVelocity and extrapolates poses with it. Left unwritten, it reads
    // whatever was in that memory (Batman: Arkham Shadow's head pose turned NaN). Games also read it
    // for gameplay (Batman's punches, throws). Hosts from v6 send the runtime's real velocities; older
    // ones (pose_input_server.py) carry none, so those report a device at rest, valid wherever the pose is.
    // debug.refract.space_velocity=0 reports rest for v6 hosts too.
#if defined(__ANDROID__)
    static const bool realVelocities = int_property("debug.refract.space_velocity", 1, 0, 1) != 0;
#else
    constexpr bool realVelocities = true;
#endif
    struct ChainHeader { XrStructureType type; void* next; };
    for (auto* next = static_cast<ChainHeader*>(location->next); next; next = static_cast<ChainHeader*>(next->next)) {
        if (next->type != XR_TYPE_SPACE_VELOCITY) continue;
        auto* velocity = reinterpret_cast<XrSpaceVelocity*>(next);
        refract::protocol::SpaceVelocity v{};
        if (realVelocities && poseFrame.version >= 6) {
            if ((location->locationFlags & 3) == 3) {
                v = space == baseSpace ? refract::protocol::SpaceVelocity{3}
                    : refract::protocol::relative_velocity(world_velocity_for_space(*spaceRecord, poseFrame),
                          world_velocity_for_space(*baseRecord, poseFrame),
                          xr_pose_to_protocol(spaceWorld), xr_pose_to_protocol(baseWorld));
            }
        } else {
            v.flags = ((location->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ? XR_SPACE_VELOCITY_LINEAR_VALID_BIT : 0) |
                      ((location->locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ? XR_SPACE_VELOCITY_ANGULAR_VALID_BIT : 0);
        }
        velocity->velocityFlags = v.flags;
        velocity->linearVelocity = {v.linear.x, v.linear.y, v.linear.z};
        velocity->angularVelocity = {v.angular.x, v.angular.y, v.angular.z};
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrAttachSessionActionSets_impl(
    XrSession session,
    const XrSessionActionSetsAttachInfo* attachInfo)
{
    log_call("xrAttachSessionActionSets");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (attachInfo == nullptr || attachInfo->type != XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    g_pendingInteractionProfileEvent = true;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrSyncActions_impl(XrSession session, const XrActionsSyncInfo* syncInfo)
{
    log_call("xrSyncActions");
    refract::runtime::apply_unity_quality();
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (syncInfo == nullptr || syncInfo->type != XR_TYPE_ACTIONS_SYNC_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const auto& frame = pose_client().latest_pose_frame();
    const XrTime now = monotonic_time_ns();
    for (auto& action : g_actionHandles) {
        if (!action.magic) continue;
        if (action.resolvedFrom != action.bindings.size()) {
            action.resolved.clear();
            for (XrPath path : action.bindings) {
                const std::string_view text{path_text(path)};
                for (uint8_t hand = 0; hand < 2; ++hand) {
                    const std::string_view prefix = hand == 0 ? "/user/hand/left/input/" : "/user/hand/right/input/";
                    if (!text.starts_with(prefix)) continue;
                    const auto component = text.substr(prefix.size());
                    action.resolved.push_back({hand, component == "aim/pose", refract::protocol::input_component(component)});
                }
            }
            action.resolvedFrom = action.bindings.size();
        }
        std::array<refract::protocol::InputValue, 3> values{};
        for (const auto& binding : action.resolved) {
            const size_t hand = binding.hand;
            auto value = refract::protocol::controller_value(frame.controllers[hand], binding.component);
            if (binding.aim && frame.version >= 3) value.active = frame.aim_active[hand] != 0;
            if (action.type == XR_ACTION_TYPE_BOOLEAN_INPUT) value.x = value.x > 0.5f ? 1.0f : 0.0f;
            for (size_t slot : {hand, size_t{2}}) {
                auto& combined = values[slot];
                const bool active = combined.active || value.active;
                if (value.x * value.x + value.y * value.y > combined.x * combined.x + combined.y * combined.y) combined = value;
                combined.active = active;
            }
        }
        for (size_t i = 0; i < values.size(); ++i) {
            auto& sample = action.samples[i];
            const auto& value = values[i];
            sample.changed = value.active && (sample.value.active != value.active || sample.value.x != value.x || sample.value.y != value.y);
            if (sample.changed) sample.changedAt = now;
            sample.value = value;
        }
    }
    return XR_SUCCESS;
}

const ActionSample& action_sample(const XrActionStateGetInfo& info) {
    const auto kind = action_space_kind(info.subactionPath);
    return reinterpret_cast<const ActionRecord*>(info.action)->samples[
        kind == SpaceKind::LeftHand ? 0 : kind == SpaceKind::RightHand ? 1 : 2];
}

XrResult XRAPI_CALL xrGetActionStateBoolean_impl(
    XrSession session,
    const XrActionStateGetInfo* getInfo,
    XrActionStateBoolean* state)
{
    log_call("xrGetActionStateBoolean");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (getInfo == nullptr || getInfo->type != XR_TYPE_ACTION_STATE_GET_INFO || state == nullptr ||
        state->type != XR_TYPE_ACTION_STATE_BOOLEAN || !is_valid_action(getInfo->action)) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (reinterpret_cast<ActionRecord*>(getInfo->action)->type != XR_ACTION_TYPE_BOOLEAN_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;
    const auto& sample = action_sample(*getInfo);
    state->currentState = sample.value.x > 0.5f;
    state->changedSinceLastSync = sample.changed;
    state->lastChangeTime = sample.changedAt;
    state->isActive = sample.value.active;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetActionStateFloat_impl(
    XrSession session,
    const XrActionStateGetInfo* getInfo,
    XrActionStateFloat* state)
{
    log_call("xrGetActionStateFloat");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (getInfo == nullptr || getInfo->type != XR_TYPE_ACTION_STATE_GET_INFO || state == nullptr ||
        state->type != XR_TYPE_ACTION_STATE_FLOAT || !is_valid_action(getInfo->action)) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (reinterpret_cast<ActionRecord*>(getInfo->action)->type != XR_ACTION_TYPE_FLOAT_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;
    const auto& sample = action_sample(*getInfo);
    state->currentState = sample.value.x;
    state->changedSinceLastSync = sample.changed;
    state->lastChangeTime = sample.changedAt;
    state->isActive = sample.value.active;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetActionStateVector2f_impl(
    XrSession session,
    const XrActionStateGetInfo* getInfo,
    XrActionStateVector2f* state)
{
    log_call("xrGetActionStateVector2f");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (getInfo == nullptr || getInfo->type != XR_TYPE_ACTION_STATE_GET_INFO || state == nullptr ||
        state->type != XR_TYPE_ACTION_STATE_VECTOR2F || !is_valid_action(getInfo->action)) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (reinterpret_cast<ActionRecord*>(getInfo->action)->type != XR_ACTION_TYPE_VECTOR2F_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;
    const auto& sample = action_sample(*getInfo);
    state->currentState = {sample.value.x, sample.value.y};
    state->changedSinceLastSync = sample.changed;
    state->lastChangeTime = sample.changedAt;
    state->isActive = sample.value.active;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetActionStatePose_impl(
    XrSession session,
    const XrActionStateGetInfo* getInfo,
    XrActionStatePose* state)
{
    log_call("xrGetActionStatePose");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (getInfo == nullptr || getInfo->type != XR_TYPE_ACTION_STATE_GET_INFO || state == nullptr ||
        state->type != XR_TYPE_ACTION_STATE_POSE || !is_valid_action(getInfo->action)) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (reinterpret_cast<ActionRecord*>(getInfo->action)->type != XR_ACTION_TYPE_POSE_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;
    state->isActive = action_sample(*getInfo).value.active;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetCurrentInteractionProfile_impl(
    XrSession session,
    XrPath topLevelUserPath,
    XrInteractionProfileState* interactionProfile)
{
    log_call("xrGetCurrentInteractionProfile");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (topLevelUserPath == XR_NULL_PATH || interactionProfile == nullptr ||
        interactionProfile->type != XR_TYPE_INTERACTION_PROFILE_STATE) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const auto kind = action_space_kind(topLevelUserPath);
    if (kind != SpaceKind::LeftHand && kind != SpaceKind::RightHand) return XR_ERROR_PATH_UNSUPPORTED;
    return xrStringToPath_impl(fake_instance(), "/interaction_profiles/oculus/touch_controller", &interactionProfile->interactionProfile);
}

XrResult XRAPI_CALL xrEnumerateBoundSourcesForAction_impl(
    XrSession session,
    const XrBoundSourcesForActionEnumerateInfo* enumerateInfo,
    uint32_t sourceCapacityInput,
    uint32_t* sourceCountOutput,
    XrPath* sources)
{
    log_call("xrEnumerateBoundSourcesForAction");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (enumerateInfo == nullptr || sourceCountOutput == nullptr ||
        enumerateInfo->type != XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO || !is_valid_action(enumerateInfo->action)) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const auto& bindings = reinterpret_cast<ActionRecord*>(enumerateInfo->action)->bindings;
    *sourceCountOutput = static_cast<uint32_t>(bindings.size());
    if (!sourceCapacityInput) return XR_SUCCESS;
    if (sourceCapacityInput < bindings.size()) return XR_ERROR_SIZE_INSUFFICIENT;
    if (!sources) return XR_ERROR_VALIDATION_FAILURE;
    std::copy(bindings.begin(), bindings.end(), sources);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetInputSourceLocalizedName_impl(
    XrSession session,
    const XrInputSourceLocalizedNameGetInfo* getInfo,
    uint32_t bufferCapacityInput,
    uint32_t* bufferCountOutput,
    char* buffer)
{
    log_call("xrGetInputSourceLocalizedName");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (getInfo == nullptr || getInfo->type != XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO || bufferCountOutput == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    constexpr const char kName[] = "Refract Input";
    constexpr uint32_t kNameSize = sizeof(kName);
    *bufferCountOutput = kNameSize;
    if (buffer != nullptr) {
        if (bufferCapacityInput < kNameSize) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        std::memcpy(buffer, kName, kNameSize);
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrApplyHapticFeedback_impl(
    XrSession session,
    const XrHapticActionInfo* hapticActionInfo,
    const XrHapticBaseHeader* hapticFeedback)
{
    log_call("xrApplyHapticFeedback");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (hapticActionInfo == nullptr || hapticActionInfo->type != XR_TYPE_HAPTIC_ACTION_INFO ||
        hapticFeedback == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    return XR_SUCCESS;
}

HandTrackerRecord* find_hand_tracker(XrHandTrackerEXT tracker) {
    for (auto& record : g_handTrackers) if (record.alive && reinterpret_cast<XrHandTrackerEXT>(&record) == tracker) return &record;
    return nullptr;
}

XrResult XRAPI_CALL xrCreateHandTrackerEXT_impl(XrSession session, const XrHandTrackerCreateInfoEXT* info, XrHandTrackerEXT* tracker) {
    if (!is_valid_session(session)) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT || !tracker ||
        (info->hand != XR_HAND_LEFT_EXT && info->hand != XR_HAND_RIGHT_EXT) || info->handJointSet != XR_HAND_JOINT_SET_DEFAULT_EXT)
        return XR_ERROR_VALIDATION_FAILURE;
    // Without XR_EXT_hand_tracking_data_source, Quest reports only optical hands. Controller-derived
    // joints would make OVRPlugin switch to hand input and drop the held controllers.
    uint32_t sources = 1;
    struct InputHeader { XrStructureType type; const void* next; };
    for (auto* next = static_cast<const InputHeader*>(info->next); next; next = static_cast<const InputHeader*>(next->next)) {
        if (next->type != XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT) continue;
        const auto* request = reinterpret_cast<const XrHandTrackingDataSourceInfoEXT*>(next);
        if (!request->requestedDataSourceCount || !request->requestedDataSources) return XR_ERROR_VALIDATION_FAILURE;
        sources = 0;
        for (uint32_t i = 0; i < request->requestedDataSourceCount; ++i) {
            const auto value = request->requestedDataSources[i];
            if (value != XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT && value != XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT) return XR_ERROR_VALIDATION_FAILURE;
            sources |= 1u << (static_cast<uint32_t>(value) - 1);
        }
    }
    g_handTrackers.push_back({true, info->hand == XR_HAND_LEFT_EXT ? 0u : 1u, sources});
    *tracker = reinterpret_cast<XrHandTrackerEXT>(&g_handTrackers.back());
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "Refract.Hands", "created hand=%u sourceMask=%u", g_handTrackers.back().hand, sources);
#endif
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrDestroyHandTrackerEXT_impl(XrHandTrackerEXT tracker) {
    auto* record = find_hand_tracker(tracker);
    if (!record) return XR_ERROR_HANDLE_INVALID;
    record->alive = false;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrLocateHandJointsEXT_impl(XrHandTrackerEXT tracker, const XrHandJointsLocateInfoEXT* info, XrHandJointLocationsEXT* locations) {
    auto* record = find_hand_tracker(tracker);
#if defined(__ANDROID__)
    static unsigned requests = 0;
    if (requests++ < 12) __android_log_print(ANDROID_LOG_INFO, "Refract.Hands", "locate request tracker=%p valid=%d count=%u base=%p", reinterpret_cast<void*>(tracker), record != nullptr, locations ? locations->jointCount : 0, info ? reinterpret_cast<void*>(info->baseSpace) : nullptr);
#endif
    if (!record) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT || !locations ||
        locations->type != XR_TYPE_HAND_JOINT_LOCATIONS_EXT || locations->jointCount != XR_HAND_JOINT_COUNT_EXT || !locations->jointLocations)
        return XR_ERROR_VALIDATION_FAILURE;
    auto* base = find_space(info->baseSpace);
    if (!base) return XR_ERROR_HANDLE_INVALID;
    const auto& frame = pose_client().latest_pose_frame();
    const auto& hand = frame.hands[record->hand];
    locations->isActive = hand.active && (hand.source == 0 || (hand.source <= 2 && (record->sources & (1u << (hand.source - 1)))));
    if (frame.version >= 5 && base->kind == SpaceKind::Local && (frame.local_origin_flags & 3) != 3)
        locations->isActive = 0;
    const auto inverseBase = inverse_pose(world_pose_for_space(*base, frame));
    for (uint32_t joint = 0; joint < XR_HAND_JOINT_COUNT_EXT; ++joint) {
        auto& out = locations->jointLocations[joint]; const auto& in = hand.joints[joint];
        out.locationFlags = locations->isActive ? in.flags : 0;
        out.pose = locations->isActive ? multiply_pose(inverseBase, protocol_pose_to_xr(in.pose)) : identity_pose();
        out.radius = locations->isActive ? in.radius : 0;
    }
    struct OutputHeader { XrStructureType type; void* next; };
    for (auto* next = static_cast<OutputHeader*>(locations->next); next; next = static_cast<OutputHeader*>(next->next)) {
        if (next->type == XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT) {
            auto* out = reinterpret_cast<XrHandTrackingDataSourceStateEXT*>(next);
            out->isActive = locations->isActive && hand.source != 0;
            out->dataSource = static_cast<XrHandTrackingDataSourceEXT>(hand.source);
        } else if (next->type == XR_TYPE_HAND_JOINT_VELOCITIES_EXT) {
            auto* out = reinterpret_cast<XrHandJointVelocitiesEXT*>(next);
            if (out->jointCount != XR_HAND_JOINT_COUNT_EXT || !out->jointVelocities) return XR_ERROR_VALIDATION_FAILURE;
            for (uint32_t joint = 0; joint < out->jointCount; ++joint) out->jointVelocities[joint] = {};
        }
    }
#if defined(__ANDROID__)
    static uint32_t reports = 0;
    if (++reports % 600 == 0) __android_log_print(ANDROID_LOG_INFO, "Refract.Hands", "locate hand=%u active=%u source=%u", record->hand, locations->isActive, hand.source);
#endif
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrStopHapticFeedback_impl(
    XrSession session,
    const XrHapticActionInfo* hapticActionInfo)
{
    log_call("xrStopHapticFeedback");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (hapticActionInfo == nullptr || hapticActionInfo->type != XR_TYPE_HAPTIC_ACTION_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrLocateViews_impl(
    XrSession session,
    const XrViewLocateInfo* viewLocateInfo,
    XrViewState* viewState,
    uint32_t viewCapacityInput,
    uint32_t* viewCountOutput,
    XrView* views)
{
    log_call("xrLocateViews");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (viewLocateInfo == nullptr || viewState == nullptr || viewCountOutput == nullptr ||
        viewLocateInfo->type != XR_TYPE_VIEW_LOCATE_INFO || viewState->type != XR_TYPE_VIEW_STATE) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (viewLocateInfo->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    if (find_space(viewLocateInfo->space) == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    *viewCountOutput = 2;
    viewState->viewStateFlags =
        XR_VIEW_STATE_ORIENTATION_VALID_BIT |
        XR_VIEW_STATE_POSITION_VALID_BIT |
        XR_VIEW_STATE_ORIENTATION_TRACKED_BIT |
        XR_VIEW_STATE_POSITION_TRACKED_BIT;

    SpaceRecord* baseRecord = find_space(viewLocateInfo->space);
    const refract::protocol::PoseFrame& poseFrame = pose_client().latest_pose_frame();
    g_lastViewPoseFrame = poseFrame;
    if (frame_sync_enabled()) record_view_pose(viewLocateInfo->displayTime, poseFrame.sequence);
    if (poseFrame.version >= 5) {
        viewState->viewStateFlags = poseFrame.hmd_flags & 15;
        if (baseRecord->kind == SpaceKind::Local) viewState->viewStateFlags &= poseFrame.local_origin_flags;
    }
    if (viewCapacityInput == 0 || views == nullptr) return XR_SUCCESS;
    const XrPosef hmdWorld = protocol_pose_to_xr(poseFrame.hmd);
    const XrPosef baseWorld = world_pose_for_space(*baseRecord, poseFrame);
    const uint32_t count = viewCapacityInput < 2 ? viewCapacityInput : 2;
    for (uint32_t i = 0; i < count; ++i) {
        if (views[i].type != XR_TYPE_VIEW) {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        XrPosef eyeOffset = identity_pose();
        eyeOffset.position.x = i == 0 ? -kEyeHalfIpdMeters : kEyeHalfIpdMeters;
        const XrPosef eyeWorld = multiply_pose(hmdWorld, eyeOffset);
        views[i].pose = multiply_pose(inverse_pose(baseWorld), eyeWorld);
        float halfWidth = 0.0f, halfHeight = 0.0f;
        view_half_angles(halfWidth, halfHeight);
        views[i].fov.angleLeft = -halfWidth;
        views[i].fov.angleRight = halfWidth;
        views[i].fov.angleUp = halfHeight;
        views[i].fov.angleDown = -halfHeight;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrDestroySpace_impl(XrSpace space)
{
    log_call("xrDestroySpace");
    auto* record = find_space(space);
    if (!record) return XR_ERROR_HANDLE_INVALID;
    record->handle.magic = 0;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrWaitFrame_impl(
    XrSession session,
    const XrFrameWaitInfo* frameWaitInfo,
    XrFrameState* frameState)
{
    log_call("xrWaitFrame");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (g_sessionState != XR_SESSION_STATE_FOCUSED) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    if (frameState == nullptr ||
        (frameWaitInfo != nullptr && frameWaitInfo->type != XR_TYPE_FRAME_WAIT_INFO) || frameState->type != XR_TYPE_FRAME_STATE) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    // debug.refract.frame_sync=1 (the SteamVR host bridge): the host sends one pose per frame of its OpenXR
    // runtime, right after that runtime's xrWaitFrame. Starting the game's frame when it arrives phase-locks
    // the two frame loops, so the game renders with the newest pose and can finish in time for the host's
    // next frame. A free-running timer beat against the host's clock: poses waited up to a frame, finished
    // images up to another, and ~4 frames a second were missed. Without a live pose stream, pace by timer.
    // Diagnostics: the app's own work between waits, and how long each wait blocks.
    static refract::protocol::PerfStats appFrameStats("app-frame"), waitStats("wait-frame");
    static XrTime lastWaitReturn = 0;
    const XrTime waitStart = monotonic_time_ns();
    if (lastWaitReturn) appFrameStats.record((waitStart - lastWaitReturn) / 1e6);
    struct RecordWait {
        XrTime start;
        ~RecordWait() { lastWaitReturn = monotonic_time_ns(); waitStats.record((lastWaitReturn - start) / 1e6); }
    } recordWait{waitStart};
    if (frame_sync_enabled()) {
        static uint64_t lastSequence = UINT64_MAX;
        static XrTime lastNewPose = 0;
        static refract::protocol::PerfStats stepStats("pose-step");  // Host frames per app frame (not ms).
        auto frame = pose_client().latest_pose_frame();
        const XrTime framePeriod = refract::protocol::display_period_or_default(frame);
        XrTime now = monotonic_time_ns();
        if (frame.sequence == lastSequence && now - lastNewPose < 100'000'000) {
            const XrTime deadline = now + framePeriod * 3 / 2;
            while (frame.sequence == lastSequence && now < deadline) {
                std::this_thread::sleep_for(std::chrono::microseconds(250));
                frame = pose_client().latest_pose_frame();
                now = monotonic_time_ns();
            }
        }
        if (frame.sequence != lastSequence) {
            if (lastSequence != UINT64_MAX) stepStats.record(static_cast<double>(frame.sequence - lastSequence));
            lastSequence = frame.sequence;
            lastNewPose = now;
            g_nextFrameStart = now + framePeriod;
            frameState->predictedDisplayTime = g_nextFrameStart;
            frameState->predictedDisplayPeriod = framePeriod;
            frameState->shouldRender = 1;
            return XR_SUCCESS;
        }
    }

    // Pace at the active host display period. Never build a queue of
    // catch-up frames after a slow render or a disconnected transport.
    const XrTime period = refract::protocol::display_period_or_default(pose_client().latest_pose_frame());
    XrTime now = monotonic_time_ns();
    if (g_nextFrameStart == 0 || now - g_nextFrameStart >= period) {
        g_nextFrameStart = now;
    }
    if (g_nextFrameStart > now) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(g_nextFrameStart - now));
        now = monotonic_time_ns();
    }
    if (now - g_nextFrameStart >= period) { g_nextFrameStart = now; }
    g_nextFrameStart += period;
    frameState->predictedDisplayTime = g_nextFrameStart;
    frameState->predictedDisplayPeriod = period;
    frameState->shouldRender = 1;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrBeginFrame_impl(XrSession session, const XrFrameBeginInfo* frameBeginInfo)
{
    log_call("xrBeginFrame");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (g_sessionState != XR_SESSION_STATE_FOCUSED) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    if (frameBeginInfo != nullptr && frameBeginInfo->type != XR_TYPE_FRAME_BEGIN_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrEndFrame_impl(XrSession session, const XrFrameEndInfo* frameEndInfo)
{
    static refract::protocol::PerfStats stats("end-frame");
    refract::protocol::PerfScope scope(stats);
    log_call("xrEndFrame");
    if (!is_valid_session(session)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (frameEndInfo == nullptr || frameEndInfo->type != XR_TYPE_FRAME_END_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (g_sessionState != XR_SESSION_STATE_FOCUSED) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
#if defined(__ANDROID__)
    g_glesShare.begin_frame();
#endif
    return submit_projection_frame(*frameEndInfo);
}

template <typename Function>
PFN_xrVoidFunction cast_function(Function function)
{
    return reinterpret_cast<PFN_xrVoidFunction>(function);
}

XrResult XRAPI_CALL xrEnumerateDisplayRefreshRatesFB_impl(XrSession session, uint32_t capacity, uint32_t* count, float* rates) {
    if (!is_valid_session(session)) return XR_ERROR_HANDLE_INVALID;
    if (!count || (capacity && !rates)) return XR_ERROR_VALIDATION_FAILURE;
    *count = 1;
    if (capacity) rates[0] = 1'000'000'000.0f / refract::protocol::display_period_or_default(pose_client().latest_pose_frame());
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetDisplayRefreshRateFB_impl(XrSession session, float* rate) {
    if (!is_valid_session(session)) return XR_ERROR_HANDLE_INVALID;
    if (!rate) return XR_ERROR_VALIDATION_FAILURE;
    *rate = 1'000'000'000.0f / refract::protocol::display_period_or_default(pose_client().latest_pose_frame());
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrRequestDisplayRefreshRateFB_impl(XrSession session, float rate) {
    if (!is_valid_session(session)) return XR_ERROR_HANDLE_INVALID;
    const float activeRate = 1'000'000'000.0f / refract::protocol::display_period_or_default(pose_client().latest_pose_frame());
    return rate == 0.0f || std::abs(rate - activeRate) < 0.01f ? XR_SUCCESS : static_cast<XrResult>(-1000101000);
}

// XR_META_recommended_layer_resolution: the host isn't GPU-bound, so recommend the layer's full
// swapchain size. OVRPlugin queries this every frame for dynamic resolution; without the extension
// each query fails and logs an error (AC Nexus also logged two 0x0 RenderTexture errors per frame).
XrResult XRAPI_CALL xrGetRecommendedLayerResolutionMETA_impl(
    XrSession session, const XrRecommendedLayerResolutionGetInfoMETA* info, XrRecommendedLayerResolutionMETA* resolution) {
    log_call("xrGetRecommendedLayerResolutionMETA");
    if (!is_valid_session(session)) return XR_ERROR_HANDLE_INVALID;
    if (!info || !resolution || info->type != XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_GET_INFO_META ||
        resolution->type != XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_META || !info->layer) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const XrSwapchainSubImage* sub = nullptr;
    if (info->layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(info->layer);
        if (projection->viewCount > 0 && projection->views) sub = &projection->views[0].subImage;
    } else if (info->layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
        sub = &reinterpret_cast<const XrCompositionLayerQuad*>(info->layer)->subImage;
    }
    const SwapchainRecord* sc = sub ? find_swapchain(sub->swapchain) : nullptr;
    resolution->isValid = sc ? 1u : 0u;
    resolution->recommendedImageDimensions = sc
        ? XrExtent2Di{static_cast<int32_t>(sc->width), static_cast<int32_t>(sc->height)} : XrExtent2Di{0, 0};
    return XR_SUCCESS;
}

#if defined(__ANDROID__)
// XR_FB_foveation profiles carry no runtime state: every swapchain is rendered
// and forwarded at full density, so a profile is only a validated token.
uint8_t g_foveationProfileToken = 0;

XrResult XRAPI_CALL xrCreateFoveationProfileFB_impl(
    XrSession session, const XrFoveationProfileCreateInfoFB* createInfo, XrFoveationProfileFB* profile) {
    log_call("xrCreateFoveationProfileFB");
    if (!is_valid_session(session)) return XR_ERROR_HANDLE_INVALID;
    if (!createInfo || !profile || createInfo->type != XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *profile = reinterpret_cast<XrFoveationProfileFB>(&g_foveationProfileToken);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrDestroyFoveationProfileFB_impl(XrFoveationProfileFB profile) {
    log_call("xrDestroyFoveationProfileFB");
    return profile == reinterpret_cast<XrFoveationProfileFB>(&g_foveationProfileToken)
        ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

XrResult XRAPI_CALL xrUpdateSwapchainFB_impl(XrSwapchain swapchain, const XrSwapchainStateBaseHeaderFB* state) {
    log_call("xrUpdateSwapchainFB");
    if (!find_swapchain(swapchain)) return XR_ERROR_HANDLE_INVALID;
    // OVRPlugin also sends Vulkan sampler state; the host samples its own copy.
    return state ? XR_SUCCESS : XR_ERROR_VALIDATION_FAILURE;
}

XrResult XRAPI_CALL xrGetSwapchainStateFB_impl(XrSwapchain swapchain, XrSwapchainStateBaseHeaderFB* state) {
    log_call("xrGetSwapchainStateFB");
    if (!find_swapchain(swapchain)) return XR_ERROR_HANDLE_INVALID;
    if (!state) return XR_ERROR_VALIDATION_FAILURE;
    if (state->type == XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB) {
        auto* foveation = reinterpret_cast<XrSwapchainStateFoveationFB*>(state);
        foveation->flags = 0;
        foveation->profile = nullptr;
    }
    // Other state types (sampler) are returned as the caller initialized them.
    return XR_SUCCESS;
}
#endif

XrResult XRAPI_CALL xrGetInstanceProcAddr_impl(
    XrInstance instance,
    const char* name,
    PFN_xrVoidFunction* function)
{
    if (name == nullptr || function == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    log_proc_request(name);

    *function = nullptr;
    const std::string_view requested{name};

    if (requested == "xrGetInstanceProcAddr") {
        *function = cast_function(xrGetInstanceProcAddr_impl);
        return XR_SUCCESS;
    }
    if (requested == "xrEnumerateDisplayRefreshRatesFB") {
        *function = cast_function(xrEnumerateDisplayRefreshRatesFB_impl);
        return XR_SUCCESS;
    }
    if (requested == "xrGetDisplayRefreshRateFB") {
        *function = cast_function(xrGetDisplayRefreshRateFB_impl);
        return XR_SUCCESS;
    }
    if (requested == "xrRequestDisplayRefreshRateFB") {
        *function = cast_function(xrRequestDisplayRefreshRateFB_impl);
        return XR_SUCCESS;
    }
    if (requested == "xrGetRecommendedLayerResolutionMETA") {
        *function = cast_function(xrGetRecommendedLayerResolutionMETA_impl);
        return XR_SUCCESS;
    }

    if (is_valid_instance(instance)) {
        if (requested == "xrCreateHandTrackerEXT") { *function = cast_function(xrCreateHandTrackerEXT_impl); return XR_SUCCESS; }
        if (requested == "xrDestroyHandTrackerEXT") { *function = cast_function(xrDestroyHandTrackerEXT_impl); return XR_SUCCESS; }
        if (requested == "xrLocateHandJointsEXT") { *function = cast_function(xrLocateHandJointsEXT_impl); return XR_SUCCESS; }
#if defined(__ANDROID__)
        if (requested == "xrCreateFoveationProfileFB") { *function = cast_function(xrCreateFoveationProfileFB_impl); return XR_SUCCESS; }
        if (requested == "xrDestroyFoveationProfileFB") { *function = cast_function(xrDestroyFoveationProfileFB_impl); return XR_SUCCESS; }
        if (requested == "xrUpdateSwapchainFB") { *function = cast_function(xrUpdateSwapchainFB_impl); return XR_SUCCESS; }
        if (requested == "xrGetSwapchainStateFB") { *function = cast_function(xrGetSwapchainStateFB_impl); return XR_SUCCESS; }
#endif
    }

    if (requested == "xrCreateInstance") {
        *function = cast_function(xrCreateInstance_impl);
        return XR_SUCCESS;
    }

    if (requested == "xrInitializeLoaderKHR") {
        *function = cast_function(xrInitializeLoaderKHR_impl);
        return XR_SUCCESS;
    }

    if (requested == "xrEnumerateInstanceExtensionProperties") {
        *function = cast_function(xrEnumerateInstanceExtensionProperties_impl);
        return XR_SUCCESS;
    }

    if (requested == "xrPollEvent") {
        *function = cast_function(xrPollEvent_impl);
        return XR_SUCCESS;
    }

    if (!is_valid_instance(instance)) {
        return XR_ERROR_HANDLE_INVALID;
    }

    if (requested == "xrDestroyInstance") {
        *function = cast_function(xrDestroyInstance_impl);
#if defined(__ANDROID__) || defined(REFRACT_INPUT_FIXTURE)
    } else if (requested == "xrConvertTimespecTimeToTimeKHR") {
        *function = cast_function(xrConvertTimespecTimeToTimeKHR_impl);
    } else if (requested == "xrConvertTimeToTimespecTimeKHR") {
        *function = cast_function(xrConvertTimeToTimespecTimeKHR_impl);
#endif
    } else if (requested == "xrGetInstanceProperties") {
        *function = cast_function(xrGetInstanceProperties_impl);
    } else if (requested == "xrGetSystem") {
        *function = cast_function(xrGetSystem_impl);
    } else if (requested == "xrGetSystemProperties") {
        *function = cast_function(xrGetSystemProperties_impl);
    } else if (requested == "xrGetOpenGLESGraphicsRequirementsKHR") {
        *function = cast_function(xrGetOpenGLESGraphicsRequirementsKHR_impl);
    } else if (requested == "xrGetVulkanGraphicsRequirementsKHR") {
        *function = cast_function(xrGetVulkanGraphicsRequirementsKHR_impl);
#if defined(__ANDROID__)
    } else if (requested == "xrGetVulkanGraphicsRequirements2KHR") {
        *function = cast_function(xrGetVulkanGraphicsRequirementsKHR_impl);
    } else if (requested == "xrGetVulkanInstanceExtensionsKHR" || requested == "xrGetVulkanDeviceExtensionsKHR") {
        *function = cast_function(xrGetVulkanExtensionsKHR_impl);
    } else if (requested == "xrGetVulkanGraphicsDeviceKHR") {
        *function = cast_function(xrGetVulkanGraphicsDeviceKHR_impl);
    } else if (requested == "xrGetVulkanGraphicsDevice2KHR") {
        *function = cast_function(xrGetVulkanGraphicsDevice2KHR_impl);
    } else if (requested == "xrCreateVulkanInstanceKHR") {
        *function = cast_function(xrCreateVulkanInstanceKHR_impl);
    } else if (requested == "xrCreateVulkanDeviceKHR") {
        *function = cast_function(xrCreateVulkanDeviceKHR_impl);
#endif
    } else if (requested == "xrCreateSession") {
        *function = cast_function(xrCreateSession_impl);
    } else if (requested == "xrBeginSession") {
        *function = cast_function(xrBeginSession_impl);
    } else if (requested == "xrEndSession") {
        *function = cast_function(xrEndSession_impl);
    } else if (requested == "xrDestroySession") {
        *function = cast_function(xrDestroySession_impl);
    } else if (requested == "xrEnumerateViewConfigurations") {
        *function = cast_function(xrEnumerateViewConfigurations_impl);
    } else if (requested == "xrEnumerateEnvironmentBlendModes") {
        *function = cast_function(xrEnumerateEnvironmentBlendModes_impl);
    } else if (requested == "xrGetViewConfigurationProperties") {
        *function = cast_function(xrGetViewConfigurationProperties_impl);
    } else if (requested == "xrEnumerateViewConfigurationViews") {
        *function = cast_function(xrEnumerateViewConfigurationViews_impl);
    } else if (requested == "xrEnumerateSwapchainFormats") {
        *function = cast_function(xrEnumerateSwapchainFormats_impl);
    } else if (requested == "xrCreateSwapchain") {
        *function = cast_function(xrCreateSwapchain_impl);
    } else if (requested == "xrDestroySwapchain") {
        *function = cast_function(xrDestroySwapchain_impl);
    } else if (requested == "xrEnumerateSwapchainImages") {
        *function = cast_function(xrEnumerateSwapchainImages_impl);
    } else if (requested == "xrAcquireSwapchainImage") {
        *function = cast_function(xrAcquireSwapchainImage_impl);
    } else if (requested == "xrWaitSwapchainImage") {
        *function = cast_function(xrWaitSwapchainImage_impl);
    } else if (requested == "xrReleaseSwapchainImage") {
        *function = cast_function(xrReleaseSwapchainImage_impl);
    } else if (requested == "xrEnumerateReferenceSpaces") {
        *function = cast_function(xrEnumerateReferenceSpaces_impl);
    } else if (requested == "xrGetReferenceSpaceBoundsRect") {
        *function = cast_function(xrGetReferenceSpaceBoundsRect_impl);
    } else if (requested == "xrCreateReferenceSpace") {
        *function = cast_function(xrCreateReferenceSpace_impl);
    } else if (requested == "xrCreateActionSet") {
        *function = cast_function(xrCreateActionSet_impl);
    } else if (requested == "xrDestroyActionSet") {
        *function = cast_function(xrDestroyActionSet_impl);
    } else if (requested == "xrCreateAction") {
        *function = cast_function(xrCreateAction_impl);
    } else if (requested == "xrDestroyAction") {
        *function = cast_function(xrDestroyAction_impl);
    } else if (requested == "xrStringToPath") {
        *function = cast_function(xrStringToPath_impl);
    } else if (requested == "xrPathToString") {
        *function = cast_function(xrPathToString_impl);
    } else if (requested == "xrSuggestInteractionProfileBindings") {
        *function = cast_function(xrSuggestInteractionProfileBindings_impl);
    } else if (requested == "xrCreateActionSpace") {
        *function = cast_function(xrCreateActionSpace_impl);
    } else if (requested == "xrLocateSpace") {
        *function = cast_function(xrLocateSpace_impl);
    } else if (requested == "xrAttachSessionActionSets") {
        *function = cast_function(xrAttachSessionActionSets_impl);
    } else if (requested == "xrSyncActions") {
        *function = cast_function(xrSyncActions_impl);
    } else if (requested == "xrGetActionStateBoolean") {
        *function = cast_function(xrGetActionStateBoolean_impl);
    } else if (requested == "xrGetActionStateFloat") {
        *function = cast_function(xrGetActionStateFloat_impl);
    } else if (requested == "xrGetActionStateVector2f") {
        *function = cast_function(xrGetActionStateVector2f_impl);
    } else if (requested == "xrGetActionStatePose") {
        *function = cast_function(xrGetActionStatePose_impl);
    } else if (requested == "xrGetCurrentInteractionProfile") {
        *function = cast_function(xrGetCurrentInteractionProfile_impl);
    } else if (requested == "xrEnumerateBoundSourcesForAction") {
        *function = cast_function(xrEnumerateBoundSourcesForAction_impl);
    } else if (requested == "xrGetInputSourceLocalizedName") {
        *function = cast_function(xrGetInputSourceLocalizedName_impl);
    } else if (requested == "xrApplyHapticFeedback") {
        *function = cast_function(xrApplyHapticFeedback_impl);
    } else if (requested == "xrStopHapticFeedback") {
        *function = cast_function(xrStopHapticFeedback_impl);
    } else if (requested == "xrLocateViews") {
        *function = cast_function(xrLocateViews_impl);
    } else if (requested == "xrDestroySpace") {
        *function = cast_function(xrDestroySpace_impl);
    } else if (requested == "xrWaitFrame") {
        *function = cast_function(xrWaitFrame_impl);
    } else if (requested == "xrBeginFrame") {
        *function = cast_function(xrBeginFrame_impl);
    } else if (requested == "xrEndFrame") {
        *function = cast_function(xrEndFrame_impl);
    } else {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }

    return XR_SUCCESS;
}

} // namespace

int session_probe()
{
    return 1;
}

XrResult negotiate_loader_runtime_interface(
    const XrNegotiateLoaderInfo* loaderInfo,
    XrNegotiateRuntimeRequest* runtimeRequest)
{
    log_call("xrNegotiateLoaderRuntimeInterface");
    if (loaderInfo == nullptr || runtimeRequest == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo)) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (loaderInfo->minApiVersion > XR_MAKE_VERSION(1, 0, 0) ||
        loaderInfo->maxApiVersion < XR_MAKE_VERSION(1, 0, 0)) {
        return XR_ERROR_API_VERSION_UNSUPPORTED;
    }

    std::memset(runtimeRequest, 0, sizeof(*runtimeRequest));
    runtimeRequest->structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    runtimeRequest->structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    runtimeRequest->structSize = sizeof(XrNegotiateRuntimeRequest);
    runtimeRequest->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    runtimeRequest->runtimeApiVersion = XR_MAKE_VERSION(1, 0, 0);
    runtimeRequest->getInstanceProcAddr = xrGetInstanceProcAddr_impl;
    return XR_SUCCESS;
}

} // namespace refract::runtime

#if defined(__ANDROID__)
extern "C" void refract_set_android_context(JavaVM* vm, jobject context)
{
    refract::runtime::pose_client().set_android_context(vm, context);
}
#endif
