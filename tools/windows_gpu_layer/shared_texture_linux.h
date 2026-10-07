#pragma once
// Linux counterpart of shared_texture.h: the eye textures are Vulkan images in exportable opaque-FD memory, and
// a socket thread hands their file descriptors to the viewer or host bridge (protocol/linux_gpu_share.h).
#include "linux_gpu_share.h"
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace refract {
struct SharedTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    int fd = -1;  // Kept open so a request can always be answered with a duplicate.
    uint64_t session = 0;
    uint32_t eye = 0;
};

// The exported textures of every device in this process, by (session, eye), and the socket that serves them.
class ShareServer {
public:
    static ShareServer& get() { static ShareServer server; return server; }
    void add(const SharedTexture& texture, uint32_t width, uint32_t height, VkFormat format, VkDeviceSize size)
    {
        start();
        std::lock_guard lock(mutex_);
        entries_[{texture.session, texture.eye}] = {texture.fd, {0, width, height, uint32_t(format), uint64_t(size)}};
    }
    void remove(const SharedTexture& texture)
    {
        std::lock_guard lock(mutex_);
        entries_.erase({texture.session, texture.eye});
    }

private:
    struct Entry { int fd; protocol::LinuxGpuShareReply reply; };
    void start()
    {
        std::lock_guard lock(mutex_);
        if (started_) return;
        started_ = true;
        const int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un address;
        const socklen_t length = protocol::linux_gpu_share_address(address);
        if (server < 0 || bind(server, reinterpret_cast<sockaddr*>(&address), length) != 0 || listen(server, 8) != 0) {
            std::fprintf(stderr, "Refract GPU layer: cannot serve shared textures (another emulator serves them?)\n");
            if (server >= 0) close(server);
            return;
        }
        std::thread([this, server] { serve(server); }).detach();
    }
    void serve(int server)
    {
        for (;;) {
            const int client = accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) continue;
            ucred peer{};
            socklen_t peerSize = sizeof(peer);
            protocol::LinuxGpuShareRequest request;
            timeval timeout{1, 0};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &peerSize) == 0 && peer.uid == getuid() &&
                recv(client, &request, sizeof(request), MSG_WAITALL) == ssize_t(sizeof(request)) &&
                request.version == protocol::kLinuxGpuShareVersion) {
                protocol::LinuxGpuShareReply reply;
                int fd = -1;
                {
                    std::lock_guard lock(mutex_);
                    const auto found = entries_.find({request.session, request.eye});
                    if (found != entries_.end()) {
                        // A duplicate taken under the lock: destroy() may close the original right after.
                        fd = fcntl(found->second.fd, F_DUPFD_CLOEXEC, 0);
                        if (fd >= 0) reply = found->second.reply;
                    }
                }
                alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
                iovec io{&reply, sizeof(reply)};
                msghdr message{};
                message.msg_iov = &io;
                message.msg_iovlen = 1;
                if (fd >= 0) {  // SCM_RIGHTS duplicates the descriptor into the receiver.
                    message.msg_control = control;
                    message.msg_controllen = sizeof(control);
                    cmsghdr* c = CMSG_FIRSTHDR(&message);
                    c->cmsg_level = SOL_SOCKET;
                    c->cmsg_type = SCM_RIGHTS;
                    c->cmsg_len = CMSG_LEN(sizeof(int));
                    std::memcpy(CMSG_DATA(c), &fd, sizeof(fd));
                }
                sendmsg(client, &message, MSG_NOSIGNAL);
                if (fd >= 0) close(fd);
            }
            close(client);
        }
    }
    std::mutex mutex_;
    bool started_ = false;
    std::map<std::pair<uint64_t, uint32_t>, Entry> entries_;
};

struct SharedDevice {
    VkDevice device{};
    PFN_vkGetDeviceProcAddr gdpa{};
    PFN_vkGetPhysicalDeviceMemoryProperties memoryProperties{};
    PFN_vkGetMemoryFdKHR getMemoryFd{};
    VkPhysicalDevice physical{};
    bool initialize(VkInstance instance, VkPhysicalDevice gpu, VkDevice vkDevice,
                    PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr getDeviceProc) {
        physical = gpu; device = vkDevice; gdpa = getDeviceProc;
        memoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(instance, "vkGetPhysicalDeviceMemoryProperties"));
        getMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(gdpa(device, "vkGetMemoryFdKHR"));
        return memoryProperties && getMemoryFd;
    }
    bool create(SharedTexture& out, uint32_t width, uint32_t height, VkFormat format, uint64_t session, uint32_t eye) {
        auto createImage = reinterpret_cast<PFN_vkCreateImage>(gdpa(device, "vkCreateImage"));
        auto getRequirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(gdpa(device, "vkGetImageMemoryRequirements"));
        auto allocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(gdpa(device, "vkAllocateMemory"));
        auto bindMemory = reinterpret_cast<PFN_vkBindImageMemory>(gdpa(device, "vkBindImageMemory"));
        if (!getMemoryFd) return false;
        // Receivers import RGBA only (OpenGL has no BGRA storage format); the eye blit converts BGRA sources.
        VkFormat shared;
        switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: shared = VK_FORMAT_R8G8B8A8_UNORM; break;
        case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: shared = VK_FORMAT_R8G8B8A8_SRGB; break;
        default: return false;
        }
        out.session = session; out.eye = eye;
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; imageInfo.pNext = &external;
        imageInfo.imageType = VK_IMAGE_TYPE_2D; imageInfo.format = shared;
        imageInfo.extent = {width, height, 1}; imageInfo.mipLevels = 1; imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT; imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        // The usages an OpenGL import may assume, so the driver lays the image out the way GL expects.
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VkResult result = createImage(device, &imageInfo, nullptr, &out.image);
        if (result != VK_SUCCESS) { std::fprintf(stderr, "Refract GPU: CreateImage=%d\n", result); return false; }
        VkMemoryRequirements req{}; getRequirements(device, out.image, &req);
        VkPhysicalDeviceMemoryProperties props{}; memoryProperties(physical, &props);
        uint32_t index = 0;
        while (index < props.memoryTypeCount &&
               (!(req.memoryTypeBits & (1u << index)) || !(props.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) ++index;
        if (index == props.memoryTypeCount) { destroy(out); return false; }
        VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.pNext = &exportInfo; dedicated.image = out.image;
        VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        memory.pNext = &dedicated; memory.allocationSize = req.size; memory.memoryTypeIndex = index;
        result = allocateMemory(device, &memory, nullptr, &out.memory);
        if (result != VK_SUCCESS) { std::fprintf(stderr, "Refract GPU: export memory=%d\n", result); destroy(out); return false; }
        result = bindMemory(device, out.image, out.memory, 0);
        VkMemoryGetFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        fdInfo.memory = out.memory; fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        if (result != VK_SUCCESS || getMemoryFd(device, &fdInfo, &out.fd) != VK_SUCCESS) { destroy(out); return false; }
        ShareServer::get().add(out, width, height, shared, req.size);
        std::fprintf(stderr, "Refract GPU: shared texture %ux%u format=%d (from %d) size=%llu\n", width, height, shared, format,
                     static_cast<unsigned long long>(req.size));
        return true;
    }
    void destroy(SharedTexture& texture) {
        if (texture.fd >= 0) { ShareServer::get().remove(texture); close(texture.fd); }
        if (texture.image) reinterpret_cast<PFN_vkDestroyImage>(gdpa(device, "vkDestroyImage"))(device, texture.image, nullptr);
        if (texture.memory) reinterpret_cast<PFN_vkFreeMemory>(gdpa(device, "vkFreeMemory"))(device, texture.memory, nullptr);
        texture = {};
    }
};
}
