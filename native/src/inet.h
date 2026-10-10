// AF_INET / AF_INET6 sockets, backed by Winsock.
#pragma once

#include <string>
#include <vector>

#include "common.h"
#include "files.h"

namespace rn {

struct GuestThread;
class InetSocket;

// socket(2) for AF_INET/AF_INET6: returns the new fd or -errno.
s64 InetSocketCreate(int domain, int type, int protocol);
InetSocket* AsInet(const FilePtr& f);

s64 InetBind(InetSocket* s, u64 addr, u32 len);
s64 InetListen(InetSocket* s, int backlog);
s64 InetAccept(GuestThread* t, InetSocket* s, u64 addr, u64 len_addr, int flags);
s64 InetConnect(GuestThread* t, InetSocket* s, u64 addr, u32 len);
s64 InetName(InetSocket* s, u64 addr, u64 len_addr, bool peer);
s64 InetSendto(InetSocket* s, u64 buf, u64 len, int flags, u64 addr, u32 addrlen);
s64 InetRecvfrom(InetSocket* s, u64 buf, u64 len, int flags, u64 addr, u64 addrlen);
s64 InetSetsockopt(InetSocket* s, int level, int name, u64 val, u32 len);
s64 InetGetsockopt(InetSocket* s, int level, int name, u64 val, u64 len_addr);
s64 InetShutdown(InetSocket* s, int how);

// netd's /dev/socket/dnsproxyd: the reply to one command ("getaddrinfo host serv flags family
// socktype protocol netid"), resolved with Winsock in bionic's wire format.
std::vector<u8> DnsProxyReply(const std::string& command);

}  // namespace rn
