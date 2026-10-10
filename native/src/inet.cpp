// AF_INET / AF_INET6 sockets on Winsock. Host sockets are always non-blocking; a
// guest's blocking call waits with IoWait, which a poller thread wakes (IoNotify) on
// every network event of every socket (WSAEventSelect).
#include "inet.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "kernel.h"
#include "net.h"

namespace rn {

using namespace lx;

namespace {

constexpr int L_AF_INET = 2, L_AF_INET6 = 10;
constexpr int L_SOCK_STREAM = 1, L_SOCK_DGRAM = 2;
constexpr int L_MSG_PEEK = 2, L_MSG_DONTWAIT = 0x40, L_MSG_WAITALL = 0x100;

int ErrnoFromWsa(int e) {
    switch (e) {
    case WSAEWOULDBLOCK: return EAGAIN_;
    case WSAEINPROGRESS: return EINPROGRESS_;
    case WSAEALREADY: return EALREADY_;
    case WSAECONNREFUSED: return ECONNREFUSED_;
    case WSAECONNRESET: return ECONNRESET_;
    case WSAECONNABORTED: return ECONNABORTED_;
    case WSAENOTCONN: return ENOTCONN_;
    case WSAEISCONN: return EISCONN_;
    case WSAETIMEDOUT: return ETIMEDOUT_;
    case WSAEHOSTUNREACH: return EHOSTUNREACH_;
    case WSAENETUNREACH: return ENETUNREACH_;
    case WSAENETDOWN: return ENETDOWN_;
    case WSAEADDRINUSE: return EADDRINUSE_;
    case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL_;
    case WSAEAFNOSUPPORT: return EAFNOSUPPORT_;
    case WSAEMSGSIZE: return EMSGSIZE_;
    case WSAENOBUFS: return ENOBUFS_;
    case WSAESHUTDOWN: return EPIPE_;
    case WSAEACCES: return EACCES_;
    case WSAEFAULT: return EFAULT_;
    case WSAENOPROTOOPT: return ENOPROTOOPT_;
    case WSAEOPNOTSUPP: return EOPNOTSUPP_;
    case WSAEDESTADDRREQ: return EDESTADDRREQ_;
    default: return EINVAL_;
    }
}

// Guest sockaddr -> host sockaddr (only the family number differs for INET6).
bool ToHostAddr(u64 addr, u32 len, sockaddr_storage* out, int* out_len) {
    if (len < 2 || len > sizeof(sockaddr_storage))
        return false;
    memset(out, 0, sizeof(*out));
    if (!SafeCopyFromGuest(out, addr, len))
        return false;
    u16 fam = out->ss_family;
    if (fam == L_AF_INET6)
        out->ss_family = AF_INET6;
    else if (fam != L_AF_INET)
        return false;
    *out_len = out->ss_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    return true;
}

void ToGuestAddr(const sockaddr_storage& in, int in_len, u64 addr, u64 len_addr) {
    if (!addr || !len_addr)
        return;
    u32 cap = 0;
    SafeCopyFromGuest(&cap, len_addr, 4);
    sockaddr_storage g = in;
    u32 len = in.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    if (in.ss_family == AF_INET6)
        g.ss_family = L_AF_INET6;
    SafeCopyToGuest(addr, &g, std::min(cap, len));
    SafeCopyToGuest(len_addr, &len, 4);
    (void)in_len;
}

// Poller: one event per socket; the thread waits on all of them (64 per wait call).
class Poller {
public:
    static Poller& Get() {
        static Poller* p = new Poller;
        return *p;
    }
    void Add(SOCKET s, HANDLE ev) {
        std::lock_guard lock(mu_);
        items_.push_back({s, ev});
        SetEvent(changed_);
    }
    void Remove(SOCKET s) {
        std::lock_guard lock(mu_);
        items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const Item& i) { return i.s == s; }),
                     items_.end());
        SetEvent(changed_);
    }

private:
    struct Item {
        SOCKET s;
        HANDLE ev;
    };
    Poller() {
        changed_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::thread([this] { Run(); }).detach();
    }
    void Run() {
        for (;;) {
            std::vector<HANDLE> handles{changed_};
            std::vector<Item> items;
            {
                std::lock_guard lock(mu_);
                items = items_;
            }
            for (size_t i = 0; i < items.size() && handles.size() < MAXIMUM_WAIT_OBJECTS; ++i)
                handles.push_back(items[i].ev);
            // More than 63 sockets: the rest are rechecked every 5 ms.
            const DWORD timeout = items.size() + 1 > MAXIMUM_WAIT_OBJECTS ? 5 : INFINITE;
            DWORD r = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, timeout);
            if (r == WAIT_OBJECT_0)
                continue;
            // Clears each signaled event and its record; the guest's next recv/send/accept
            // re-arms the event (readiness itself is read with select() in Poll()).
            for (const Item& i : items)
                if (WaitForSingleObject(i.ev, 0) == WAIT_OBJECT_0) {
                    WSANETWORKEVENTS ne;
                    WSAEnumNetworkEvents(i.s, i.ev, &ne);
                }
            IoNotify();
        }
    }
    std::mutex mu_;
    std::vector<Item> items_;
    HANDLE changed_;
};

}  // namespace

class InetSocket : public FileObject {
public:
    InetSocket(SOCKET s, int family, int type) : s_(s), family_(family), type_(type) {
        ev_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        WSAEventSelect(s_, ev_, FD_READ | FD_WRITE | FD_ACCEPT | FD_CONNECT | FD_CLOSE);
        Poller::Get().Add(s_, ev_);
        path = type == L_SOCK_STREAM ? "socket:[tcp]" : "socket:[udp]";
    }
    ~InetSocket() override {
        Poller::Get().Remove(s_);
        closesocket(s_);
        CloseHandle(ev_);
        IoNotify();
    }

    s64 Read(void* buf, u64 n) override { return Recv(reinterpret_cast<u64>(buf), n, 0, 0, 0); }
    s64 Write(const void* buf, u64 n) override {
        // `buf` is guest memory (possibly not yet committed): copy it first.
        std::vector<u8> tmp(n);
        if (n && !SafeCopyFromGuest(tmp.data(), reinterpret_cast<u64>(buf), n))
            return -EFAULT_;
        return Send(tmp.data(), n, 0, nullptr, 0);
    }
    u32 Poll() override {
        fd_set rd, wr, ex;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_ZERO(&ex);
        FD_SET(s_, &rd);
        FD_SET(s_, &wr);
        FD_SET(s_, &ex);
        timeval tv{0, 0};
        if (select(0, &rd, &wr, &ex, &tv) == SOCKET_ERROR)
            return kPollErr;
        u32 ev = 0;
        if (FD_ISSET(s_, &rd)) {
            ev |= kPollIn | kPollRdNorm;
            if (type_ == L_SOCK_STREAM && !listening_) {
                char c;
                if (recv(s_, &c, 1, MSG_PEEK) == 0)
                    ev |= kPollHup;
            }
        }
        if (FD_ISSET(s_, &wr))
            ev |= kPollOut | kPollWrNorm;
        if (FD_ISSET(s_, &ex))
            ev |= kPollErr;  // failed non-blocking connect
        if (type_ == L_SOCK_STREAM && !listening_ && !connected_ && !connecting_)
            ev &= ~(kPollOut | kPollWrNorm);
        if (connecting_ && (ev & (kPollOut | kPollErr))) {
            connecting_ = false;
            connected_ = !(ev & kPollErr);
        }
        return ev;
    }
    s64 Ioctl(u64 req, u64 arg) override {
        if (req == 0x541B) {  // FIONREAD
            u_long n = 0;
            ioctlsocket(s_, FIONREAD, &n);
            int v = static_cast<int>(n);
            return SafeCopyToGuest(arg, &v, 4) ? 0 : -EFAULT_;
        }
        if (req == 0x5421)  // FIONBIO
            return 0;
        return -ENOTTY_;
    }
    int Stat(lx::Stat* st) override {
        FileObject::Stat(st);
        st->st_mode = S_IFSOCK_ | 0777;
        return 0;
    }

    s64 Bind(u64 addr, u32 len) {
        sockaddr_storage sa;
        int sl;
        if (!ToHostAddr(addr, len, &sa, &sl))
            return -EINVAL_;
        return bind(s_, reinterpret_cast<sockaddr*>(&sa), sl) == 0 ? 0 : -ErrnoFromWsa(WSAGetLastError());
    }
    s64 Listen(int backlog) {
        if (listen(s_, backlog > 0 ? backlog : SOMAXCONN) != 0)
            return -ErrnoFromWsa(WSAGetLastError());
        listening_ = true;
        return 0;
    }
    s64 Connect(GuestThread* t, u64 addr, u32 len) {
        sockaddr_storage sa;
        int sl;
        if (!ToHostAddr(addr, len, &sa, &sl))
            return -EINVAL_;
        if (connect(s_, reinterpret_cast<sockaddr*>(&sa), sl) == 0) {
            connected_ = true;
            return 0;
        }
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK)
            return -ErrnoFromWsa(e);
        if (type_ != L_SOCK_STREAM) {
            connected_ = true;
            return 0;
        }
        connecting_ = true;
        if (Nonblocking(0))
            return -EINPROGRESS_;
        for (;;) {
            u64 gen = IoGeneration();
            u32 ev = Poll();
            if (ev & kPollErr) {
                int err = 0, el = sizeof(err);
                getsockopt(s_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &el);
                return -ErrnoFromWsa(err ? err : WSAECONNREFUSED);
            }
            if (connected_)
                return 0;
            if (!IoWait(t, gen, Deadline(snd_timeout_ms_)))
                return -EINTR_;
            if (snd_timeout_ms_ > 0 && static_cast<s64>(MonotonicNs()) >= Deadline(snd_timeout_ms_))
                return -EINPROGRESS_;
        }
    }
    s64 Accept(GuestThread* t, u64 addr, u64 len_addr, int flags) {
        const s64 deadline = Deadline(rcv_timeout_ms_);
        for (;;) {
            u64 gen = IoGeneration();
            sockaddr_storage sa{};
            int sl = sizeof(sa);
            SOCKET c = accept(s_, reinterpret_cast<sockaddr*>(&sa), &sl);
            if (c != INVALID_SOCKET) {
                auto f = std::make_shared<InetSocket>(c, family_, type_);
                f->connected_ = true;
                f->status_flags = O_RDWR_ | ((flags & 04000) ? O_NONBLOCK_ : 0);
                ToGuestAddr(sa, sl, addr, len_addr);
                return Fds().Install(f, (flags & 02000000) != 0);
            }
            int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK)
                return -ErrnoFromWsa(e);
            if (Nonblocking(0))
                return -EAGAIN_;
            if (!IoWait(t, gen, deadline))
                return -EINTR_;
            if (deadline >= 0 && static_cast<s64>(MonotonicNs()) >= deadline)
                return -EAGAIN_;
        }
    }
    s64 Name(u64 addr, u64 len_addr, bool peer) {
        sockaddr_storage sa{};
        int sl = sizeof(sa);
        int r = peer ? getpeername(s_, reinterpret_cast<sockaddr*>(&sa), &sl)
                     : getsockname(s_, reinterpret_cast<sockaddr*>(&sa), &sl);
        if (r != 0)
            return -ErrnoFromWsa(WSAGetLastError());
        ToGuestAddr(sa, sl, addr, len_addr);
        return 0;
    }
    // `data` is host memory.
    s64 Send(const u8* data, u64 n, int flags, const sockaddr_storage* to, int to_len) {
        GuestThread* t = CurrentThread();
        const s64 deadline = Deadline(snd_timeout_ms_);
        u64 sent = 0;
        for (;;) {
            u64 gen = IoGeneration();
            int chunk = static_cast<int>(std::min<u64>(n - sent, 1u << 30));
            int r = to ? sendto(s_, reinterpret_cast<const char*>(data + sent), chunk, 0,
                                reinterpret_cast<const sockaddr*>(to), to_len)
                       : send(s_, reinterpret_cast<const char*>(data + sent), chunk, 0);
            if (r >= 0) {
                sent += static_cast<u64>(r);
                if (sent >= n || type_ != L_SOCK_STREAM)
                    return static_cast<s64>(sent);
                continue;
            }
            int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK)
                return sent ? static_cast<s64>(sent) : -ErrnoFromWsa(e);
            if (Nonblocking(flags) || !t)
                return sent ? static_cast<s64>(sent) : -EAGAIN_;
            if (!IoWait(t, gen, deadline))
                return sent ? static_cast<s64>(sent) : -EINTR_;
            if (deadline >= 0 && static_cast<s64>(MonotonicNs()) >= deadline)
                return sent ? static_cast<s64>(sent) : -EAGAIN_;
        }
    }
    // Receives into guest memory at `buf`.
    s64 Recv(u64 buf, u64 n, int flags, u64 from, u64 from_len) {
        GuestThread* t = CurrentThread();
        const s64 deadline = Deadline(rcv_timeout_ms_);
        std::vector<char> tmp(static_cast<size_t>(std::min<u64>(n, 1u << 26)));
        u64 got = 0;
        for (;;) {
            u64 gen = IoGeneration();
            sockaddr_storage sa{};
            int sl = sizeof(sa);
            int want = static_cast<int>(tmp.size() - got);
            int r = recvfrom(s_, tmp.data() + got, want, (flags & L_MSG_PEEK) ? MSG_PEEK : 0,
                             reinterpret_cast<sockaddr*>(&sa), &sl);
            if (r > 0 || (r == 0 && want > 0)) {
                if (from && type_ != L_SOCK_STREAM)
                    ToGuestAddr(sa, sl, from, from_len);
                got += static_cast<u64>(r);
                if (r > 0 && (flags & L_MSG_WAITALL) && got < tmp.size() && type_ == L_SOCK_STREAM)
                    continue;
                break;
            }
            int e = r < 0 ? WSAGetLastError() : 0;
            if (r < 0 && e == WSAEMSGSIZE) {  // datagram truncated: Linux returns the part that fit
                got = tmp.size();
                break;
            }
            if (r < 0 && e != WSAEWOULDBLOCK) {
                if (got)
                    break;
                return -ErrnoFromWsa(e);
            }
            if (got || Nonblocking(flags) || !t)
                return got ? static_cast<s64>(got) : -EAGAIN_;
            if (!IoWait(t, gen, deadline))
                return -EINTR_;
            if (deadline >= 0 && static_cast<s64>(MonotonicNs()) >= deadline)
                return -EAGAIN_;
        }
        if (got && !SafeCopyToGuest(buf, tmp.data(), got))
            return -EFAULT_;
        return static_cast<s64>(got);
    }
    s64 SetOpt(int level, int name, u64 val, u32 len) {
        int v = 0;
        if (len >= 4)
            SafeCopyFromGuest(&v, val, 4);
        if (level == 1) {  // SOL_SOCKET
            switch (name) {
            case 20:  // SO_RCVTIMEO
            case 21: {  // SO_SNDTIMEO
                s64 tv[2] = {};
                SafeCopyFromGuest(tv, val, std::min<u32>(len, 16));
                s64 ms = tv[0] * 1000 + tv[1] / 1000;
                (name == 20 ? rcv_timeout_ms_ : snd_timeout_ms_) = ms;
                return 0;
            }
            case 2: return HostOpt(SOL_SOCKET, SO_REUSEADDR, v);
            case 6: return HostOpt(SOL_SOCKET, SO_BROADCAST, v);
            case 7: return HostOpt(SOL_SOCKET, SO_SNDBUF, v);
            case 8: return HostOpt(SOL_SOCKET, SO_RCVBUF, v);
            case 9: return HostOpt(SOL_SOCKET, SO_KEEPALIVE, v);
            default: return 0;  // SO_REUSEPORT, SO_PRIORITY, ...: accepted
            }
        }
        if (level == 6 && name == 1)  // IPPROTO_TCP, TCP_NODELAY
            return HostOpt(IPPROTO_TCP, TCP_NODELAY, v);
        if (level == 0 && name == 1)  // IP_TOS
            return 0;
        return 0;
    }
    s64 GetOpt(int level, int name, u64 val, u64 len_addr) {
        int v = 0;
        if (level == 1) {
            int hl = sizeof(v);
            switch (name) {
            case 4:  // SO_ERROR
                getsockopt(s_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&v), &hl);
                v = v ? ErrnoFromWsa(v) : 0;
                break;
            case 3: v = type_; break;  // SO_TYPE
            case 39: v = family_; break;  // SO_DOMAIN
            case 7: getsockopt(s_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&v), &hl); break;
            case 8: getsockopt(s_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&v), &hl); break;
            case 30: v = listening_; break;  // SO_ACCEPTCONN
            default: break;
            }
        } else if (level == 6 && name == 1) {
            int hl = sizeof(v);
            getsockopt(s_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&v), &hl);
        }
        if (val)
            SafeCopyToGuest(val, &v, 4);
        u32 len = 4;
        if (len_addr)
            SafeCopyToGuest(len_addr, &len, 4);
        return 0;
    }
    s64 Shutdown(int how) { return shutdown(s_, how) == 0 ? 0 : -ErrnoFromWsa(WSAGetLastError()); }

private:
    bool Nonblocking(int flags) const { return (status_flags & O_NONBLOCK_) || (flags & L_MSG_DONTWAIT); }
    static s64 Deadline(s64 timeout_ms) {
        return timeout_ms > 0 ? static_cast<s64>(MonotonicNs()) + timeout_ms * 1000000ll : -1;
    }
    s64 HostOpt(int level, int name, int v) {
        return setsockopt(s_, level, name, reinterpret_cast<const char*>(&v), sizeof(v)) == 0
                   ? 0
                   : -ErrnoFromWsa(WSAGetLastError());
    }

    SOCKET s_;
    HANDLE ev_;
    int family_, type_;
    std::atomic<bool> listening_{false}, connected_{false}, connecting_{false};
    s64 rcv_timeout_ms_ = 0, snd_timeout_ms_ = 0;
};

s64 InetSocketCreate(int domain, int type, int protocol) {
    static const bool started = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    if (!started)
        return -ENETDOWN_;
    const int base = type & 0xf;
    if (base != L_SOCK_STREAM && base != L_SOCK_DGRAM)
        return -EPROTONOSUPPORT_;
    SOCKET s = socket(domain == L_AF_INET6 ? AF_INET6 : AF_INET, base == L_SOCK_STREAM ? SOCK_STREAM : SOCK_DGRAM,
                      protocol);
    if (s == INVALID_SOCKET)
        return -ErrnoFromWsa(WSAGetLastError());
    auto f = std::make_shared<InetSocket>(s, domain, base);
    f->status_flags = O_RDWR_ | ((type & 04000) ? O_NONBLOCK_ : 0);
    return Fds().Install(f, (type & 02000000) != 0);
}

InetSocket* AsInet(const FilePtr& f) { return dynamic_cast<InetSocket*>(f.get()); }

s64 InetBind(InetSocket* s, u64 addr, u32 len) { return s->Bind(addr, len); }
s64 InetListen(InetSocket* s, int backlog) { return s->Listen(backlog); }
s64 InetAccept(GuestThread* t, InetSocket* s, u64 addr, u64 len_addr, int flags) {
    return s->Accept(t, addr, len_addr, flags);
}
s64 InetConnect(GuestThread* t, InetSocket* s, u64 addr, u32 len) { return s->Connect(t, addr, len); }
s64 InetName(InetSocket* s, u64 addr, u64 len_addr, bool peer) { return s->Name(addr, len_addr, peer); }
s64 InetSendto(InetSocket* s, u64 buf, u64 len, int flags, u64 addr, u32 addrlen) {
    std::vector<u8> tmp(len);
    if (len && !SafeCopyFromGuest(tmp.data(), buf, len))
        return -EFAULT_;
    sockaddr_storage to;
    int to_len = 0;
    if (addr && addrlen) {
        if (!ToHostAddr(addr, addrlen, &to, &to_len))
            return -EINVAL_;
        return s->Send(tmp.data(), len, flags, &to, to_len);
    }
    return s->Send(tmp.data(), len, flags, nullptr, 0);
}
s64 InetRecvfrom(InetSocket* s, u64 buf, u64 len, int flags, u64 addr, u64 addrlen) {
    return s->Recv(buf, len, flags, addr, addrlen);
}
s64 InetSetsockopt(InetSocket* s, int level, int name, u64 val, u32 len) { return s->SetOpt(level, name, val, len); }
s64 InetGetsockopt(InetSocket* s, int level, int name, u64 val, u64 len_addr) {
    return s->GetOpt(level, name, val, len_addr);
}
s64 InetShutdown(InetSocket* s, int how) { return s->Shutdown(how); }

}  // namespace rn
