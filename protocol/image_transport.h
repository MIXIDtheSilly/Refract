#pragma once

#include "image_frame.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace refract::protocol {

class TcpImageServer {
public:
    using FrameCallback = std::function<bool(const ImageFrameHeader&, const ImageProjection&, std::vector<uint8_t>&&)>;
    // Called on an acknowledgment thread after a GPU frame's callback. The
    // producer may reuse its shared textures only after this returns true.
    using AckReadyCallback = std::function<bool(uint64_t sequence)>;

    int serve(uint16_t port, uint32_t maxFrames);
    int serve_with_callback(uint16_t port, uint32_t maxFrames, const FrameCallback& callback,
                            const AckReadyCallback& ackReady = {});
};

} // namespace refract::protocol
