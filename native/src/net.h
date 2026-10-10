// Pipes, eventfd, epoll, memfd and sockets.
#pragma once

#include "common.h"

namespace rn {

struct GuestThread;

// Readiness notification for blocking I/O: read IoGeneration() before checking a
// condition, then IoWait() returns as soon as anything changed since.
u64 IoGeneration();
void IoNotify();
// Returns false if interrupted by a signal. deadline_ns < 0 waits forever.
bool IoWait(GuestThread* t, u64 seen_generation, s64 deadline_ns);

s64 SysPipe2(u64 fds_addr, int flags);
s64 SysEventfd(u32 initval, int flags);
s64 SysEpollCreate(int flags);
s64 SysEpollCtl(int epfd, int op, int fd, u64 event_addr);
s64 SysEpollWait(GuestThread* t, int epfd, u64 events, int maxevents, int timeout_ms);
s64 SysMemfdCreate(u64 name_addr, int flags);

s64 SysSocket(int domain, int type, int protocol);
s64 SysSocketpair(int domain, int type, int protocol, u64 sv);
s64 SysBind(int fd, u64 addr, u32 len);
s64 SysListen(int fd, int backlog);
s64 SysAccept(GuestThread* t, int fd, u64 addr, u64 len_addr, int flags);
s64 SysConnect(GuestThread* t, int fd, u64 addr, u32 len);
s64 SysGetsockname(int fd, u64 addr, u64 len_addr, bool peer);
s64 SysSendto(GuestThread* t, int fd, u64 buf, u64 len, int flags, u64 addr, u32 addrlen);
s64 SysRecvfrom(GuestThread* t, int fd, u64 buf, u64 len, int flags, u64 addr, u64 addrlen);
s64 SysSetsockopt(int fd, int level, int name, u64 val, u32 len);
s64 SysGetsockopt(int fd, int level, int name, u64 val, u64 len_addr);
s64 SysShutdown(int fd, int how);
s64 SysSendmsg(GuestThread* t, int fd, u64 msg, int flags);
s64 SysRecvmsg(GuestThread* t, int fd, u64 msg, int flags);

}  // namespace rn
