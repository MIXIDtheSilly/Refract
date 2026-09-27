#pragma once

#include "pose_frame.h"
#include "pose_stream_decoder.h"
#include <mutex>

#if defined(__ANDROID__)
#include <cstring>
#include <jni.h>
#include <sys/system_properties.h>
#endif

namespace refract::runtime {

class PoseClient {
public:
    // Return a snapshot: callers may use it while another thread polls input.
    refract::protocol::PoseFrame latest_pose_frame();
#if defined(__ANDROID__)
    void set_android_context(JavaVM* vm, jobject context);
#endif

private:
    std::mutex mutex_;
    bool query_pose_broker();
    bool ensure_connected();
    bool ensure_emulator_connected();
    void read_available_frames();
    void close_socket();

#if defined(__ANDROID__)
    JavaVM* java_vm_ = nullptr;
    jobject android_context_ = nullptr;
    uint32_t broker_retry_countdown_ = 0;
#endif
    int socket_ = -1;
    bool connecting_ = false;
#if defined(__ANDROID__)
    const bool emulator_ = [] {
        char hardware[PROP_VALUE_MAX]{};
        __system_property_get("ro.hardware", hardware);
        return std::strcmp(hardware, "ranchu") == 0 || std::strcmp(hardware, "goldfish") == 0;
    }();
#endif
    uint64_t next_connect_ns_ = 0;
    refract::protocol::PoseStreamDecoder decoder_;
    uint32_t retry_countdown_ = 0;
    refract::protocol::PoseFrame latest_{
        refract::protocol::kPoseFrameMagic,
        refract::protocol::kPoseFrameVersion,
        refract::protocol::kPoseFrameType,
        0,
        0,
        {0.0f, 1.65f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        {-0.25f, 1.25f, -0.45f, 0.0f, 0.0f, 0.0f, 1.0f},
        {0.25f, 1.25f, -0.45f, 0.0f, 0.0f, 0.0f, 1.0f},
    };
};

PoseClient& pose_client();

} // namespace refract::runtime
