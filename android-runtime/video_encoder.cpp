#include "video_encoder.h"
#include "perf_stats.h"

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace refract::runtime {

namespace {

constexpr const char* kTag = "Refract.Video";
constexpr int32_t kColorFormatYuv420Planar = 19;
constexpr int32_t kColorFormatYuv420SemiPlanar = 21;
constexpr int32_t kColorFormatYuv420Flexible = 0x7F420888;
constexpr uint32_t kBufferFlagKeyFrame = 1;

int video_bitrate()
{
    char value[PROP_VALUE_MAX]{};
    const int mbps = __system_property_get("debug.refract.video_mbps", value) > 0 ? std::atoi(value) : 0;
    return std::clamp(mbps > 0 ? mbps : 40, 2, 200) * 1'000'000;
}

inline uint8_t luma(int r, int g, int b) { return static_cast<uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16); }

// BT.601 limited range; chroma is the average of each 2x2 block. w and h are even.
void rgba_to_yuv(const uint8_t* rgba, size_t srcStride, uint32_t w, uint32_t h,
                 uint8_t* yPlane, size_t yStride, uint8_t* uPlane, uint8_t* vPlane, size_t cStride, size_t cStep)
{
    for (uint32_t y = 0; y < h; y += 2) {
        const uint8_t* s0 = rgba + y * srcStride;
        const uint8_t* s1 = s0 + srcStride;
        uint8_t* y0 = yPlane + y * yStride;
        uint8_t* y1 = y0 + yStride;
        uint8_t* u = uPlane + (y / 2) * cStride;
        uint8_t* v = vPlane + (y / 2) * cStride;
        for (uint32_t x = 0; x < w; x += 2) {
            const uint8_t* a = s0 + x * 4;
            const uint8_t* b = s1 + x * 4;
            y0[x] = luma(a[0], a[1], a[2]);
            y0[x + 1] = luma(a[4], a[5], a[6]);
            y1[x] = luma(b[0], b[1], b[2]);
            y1[x + 1] = luma(b[4], b[5], b[6]);
            const int r = a[0] + a[4] + b[0] + b[4];
            const int g = a[1] + a[5] + b[1] + b[5];
            const int bl = a[2] + a[6] + b[2] + b[6];
            u[x / 2 * cStep] = static_cast<uint8_t>(((-38 * r - 74 * g + 112 * bl + 512) >> 10) + 128);
            v[x / 2 * cStep] = static_cast<uint8_t>(((112 * r - 94 * g - 18 * bl + 512) >> 10) + 128);
        }
    }
}

// True if the Annex-B access unit's first NAL unit is an SPS.
bool starts_with_sps(const uint8_t* data, size_t size)
{
    for (size_t i = 0; i + 3 < size && i < 64; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return (data[i + 3] & 0x1f) == 7;
    }
    return false;
}

} // namespace

bool H264StreamEncoder::open(uint32_t width, uint32_t height)
{
    close();
    const int32_t formats[] = {kColorFormatYuv420SemiPlanar, kColorFormatYuv420Flexible, kColorFormatYuv420Planar};
    for (int32_t colorFormat : formats) {
        codec_ = AMediaCodec_createEncoderByType("video/avc");
        if (!codec_) break;
        AMediaFormat* format = AMediaFormat_new();
        AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, static_cast<int32_t>(width * 2));
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, static_cast<int32_t>(height));
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, colorFormat);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, bitrate_);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, 90);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 2);
        AMediaFormat_setInt32(format, "priority", 0);                       // Realtime.
        AMediaFormat_setInt32(format, "latency", 1);                        // One frame in, one frame out.
        AMediaFormat_setInt32(format, "max-bframes", 0);
        AMediaFormat_setInt32(format, "prepend-sps-pps-to-idr-frames", 1);
        const media_status_t configured = AMediaCodec_configure(codec_, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(format);
        if (configured == AMEDIA_OK && AMediaCodec_start(codec_) == AMEDIA_OK) {
            int32_t stride = 0, slice = 0, actual = colorFormat;
            if (AMediaFormat* input = AMediaCodec_getInputFormat(codec_)) {
                AMediaFormat_getInt32(input, "stride", &stride);
                AMediaFormat_getInt32(input, "slice-height", &slice);
                AMediaFormat_getInt32(input, AMEDIAFORMAT_KEY_COLOR_FORMAT, &actual);
                AMediaFormat_delete(input);
            }
            stride_ = stride >= static_cast<int32_t>(width * 2) ? static_cast<size_t>(stride) : width * 2;
            sliceHeight_ = slice >= static_cast<int32_t>(height) ? static_cast<size_t>(slice) : height;
            planar_ = actual == kColorFormatYuv420Planar;
            eyeWidth_ = width;
            eyeHeight_ = height;
            needKey_ = keyRequested_ = true;  // A new encoder starts with a key frame.
            __android_log_print(ANDROID_LOG_INFO, kTag, "H.264 encoder %ux%u (eyes side by side) %d Mbps color=0x%x stride=%zu slice=%zu",
                width * 2, height, bitrate_ / 1'000'000, actual, stride_, sliceHeight_);
            return true;
        }
        __android_log_print(ANDROID_LOG_WARN, kTag, "encoder rejected color format 0x%x at %ux%u", colorFormat, width * 2, height);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    return false;
}

void H264StreamEncoder::close()
{
    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    inFlight_ = 0;
    config_.clear();
    for (auto& pending : pending_) pending = {};
}

void H264StreamEncoder::request_key_frame()
{
    AMediaFormat* parameters = AMediaFormat_new();
    AMediaFormat_setInt32(parameters, "request-sync", 0);
    AMediaCodec_setParameters(codec_, parameters);
    AMediaFormat_delete(parameters);
}

bool H264StreamEncoder::encode(uint64_t sequence, uint32_t width, uint32_t height, const uint8_t* eyes,
                               const refract::protocol::ImageProjection& projection, bool connected, bool nv12)
{
    if (failed_) return false;
    if (!connected) {
        needKey_ = true;
        keyRequested_ = false;
        return true;  // Nobody to send to; skip the work.
    }
    static refract::protocol::PerfStats stats("video-encode");
    refract::protocol::PerfScope scope(stats);
    const uint32_t w = width & ~1u, h = height & ~1u;
    const int bitrate = video_bitrate();
    if (!codec_ || w != eyeWidth_ || h != eyeHeight_ || bitrate != bitrate_) {
        bitrate_ = bitrate;
        if (!open(w, h)) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "no usable H.264 encoder; falling back to raw pixels");
            failed_ = true;
            return false;
        }
    }

    const ssize_t index = AMediaCodec_dequeueInputBuffer(codec_, 2000);
    if (index < 0) return true;  // Encoder still busy: drop this frame.
    size_t capacity = 0;
    uint8_t* buffer = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(index), &capacity);
    const size_t chromaStride = planar_ ? stride_ / 2 : stride_;
    const size_t bytes = stride_ * sliceHeight_ + (planar_ ? 2 * chromaStride * (sliceHeight_ / 2) : stride_ * (sliceHeight_ / 2));
    if (!buffer || capacity < bytes) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "input buffer too small (%zu < %zu)", capacity, bytes);
        AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(index), 0, 0, 0, 0);
        failed_ = true;
        close();
        return false;
    }
    if (needKey_ && !keyRequested_) {
        request_key_frame();
        keyRequested_ = true;
    }

    uint8_t* uPlane = buffer + stride_ * sliceHeight_;
    uint8_t* vPlane = planar_ ? uPlane + chromaStride * (sliceHeight_ / 2) : uPlane + 1;
    const size_t step = planar_ ? 1 : 2;
    if (nv12) {
        // Converted on the GPU: only the encoder's stride/slice layout remains.
        const size_t rowBytes = static_cast<size_t>(w) * 2;
        const uint8_t* uv = eyes + rowBytes * h;
        for (uint32_t y = 0; y < h; ++y) std::memcpy(buffer + y * stride_, eyes + y * rowBytes, rowBytes);
        for (uint32_t y = 0; y < h / 2; ++y) {
            const uint8_t* source = uv + y * rowBytes;
            if (!planar_) {
                std::memcpy(uPlane + y * chromaStride, source, rowBytes);
                continue;
            }
            for (size_t x = 0; x < rowBytes / 2; ++x) {
                uPlane[y * chromaStride + x] = source[2 * x];
                vPlane[y * chromaStride + x] = source[2 * x + 1];
            }
        }
    } else {
        const size_t eyeBytes = static_cast<size_t>(width) * height * 4;
        for (uint32_t eye = 0; eye < 2; ++eye) {
            rgba_to_yuv(eyes + eye * eyeBytes, static_cast<size_t>(width) * 4, w, h,
                        buffer + eye * w, stride_, uPlane + eye * (w / 2) * step, vPlane + eye * (w / 2) * step, chromaStride, step);
        }
    }

    const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    nextPts_ = std::max(nextPts_ + 1, now);
    auto& slot = pending_[static_cast<size_t>(nextPts_) % std::size(pending_)];
    slot = {nextPts_, sequence, projection};
    if (AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(index), 0, bytes, static_cast<uint64_t>(nextPts_), 0) == AMEDIA_OK) {
        ++inFlight_;
    }
    return true;
}

void H264StreamEncoder::drain(const Sink& sink, int64_t timeoutUs)
{
    while (codec_) {
        AMediaCodecBufferInfo info{};
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec_, &info, timeoutUs);
        timeoutUs = 0;
        if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (index < 0) return;
        size_t capacity = 0;
        const uint8_t* data = AMediaCodec_getOutputBuffer(codec_, static_cast<size_t>(index), &capacity);
        const uint8_t* unit = data ? data + info.offset : nullptr;
        const size_t size = unit && info.offset + info.size <= capacity ? static_cast<size_t>(info.size) : 0;
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
            if (size) config_.assign(unit, unit + size);
        } else if (size) {
            inFlight_ = std::max(0, inFlight_ - 1);
            const bool key = (info.flags & kBufferFlagKeyFrame) != 0;
            const auto& meta = pending_[static_cast<size_t>(info.presentationTimeUs) % std::size(pending_)];
            // A viewer can only start decoding at a key frame: drop the rest until one arrives.
            if (meta.pts == info.presentationTimeUs && (key || !needKey_)) {
                Output out{unit, size, key, meta.sequence, eyeWidth_, eyeHeight_, &meta.projection};
                if (key && !starts_with_sps(unit, size) && !config_.empty()) {
                    packet_.assign(config_.begin(), config_.end());
                    packet_.insert(packet_.end(), unit, unit + size);
                    out.data = packet_.data();
                    out.size = packet_.size();
                }
                static refract::protocol::PerfStats sendStats("video-send");
                refract::protocol::PerfScope scope(sendStats);
                if (sink(out)) {
                    if (key) needKey_ = false;
                } else {
                    needKey_ = true;
                    keyRequested_ = false;
                }
            }
        }
        AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(index), false);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) return;
    }
}

} // namespace refract::runtime
