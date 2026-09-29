#include "openxr_host.h"
#include "menu_shortcut.h"
#include "perf_stats.h"
#include "mirror_window.h"
#include "loading_screen.h"
#include "windows_gpu_receiver.h"
#include "debug_frame_capture.h"
#include "windows_gpu_frame.h"

#include "gpu_transport.h"
#include "image_transport.h"
#include "space_velocity.h"
#include "transport_tcp.h"
#include "video_transport.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <memory>
#include <mutex>
#include <string_view>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#define XR_USE_TIMESPEC
#else
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#if defined(_WIN32)
#else
#include <dlfcn.h>
#include <time.h>
#endif

namespace refract::host {

namespace {

constexpr float kAppProjectionHalfFovRadians = 0.95f;

bool parse_u16(const char* text, uint16_t* value)
{
    uint32_t parsed = 0;
    const std::string_view input{text};
    const auto result = std::from_chars(input.data(), input.data() + input.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != input.data() + input.size() || parsed > 65535) {
        return false;
    }
    *value = static_cast<uint16_t>(parsed);
    return true;
}

bool parse_u32(const char* text, uint32_t* value)
{
    const std::string_view input{text};
    const auto result = std::from_chars(input.data(), input.data() + input.size(), *value);
    return result.ec == std::errc{} && result.ptr == input.data() + input.size();
}

void print_usage()
{
    std::fprintf(stderr, "Usage:\n");
    std::fprintf(stderr, "  refract-host-bridge --serve [port] [frames]\n");
    std::fprintf(stderr, "  refract-host-bridge --serve-openxr [port] [frames] [game-name]\n");
    std::fprintf(stderr, "  refract-host-bridge --serve-images [port] [frames]\n");
    std::fprintf(stderr, "  refract-host-bridge --serve-gpu-fds [socket-path] [frames]\n");
    std::fprintf(stderr, "  refract-host-bridge --video-recv-udp [port] [frames]\n");
    std::fprintf(stderr, "  refract-host-bridge --video-send-synthetic [host] [port] [frames] [fps]\n");
    std::fprintf(stderr, "  refract-host-bridge --video-send-rgba [host] [port] [frames] [fps] [width] [height]\n");
    std::fprintf(stderr, "  refract-host-bridge --smoke\n");
}

const char* xr_result_name(XrResult result)
{
    switch (result) {
    case XR_SUCCESS:
        return "XR_SUCCESS";
    case XR_TIMEOUT_EXPIRED:
        return "XR_TIMEOUT_EXPIRED";
    case XR_FRAME_DISCARDED:
        return "XR_FRAME_DISCARDED";
    case XR_SESSION_LOSS_PENDING:
        return "XR_SESSION_LOSS_PENDING";
    case XR_EVENT_UNAVAILABLE:
        return "XR_EVENT_UNAVAILABLE";
    case XR_SESSION_NOT_FOCUSED:
        return "XR_SESSION_NOT_FOCUSED";
    case XR_ERROR_RUNTIME_FAILURE:
        return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_VALIDATION_FAILURE:
        return "XR_ERROR_VALIDATION_FAILURE";
    case XR_ERROR_RUNTIME_UNAVAILABLE:
        return "XR_ERROR_RUNTIME_UNAVAILABLE";
    case XR_ERROR_EXTENSION_NOT_PRESENT:
        return "XR_ERROR_EXTENSION_NOT_PRESENT";
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE:
        return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
    case XR_ERROR_INITIALIZATION_FAILED:
        return "XR_ERROR_INITIALIZATION_FAILED";
    case XR_ERROR_GRAPHICS_DEVICE_INVALID:
        return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
    case XR_ERROR_CALL_ORDER_INVALID:
        return "XR_ERROR_CALL_ORDER_INVALID";
    default:
        return "XR_RESULT_UNKNOWN";
    }
}

refract::protocol::Pose to_protocol_pose(const XrPosef& pose)
{
    refract::protocol::Pose out{};
    out.x = pose.position.x;
    out.y = pose.position.y;
    out.z = pose.position.z;
    out.qx = pose.orientation.x;
    out.qy = pose.orientation.y;
    out.qz = pose.orientation.z;
    out.qw = pose.orientation.w;
    return out;
}

// Only for a fully valid pose: a velocity without its pose means nothing to the game.
refract::protocol::SpaceVelocity to_protocol_velocity(const XrSpaceVelocity& v, XrSpaceLocationFlags poseFlags)
{
    refract::protocol::SpaceVelocity out{};
    if ((poseFlags & 3) != 3) return out;
    if (v.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) {
        out.flags |= 1;
        out.linear = {v.linearVelocity.x, v.linearVelocity.y, v.linearVelocity.z};
    }
    if (v.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) {
        out.flags |= 2;
        out.angular = {v.angularVelocity.x, v.angularVelocity.y, v.angularVelocity.z};
    }
    return refract::protocol::clean_velocity(out);
}

uint64_t monotonic_time_ns()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

bool decode_video_frame_to_image(const refract::protocol::EncodedVideoFrame& frame,
                                 refract::protocol::ImageFrameHeader* header,
                                 std::vector<uint8_t>* pixels)
{
    if (frame.codec != refract::protocol::kEncodedVideoCodecRefractRgba8) {
        return false;
    }
    const uint64_t expected = static_cast<uint64_t>(frame.width) * frame.height * 4;
    if (frame.width == 0 || frame.height == 0 || frame.payload.size() != expected) {
        return false;
    }

    refract::protocol::ImageFrameHeader outHeader{};
    outHeader.width = frame.width;
    outHeader.height = frame.height;
    outHeader.layers = 1;
    outHeader.sequence = frame.frame_id;
    outHeader.monotonic_time_ns = frame.capture_time_ns;
    outHeader.payload_size = expected;
    *header = outHeader;
    *pixels = frame.payload;
    return true;
}

#if defined(_WIN32)
template <typename T>
class ComPtr {
public:
    ~ComPtr()
    {
        if (ptr_ != nullptr) {
            ptr_->Release();
        }
    }

    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept
        : ptr_(other.ptr_)
    {
        other.ptr_ = nullptr;
    }
    ComPtr& operator=(ComPtr&& other) noexcept
    {
        if (this != &other) {
            if (ptr_ != nullptr) {
                ptr_->Release();
            }
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T* get() const { return ptr_; }
    T** put()
    {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
        return &ptr_;
    }

private:
    T* ptr_ = nullptr;
};
#endif

class OpenXrLoader {
public:
    ~OpenXrLoader()
    {
#if defined(_WIN32)
        if (library_ != nullptr) {
            FreeLibrary(library_);
        }
#else
        if (library_ != nullptr) {
            dlclose(library_);
        }
#endif
    }

    bool load()
    {
#if defined(_WIN32)
        char loaderPath[1024]{};
        const DWORD loaderPathSize = GetEnvironmentVariableA("REFRACT_OPENXR_LOADER", loaderPath, sizeof(loaderPath));
        if (loaderPathSize > 0 && loaderPathSize < sizeof(loaderPath)) {
            library_ = LoadLibraryA(loaderPath);
        }
        if (library_ == nullptr) {
            library_ = LoadLibraryA("openxr_loader.dll");
        }
        if (library_ == nullptr) {
            library_ = LoadLibraryA(
                "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openxr_loader.dll");
        }
        if (library_ == nullptr) {
            std::fprintf(stderr, "Refract OpenXR: failed to load openxr_loader.dll\n");
            return false;
        }
        auto get_symbol = [&](const char* name) -> void* {
            return reinterpret_cast<void*>(GetProcAddress(library_, name));
        };
#else
        library_ = dlopen("libopenxr_loader.so.1", RTLD_NOW | RTLD_LOCAL);
        if (library_ == nullptr) {
            library_ = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (library_ == nullptr) {
            std::fprintf(stderr, "Refract OpenXR: failed to load libopenxr_loader.so.1\n");
            return false;
        }
        auto get_symbol = [&](const char* name) -> void* {
            return dlsym(library_, name);
        };
#endif

        createInstance = reinterpret_cast<PFN_xrCreateInstance>(get_symbol("xrCreateInstance"));
        getInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(get_symbol("xrGetInstanceProcAddr"));
        if (createInstance == nullptr || getInstanceProcAddr == nullptr) {
            std::fprintf(stderr, "Refract OpenXR: loader is missing required exports\n");
            return false;
        }
        return true;
    }

    PFN_xrCreateInstance createInstance = nullptr;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr = nullptr;

private:
#if defined(_WIN32)
    HMODULE library_ = nullptr;
#else
    void* library_ = nullptr;
#endif
};

struct HostImageFrame {
    void store(const refract::protocol::ImageFrameHeader& newHeader, std::vector<uint8_t>&& newPixels,
               const refract::protocol::ImageProjection& newProjection = {})
    {
        static refract::protocol::FrameIntervals stats("host-image-arrival");
        stats.record();
        std::lock_guard<std::mutex> lock(mutex);
        header = newHeader;
        projection = newProjection;
        pixels = std::make_shared<std::vector<uint8_t>>(std::move(newPixels));
        hasFrame = true;
    }

    bool snapshot(refract::protocol::ImageFrameHeader* outHeader, std::shared_ptr<const std::vector<uint8_t>>* outPixels,
                  refract::protocol::ImageProjection* outProjection)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!hasFrame || pixels == nullptr) {
            return false;
        }
        *outHeader = header;
        *outProjection = projection;
        *outPixels = pixels;
        return true;
    }

    std::mutex mutex;
    refract::protocol::ImageFrameHeader header{};
    refract::protocol::ImageProjection projection{};
    std::shared_ptr<std::vector<uint8_t>> pixels;
    bool hasFrame = false;
};

class OpenXrPoseSource {
public:
    explicit OpenXrPoseSource(HostImageFrame* imageFrame = nullptr)
        : imageFrame_(imageFrame)
    {
    }

    ~OpenXrPoseSource()
    {
        if (session_ != XR_NULL_HANDLE && endSession_ != nullptr && sessionRunning_) {
            endSession_(session_);
        }
        if (viewSpace_ != XR_NULL_HANDLE && destroySpace_ != nullptr) {
            destroySpace_(viewSpace_);
        }
        if (localSpace_ != XR_NULL_HANDLE && destroySpace_ != nullptr) {
            destroySpace_(localSpace_);
        }
        if (appLocalSpace_ != XR_NULL_HANDLE && destroySpace_) destroySpace_(appLocalSpace_);
#if defined(_WIN32)
        if (projectionSwapchain_ != XR_NULL_HANDLE && destroySwapchain_ != nullptr) {
            destroySwapchain_(projectionSwapchain_);
            projectionSwapchain_ = XR_NULL_HANDLE;
        }
#endif
        for (XrSpace& handSpace : handSpaces_) {
            if (handSpace != XR_NULL_HANDLE && destroySpace_ != nullptr) {
                destroySpace_(handSpace);
                handSpace = XR_NULL_HANDLE;
            }
        }
        for (auto tracker : handTrackers_) {
            if (tracker && destroyHandTracker_) destroyHandTracker_(tracker);
        }
        for (XrSpace space : aimSpaces_) {
            if (space != XR_NULL_HANDLE && destroySpace_) destroySpace_(space);
        }
        if (aimPoseAction_ != XR_NULL_HANDLE && destroyAction_) destroyAction_(aimPoseAction_);
        if (handPoseAction_ != XR_NULL_HANDLE && destroyAction_ != nullptr) {
            destroyAction_(handPoseAction_);
            handPoseAction_ = XR_NULL_HANDLE;
        }
        if (actionSet_ != XR_NULL_HANDLE && destroyActionSet_ != nullptr) {
            destroyActionSet_(actionSet_);
            actionSet_ = XR_NULL_HANDLE;
        }
        if (session_ != XR_NULL_HANDLE && destroySession_ != nullptr) {
            destroySession_(session_);
        }
        if (instance_ != XR_NULL_HANDLE && destroyInstance_ != nullptr) {
            destroyInstance_(instance_);
        }
    }

    bool initialize(const std::string& gameName)
    {
        if (!loader_.load()) {
            return false;
        }

        std::vector<const char*> extensions = {
#if defined(_WIN32)
            XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
#else
            XR_MND_HEADLESS_EXTENSION_NAME,
            XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME,
#endif
        };
        PFN_xrVoidFunction enumerateRaw = nullptr;
        loader_.getInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &enumerateRaw);
        if (enumerateRaw) {
            auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(enumerateRaw);
            uint32_t count = 0; enumerate(nullptr, 0, &count, nullptr);
            std::vector<XrExtensionProperties> available(count, {XR_TYPE_EXTENSION_PROPERTIES});
            if (enumerate(nullptr, count, &count, available.data()) == XR_SUCCESS) {
                for (const auto& ext : available) {
                    if (std::strcmp(ext.extensionName, XR_EXT_HAND_TRACKING_EXTENSION_NAME) == 0) handTrackingEnabled_ = true;
                    if (std::strcmp(ext.extensionName, XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME) == 0) handDataSourceEnabled_ = true;
#if defined(_WIN32)
                    if (std::strcmp(ext.extensionName, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME) == 0)
                        win32TimeEnabled_ = true;
#endif
                }
            }
        }
#if defined(_WIN32)
        if (win32TimeEnabled_) extensions.push_back(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
#endif
        if (handTrackingEnabled_) {
            extensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
            if (handDataSourceEnabled_) extensions.push_back(XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME);
        }
        XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
        std::string applicationName = gameName + " \xe2\x80\x93 Refract";
        size_t nameLength = (std::min)(applicationName.size(), size_t(XR_MAX_APPLICATION_NAME_SIZE - 1));
        while (nameLength < applicationName.size() && nameLength &&
               (static_cast<unsigned char>(applicationName[nameLength]) & 0xc0) == 0x80) --nameLength;
        std::memcpy(instanceInfo.applicationInfo.applicationName, applicationName.data(), nameLength);
        instanceInfo.applicationInfo.applicationVersion = 1;
        std::strncpy(instanceInfo.applicationInfo.engineName, "Refract", XR_MAX_ENGINE_NAME_SIZE - 1);
        instanceInfo.applicationInfo.engineVersion = 1;
#if defined(_WIN32)
        instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
#else
        instanceInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
#endif
        instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        instanceInfo.enabledExtensionNames = extensions.data();

        XrResult result = loader_.createInstance(&instanceInfo, &instance_);
        if (result != XR_SUCCESS) {
            std::fprintf(
                stderr,
                "Refract OpenXR: xrCreateInstance failed: %s (%d). Required extension: %s\n",
                xr_result_name(result),
                result,
#if defined(_WIN32)
                XR_KHR_D3D11_ENABLE_EXTENSION_NAME
#else
                XR_MND_HEADLESS_EXTENSION_NAME
#endif
            );
            return false;
        }

        if (!load_instance_functions()) {
            return false;
        }

        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        result = getSystem_(instance_, &systemInfo, &systemId_);
        if (result != XR_SUCCESS) {
            std::fprintf(stderr, "Refract OpenXR: xrGetSystem failed: %s (%d)\n", xr_result_name(result), result);
            return false;
        }

#if defined(_WIN32)
        if (!create_d3d11_device()) {
            return false;
        }
        XrGraphicsBindingD3D11KHR graphicsBinding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        graphicsBinding.device = d3dDevice_.get();
#endif

        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
        sessionInfo.systemId = systemId_;
#if defined(_WIN32)
        sessionInfo.next = &graphicsBinding;
#endif
        result = createSession_(instance_, &sessionInfo, &session_);
        if (result != XR_SUCCESS) {
            std::fprintf(stderr, "Refract OpenXR: xrCreateSession failed: %s (%d)\n", xr_result_name(result), result);
            return false;
        }

        // Android games commonly request STAGE/floor tracking. Keep the shared
        // tracking world floor-relative when SteamVR provides a stage space.
        const bool hasFloor = create_reference_space(XR_REFERENCE_SPACE_TYPE_STAGE, &localSpace_);
        trackingSpaceType_ = hasFloor ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
        std::fprintf(stderr, "Refract OpenXR: tracking origin=%s\n", hasFloor ? "STAGE (floor)" : "LOCAL");
        if ((!hasFloor && !create_reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL, &localSpace_)) ||
            !create_reference_space(XR_REFERENCE_SPACE_TYPE_VIEW, &viewSpace_) ||
            !create_reference_space(XR_REFERENCE_SPACE_TYPE_LOCAL, &appLocalSpace_)) {
            return false;
        }

#if defined(_WIN32)
        if (!create_projection_swapchain()) return false;
#endif
        initialize_controller_actions();
        initialize_hand_tracking();

        std::fprintf(
            stderr,
            "Refract OpenXR: host tracking source initialized with %s\n",
#if defined(_WIN32)
            XR_KHR_D3D11_ENABLE_EXTENSION_NAME
#else
            XR_MND_HEADLESS_EXTENSION_NAME
#endif
        );
        return true;
    }

    bool open_mirror(const std::string& gameName) {
#if defined(_WIN32)
        return mirror_.open(d3dDevice_.get(), gameName);
#else
        return true;
#endif
    }
    bool pump_mirror() {
#if defined(_WIN32)
        return mirror_.pump();
#else
        return true;
#endif
    }

    refract::protocol::PoseFrame make_frame(uint64_t sequence)
    {
        pump_events();

        refract::protocol::PoseFrame frame = latest_frame(sequence);
        frame.hmd_flags = frame.local_origin_flags = 0;
        frame.hmd_velocity = frame.local_origin_velocity = {};
        frame.controllers[0] = {};
        frame.controllers[1] = {};
        for (size_t hand = 0; hand < 2; ++hand) {
            frame.grip_flags[hand] = frame.aim_flags[hand] = 0;
            frame.aim_active[hand] = 0;
            frame.grip_velocity[hand] = frame.aim_velocity[hand] = {};
        }
        frame.sequence = sequence;
        frame.monotonic_time_ns = monotonic_time_ns();
#if defined(_WIN32)
        frame.render_width = projectionWidth_;
        frame.render_height = projectionHeight_;
#endif

        if (!sessionRunning_) {
            publish_pose(frame);
            return frame;
        }

        XrTime locateTime = current_xr_time();
        XrTime frameDisplayTime = locateTime;
        bool beganFrame = false;

        if (useFrameLoop_) {
            XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState frameState{XR_TYPE_FRAME_STATE};
            XrResult result;
            {
                static refract::protocol::PerfStats stats("host-wait-frame");
                refract::protocol::PerfScope scope(stats);
                result = waitFrame_(session_, &waitInfo, &frameState);
            }
            if (result == XR_SUCCESS) {
                frameDisplayTime = frameState.predictedDisplayTime;
                // Poses are predicted to the runtime's display time (~38 ms ahead over Steam Link).
                // REFRACT_POSE_PREDICTION=0 locates the newest tracked poses instead: nothing overshoots, but
                // the runtime then has to reproject the whole pipeline delay, which looked smeared.
                const XrTime now = posePrediction_ ? 0 : current_xr_time();
                locateTime = now ? (std::min)(now, frameState.predictedDisplayTime) : frameState.predictedDisplayTime;
                if (now && sequence % 900 == 0) {
                    std::fprintf(stderr, "Refract OpenXR: runtime display time is %.1f ms ahead; poses located now\n",
                        (frameState.predictedDisplayTime - now) / 1e6);
                }
                if (frameState.shouldRender && refract::protocol::valid_display_period(frameState.predictedDisplayPeriod)) {
                    frame.display_period_ns = static_cast<uint32_t>(frameState.predictedDisplayPeriod);
                }

                XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
                {
                    static refract::protocol::PerfStats stats("host-begin-frame");
                    refract::protocol::PerfScope scope(stats);
                    result = beginFrame_(session_, &beginInfo);
                }
                if (result == XR_SUCCESS) {
                    beganFrame = true;
                } else if (result == XR_ERROR_CALL_ORDER_INVALID) {
                    std::fprintf(
                        stderr,
                        "Refract OpenXR: xrBeginFrame was rejected in headless mode; continuing with pose-only locate\n");
                    useFrameLoop_ = false;
                } else {
                    std::fprintf(stderr, "Refract OpenXR: xrBeginFrame failed: %s (%d)\n", xr_result_name(result), result);
                    publish_pose(frame);
                    return frame;
                }
            } else if (result == XR_FRAME_DISCARDED) {
                publish_pose(frame);
                return frame;
            } else {
                std::fprintf(stderr, "Refract OpenXR: xrWaitFrame failed: %s (%d)\n", xr_result_name(result), result);
                useFrameLoop_ = false;
            }
        }

        if (locateTime == 0) {
            locateTime = current_xr_time();
        }
        if (locateTime == 0) {
            publish_pose(frame);
            return frame;
        }

        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        XrSpaceLocation origin{XR_TYPE_SPACE_LOCATION};
        XrSpaceVelocity headVelocity{XR_TYPE_SPACE_VELOCITY};
        XrSpaceVelocity originVelocity{XR_TYPE_SPACE_VELOCITY};
        location.next = &headVelocity;
        origin.next = &originVelocity;
        if (!localOriginInitialized_) {
            XrSpaceLocation initialHead{XR_TYPE_SPACE_LOCATION};
            if (locateSpace_(viewSpace_, localSpace_, locateTime, &initialHead) == XR_SUCCESS &&
                (initialHead.locationFlags & 3) == 3) {
                XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
                info.referenceSpaceType = trackingSpaceType_;
                // Correct eye height without recentering heading or horizontal
                // position from a headset that may still be resting on a desk.
                // Use the common tracking axes: native LOCAL may be rotated
                // relative to STAGE by the runtime's seated calibration.
                info.poseInReferenceSpace.orientation.w = 1.0f;
                info.poseInReferenceSpace.position.y = initialHead.pose.position.y;
                XrSpace startupLocal = XR_NULL_HANDLE;
                if (createReferenceSpace_(session_, &info, &startupLocal) == XR_SUCCESS) {
                    destroySpace_(appLocalSpace_);
                    appLocalSpace_ = startupLocal;
                    localOriginInitialized_ = true;
                }
            }
        }
        if (localOriginInitialized_ && locateSpace_(appLocalSpace_, localSpace_, locateTime, &origin) == XR_SUCCESS &&
            (origin.locationFlags & 3) == 3) {
            frame.local_origin = to_protocol_pose(origin.pose);
            frame.local_origin_flags = static_cast<uint32_t>(origin.locationFlags);
            frame.local_origin_velocity = to_protocol_velocity(originVelocity, origin.locationFlags);
            if (!reportedLocalOrigin_) {
                std::fprintf(stderr, "Refract OpenXR: LOCAL origin in tracking world=(%.3f %.3f %.3f) q=(%.4f %.4f %.4f %.4f)\n",
                    origin.pose.position.x, origin.pose.position.y, origin.pose.position.z,
                    origin.pose.orientation.x, origin.pose.orientation.y, origin.pose.orientation.z, origin.pose.orientation.w);
                reportedLocalOrigin_ = true;
            }
        }
        XrResult result = locateSpace_(viewSpace_, localSpace_, locateTime, &location);
        if (result == XR_SUCCESS &&
            (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
            frame.hmd = to_protocol_pose(location.pose);
            frame.hmd_flags = static_cast<uint32_t>(location.locationFlags);
            frame.hmd_velocity = to_protocol_velocity(headVelocity, location.locationFlags);
            publishedPoses_[publishedPoseCount_++ % publishedPoses_.size()] = {frame.hmd, frameDisplayTime};
            if (sequence % 90 == 0) {
                std::fprintf(
                    stderr,
                    "Refract OpenXR: pose seq=%llu hmd=(%.3f %.3f %.3f)\n",
                    static_cast<unsigned long long>(sequence),
                    frame.hmd.x,
                    frame.hmd.y,
                    frame.hmd.z);
            }
        }

        // REFRACT_PREDICTION_LEAD_MS predicts controllers and hands that much further, for the frames the
        // pipeline adds after this one. Off by default: the measured delay (half or all of it) overshot.
        // The head is never predicted further; the runtime reprojects its rotation anyway.
        const XrTime handTime = locateTime + (useFrameLoop_ && posePrediction_ ? predictionLead_ : 0);
        locate_controller_spaces(frame, handTime, sequence);
        locate_hand_joints(frame, handTime, sequence);
        publish_pose(frame);

        if (beganFrame) {
            std::array<XrCompositionLayerProjectionView, 2> projectionViews{};
            XrCompositionLayerProjection projectionLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            std::array<XrCompositionLayerQuad, refract::protocol::kMaxCompositionLayers> quadLayers{};
            const XrCompositionLayerBaseHeader* layers[refract::protocol::kMaxCompositionLayers]{};
            bool mixedProjection = false;
            uint32_t layerCount = 0;
#if defined(_WIN32)
            if (projectionSwapchain_ != XR_NULL_HANDLE &&
                update_projection_layer(frameDisplayTime, projectionViews, projectionLayer, quadLayers, layerCount, mixedProjection)) {
                if (layerCount) {
                    const uint32_t offset = mixedProjection ? 1 : 0;
                    if (mixedProjection) layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer);
                    for (uint32_t i = 0; i < layerCount; ++i)
                        layers[offset + i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quadLayers[i]);
                    layerCount += offset;
                } else {
                    layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer);
                    layerCount = 1;
                }
            }
#endif

            XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime = frameDisplayTime;
            endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            endInfo.layerCount = layerCount;
            endInfo.layers = layerCount > 0 ? layers : nullptr;
            {
                static refract::protocol::PerfStats stats("host-end-frame");
                refract::protocol::PerfScope scope(stats);
                result = endFrame_(session_, &endInfo);
            }
            if (result != XR_SUCCESS) {
                std::fprintf(stderr, "Refract OpenXR: xrEndFrame failed: %s (%d)\n", xr_result_name(result), result);
                useFrameLoop_ = false;
            }
        }

        return frame;
    }

    refract::protocol::PoseFrame latest_frame(uint64_t sequence)
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        refract::protocol::PoseFrame frame = latest_;
        frame.sequence = sequence;
        frame.monotonic_time_ns = monotonic_time_ns();
        return frame;
    }

    // Receive on the image thread, independently of the OpenXR compositor clock.
    // The same lock protects cross-device cache ownership and its render poses.
    bool receive_image(const refract::protocol::ImageFrameHeader& header,
                       const refract::protocol::ImageProjection& projection,
                       std::vector<uint8_t>&& pixels) {
#if defined(_WIN32)
        static refract::protocol::PerfStats stats("host-receive-image");
        refract::protocol::PerfScope scope(stats);
        const auto lockStart = std::chrono::steady_clock::now();
        std::lock_guard lock(gpuMutex_);
        static refract::protocol::PerfStats lockStats("host-receive-lock");
        lockStats.record(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lockStart).count());
        if (refract::protocol::mixed_gpu_version(header.version)) {
            if (!refract::protocol::valid_mixed_part(header.version, header.reserved) ||
                pixels.size() != sizeof(refract::protocol::WindowsGpuFrame) ||
                header.width > projectionWidth_ || header.height > projectionHeight_) return false;
            const uint32_t count = header.reserved >> 16, index = header.reserved & 0xffff;
            if (index == 0) { pendingMixedBank_ = 1 - activeMixedBank_; pendingMixedCount_ = count; pendingMixedIndex_ = 0; pendingMixedSequence_ = header.sequence; }
            if (count != pendingMixedCount_ || index != pendingMixedIndex_ || header.sequence != pendingMixedSequence_) return false;
            auto& part = mixedFrames_[pendingMixedBank_][index];
            refract::protocol::WindowsGpuFrame gpu{}; std::memcpy(&gpu, pixels.data(), sizeof(gpu));
            if (!part.receiver.receive(receiveDevice_.get(), receiveContext_.get(), gpu, header.sequence,
                    header.width, header.height, static_cast<DXGI_FORMAT>(projectionFormat_))) { pendingMixedCount_ = 0; return false; }
            part.header = header; part.projection = projection;
            ++pendingMixedIndex_; ++pendingMixedSequence_;
            if (pendingMixedIndex_ == count) {
                activeMixedBank_ = pendingMixedBank_; activeMixedCount_ = count;
                auto complete = mixedFrames_[activeMixedBank_][0].header;
                complete.sequence = header.sequence; // Publish only after all GPU copies complete.
                imageFrame_->store(complete, std::move(pixels), mixedFrames_[activeMixedBank_][0].projection);
                pendingMixedCount_ = 0;
            }
            return true;
        }
        if ((header.version == refract::protocol::kWindowsGpuFrameVersion || header.version == refract::protocol::kQuadGpuFrameVersion)) {
            if (pixels.size() != sizeof(refract::protocol::WindowsGpuFrame) ||
                header.width > projectionWidth_ || header.height > projectionHeight_) return false;
            refract::protocol::WindowsGpuFrame gpu{};
            std::memcpy(&gpu, pixels.data(), sizeof(gpu));
            if (!gpuReceiver_.receive(receiveDevice_.get(), receiveContext_.get(), gpu,
                    header.sequence, header.width, header.height,
                    static_cast<DXGI_FORMAT>(projectionFormat_))) {
                // A failed import may invalidate the receiver cache. Do not
                // display old metadata against a partial or missing cache.
                std::lock_guard frameLock(imageFrame_->mutex);
                imageFrame_->hasFrame = false;
                return false;
            }
        }
#else
        if ((header.version == refract::protocol::kWindowsGpuFrameVersion || header.version == refract::protocol::kQuadGpuFrameVersion)) return false;
#endif
        imageFrame_->store(header, std::move(pixels), projection);
        return true; // ACK only after the GPU copy and metadata publication.
    }

    bool drives_frame_loop() const { return sessionRunning_ && useFrameLoop_; }

private:
    void publish_pose(const refract::protocol::PoseFrame& frame)
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        latest_ = frame;
    }

    template <typename T>
    bool load_func(const char* name, T* out)
    {
        PFN_xrVoidFunction function = nullptr;
        const XrResult result = loader_.getInstanceProcAddr(instance_, name, &function);
        if (result != XR_SUCCESS || function == nullptr) {
            std::fprintf(stderr, "Refract OpenXR: failed to load %s: %s (%d)\n", name, xr_result_name(result), result);
            return false;
        }
        *out = reinterpret_cast<T>(function);
        return true;
    }

    bool load_instance_functions()
    {
        return load_func("xrDestroyInstance", &destroyInstance_) &&
            load_func("xrGetSystem", &getSystem_) &&
            load_func("xrEnumerateViewConfigurationViews", &enumerateViewConfigurationViews_) &&
            load_func("xrCreateSession", &createSession_) &&
            load_func("xrDestroySession", &destroySession_) &&
            load_func("xrCreateReferenceSpace", &createReferenceSpace_) &&
            load_func("xrDestroySpace", &destroySpace_) &&
            load_func("xrPollEvent", &pollEvent_) &&
            load_func("xrBeginSession", &beginSession_) &&
            load_func("xrEndSession", &endSession_) &&
            load_func("xrWaitFrame", &waitFrame_) &&
            load_func("xrBeginFrame", &beginFrame_) &&
            load_func("xrEndFrame", &endFrame_) &&
            load_func("xrLocateSpace", &locateSpace_) &&
            load_func("xrLocateViews", &locateViews_) &&
            load_func("xrStringToPath", &stringToPath_) &&
            load_func("xrCreateActionSet", &createActionSet_) &&
            load_func("xrDestroyActionSet", &destroyActionSet_) &&
            load_func("xrCreateAction", &createAction_) &&
            load_func("xrDestroyAction", &destroyAction_) &&
            load_func("xrSuggestInteractionProfileBindings", &suggestInteractionProfileBindings_) &&
            load_func("xrAttachSessionActionSets", &attachSessionActionSets_) &&
            load_func("xrCreateActionSpace", &createActionSpace_) &&
            load_func("xrSyncActions", &syncActions_) &&
            load_func("xrGetActionStatePose", &getActionStatePose_) &&
            load_func("xrGetActionStateBoolean", &getActionStateBoolean_) &&
            load_func("xrGetActionStateFloat", &getActionStateFloat_) &&
            load_func("xrGetActionStateVector2f", &getActionStateVector2f_)
#if defined(_WIN32)
            && load_func("xrEnumerateSwapchainFormats", &enumerateSwapchainFormats_)
            && load_func("xrCreateSwapchain", &createSwapchain_)
            && load_func("xrDestroySwapchain", &destroySwapchain_)
            && load_func("xrEnumerateSwapchainImages", &enumerateSwapchainImages_)
            && load_func("xrAcquireSwapchainImage", &acquireSwapchainImage_)
            && load_func("xrWaitSwapchainImage", &waitSwapchainImage_)
            && load_func("xrReleaseSwapchainImage", &releaseSwapchainImage_)
            && load_func("xrGetD3D11GraphicsRequirementsKHR", &getD3D11GraphicsRequirements_)
#endif
#if !defined(_WIN32)
            && load_func("xrConvertTimespecTimeToTimeKHR", &convertTimespecTimeToTime_)
#endif
            ;
    }

    XrTime current_xr_time()
    {
#if defined(_WIN32)
        if (win32TimeEnabled_ && convertPerformanceCounter_ == nullptr &&
            !load_func("xrConvertWin32PerformanceCounterToTimeKHR", &convertPerformanceCounter_)) {
            win32TimeEnabled_ = false;
        }
        LARGE_INTEGER counter{};
        XrTime xrTime = 0;
        if (convertPerformanceCounter_ && QueryPerformanceCounter(&counter) &&
            convertPerformanceCounter_(instance_, &counter, &xrTime) == XR_SUCCESS) {
            return xrTime;
        }
#else
        if (convertTimespecTimeToTime_ != nullptr) {
            timespec now{};
            if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
                XrTime xrTime = 0;
                const XrResult result = convertTimespecTimeToTime_(instance_, &now, &xrTime);
                if (result == XR_SUCCESS) {
                    return xrTime;
                }
            }
        }
#endif
        return 0;
    }

#if defined(_WIN32)
    bool create_d3d11_device()
    {
        XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        const XrResult result = getD3D11GraphicsRequirements_(instance_, systemId_, &requirements);
        if (result != XR_SUCCESS) {
            std::fprintf(
                stderr,
                "Refract OpenXR: xrGetD3D11GraphicsRequirementsKHR failed: %s (%d)\n",
                xr_result_name(result),
                result);
            return false;
        }

        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.put()));
        if (FAILED(hr)) {
            std::fprintf(stderr, "Refract OpenXR: CreateDXGIFactory1 failed: 0x%08lx\n", static_cast<unsigned long>(hr));
            return false;
        }

        ComPtr<IDXGIAdapter1> selectedAdapter;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            hr = factory.get()->EnumAdapters1(i, adapter.put());
            if (hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(hr)) {
                continue;
            }

            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter.get()->GetDesc1(&desc))) {
                continue;
            }

            if (desc.AdapterLuid.HighPart == requirements.adapterLuid.HighPart &&
                desc.AdapterLuid.LowPart == requirements.adapterLuid.LowPart) {
                selectedAdapter = std::move(adapter);
                break;
            }
        }

        if (selectedAdapter.get() == nullptr) {
            std::fprintf(stderr, "Refract OpenXR: failed to find D3D11 adapter requested by OpenXR runtime\n");
            return false;
        }

        const D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL createdFeatureLevel{};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        hr = D3D11CreateDevice(
            selectedAdapter.get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            flags,
            featureLevels,
            sizeof(featureLevels) / sizeof(featureLevels[0]),
            D3D11_SDK_VERSION,
            d3dDevice_.put(),
            &createdFeatureLevel,
            d3dContext_.put());
        if (FAILED(hr)) {
            std::fprintf(stderr, "Refract OpenXR: D3D11CreateDevice failed: 0x%08lx\n", static_cast<unsigned long>(hr));
            return false;
        }

        // SteamVR also uses the binding device's immediate context during
        // xrEndFrame. Protect those accesses as well as our own copy commands.
        ComPtr<ID3D11Multithread> multithread;
        if (FAILED(d3dContext_.get()->QueryInterface(__uuidof(ID3D11Multithread),
                reinterpret_cast<void**>(multithread.put())))) return false;
        multithread.get()->SetMultithreadProtected(TRUE);

        if (createdFeatureLevel < requirements.minFeatureLevel) {
            std::fprintf(stderr, "Refract OpenXR: D3D11 feature level is below runtime requirement\n");
            return false;
        }

        // Reception has its own immediate context: SteamVR can hold its
        // binding context while pacing xrEndFrame without delaying Android.
        hr = D3D11CreateDevice(selectedAdapter.get(), D3D_DRIVER_TYPE_UNKNOWN,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels,
            sizeof(featureLevels) / sizeof(featureLevels[0]), D3D11_SDK_VERSION,
            receiveDevice_.put(), nullptr, receiveContext_.put());
        if (FAILED(hr)) return false;

        std::fprintf(stderr, "Refract OpenXR: D3D11 graphics binding ready\n");
        return true;
    }

    bool create_projection_swapchain()
    {
        uint32_t viewCount = 0;
        if (enumerateViewConfigurationViews_(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                0, &viewCount, nullptr) != XR_SUCCESS || viewCount != 2) return false;
        std::array<XrViewConfigurationView, 2> configViews{{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
        if (enumerateViewConfigurationViews_(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                2, &viewCount, configViews.data()) != XR_SUCCESS) return false;
        projectionWidth_ = projectionHeight_ = 0;
        // The stereo transport uses equal-size array slices. Accommodate both
        // recommendations if the runtime recommends asymmetric view sizes.
        for (const auto& view : configViews) {
            projectionWidth_ = (std::max)(projectionWidth_, view.recommendedImageRectWidth);
            projectionHeight_ = (std::max)(projectionHeight_, view.recommendedImageRectHeight);
        }
        if (!refract::protocol::valid_render_extent(projectionWidth_, projectionHeight_)) {
            std::fprintf(stderr, "Refract OpenXR: unsupported recommended eye extent %ux%u\n", projectionWidth_, projectionHeight_);
            return false;
        }
        std::fprintf(stderr, "Refract OpenXR: runtime recommended stereo extent %ux%u\n", projectionWidth_, projectionHeight_);
        uint32_t formatCount = 0;
        XrResult result = enumerateSwapchainFormats_(session_, 0, &formatCount, nullptr);
        if (result != XR_SUCCESS || formatCount == 0) {
            std::fprintf(stderr, "Refract OpenXR: xrEnumerateSwapchainFormats failed: %s (%d)\n", xr_result_name(result), result);
            return false;
        }

        std::vector<int64_t> formats(formatCount);
        result = enumerateSwapchainFormats_(session_, formatCount, &formatCount, formats.data());
        if (result != XR_SUCCESS) {
            std::fprintf(stderr, "Refract OpenXR: xrEnumerateSwapchainFormats(list) failed: %s (%d)\n", xr_result_name(result), result);
            return false;
        }

        std::fprintf(stderr, "Refract OpenXR: supported swapchain formats:");
        for (int64_t format : formats) {
            std::fprintf(stderr, " %lld", static_cast<long long>(format));
        }
        std::fprintf(stderr, "\n");

        int64_t selectedFormat = formats[0];
        constexpr int64_t preferredFormats[] = {
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        };
        for (int64_t preferred : preferredFormats) {
            for (int64_t format : formats) {
                if (format == preferred) {
                    selectedFormat = format;
                    break;
                }
            }
            if (selectedFormat == preferred) {
                break;
            }
        }
        projectionFormat_ = selectedFormat;

        XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
        swapchainInfo.format = selectedFormat;
        swapchainInfo.sampleCount = 1;
        swapchainInfo.width = projectionWidth_;
        swapchainInfo.height = projectionHeight_;
        swapchainInfo.faceCount = 1;
        swapchainInfo.arraySize = projectionArraySize_;
        swapchainInfo.mipCount = 1;

        result = createSwapchain_(session_, &swapchainInfo, &projectionSwapchain_);
        if (result != XR_SUCCESS) {
            std::fprintf(stderr, "Refract OpenXR: xrCreateSwapchain failed: %s (%d)\n", xr_result_name(result), result);
            return false;
        }

        uint32_t imageCount = 0;
        result = enumerateSwapchainImages_(projectionSwapchain_, 0, &imageCount, nullptr);
        if (result != XR_SUCCESS || imageCount == 0) {
            std::fprintf(stderr, "Refract OpenXR: xrEnumerateSwapchainImages failed: %s (%d)\n", xr_result_name(result), result);
            return false;
        }

        projectionImages_.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        uploadedAndroidSequenceByImage_.assign(imageCount, UINT64_MAX);
        uploadedGpuSessionByImage_.assign(imageCount, 0);
        uploadedProjectionByImage_.resize(imageCount);
        uploadedMixedQuads_.resize(imageCount);
        uploadedMixedExtents_.resize(imageCount);
        uploadedMixedCounts_.assign(imageCount, 0);
        uploadedMixedTimes_.assign(imageCount, 0);
        uploadedExtentByImage_.resize(imageCount, {static_cast<int32_t>(projectionWidth_), static_cast<int32_t>(projectionHeight_)});
        loadingByImage_.assign(imageCount, 0);
        loadingCardByImage_.assign(imageCount, 0);
        result = enumerateSwapchainImages_(
            projectionSwapchain_,
            imageCount,
            &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(projectionImages_.data()));
        if (result != XR_SUCCESS) {
            std::fprintf(stderr, "Refract OpenXR: xrEnumerateSwapchainImages(list) failed: %s (%d)\n", xr_result_name(result), result);
            return false;
        }

        std::fprintf(
            stderr,
            "Refract OpenXR: projection swapchain ready %ux%u images=%u\n",
            projectionWidth_,
            projectionHeight_,
            imageCount);
        return true;
    }

    bool update_projection_layer(
        XrTime displayTime,
        std::array<XrCompositionLayerProjectionView, 2>& projectionViews,
        XrCompositionLayerProjection& projectionLayer,
        std::array<XrCompositionLayerQuad, refract::protocol::kMaxCompositionLayers>& quadLayers,
        uint32_t& quadCount, bool& mixedProjection)
    {
        static refract::protocol::PerfStats stats("host-projection");
        refract::protocol::PerfScope scope(stats);
        // Grow only when an actual complete batch needs more texture slices.
        // Ordinary stereo games keep their original two-slice allocation.
        {
            std::lock_guard lock(gpuMutex_);
            refract::protocol::ImageFrameHeader header{};
            refract::protocol::ImageProjection composition{};
            std::shared_ptr<const std::vector<uint8_t>> payload;
            if (imageFrame_ && imageFrame_->snapshot(&header, &payload, &composition) && refract::protocol::mixed_gpu_version(header.version)) {
                const uint32_t needed = activeMixedCount_ + (composition.view_count == 2 ? 1 : 0);
                if (needed > projectionArraySize_) {
                    d3dContext_.get()->Flush();
                    if (destroySwapchain_(projectionSwapchain_) != XR_SUCCESS) return false;
                    projectionSwapchain_ = XR_NULL_HANDLE;
                    projectionImages_.clear();
                    projectionArraySize_ = needed;
                    if (!create_projection_swapchain()) return false;
                    std::fprintf(stderr, "Refract OpenXR: expanded compositor to %u slices for %u layers\n", needed, activeMixedCount_);
                }
            }
        }
        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult result = acquireSwapchainImage_(projectionSwapchain_, &acquireInfo, &imageIndex);
        if (result != XR_SUCCESS || imageIndex >= projectionImages_.size()) {
            return false;
        }

        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        result = waitSwapchainImage_(projectionSwapchain_, &waitInfo);
        if (result != XR_SUCCESS) {
            return false;
        }

        fill_projection_texture(projectionImages_[imageIndex].texture, imageIndex);

        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        result = releaseSwapchainImage_(projectionSwapchain_, &releaseInfo);
        if (result != XR_SUCCESS) {
            return false;
        }

        if (loadingByImage_[imageIndex]) {
            mixedProjection = false;
            quadCount = place_loading_card(displayTime, quadLayers[0]) ? 1 : 0;
            return quadCount != 0;
        }

        const auto& composition = uploadedProjectionByImage_[imageIndex];
        const bool mixedBatch = uploadedMixedCounts_[imageIndex] != 0;
        mixedProjection = mixedBatch && composition.view_count == 2;
        quadCount = mixedBatch ? uploadedMixedCounts_[imageIndex] : composition.quad_count();
        if (quadCount) {
            for (uint32_t i = 0; i < quadCount; ++i) {
                const auto& source = mixedBatch ? uploadedMixedQuads_[imageIndex][i] : composition.quads[i];
                auto& quad = quadLayers[i];
                quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                quad.space = localSpace_;
                quad.layerFlags = source.layer_flags;
                quad.eyeVisibility = static_cast<XrEyeVisibility>(source.eye_visibility);
                quad.pose.position = {source.pose.x, source.pose.y, source.pose.z};
                quad.pose.orientation = {source.pose.qx, source.pose.qy, source.pose.qz, source.pose.qw};
                quad.size = {source.width, source.height};
                quad.subImage.swapchain = projectionSwapchain_;
                quad.subImage.imageArrayIndex = mixedBatch ? i + (mixedProjection ? 2 : 0) : i;
                quad.subImage.imageRect.extent = mixedBatch ? uploadedMixedExtents_[imageIndex][i] : uploadedExtentByImage_[imageIndex];
            }
            if (!mixedProjection) return true;
        }

        XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locateInfo.displayTime = displayTime;
        locateInfo.space = localSpace_;

        XrViewState viewState{XR_TYPE_VIEW_STATE};
        std::array<XrView, 2> views{XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
        uint32_t viewCount = 0;
        result = locateViews_(session_, &locateInfo, &viewState, static_cast<uint32_t>(views.size()), &viewCount, views.data());
        if (result != XR_SUCCESS || viewCount < 2) {
            return false;
        }

        for (uint32_t i = 0; i < 2; ++i) {
            projectionViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            projectionViews[i].pose = views[i].pose;
            projectionViews[i].fov.angleLeft = -kAppProjectionHalfFovRadians;
            projectionViews[i].fov.angleRight = kAppProjectionHalfFovRadians;
            projectionViews[i].fov.angleUp = kAppProjectionHalfFovRadians;
            projectionViews[i].fov.angleDown = -kAppProjectionHalfFovRadians;
            // Metadata must describe the camera that rendered this exact
            // texture, not the newest tracking pose sampled after rendering.
            const auto& projection = uploadedProjectionByImage_[imageIndex];
            if (projection.view_count == 2) {
                const auto& eye = projection.views[i];
                projectionViews[i].pose.position = {eye.pose.x, eye.pose.y, eye.pose.z};
                projectionViews[i].pose.orientation = {eye.pose.qx, eye.pose.qy, eye.pose.qz, eye.pose.qw};
                projectionViews[i].fov = {eye.angle_left, eye.angle_right, eye.angle_up, eye.angle_down};
            }
            projectionViews[i].subImage.swapchain = projectionSwapchain_;
            projectionViews[i].subImage.imageRect.offset = {0, 0};
            projectionViews[i].subImage.imageRect.extent = {
                static_cast<int32_t>(projectionWidth_),
                static_cast<int32_t>(projectionHeight_),
            };
            projectionViews[i].subImage.imageArrayIndex = i;
            if (projection.view_count == 2) {
                projectionViews[i].subImage.imageRect.extent = uploadedExtentByImage_[imageIndex];
            }
        }
        if (uploadedProjectionByImage_[imageIndex].view_count == 2)
            measure_pose_latency(uploadedProjectionByImage_[imageIndex], uploadedAndroidSequenceByImage_[imageIndex], displayTime);

        projectionLayer.space = localSpace_;
        projectionLayer.layerFlags = uploadedProjectionByImage_[imageIndex].layer_flags;
        projectionLayer.viewCount = 2;
        projectionLayer.views = projectionViews.data();
        if (!reportedProjectionSubmit_) {
            std::fprintf(stderr, "Refract OpenXR: submitting projection layer to runtime\n");
            reportedProjectionSubmit_ = true;
        }
        return true;
    }

    // Finds the published head pose a newly shown image was rendered with (the runtime's eyes are that
    // pose +/- half the IPD, so their midpoint is exact) and measures how long after that pose's display
    // time the image is shown: the delay Refract's pipeline adds on top of the runtime's own. Diagnostic
    // only. Frames rendered while the head is still match several poses and are skipped.
    void measure_pose_latency(const refract::protocol::ImageProjection& projection, uint64_t imageSequence, XrTime displayTime)
    {
        if (imageSequence == UINT64_MAX || imageSequence == lastLatencyImage_) return;
        lastLatencyImage_ = imageSequence;
        const auto& l = projection.views[0].pose;
        const auto& r = projection.views[1].pose;
        const float x = (l.x + r.x) * 0.5f, y = (l.y + r.y) * 0.5f, z = (l.z + r.z) * 0.5f;
        auto distance = [&](const refract::protocol::Pose& p) {
            const float dot = std::fabs(p.qx * l.qx + p.qy * l.qy + p.qz * l.qz + p.qw * l.qw);
            return std::sqrt((p.x - x) * (p.x - x) + (p.y - y) * (p.y - y) + (p.z - z) * (p.z - z)) +
                   0.1f * std::sqrt((std::max)(0.0f, 1.0f - dot * dot));  // 0.1 m per radian (~1.7 mm/degree).
        };
        const size_t count = (std::min<size_t>)(publishedPoseCount_, publishedPoses_.size());
        float best = 1e9f, second = 1e9f;
        XrTime bestTime = 0;
        for (size_t i = 0; i < count; ++i) {
            const float d = distance(publishedPoses_[i].hmd);
            if (d < best) { second = best; best = d; bestTime = publishedPoses_[i].displayTime; }
            else if (d < second) { second = d; }
        }
        if (best > 2e-5f || second < 1e-4f || displayTime < bestTime) return;
        latencySamples_.push_back(displayTime - bestTime);
        if (latencySamples_.size() < 60) return;
        std::sort(latencySamples_.begin(), latencySamples_.end());
        const XrDuration median = latencySamples_[latencySamples_.size() / 2];
        const XrDuration p90 = latencySamples_[latencySamples_.size() * 9 / 10];
        latencySamples_.clear();
        std::fprintf(stderr, "Refract Latency: rendered pose shown %.1f ms after its frame's display time (p90 %.1f ms)\n",
            median / 1e6, p90 / 1e6);
    }

    void fill_projection_texture(ID3D11Texture2D* texture, uint32_t imageIndex)
    {
        const auto lockStart = std::chrono::steady_clock::now();
        std::lock_guard lock(gpuMutex_);
        static refract::protocol::PerfStats lockStats("host-projection-lock");
        lockStats.record(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lockStart).count());
        if (texture == nullptr || d3dContext_.get() == nullptr) {
            return;
        }
        if (upload_android_frame(texture, imageIndex)) {
            loadingByImage_[imageIndex] = 0;
            loadingCardByImage_[imageIndex] = 0;
            loadingActive_ = false;
            return;
        }
        draw_loading_card(texture, imageIndex);
    }

    // Until the game's first frame arrives, slice 0 holds the loading card instead of a stereo image.
    void draw_loading_card(ID3D11Texture2D* texture, uint32_t imageIndex)
    {
        const auto now = std::chrono::steady_clock::now();
        if (!loadingActive_) {
            loadingActive_ = true;
            loadingStart_ = now;
            loadingAnchored_ = false;
        }
        if (!loading_.prepared()) {
            const uint32_t width = (std::min)(1280u, projectionWidth_);
            loading_.prepare(width, (std::min)(width * 5 / 8, projectionHeight_));
        }
        loading_.animate(std::chrono::duration<double>(now - loadingStart_).count());

        // Each swapchain image needs the whole card once; after that only the bar moves.
        const bool whole = loading_.fading() || !loadingCardByImage_[imageIndex];
        const auto bar = loading_.bar_rect();
        const D3D11_BOX box = whole ? D3D11_BOX{0, 0, 0, loading_.width(), loading_.height(), 1}
                                    : D3D11_BOX{bar.left, bar.top, 0, bar.right, bar.bottom, 1};
        const uint8_t* source = loading_.pixels() + static_cast<size_t>(box.top) * loading_.stride() + box.left * 4;
        d3dContext_.get()->UpdateSubresource(texture, 0, &box, source, loading_.stride(), 0);
        loadingCardByImage_[imageIndex] = !loading_.fading();

        // The image no longer holds an Android frame, so the next one must upload again.
        loadingByImage_[imageIndex] = 1;
        uploadedAndroidSequenceByImage_[imageIndex] = UINT64_MAX;
        uploadedGpuSessionByImage_[imageIndex] = 0;
        uploadedMixedCounts_[imageIndex] = 0;
        uploadedProjectionByImage_[imageIndex] = {};
        uploadedExtentByImage_[imageIndex] = {static_cast<int32_t>(projectionWidth_), static_cast<int32_t>(projectionHeight_)};
        mirror_.present(d3dContext_.get(), texture, loading_.width(), loading_.height(),
            static_cast<DXGI_FORMAT>(projectionFormat_), kLoadingMirrorSequence | ++loadingMirrorFrame_);
    }

    // Floats the card in front of the user at eye level. It stays put while they look around it
    // and glides back in front once they turn well away, like a system loading panel.
    bool place_loading_card(XrTime displayTime, XrCompositionLayerQuad& quad)
    {
        constexpr float kDistance = 1.8f, kWidth = 1.2f;
        constexpr float kFollowAngle = 0.55f, kSettledAngle = 0.02f;  // Radians.
        constexpr float kTwoPi = 6.28318531f;
        constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        if (locateSpace_(viewSpace_, localSpace_, displayTime, &head) == XR_SUCCESS && (head.locationFlags & kValid) == kValid) {
            const auto& q = head.pose.orientation;
            const float forwardX = -2.0f * (q.x * q.z + q.w * q.y);
            const float forwardZ = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
            const bool level = forwardX * forwardX + forwardZ * forwardZ > 0.04f;  // Not looking straight up or down.
            const float yaw = level ? std::atan2(-forwardX, -forwardZ) : loadingYaw_;
            const float dt = loadingAnchored_ ? std::clamp(static_cast<float>(displayTime - loadingTime_) * 1e-9f, 0.0f, 0.1f) : 0.0f;
            loadingTime_ = displayTime;
            if (!loadingAnchored_) {
                loadingAnchored_ = level;
                loadingFollowing_ = false;
                loadingYaw_ = yaw;
                loadingHead_ = head.pose.position;
            }
            const float turn = std::remainder(yaw - loadingYaw_, kTwoPi);
            if (std::fabs(turn) > kFollowAngle) loadingFollowing_ = true;
            if (loadingFollowing_) {
                loadingYaw_ += turn * (1.0f - std::exp(-4.0f * dt));
                if (std::fabs(turn) < kSettledAngle) loadingFollowing_ = false;
            }
            const float glide = 1.0f - std::exp(-3.0f * dt);
            loadingHead_.x += (head.pose.position.x - loadingHead_.x) * glide;
            loadingHead_.y += (head.pose.position.y - loadingHead_.y) * glide;
            loadingHead_.z += (head.pose.position.z - loadingHead_.z) * glide;
        }
        if (!loadingAnchored_) return false;

        quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        quad.space = localSpace_;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.pose.position = {loadingHead_.x - std::sin(loadingYaw_) * kDistance, loadingHead_.y,
                              loadingHead_.z - std::cos(loadingYaw_) * kDistance};
        quad.pose.orientation = {0.0f, std::sin(loadingYaw_ * 0.5f), 0.0f, std::cos(loadingYaw_ * 0.5f)};
        quad.size = {kWidth, kWidth * loading_.height() / loading_.width()};
        quad.subImage.swapchain = projectionSwapchain_;
        quad.subImage.imageArrayIndex = 0;
        quad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(loading_.width()), static_cast<int32_t>(loading_.height())}};
        return true;
    }

    bool upload_android_frame(ID3D11Texture2D* texture, uint32_t imageIndex)
    {
        if (imageFrame_ == nullptr) {
            return false;
        }
        if (imageIndex >= uploadedAndroidSequenceByImage_.size()) {
            return false;
        }

        refract::protocol::ImageFrameHeader header{};
        refract::protocol::ImageProjection projection{};
        std::shared_ptr<const std::vector<uint8_t>> pixels;
        if (!imageFrame_->snapshot(&header, &pixels, &projection)) {
            return false;
        }
        if (header.width == 0 || header.height == 0 || header.layers == 0 || pixels == nullptr || pixels->empty()) {
            return false;
        }
        if (refract::protocol::mixed_gpu_version(header.version)) {
            if (activeMixedCount_ < 2 || activeMixedCount_ > refract::protocol::kMaxCompositionLayers ||
                activeMixedCount_ + (projection.view_count == 2 ? 1u : 0u) > projectionArraySize_) return false;
            if (uploadedAndroidSequenceByImage_[imageIndex] != header.sequence || uploadedMixedTimes_[imageIndex] != header.monotonic_time_ns || !uploadedMixedCounts_[imageIndex]) {
                for (uint32_t i = 0; i < activeMixedCount_; ++i) {
                    auto& part = mixedFrames_[activeMixedBank_][i];
                    const bool scene = projection.view_count == 2;
                    const uint32_t slice = scene && i ? i + 1 : i;
                    if (!part.receiver.copy_to(d3dContext_.get(), texture, slice, scene && i == 0 ? 2 : 1)) return false;
                    const uint32_t firstQuad = projection.view_count == 2 ? 1 : 0;
                    if (i >= firstQuad) {
                        uploadedMixedQuads_[imageIndex][i - firstQuad] = part.projection.quads[0];
                        uploadedMixedExtents_[imageIndex][i - firstQuad] = {static_cast<int32_t>(part.header.width), static_cast<int32_t>(part.header.height)};
                    }
                }
                uploadedProjectionByImage_[imageIndex] = projection;
                uploadedExtentByImage_[imageIndex] = {static_cast<int32_t>(header.width), static_cast<int32_t>(header.height)};
                uploadedAndroidSequenceByImage_[imageIndex] = header.sequence;
                uploadedGpuSessionByImage_[imageIndex] = 0;
                uploadedMixedCounts_[imageIndex] = activeMixedCount_ - (projection.view_count == 2 ? 1 : 0);
                uploadedMixedTimes_[imageIndex] = header.monotonic_time_ns;
            }
            mirror_.present(d3dContext_.get(), texture, header.width, header.height, static_cast<DXGI_FORMAT>(projectionFormat_), header.sequence);
            debug_capture_frame(d3dContext_.get(), texture, header.width, header.height, header.sequence);
            return true;
        }
        uploadedMixedCounts_[imageIndex] = 0;
        if ((header.version == refract::protocol::kWindowsGpuFrameVersion || header.version == refract::protocol::kQuadGpuFrameVersion)) {
            if (pixels->size() != sizeof(refract::protocol::WindowsGpuFrame)) return false;
            refract::protocol::WindowsGpuFrame gpu{};
            std::memcpy(&gpu, pixels->data(), sizeof(gpu));
            if (uploadedGpuSessionByImage_[imageIndex] == gpu.session &&
                uploadedAndroidSequenceByImage_[imageIndex] == header.sequence &&
                uploadedExtentByImage_[imageIndex].width == static_cast<int32_t>(header.width) &&
                uploadedExtentByImage_[imageIndex].height == static_cast<int32_t>(header.height)) {
                mirror_.present(d3dContext_.get(), texture, header.width, header.height,
                    static_cast<DXGI_FORMAT>(projectionFormat_), header.sequence);
                return true;
            }
            if (!gpuReceiver_.copy_to(d3dContext_.get(), texture)) return false;
            uploadedGpuSessionByImage_[imageIndex] = gpu.session;
            uploadedAndroidSequenceByImage_[imageIndex] = header.sequence;
            uploadedProjectionByImage_[imageIndex] = projection;
            uploadedExtentByImage_[imageIndex] = {static_cast<int32_t>(header.width), static_cast<int32_t>(header.height)};
            mirror_.present(d3dContext_.get(), texture, header.width, header.height, static_cast<DXGI_FORMAT>(projectionFormat_), header.sequence);
            if (projection.view_count == 2) debug_capture_frame(d3dContext_.get(), texture, header.width, header.height, header.sequence);
            if (!reportedGpuImage_) { std::fprintf(stderr, "Refract GPU: shared Windows textures active; no pixel TCP transfer\n"); reportedGpuImage_ = true; }
            return true;
        }
        if (header.sequence == uploadedAndroidSequenceByImage_[imageIndex]) {
            mirror_.present(d3dContext_.get(), texture, uploadedExtentByImage_[imageIndex].width,
                uploadedExtentByImage_[imageIndex].height, static_cast<DXGI_FORMAT>(projectionFormat_), header.sequence);
            return true;
        }
        uploadedGpuSessionByImage_[imageIndex] = 0;
        if (projection.view_count == 2 && (header.width > projectionWidth_ || header.height > projectionHeight_)) {
            return false; // Cropping here would change the angular scale.
        }

        const uint32_t copyWidth = header.width < projectionWidth_ ? header.width : projectionWidth_;
        const uint32_t copyHeight = header.height < projectionHeight_ ? header.height : projectionHeight_;
        const uint32_t sourceStride = header.width * header.bytes_per_pixel;
        const bool needsBgra =
            projectionFormat_ == DXGI_FORMAT_B8G8R8A8_UNORM ||
            projectionFormat_ == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

        std::vector<uint8_t> uploadBuffer(static_cast<size_t>(header.width) * header.height * header.layers * 4);
        for (uint32_t layer = 0; layer < header.layers; ++layer) {
            const uint64_t layerOffset = static_cast<uint64_t>(layer) * header.width * header.height * header.bytes_per_pixel;
            for (uint32_t y = 0; y < header.height; ++y) {
                const uint32_t sourceY = header.height - 1 - y;
                const uint64_t sourceRow = layerOffset + static_cast<uint64_t>(sourceY) * sourceStride;
                const uint64_t destRow =
                    (static_cast<uint64_t>(layer) * header.width * header.height + static_cast<uint64_t>(y) * header.width) * 4;
                for (uint32_t x = 0; x < header.width; ++x) {
                    const uint64_t source = sourceRow + static_cast<uint64_t>(x) * header.bytes_per_pixel;
                    const uint64_t dest = destRow + static_cast<uint64_t>(x) * 4;
                    if (needsBgra) {
                        uploadBuffer[dest + 0] = (*pixels)[source + 2];
                        uploadBuffer[dest + 1] = (*pixels)[source + 1];
                        uploadBuffer[dest + 2] = (*pixels)[source + 0];
                        uploadBuffer[dest + 3] = (*pixels)[source + 3];
                    } else {
                        uploadBuffer[dest + 0] = (*pixels)[source + 0];
                        uploadBuffer[dest + 1] = (*pixels)[source + 1];
                        uploadBuffer[dest + 2] = (*pixels)[source + 2];
                        uploadBuffer[dest + 3] = (*pixels)[source + 3];
                    }
                }
            }
        }

        const uint8_t* uploadPixels = uploadBuffer.data();
        const uint32_t uploadStride = header.width * 4;

        const uint32_t sourceLayers = header.layers;
        for (uint32_t targetLayer = 0; targetLayer < 2; ++targetLayer) {
            const uint32_t sourceLayer = sourceLayers > 1 ? targetLayer % sourceLayers : 0;
            const uint64_t sourceOffset =
                static_cast<uint64_t>(sourceLayer) * header.width * header.height * 4;
            if (sourceOffset >= uploadBuffer.size()) {
                continue;
            }

            D3D11_BOX box{};
            box.left = 0;
            box.top = 0;
            box.front = 0;
            box.right = copyWidth;
            box.bottom = copyHeight;
            box.back = 1;
            d3dContext_.get()->UpdateSubresource(
                texture,
                targetLayer,
                &box,
                uploadPixels + sourceOffset,
                uploadStride,
                uploadStride * header.height);
        }

        mirror_.present(d3dContext_.get(), texture, copyWidth, copyHeight, static_cast<DXGI_FORMAT>(projectionFormat_), header.sequence);
        if (!reportedAndroidImageSubmit_) {
            std::fprintf(
                stderr,
                "Refract OpenXR: submitting Android image frames to SteamVR (%ux%u layers=%u)\n",
                header.width,
                header.height,
                header.layers);
            reportedAndroidImageSubmit_ = true;
        }
        uploadedAndroidSequenceByImage_[imageIndex] = header.sequence;
        uploadedProjectionByImage_[imageIndex] = projection;
        uploadedExtentByImage_[imageIndex] = {static_cast<int32_t>(copyWidth), static_cast<int32_t>(copyHeight)};
        if (projection.view_count == 2 && !reportedStereoProjection_) {
            const auto& l = projection.views[0];
            const auto& r = projection.views[1];
            const float dx = r.pose.x-l.pose.x, dy = r.pose.y-l.pose.y, dz = r.pose.z-l.pose.z;
            std::fprintf(stderr, "Refract OpenXR: stereo render metadata active; camera separation=%.1fmm left FOV=(%.3f %.3f %.3f %.3f)\n",
                         std::sqrt(dx*dx+dy*dy+dz*dz)*1000, l.angle_left, l.angle_right, l.angle_up, l.angle_down);
            reportedStereoProjection_ = true;
        }
        return true;
    }
#endif

    bool create_reference_space(XrReferenceSpaceType type, XrSpace* space)
    {
        XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        spaceInfo.referenceSpaceType = type;
        spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
        const XrResult result = createReferenceSpace_(session_, &spaceInfo, space);
        if (result != XR_SUCCESS) {
            std::fprintf(stderr, "Refract OpenXR: xrCreateReferenceSpace failed: %s (%d)\n", xr_result_name(result), result);
            return false;
        }
        return true;
    }

    bool string_to_path(const char* text, XrPath* path)
    {
        const XrResult result = stringToPath_(instance_, text, path);
        if (result != XR_SUCCESS) {
            std::fprintf(stderr, "Refract OpenXR: xrStringToPath(%s) failed: %s (%d)\n", text, xr_result_name(result), result);
            return false;
        }
        return true;
    }

    enum InputAction { Trigger, Squeeze, Stick, Primary, Secondary, Menu, StickPress,
                       PrimaryContact, SecondaryContact, TriggerContact, StickContact, ThumbrestContact, InputCount };

    void initialize_hand_tracking() {
        if (!handTrackingEnabled_ || !load_func("xrCreateHandTrackerEXT", &createHandTracker_) ||
            !load_func("xrDestroyHandTrackerEXT", &destroyHandTracker_) ||
            !load_func("xrLocateHandJointsEXT", &locateHandJoints_)) return;
        for (size_t hand = 0; hand < 2; ++hand) {
            XrHandTrackingDataSourceEXT sources[] = {XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT, XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT};
            XrHandTrackingDataSourceInfoEXT sourceInfo{XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT};
            sourceInfo.requestedDataSourceCount = 2; sourceInfo.requestedDataSources = sources;
            XrHandTrackerCreateInfoEXT info{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
            info.hand = hand == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
            info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
            if (handDataSourceEnabled_) info.next = &sourceInfo;
            auto result = createHandTracker_(session_, &info, &handTrackers_[hand]);
            std::fprintf(stderr, "Refract Hands: create hand=%zu result=%d\n", hand, result);
        }
    }

    void locate_hand_joints(refract::protocol::PoseFrame& frame, XrTime time, uint64_t sequence) {
        frame.hands[0] = {}; frame.hands[1] = {};
        frame.hand_tracking_supported = handTrackers_[0] != XR_NULL_HANDLE && handTrackers_[1] != XR_NULL_HANDLE;
        for (size_t hand = 0; hand < 2; ++hand) {
            if (!handTrackers_[hand]) continue;
            XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT]{};
            XrHandTrackingDataSourceStateEXT source{XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT};
            XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
            locations.jointCount = XR_HAND_JOINT_COUNT_EXT; locations.jointLocations = joints;
            if (handDataSourceEnabled_) locations.next = &source;
            XrHandJointsLocateInfoEXT info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
            info.baseSpace = localSpace_; info.time = time;
            const auto result = locateHandJoints_(handTrackers_[hand], &info, &locations);
            auto& target = frame.hands[hand];
            if (result == XR_SUCCESS && locations.isActive) {
                target.active = 1;
                target.source = source.isActive ? static_cast<uint32_t>(source.dataSource) : 0;
                for (size_t joint = 0; joint < XR_HAND_JOINT_COUNT_EXT; ++joint) {
                    target.joints[joint] = {joints[joint].locationFlags, to_protocol_pose(joints[joint].pose), joints[joint].radius};
                }
            }
            if (sequence % 360 == 0) std::fprintf(stderr, "Refract Hands: hand=%zu active=%u source=%u result=%d\n", hand, target.active, target.source, result);
        }
    }

    void initialize_controller_actions()
    {
        if (!string_to_path("/user/hand/left", &handSubactionPaths_[0]) ||
            !string_to_path("/user/hand/right", &handSubactionPaths_[1])) {
            return;
        }

        XrActionSetCreateInfo actionSetInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strncpy(actionSetInfo.actionSetName, "refract_gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
        std::strncpy(actionSetInfo.localizedActionSetName, "Refract gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
        actionSetInfo.priority = 0;
        XrResult result = createActionSet_(instance_, &actionSetInfo, &actionSet_);
        if (result != XR_SUCCESS) {
            std::fprintf(stderr, "Refract OpenXR: xrCreateActionSet failed: %s (%d)\n", xr_result_name(result), result);
            actionSet_ = XR_NULL_HANDLE;
            return;
        }

        XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
        actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
        std::strncpy(actionInfo.actionName, "hand_pose", XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(actionInfo.localizedActionName, "Hand pose", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        actionInfo.countSubactionPaths = static_cast<uint32_t>(handSubactionPaths_.size());
        actionInfo.subactionPaths = handSubactionPaths_.data();
        result = createAction_(actionSet_, &actionInfo, &handPoseAction_);
        if (result != XR_SUCCESS) {
            std::fprintf(stderr, "Refract OpenXR: xrCreateAction(hand_pose) failed: %s (%d)\n", xr_result_name(result), result);
            return;
        }

        std::strncpy(actionInfo.actionName, "aim_pose", XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(actionInfo.localizedActionName, "Aim pose", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        if (createAction_(actionSet_, &actionInfo, &aimPoseAction_) != XR_SUCCESS) return;

        constexpr const char* inputNames[] = {"trigger", "squeeze", "stick", "primary", "secondary", "menu",
            "stick_press", "primary_touch", "secondary_touch", "trigger_touch", "stick_touch", "thumbrest_touch"};
        for (size_t i = 0; i < InputCount; ++i) {
            actionInfo.actionType = i <= Squeeze ? XR_ACTION_TYPE_FLOAT_INPUT :
                i == Stick ? XR_ACTION_TYPE_VECTOR2F_INPUT : XR_ACTION_TYPE_BOOLEAN_INPUT;
            std::snprintf(actionInfo.actionName, sizeof(actionInfo.actionName), "%s", inputNames[i]);
            std::snprintf(actionInfo.localizedActionName, sizeof(actionInfo.localizedActionName), "%s", inputNames[i]);
            if (createAction_(actionSet_, &actionInfo, &inputActions_[i]) != XR_SUCCESS) {
                std::fprintf(stderr, "Refract OpenXR: cannot create input action %s\n", inputNames[i]);
                return;
            }
        }

        suggest_pose_bindings("/interaction_profiles/oculus/touch_controller", "/input/grip/pose");
        suggest_pose_bindings("/interaction_profiles/valve/index_controller", "/input/grip/pose");
        suggest_pose_bindings("/interaction_profiles/htc/vive_controller", "/input/grip/pose");

        XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attachInfo.countActionSets = 1;
        attachInfo.actionSets = &actionSet_;
        result = attachSessionActionSets_(session_, &attachInfo);
        if (result != XR_SUCCESS) {
            std::fprintf(stderr, "Refract OpenXR: xrAttachSessionActionSets failed: %s (%d)\n", xr_result_name(result), result);
            return;
        }

        for (size_t i = 0; i < handSpaces_.size(); ++i) {
            XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            spaceInfo.action = handPoseAction_;
            spaceInfo.subactionPath = handSubactionPaths_[i];
            spaceInfo.poseInActionSpace.orientation.w = 1.0f;
            result = createActionSpace_(session_, &spaceInfo, &handSpaces_[i]);
            if (result != XR_SUCCESS) {
                std::fprintf(
                    stderr,
                    "Refract OpenXR: xrCreateActionSpace hand %zu failed: %s (%d)\n",
                    i,
                    xr_result_name(result),
                    result);
                handSpaces_[i] = XR_NULL_HANDLE;
            }
        }

        for (size_t i = 0; i < aimSpaces_.size(); ++i) {
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            info.action = aimPoseAction_; info.subactionPath = handSubactionPaths_[i];
            info.poseInActionSpace.orientation.w = 1;
            if (createActionSpace_(session_, &info, &aimSpaces_[i]) != XR_SUCCESS) return;
        }

        controllerActionsReady_ = handSpaces_[0] != XR_NULL_HANDLE || handSpaces_[1] != XR_NULL_HANDLE;
        if (controllerActionsReady_) {
            std::fprintf(stderr, "Refract OpenXR: controller pose actions ready\n");
        }
    }

    void suggest_pose_bindings(const char* interactionProfilePath, const char* poseInputSuffix)
    {
        if (handPoseAction_ == XR_NULL_HANDLE) {
            return;
        }

        XrPath profilePath = XR_NULL_PATH;
        if (!string_to_path(interactionProfilePath, &profilePath)) {
            return;
        }

        char leftBindingText[128]{};
        char rightBindingText[128]{};
        std::snprintf(leftBindingText, sizeof(leftBindingText), "/user/hand/left%s", poseInputSuffix);
        std::snprintf(rightBindingText, sizeof(rightBindingText), "/user/hand/right%s", poseInputSuffix);

        std::vector<XrActionSuggestedBinding> bindings(2);
        if (!string_to_path(leftBindingText, &bindings[0].binding) ||
            !string_to_path(rightBindingText, &bindings[1].binding)) {
            return;
        }
        bindings[0].action = handPoseAction_;
        bindings[1].action = handPoseAction_;

        for (const char* hand : {"left", "right"}) {
            XrPath path = XR_NULL_PATH;
            const std::string name = std::string("/user/hand/") + hand + "/input/aim/pose";
            if (string_to_path(name.c_str(), &path)) bindings.push_back({aimPoseAction_, path});
        }
        const bool touch = std::strstr(interactionProfilePath, "oculus") != nullptr;
        const bool vive = std::strstr(interactionProfilePath, "vive") != nullptr;
        for (size_t hand = 0; hand < 2; ++hand) {
            const char* primary = touch && hand == 0 ? "x" : "a";
            const char* secondary = touch && hand == 0 ? "y" : "b";
            std::array<std::string, InputCount> components{
                "trigger/value", vive ? "squeeze/click" : "squeeze/value",
                vive ? "trackpad" : "thumbstick",
                vive ? "" : std::string(primary) + "/click",
                vive ? "" : std::string(secondary) + "/click",
                vive || (touch && hand == 0) ? "menu/click" : "",
                vive ? "trackpad/click" : "thumbstick/click",
                vive ? "" : std::string(primary) + "/touch",
                vive ? "" : std::string(secondary) + "/touch",
                vive ? "" : "trigger/touch",
                vive ? "trackpad/touch" : "thumbstick/touch",
                touch ? "thumbrest/touch" : ""};
            for (size_t i = 0; i < InputCount; ++i) {
                if (components[i].empty()) continue;
                const std::string path = std::string(hand == 0 ? "/user/hand/left/input/" : "/user/hand/right/input/") + components[i];
                XrPath binding = XR_NULL_PATH;
                if (string_to_path(path.c_str(), &binding)) bindings.push_back({inputActions_[i], binding});
            }
        }

        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = profilePath;
        suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        suggested.suggestedBindings = bindings.data();
        const XrResult result = suggestInteractionProfileBindings_(instance_, &suggested);
        if (result != XR_SUCCESS) {
            std::fprintf(
                stderr,
                "Refract OpenXR: bindings for %s rejected: %s (%d)\n",
                interactionProfilePath,
                xr_result_name(result),
                result);
        }
    }

    void locate_controller_spaces(refract::protocol::PoseFrame& frame, XrTime locateTime, uint64_t sequence)
    {
        if (!controllerActionsReady_) {
            return;
        }

        XrActiveActionSet activeActionSet{};
        activeActionSet.actionSet = actionSet_;
        activeActionSet.subactionPath = XR_NULL_PATH;
        XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &activeActionSet;
        XrResult result = syncActions_(session_, &syncInfo);
        if (result == XR_SESSION_NOT_FOCUSED) {
            menuShortcut_.reset();
            return;
        }
        if (result != XR_SUCCESS) {
            menuShortcut_.reset();
            if (!reportedSyncFailure_) {
                std::fprintf(stderr, "Refract OpenXR: xrSyncActions failed: %s (%d)\n", xr_result_name(result), result);
                reportedSyncFailure_ = true;
            }
            return;
        }

        bool locatedAny = false;
        for (size_t i = 0; i < handSpaces_.size(); ++i) {
            if (handSpaces_[i] == XR_NULL_HANDLE) {
                continue;
            }

            XrActionStateGetInfo stateInfo{XR_TYPE_ACTION_STATE_GET_INFO};
            stateInfo.action = aimPoseAction_;
            stateInfo.subactionPath = handSubactionPaths_[i];
            XrActionStatePose aimState{XR_TYPE_ACTION_STATE_POSE};
            if (getActionStatePose_(session_, &stateInfo, &aimState) == XR_SUCCESS && aimState.isActive) {
                frame.aim_active[i] = 1;
                XrSpaceLocation aimLocation{XR_TYPE_SPACE_LOCATION};
                XrSpaceVelocity aimVelocity{XR_TYPE_SPACE_VELOCITY};
                aimLocation.next = &aimVelocity;
                if (locateSpace_(aimSpaces_[i], localSpace_, locateTime, &aimLocation) == XR_SUCCESS) {
                    frame.aim_flags[i] = aimLocation.locationFlags;
                    frame.aim[i] = to_protocol_pose(aimLocation.pose);
                    frame.aim_velocity[i] = to_protocol_velocity(aimVelocity, aimLocation.locationFlags);
                }
            }
            stateInfo.action = handPoseAction_;
            stateInfo.subactionPath = handSubactionPaths_[i];
            XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
            result = getActionStatePose_(session_, &stateInfo, &state);
            if (result != XR_SUCCESS || state.isActive == XR_FALSE) {
                continue;
            }

            auto& input = frame.controllers[i];
            input.active = 1;
            constexpr uint32_t buttonBits[] = {refract::protocol::PrimaryClick, refract::protocol::SecondaryClick,
                refract::protocol::MenuClick, refract::protocol::StickClick, refract::protocol::PrimaryTouch,
                refract::protocol::SecondaryTouch, refract::protocol::TriggerTouch, refract::protocol::StickTouch,
                refract::protocol::ThumbrestTouch};
            for (size_t action = 0; action < InputCount; ++action) {
                stateInfo.action = inputActions_[action];
                if (action <= Squeeze) {
                    XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
                    if (getActionStateFloat_(session_, &stateInfo, &value) == XR_SUCCESS && value.isActive)
                        (action == Trigger ? input.trigger : input.squeeze) = value.currentState;
                } else if (action == Stick) {
                    XrActionStateVector2f value{XR_TYPE_ACTION_STATE_VECTOR2F};
                    if (getActionStateVector2f_(session_, &stateInfo, &value) == XR_SUCCESS && value.isActive) {
                        input.stick_x = value.currentState.x; input.stick_y = value.currentState.y;
                    }
                } else {
                    XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
                    if (getActionStateBoolean_(session_, &stateInfo, &value) == XR_SUCCESS && value.isActive && value.currentState)
                        input.buttons |= buttonBits[action - Primary];
                }
            }

            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            XrSpaceVelocity gripVelocity{XR_TYPE_SPACE_VELOCITY};
            location.next = &gripVelocity;
            result = locateSpace_(handSpaces_[i], localSpace_, locateTime, &location);
            if (result == XR_SUCCESS) frame.grip_flags[i] = location.locationFlags;
            if (result != XR_SUCCESS ||
                (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0 ||
                (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
                continue;
            }

            refract::protocol::Pose& target = (i == 0) ? frame.left_controller : frame.right_controller;
            target = to_protocol_pose(location.pose);
            frame.grip_velocity[i] = to_protocol_velocity(gripVelocity, location.locationFlags);
            locatedAny = true;
        }

        menuShortcut_.apply(frame.controllers, static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count()));

        if (locatedAny) {
            if (sequence % 90 == 0) {
                std::fprintf(
                    stderr,
                    "Refract OpenXR: controllers seq=%llu left=(%.3f %.3f %.3f) right=(%.3f %.3f %.3f)\n",
                    static_cast<unsigned long long>(sequence),
                    frame.left_controller.x,
                    frame.left_controller.y,
                    frame.left_controller.z,
                    frame.right_controller.x,
                    frame.right_controller.y,
                    frame.right_controller.z);
            }
        }
    }

    void pump_events()
    {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (pollEvent_(instance_, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto* stateEvent = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                handle_session_state(stateEvent->state);
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    void handle_session_state(XrSessionState state)
    {
        std::fprintf(stderr, "Refract OpenXR: session state=%d\n", static_cast<int>(state));
        if (state == XR_SESSION_STATE_READY && !sessionRunning_) {
            XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
            beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            const XrResult result = beginSession_(session_, &beginInfo);
            if (result == XR_SUCCESS) {
                sessionRunning_ = true;
                std::fprintf(stderr, "Refract OpenXR: host session running\n");
            } else {
                std::fprintf(stderr, "Refract OpenXR: xrBeginSession failed: %s (%d)\n", xr_result_name(result), result);
            }
        } else if (state == XR_SESSION_STATE_STOPPING && sessionRunning_) {
            endSession_(session_);
            sessionRunning_ = false;
            std::fprintf(stderr, "Refract OpenXR: host session stopped\n");
        } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
            sessionRunning_ = false;
        }
    }

    OpenXrLoader loader_;
    HostImageFrame* imageFrame_ = nullptr;
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace localSpace_ = XR_NULL_HANDLE;
    XrSpace appLocalSpace_ = XR_NULL_HANDLE;
    XrReferenceSpaceType trackingSpaceType_ = XR_REFERENCE_SPACE_TYPE_LOCAL;
    bool reportedLocalOrigin_ = false;
    bool localOriginInitialized_ = false;
    XrSpace viewSpace_ = XR_NULL_HANDLE;
    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction handPoseAction_ = XR_NULL_HANDLE;
    XrAction aimPoseAction_ = XR_NULL_HANDLE;
    bool handTrackingEnabled_ = false, handDataSourceEnabled_ = false;
    PFN_xrCreateHandTrackerEXT createHandTracker_ = nullptr;
    PFN_xrDestroyHandTrackerEXT destroyHandTracker_ = nullptr;
    PFN_xrLocateHandJointsEXT locateHandJoints_ = nullptr;
    std::array<XrHandTrackerEXT, 2> handTrackers_{};
    std::array<XrAction, InputCount> inputActions_{};
    std::array<XrPath, 2> handSubactionPaths_{XR_NULL_PATH, XR_NULL_PATH};
    std::array<XrSpace, 2> handSpaces_{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<XrSpace, 2> aimSpaces_{XR_NULL_HANDLE, XR_NULL_HANDLE};
    bool sessionRunning_ = false;
    bool useFrameLoop_ = true;
    bool controllerActionsReady_ = false;
    MenuShortcut menuShortcut_;
    bool reportedSyncFailure_ = false;
    refract::protocol::PoseFrame latest_{};
    std::mutex frameMutex_;
    struct PublishedPose { refract::protocol::Pose hmd; XrTime displayTime = 0; };
    std::array<PublishedPose, 32> publishedPoses_{};
    uint64_t publishedPoseCount_ = 0;
    uint64_t lastLatencyImage_ = UINT64_MAX;
    std::vector<XrDuration> latencySamples_;
    static XrDuration read_fixed_prediction_lead() {
        const char* value = std::getenv("REFRACT_PREDICTION_LEAD_MS");
        return value && *value ? static_cast<XrDuration>(std::atof(value) * 1e6) : XrDuration(-1);
    }
    XrDuration predictionLead_ = (std::max<XrDuration>)(read_fixed_prediction_lead(), 0);
    const bool posePrediction_ = [] {
        const char* value = std::getenv("REFRACT_POSE_PREDICTION");
        return !value || std::strcmp(value, "0") != 0;
    }();

    PFN_xrDestroyInstance destroyInstance_ = nullptr;
    PFN_xrGetSystem getSystem_ = nullptr;
    PFN_xrEnumerateViewConfigurationViews enumerateViewConfigurationViews_ = nullptr;
    PFN_xrCreateSession createSession_ = nullptr;
    PFN_xrDestroySession destroySession_ = nullptr;
    PFN_xrCreateReferenceSpace createReferenceSpace_ = nullptr;
    PFN_xrDestroySpace destroySpace_ = nullptr;
    PFN_xrPollEvent pollEvent_ = nullptr;
    PFN_xrBeginSession beginSession_ = nullptr;
    PFN_xrEndSession endSession_ = nullptr;
    PFN_xrWaitFrame waitFrame_ = nullptr;
    PFN_xrBeginFrame beginFrame_ = nullptr;
    PFN_xrEndFrame endFrame_ = nullptr;
    PFN_xrLocateSpace locateSpace_ = nullptr;
    PFN_xrLocateViews locateViews_ = nullptr;
    PFN_xrStringToPath stringToPath_ = nullptr;
    PFN_xrCreateActionSet createActionSet_ = nullptr;
    PFN_xrDestroyActionSet destroyActionSet_ = nullptr;
    PFN_xrCreateAction createAction_ = nullptr;
    PFN_xrDestroyAction destroyAction_ = nullptr;
    PFN_xrSuggestInteractionProfileBindings suggestInteractionProfileBindings_ = nullptr;
    PFN_xrAttachSessionActionSets attachSessionActionSets_ = nullptr;
    PFN_xrCreateActionSpace createActionSpace_ = nullptr;
    PFN_xrSyncActions syncActions_ = nullptr;
    PFN_xrGetActionStatePose getActionStatePose_ = nullptr;
    PFN_xrGetActionStateBoolean getActionStateBoolean_ = nullptr;
    PFN_xrGetActionStateFloat getActionStateFloat_ = nullptr;
    PFN_xrGetActionStateVector2f getActionStateVector2f_ = nullptr;
#if defined(_WIN32)
    PFN_xrEnumerateSwapchainFormats enumerateSwapchainFormats_ = nullptr;
    PFN_xrCreateSwapchain createSwapchain_ = nullptr;
    PFN_xrDestroySwapchain destroySwapchain_ = nullptr;
    PFN_xrEnumerateSwapchainImages enumerateSwapchainImages_ = nullptr;
    PFN_xrAcquireSwapchainImage acquireSwapchainImage_ = nullptr;
    PFN_xrWaitSwapchainImage waitSwapchainImage_ = nullptr;
    PFN_xrReleaseSwapchainImage releaseSwapchainImage_ = nullptr;
    PFN_xrGetD3D11GraphicsRequirementsKHR getD3D11GraphicsRequirements_ = nullptr;
    std::mutex gpuMutex_;
    WindowsGpuReceiver gpuReceiver_;
    struct MixedFramePart { WindowsGpuReceiver receiver; refract::protocol::ImageFrameHeader header{}; refract::protocol::ImageProjection projection{}; };
    std::array<std::array<MixedFramePart, refract::protocol::kMaxCompositionLayers>, 2> mixedFrames_{};
    uint32_t activeMixedBank_ = 0, activeMixedCount_ = 0, pendingMixedBank_ = 1, pendingMixedCount_ = 0, pendingMixedIndex_ = 0;
    uint64_t pendingMixedSequence_ = 0;
    std::vector<std::array<refract::protocol::ImageQuad, refract::protocol::kMaxCompositionLayers>> uploadedMixedQuads_;
    std::vector<std::array<XrExtent2Di, refract::protocol::kMaxCompositionLayers>> uploadedMixedExtents_;
    std::vector<uint32_t> uploadedMixedCounts_;
    std::vector<uint64_t> uploadedMixedTimes_;
    ComPtr<ID3D11Device> receiveDevice_;
    ComPtr<ID3D11DeviceContext> receiveContext_;
    bool reportedGpuImage_ = false;
    MirrorWindow mirror_;
    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<ID3D11DeviceContext> d3dContext_;
    XrSwapchain projectionSwapchain_ = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> projectionImages_;
    std::vector<uint64_t> uploadedAndroidSequenceByImage_;
    std::vector<uint64_t> uploadedGpuSessionByImage_;
    std::vector<refract::protocol::ImageProjection> uploadedProjectionByImage_;
    std::vector<XrExtent2Di> uploadedExtentByImage_;
    bool reportedStereoProjection_ = false;
    int64_t projectionFormat_ = 0;
    uint32_t projectionArraySize_ = 2;
    uint32_t projectionWidth_ = refract::protocol::kTransportEyeDimension;
    uint32_t projectionHeight_ = refract::protocol::kTransportEyeDimension;
    // Mirror sequences for loading frames, kept apart from Android frame sequences.
    static constexpr uint64_t kLoadingMirrorSequence = 1ull << 62;
    LoadingScreen loading_;
    std::vector<uint8_t> loadingByImage_;      // The image holds the loading card...
    std::vector<uint8_t> loadingCardByImage_;  // ...all of it, at full brightness.
    std::chrono::steady_clock::time_point loadingStart_{};
    uint64_t loadingMirrorFrame_ = 0;
    XrTime loadingTime_ = 0;
    XrVector3f loadingHead_{};
    float loadingYaw_ = 0.0f;
    bool loadingActive_ = false, loadingAnchored_ = false, loadingFollowing_ = false;
    bool reportedProjectionSubmit_ = false;
    bool reportedAndroidImageSubmit_ = false;
#endif
#if !defined(_WIN32)
    PFN_xrConvertTimespecTimeToTimeKHR convertTimespecTimeToTime_ = nullptr;
#else
    bool win32TimeEnabled_ = false;
    PFN_xrConvertWin32PerformanceCounterToTimeKHR convertPerformanceCounter_ = nullptr;
#endif
};

} // namespace

int OpenXrHost::run(int argc, char** argv)
{
    if (argc <= 1 || std::string_view(argv[1]) == "--smoke") {
        std::fprintf(stderr, "Refract host bridge smoke OK\n");
        return 0;
    }

    const std::string_view mode(argv[1]);
    if (mode == "--serve-gpu-fds") {
        const char* socket_path = argc >= 3 ? argv[2] : "/tmp/refract-gpu-frame.sock";
        uint32_t frames = 0;
        if (argc >= 4 && !parse_u32(argv[3], &frames)) {
            std::fprintf(stderr, "Invalid frame count: %s\n", argv[3]);
            return 2;
        }

        refract::protocol::UnixFdFrameServer server;
        return server.serve(socket_path, frames, [](const refract::protocol::GpuFrameDescriptor& descriptor,
                                                    std::vector<refract::protocol::UniqueFd>&& fds) {
            std::fprintf(stderr,
                         "Refract GPU: frame seq=%llu %ux%u layers=%u drm_format=0x%llx modifier=0x%llx planes=%u fds=%zu\n",
                         static_cast<unsigned long long>(descriptor.sequence),
                         descriptor.width,
                         descriptor.height,
                         descriptor.layers,
                         static_cast<unsigned long long>(descriptor.drm_format),
                         static_cast<unsigned long long>(descriptor.drm_modifier),
                         descriptor.plane_count,
                         fds.size());
        });
    }

    if (mode == "--serve-images") {
        uint16_t port = 38491;
        uint32_t frames = 0;
        if (argc >= 3 && !parse_u16(argv[2], &port)) { return 2; }
        if (argc >= 4 && !parse_u32(argv[3], &frames)) { return 2; }
        refract::protocol::TcpImageServer receiver;
        return receiver.serve(port, frames);
    }

    if (mode == "--video-recv-udp") {
        uint16_t port = 38492;
        uint32_t frames = 0;
        if (argc >= 3 && !parse_u16(argv[2], &port)) {
            std::fprintf(stderr, "Invalid port: %s\n", argv[2]);
            return 2;
        }
        if (argc >= 4 && !parse_u32(argv[3], &frames)) {
            std::fprintf(stderr, "Invalid frame count: %s\n", argv[3]);
            return 2;
        }

        uint64_t first_time = 0;
        uint64_t last_time = 0;
        uint32_t received = 0;
        refract::protocol::UdpVideoReceiver receiver;
        return receiver.receive(port, frames, [&](refract::protocol::EncodedVideoFrame&& frame) {
            if (first_time == 0) {
                first_time = monotonic_time_ns();
            }
            last_time = monotonic_time_ns();
            ++received;
            if (received == 1 || received % 90 == 0) {
                const double seconds = last_time > first_time ? static_cast<double>(last_time - first_time) / 1000000000.0 : 0.0;
                const double fps = seconds > 0.0 ? static_cast<double>(received - 1) / seconds : 0.0;
                std::fprintf(stderr,
                             "Refract Video UDP: frame=%llu %ux%u codec=%u bytes=%zu avg_fps=%.2f\n",
                             static_cast<unsigned long long>(frame.frame_id),
                             frame.width,
                             frame.height,
                             frame.codec,
                             frame.payload.size(),
                             fps);
            }
        });
    }

    if (mode == "--video-send-synthetic") {
        const char* host = argc >= 3 ? argv[2] : "127.0.0.1";
        uint16_t port = 38492;
        uint32_t frames = 900;
        uint32_t fps = 90;
        if (argc >= 4 && !parse_u16(argv[3], &port)) {
            std::fprintf(stderr, "Invalid port: %s\n", argv[3]);
            return 2;
        }
        if (argc >= 5 && !parse_u32(argv[4], &frames)) {
            std::fprintf(stderr, "Invalid frame count: %s\n", argv[4]);
            return 2;
        }
        if (argc >= 6 && (!parse_u32(argv[5], &fps) || fps == 0)) {
            std::fprintf(stderr, "Invalid fps: %s\n", argv[5]);
            return 2;
        }

        refract::protocol::UdpVideoSender sender;
        if (!sender.open(host, port)) {
            return 1;
        }

        const auto frame_interval = std::chrono::nanoseconds(1000000000ull / fps);
        auto next_frame = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < frames; ++i) {
            auto frame = refract::protocol::make_synthetic_encoded_frame(i, 1920, 1080, 64 * 1024);
            if (!sender.send_frame(frame)) {
                std::fprintf(stderr, "Refract Video UDP: send failed at frame %u\n", i);
                return 1;
            }
            next_frame += frame_interval;
            std::this_thread::sleep_until(next_frame);
        }
        return 0;
    }

    if (mode == "--video-send-rgba") {
        const char* host = argc >= 3 ? argv[2] : "127.0.0.1";
        uint16_t port = 38492;
        uint32_t frames = 900;
        uint32_t fps = 90;
        uint32_t width = 320;
        uint32_t height = 180;
        if (argc >= 4 && !parse_u16(argv[3], &port)) {
            std::fprintf(stderr, "Invalid port: %s\n", argv[3]);
            return 2;
        }
        if (argc >= 5 && !parse_u32(argv[4], &frames)) {
            std::fprintf(stderr, "Invalid frame count: %s\n", argv[4]);
            return 2;
        }
        if (argc >= 6 && (!parse_u32(argv[5], &fps) || fps == 0)) {
            std::fprintf(stderr, "Invalid fps: %s\n", argv[5]);
            return 2;
        }
        if (argc >= 7 && !parse_u32(argv[6], &width)) {
            std::fprintf(stderr, "Invalid width: %s\n", argv[6]);
            return 2;
        }
        if (argc >= 8 && !parse_u32(argv[7], &height)) {
            std::fprintf(stderr, "Invalid height: %s\n", argv[7]);
            return 2;
        }

        refract::protocol::UdpVideoSender sender;
        if (!sender.open(host, port)) {
            return 1;
        }

        const auto frame_interval = std::chrono::nanoseconds(1000000000ull / fps);
        auto next_frame = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < frames; ++i) {
            auto frame = refract::protocol::make_synthetic_rgba_frame(i, width, height);
            if (!sender.send_frame(frame)) {
                std::fprintf(stderr, "Refract Video UDP: RGBA send failed at frame %u\n", i);
                return 1;
            }
            next_frame += frame_interval;
            std::this_thread::sleep_until(next_frame);
        }
        return 0;
    }

    if (mode != "--serve" && mode != "--serve-openxr") {
        print_usage();
        return 2;
    }

    uint16_t port = 38490;
    uint32_t frames = 0;
    if (argc >= 3 && !parse_u16(argv[2], &port)) {
        std::fprintf(stderr, "Invalid port: %s\n", argv[2]);
        return 2;
    }
    if (argc >= 4 && !parse_u32(argv[3], &frames)) {
        std::fprintf(stderr, "Invalid frame count: %s\n", argv[3]);
        return 2;
    }

    refract::protocol::TcpPoseServer server;
    if (mode == "--serve-openxr") {
        auto imageFrame = std::make_shared<HostImageFrame>();
        auto poseSource = std::make_shared<OpenXrPoseSource>(imageFrame.get());
        const std::string gameName = argc >= 5 ? argv[4] : "Android game";
        if (!poseSource->initialize(gameName) || !poseSource->open_mirror(gameName)) {
            return 1;
        }
        std::thread imageThread([imageFrame, poseSource] {
            refract::protocol::TcpImageServer imageServer;
            imageServer.serve_with_callback(
                38491,
                0,
                [&](const refract::protocol::ImageFrameHeader& header, const refract::protocol::ImageProjection& projection, std::vector<uint8_t>&& pixels) {
                    return poseSource->receive_image(header, projection, std::move(pixels));
                });
        });
        imageThread.detach();

        std::thread videoThread([imageFrame] {
            refract::protocol::UdpVideoReceiver videoReceiver;
            videoReceiver.receive(38492, 0, [&](refract::protocol::EncodedVideoFrame&& frame) {
                refract::protocol::ImageFrameHeader header{};
                std::vector<uint8_t> pixels;
                if (!decode_video_frame_to_image(frame, &header, &pixels)) {
                    static bool reportedUnsupported = false;
                    if (!reportedUnsupported) {
                        std::fprintf(stderr, "Refract Video UDP: received unsupported codec %u for OpenXR image feed\n", frame.codec);
                        reportedUnsupported = true;
                    }
                    return;
                }
                imageFrame->store(header, std::move(pixels));
            });
        });
        videoThread.detach();

        std::thread poseThread([poseSource, imageFrame, port, frames] {
            refract::protocol::TcpPoseServer poseServer;
            poseServer.serve_with_producer(port, frames, [&](uint64_t sequence) {
                return poseSource->latest_frame(sequence);
            });
        });
        poseThread.detach();

        uint64_t sequence = 0;
        while ((frames == 0 || sequence < frames) && poseSource->pump_mirror()) {
            poseSource->make_frame(sequence++);
            // OpenXR paces an active frame loop. An extra Windows sleep can
            // consume a timer tick and lower the compositor submission rate.
            if (!poseSource->drives_frame_loop()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return 0;
   }

    return server.serve(port, frames);
}

} // namespace refract::host
