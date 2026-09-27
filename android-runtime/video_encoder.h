#pragma once

#include "image_frame.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

struct AMediaCodec;

namespace refract::runtime {

// Hardware H.264 (MediaCodec) for the image stream: both eyes side by side in one
// picture. Runs on the async sender thread; the caller alternates encode() and drain().
class H264StreamEncoder {
public:
    struct Output {
        const uint8_t* data = nullptr;
        size_t size = 0;
        bool key = false;
        uint64_t sequence = 0;
        uint32_t eyeWidth = 0, eyeHeight = 0;
        const refract::protocol::ImageProjection* projection = nullptr;
    };
    // Returns false when the frame was not delivered (no consumer); the next frame is then a key frame.
    using Sink = std::function<bool(const Output&)>;

    ~H264StreamEncoder() { close(); }

    // eyes: two consecutive width x height RGBA8 images, or with nv12 one tight NV12 picture of
    // both eyes side by side ((2 * width) x height luma, then interleaved UV; even sizes).
    // connected: a consumer is listening (without one nothing is encoded).
    // Returns false if the encoder cannot be used.
    bool encode(uint64_t sequence, uint32_t width, uint32_t height, const uint8_t* eyes,
                const refract::protocol::ImageProjection& projection, bool connected, bool nv12 = false);
    // Sends finished frames; waits up to timeoutUs for the first one.
    void drain(const Sink& sink, int64_t timeoutUs);
    bool in_flight() const { return inFlight_ > 0; }
    void close();

private:
    struct Pending {
        int64_t pts = -1;
        uint64_t sequence = 0;
        refract::protocol::ImageProjection projection{};
    };
    bool open(uint32_t width, uint32_t height);
    void request_key_frame();

    AMediaCodec* codec_ = nullptr;
    uint32_t eyeWidth_ = 0, eyeHeight_ = 0;  // Encoded eye size (even).
    size_t stride_ = 0, sliceHeight_ = 0;
    bool planar_ = false;
    int bitrate_ = 0;
    bool failed_ = false;
    bool needKey_ = true;
    bool keyRequested_ = false;
    int inFlight_ = 0;
    int64_t nextPts_ = 0;
    Pending pending_[32];
    std::vector<uint8_t> config_;  // SPS/PPS, prepended to key frames that lack them.
    std::vector<uint8_t> packet_;
};

} // namespace refract::runtime
