#include "image_transport.h"
#include "windows_gpu_frame.h"

#include <cstdint>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace refract::protocol {

namespace {

class SocketRuntime {
public:
    SocketRuntime()
    {
#if defined(_WIN32)
        WSADATA data{};
        ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
        ok_ = true;
#endif
    }

    ~SocketRuntime()
    {
#if defined(_WIN32)
        if (ok_) {
            WSACleanup();
        }
#endif
    }

    bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

void close_socket(SocketHandle socket)
{
#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
}

bool recv_all(SocketHandle socket, void* data, size_t size)
{
    char* cursor = static_cast<char*>(data);
    size_t remaining = size;
    while (remaining > 0) {
#if defined(_WIN32)
        const int received = recv(socket, cursor, static_cast<int>(remaining), 0);
#else
        const ssize_t received = recv(socket, cursor, remaining, 0);
#endif
        if (received <= 0) {
            return false;
        }
        cursor += received;
        remaining -= static_cast<size_t>(received);
    }
    return true;
}

bool recv_payload(SocketHandle socket, std::vector<uint8_t>* payload, uint64_t size)
{
    payload->resize(static_cast<size_t>(size));
    if (payload->empty()) {
        return true;
    }
    return recv_all(socket, payload->data(), payload->size());
}

bool send_ack(SocketHandle socket, uint64_t acknowledgment)
{
    const char* bytes = reinterpret_cast<const char*>(&acknowledgment);
    size_t sent = 0;
    while (sent < sizeof(acknowledgment)) {
        int n = send(socket, bytes + sent, static_cast<int>(sizeof(acknowledgment) - sent), 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

void stop_socket(SocketHandle socket)
{
#if defined(_WIN32)
    shutdown(socket, SD_BOTH);
#else
    shutdown(socket, SHUT_RDWR);
#endif
}

} // namespace

int TcpImageServer::serve(uint16_t port, uint32_t maxFrames)
{
    return serve_with_callback(port, maxFrames, {});
}

int TcpImageServer::serve_with_callback(uint16_t port, uint32_t maxFrames, const FrameCallback& callback,
                                       const AckReadyCallback& ackReady)
{
    SocketRuntime runtime;
    if (!runtime.ok()) {
        std::fprintf(stderr, "Refract Image TCP: socket runtime init failed\n");
        return 1;
    }

    SocketHandle server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == kInvalidSocket) {
        std::fprintf(stderr, "Refract Image TCP: socket creation failed\n");
        return 1;
    }

    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::fprintf(stderr, "Refract Image TCP: bind failed on port %u\n", static_cast<unsigned>(port));
        close_socket(server);
        return 1;
    }

    if (listen(server, 1) != 0) {
        std::fprintf(stderr, "Refract Image TCP: listen failed\n");
        close_socket(server);
        return 1;
    }

    std::fprintf(stderr, "Refract Image TCP: listening on 0.0.0.0:%u\n", static_cast<unsigned>(port));
    uint32_t receivedFrames = 0;
    while (maxFrames == 0 || receivedFrames < maxFrames) {
        SocketHandle client = accept(server, nullptr, nullptr);
        if (client == kInvalidSocket) {
            std::fprintf(stderr, "Refract Image TCP: accept failed\n");
            close_socket(server);
            return 1;
        }

        std::fprintf(stderr, "Refract Image TCP: client connected\n");
        int receiveBufferSize = 4 * 1024 * 1024;
        setsockopt(client, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveBufferSize), sizeof(receiveBufferSize));
        // Acknowledgments are 8 bytes; without this Nagle holds each one back until the previous
        // one is TCP-acknowledged, which stalls a sender that keeps several frames in flight.
        int noDelay = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        std::mutex ackMutex;
        std::condition_variable ackCv;
        std::deque<std::pair<uint64_t, bool>> pendingAcks;
        bool stopAcks = false;
        std::thread ackThread;
        if (ackReady) {
            ackThread = std::thread([&] {
                for (;;) {
                    std::unique_lock lock(ackMutex);
                    ackCv.wait(lock, [&] { return stopAcks || !pendingAcks.empty(); });
                    if (pendingAcks.empty()) return;
                    const auto [sequence, consumed] = pendingAcks.front();
                    pendingAcks.pop_front();
                    lock.unlock();
                    const uint64_t acknowledgment = consumed && ackReady(sequence) ? sequence : UINT64_MAX;
                    if (!send_ack(client, acknowledgment)) {
                        stop_socket(client);  // Wake the receiver if it is waiting for another frame.
                        return;
                    }
                }
            });
        }
        while (maxFrames == 0 || receivedFrames < maxFrames) {
            ImageFrameHeader header{};
            if (!recv_all(client, &header, sizeof(header))) {
                std::fprintf(stderr, "Refract Image TCP: client disconnected\n");
                break;
            }

            const bool mixed = mixed_gpu_version(header.version);
            const bool video = video_version(header.version);
            const bool quads = header.version == kQuadImageFrameVersion || header.version == kQuadGpuFrameVersion ||
                header.version == kMixedQuadGpuFrameVersion || header.version == kQuadVideoImageFrameVersion;
            const bool gpu = header.version == kWindowsGpuFrameVersion || header.version == kQuadGpuFrameVersion || mixed;
            const bool projected = header.version == kProjectionImageFrameVersion || gpu || quads || video;
            const uint32_t expectedHeaderSize = sizeof(ImageFrameHeader) + (projected ? sizeof(ImageProjection) : 0);
            if ((mixed ? !valid_mixed_part(header.version, header.reserved) : header.reserved > (video ? kVideoFrameKey : 0)) ||
                header.magic != kImageFrameMagic || (header.version != kImageFrameVersion && !projected) ||
                header.type != (gpu ? kWindowsGpuFrameType : video ? kImageFrameTypeVideo : kImageFrameTypeRgba8) ||
                header.header_size != expectedHeaderSize ||
                header.format != (video ? kImageFrameFormatH264 : kImageFrameFormatRgba8) ||
                header.bytes_per_pixel != (video ? 0 : 4) || !header.width || !header.height ||
                !valid_render_extent(header.width, header.height) || !header.layers || header.layers > 4 ||
                (projected && header.layers != 2)) {
                std::fprintf(stderr, "Refract Image TCP: invalid image header\n");
                break;
            }

            const uint64_t expected =
                gpu ? sizeof(WindowsGpuFrame) : static_cast<uint64_t>(header.width) * header.height * header.layers * header.bytes_per_pixel;
            if ((video ? !header.payload_size || header.payload_size > kMaxVideoPayload : header.payload_size != expected) ||
                header.payload_size > 128ull * 1024ull * 1024ull) {
                std::fprintf(stderr, "Refract Image TCP: invalid payload size %llu\n",
                    static_cast<unsigned long long>(header.payload_size));
                break;
            }

            std::vector<uint8_t> payload;
            ImageProjection projection{};
            if (projected && (!recv_all(client, &projection, sizeof(projection)) ||
                !(quads ? valid_quads(projection) : valid_projection(projection)) ||
                (mixed && quads && projection.quad_count() != 1))) {
                std::fprintf(stderr, "Refract Image TCP: invalid projection metadata\n");
                break;
            }
            if (!recv_payload(client, &payload, header.payload_size)) {
                std::fprintf(stderr, "Refract Image TCP: client disconnected during payload\n");
                break;
            }

            bool consumed = callback && callback(header, projection, std::move(payload));
            if (gpu) {
                if (ackReady) {
                    std::lock_guard lock(ackMutex);
                    pendingAcks.emplace_back(header.sequence, consumed);
                    ackCv.notify_one();
                } else if (!send_ack(client, consumed ? header.sequence : UINT64_MAX)) {
                    break;
                }
            }

            if (receivedFrames % 10 == 0) {
                std::fprintf(
                    stderr,
                    "Refract Image TCP: frame seq=%llu %ux%u layers=%u bytes=%llu\n",
                    static_cast<unsigned long long>(header.sequence),
                    header.width,
                    header.height,
                    header.layers,
                    static_cast<unsigned long long>(header.payload_size));
            }
            ++receivedFrames;
        }
        if (ackThread.joinable()) {
            {
                std::lock_guard lock(ackMutex);
                stopAcks = true;
            }
            ackCv.notify_one();
            ackThread.join();
        }
        close_socket(client);
    }

    close_socket(server);
    return 0;
}

} // namespace refract::protocol
