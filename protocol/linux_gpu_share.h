#pragma once
// Linux counterpart of the named D3D11 textures the Windows GPU layer shares (tools/windows_gpu_layer): the layer,
// loaded into the emulator's host Vulkan, exports each eye texture as an opaque-FD Vulkan allocation and hands a
// duplicate of the file descriptor to whoever asks on a per-user abstract Unix socket. A receiver asks for
// (session, eye) as named by a GPU frame (WindowsGpuFrame::session) and imports the memory (Vulkan
// VK_KHR_external_memory_fd or OpenGL GL_EXT_memory_object_fd). Only processes of the same user are served.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace refract::protocol {

constexpr uint32_t kLinuxGpuShareVersion = 1;
struct LinuxGpuShareRequest {
    uint64_t session = 0;
    uint32_t eye = 0;
    uint32_t version = kLinuxGpuShareVersion;
};
// The image is 2D, one mip level and layer, optimal tiling, in a dedicated allocation of `size` bytes.
struct LinuxGpuShareReply {
    int32_t status = -1;  // 0: a file descriptor accompanies the reply.
    uint32_t width = 0, height = 0;
    uint32_t format = 0;  // VkFormat: R8G8B8A8_UNORM or R8G8B8A8_SRGB.
    uint64_t size = 0;
};
static_assert(sizeof(LinuxGpuShareRequest) == 16 && sizeof(LinuxGpuShareReply) == 24);

// The abstract socket's address (sun_path starts with a zero byte); returns its length for bind/connect.
inline socklen_t linux_gpu_share_address(sockaddr_un& address)
{
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    const int length = std::snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1, "refract-gpu-share-%u", unsigned(getuid()));
    return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + length);
}

// Asks the layer for one eye texture. Returns a file descriptor the caller owns (or -1) and fills `reply`.
inline int request_linux_gpu_share(uint64_t session, uint32_t eye, LinuxGpuShareReply& reply)
{
    reply = {};
    const int socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket < 0) return -1;
    timeval timeout{2, 0};
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_un address;
    const socklen_t length = linux_gpu_share_address(address);
    const LinuxGpuShareRequest request{session, eye};
    if (connect(socket, reinterpret_cast<sockaddr*>(&address), length) != 0 ||
        send(socket, &request, sizeof(request), MSG_NOSIGNAL) != ssize_t(sizeof(request))) {
        close(socket);
        return -1;
    }
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    iovec io{&reply, sizeof(reply)};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    const ssize_t received = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
    close(socket);
    int fd = -1;
    for (cmsghdr* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) std::memcpy(&fd, CMSG_DATA(c), sizeof(fd));
    if (received != ssize_t(sizeof(reply)) || reply.status != 0 || fd < 0) {
        if (fd >= 0) close(fd);
        if (received != ssize_t(sizeof(reply))) reply.status = -1;
        return -1;
    }
    return fd;
}

}  // namespace refract::protocol
