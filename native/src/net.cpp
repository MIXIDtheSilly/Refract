#include "net.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "files.h"
#include "inet.h"
#include "kernel.h"

namespace rn {

using namespace lx;

// Readiness notifications ------------------------------------------------------

namespace {
std::mutex g_io_mu;
std::set<GuestThread*> g_io_waiters;
std::atomic<u64> g_io_gen{1};
}  // namespace

u64 IoGeneration() { return g_io_gen.load(); }

void IoNotify() {
    g_io_gen.fetch_add(1);
    std::lock_guard lock(g_io_mu);
    for (GuestThread* t : g_io_waiters)
        InterruptThread(t);
}

bool IoWait(GuestThread* t, u64 seen, s64 deadline_ns) {
    {
        std::lock_guard lock(g_io_mu);
        g_io_waiters.insert(t);
    }
    bool ok = true;
    while (g_io_gen.load() == seen) {
        if (t->SignalPending()) {
            ok = false;
            break;
        }
        DWORD ms = INFINITE;
        if (deadline_ns >= 0) {
            u64 now = MonotonicNs();
            if (now >= static_cast<u64>(deadline_ns))
                break;
            ms = static_cast<DWORD>((static_cast<u64>(deadline_ns) - now + 999999) / 1000000);
        }
        WaitForSingleObject(t->wake_event, ms);
    }
    std::lock_guard lock(g_io_mu);
    g_io_waiters.erase(t);
    return ok;
}

namespace {

// Byte streams (pipes, socketpairs) -------------------------------------------

struct StreamBuf {
    std::mutex mu;
    std::deque<u8> data;
    bool writer_closed = false;
    bool reader_closed = false;
};

s64 StreamRead(StreamBuf& b, void* buf, u64 n, bool nonblock) {
    GuestThread* t = CurrentThread();
    for (;;) {
        u64 gen = IoGeneration();
        {
            std::lock_guard lock(b.mu);
            if (!b.data.empty()) {
                u64 take = std::min<u64>(n, b.data.size());
                std::vector<u8> tmp(b.data.begin(), b.data.begin() + take);
                if (!SafeCopyToGuest(reinterpret_cast<u64>(buf), tmp.data(), take))
                    return -EFAULT_;
                b.data.erase(b.data.begin(), b.data.begin() + take);
                IoNotify();
                return static_cast<s64>(take);
            }
            if (b.writer_closed)
                return 0;
        }
        if (nonblock || !t)
            return -EAGAIN_;
        if (!IoWait(t, gen, -1))
            return -EINTR_;
    }
}

s64 StreamWrite(StreamBuf& b, const void* buf, u64 n) {
    {
        std::lock_guard lock(b.mu);
        if (b.reader_closed)
            return -EPIPE_;
        const u8* p = static_cast<const u8*>(buf);
        b.data.insert(b.data.end(), p, p + n);
    }
    IoNotify();
    return static_cast<s64>(n);
}

class PipeRead : public FileObject {
public:
    explicit PipeRead(std::shared_ptr<StreamBuf> b) : b_(std::move(b)) {}
    ~PipeRead() override {
        {
            std::lock_guard lock(b_->mu);
            b_->reader_closed = true;
        }
        IoNotify();
    }
    s64 Read(void* buf, u64 n) override { return StreamRead(*b_, buf, n, status_flags & O_NONBLOCK_); }
    u32 Poll() override {
        std::lock_guard lock(b_->mu);
        u32 ev = 0;
        if (!b_->data.empty())
            ev |= kPollIn | kPollRdNorm;
        if (b_->writer_closed)
            ev |= kPollHup;
        return ev;
    }
    int Stat(lx::Stat* st) override {
        FileObject::Stat(st);
        st->st_mode = S_IFIFO_ | 0600;
        return 0;
    }

private:
    std::shared_ptr<StreamBuf> b_;
};

class PipeWrite : public FileObject {
public:
    explicit PipeWrite(std::shared_ptr<StreamBuf> b) : b_(std::move(b)) {}
    ~PipeWrite() override {
        {
            std::lock_guard lock(b_->mu);
            b_->writer_closed = true;
        }
        IoNotify();
    }
    s64 Write(const void* buf, u64 n) override { return StreamWrite(*b_, buf, n); }
    u32 Poll() override {
        std::lock_guard lock(b_->mu);
        return b_->reader_closed ? (kPollErr | kPollOut) : (kPollOut | kPollWrNorm);
    }
    int Stat(lx::Stat* st) override {
        FileObject::Stat(st);
        st->st_mode = S_IFIFO_ | 0600;
        return 0;
    }

private:
    std::shared_ptr<StreamBuf> b_;
};

// eventfd ----------------------------------------------------------------------

class EventFdFile : public FileObject {
public:
    EventFdFile(u64 init, bool semaphore) : count_(init), semaphore_(semaphore) {}
    s64 Read(void* buf, u64 n) override {
        if (n < 8)
            return -EINVAL_;
        GuestThread* t = CurrentThread();
        for (;;) {
            u64 gen = IoGeneration();
            {
                std::lock_guard lock(mu_);
                if (count_) {
                    u64 v = semaphore_ ? 1 : count_;
                    count_ -= v;
                    if (!SafeCopyToGuest(reinterpret_cast<u64>(buf), &v, 8))
                        return -EFAULT_;
                    IoNotify();
                    return 8;
                }
            }
            if ((status_flags & O_NONBLOCK_) || !t)
                return -EAGAIN_;
            if (!IoWait(t, gen, -1))
                return -EINTR_;
        }
    }
    s64 Write(const void* buf, u64 n) override {
        if (n < 8)
            return -EINVAL_;
        u64 v;
        memcpy(&v, buf, 8);
        if (v == ~0ull)
            return -EINVAL_;
        {
            std::lock_guard lock(mu_);
            count_ += v;
        }
        IoNotify();
        return 8;
    }
    u32 Poll() override {
        std::lock_guard lock(mu_);
        return (count_ ? (kPollIn | kPollRdNorm) : 0) | kPollOut | kPollWrNorm;
    }

private:
    std::mutex mu_;
    u64 count_;
    bool semaphore_;
};

// epoll ------------------------------------------------------------------------

class EpollFile : public FileObject {
public:
    struct Item {
        std::weak_ptr<FileObject> file;
        u32 events;
        u64 data;
        bool disabled;
    };
    std::mutex mu;
    std::map<int, Item> items;
};

// memfd ------------------------------------------------------------------------

class MemFdFile : public FileObject {
public:
    s64 Read(void* buf, u64 n) override {
        std::lock_guard lock(mu_);
        s64 r = PreadLocked(buf, n, pos_);
        if (r > 0)
            pos_ += r;
        return r;
    }
    s64 Write(const void* buf, u64 n) override {
        std::lock_guard lock(mu_);
        s64 r = PwriteLocked(buf, n, pos_);
        if (r > 0)
            pos_ += r;
        return r;
    }
    s64 Pread(void* buf, u64 n, u64 off) override {
        std::lock_guard lock(mu_);
        return PreadLocked(buf, n, off);
    }
    s64 Pwrite(const void* buf, u64 n, u64 off) override {
        std::lock_guard lock(mu_);
        return PwriteLocked(buf, n, off);
    }
    s64 Seek(s64 off, int whence) override {
        std::lock_guard lock(mu_);
        s64 base = whence == 0 ? 0 : whence == 1 ? static_cast<s64>(pos_) : static_cast<s64>(data_.size());
        if (base + off < 0)
            return -EINVAL_;
        pos_ = base + off;
        return static_cast<s64>(pos_);
    }
    int Truncate(u64 len) override {
        std::lock_guard lock(mu_);
        data_.resize(len);
        return 0;
    }
    int Stat(lx::Stat* st) override {
        FileObject::Stat(st);
        std::lock_guard lock(mu_);
        st->st_size = static_cast<s64>(data_.size());
        return 0;
    }

private:
    s64 PreadLocked(void* buf, u64 n, u64 off) {
        if (off >= data_.size())
            return 0;
        u64 take = std::min<u64>(n, data_.size() - off);
        // buf may be guest memory or (for mmap) freshly committed host memory.
        if (!SafeCopyToGuest(reinterpret_cast<u64>(buf), data_.data() + off, take))
            return -EFAULT_;
        return static_cast<s64>(take);
    }
    s64 PwriteLocked(const void* buf, u64 n, u64 off) {
        if (off + n > data_.size())
            data_.resize(off + n);
        memcpy(data_.data() + off, buf, n);
        return static_cast<s64>(n);
    }
    std::mutex mu_;
    std::vector<u8> data_;
    u64 pos_ = 0;
};

// Sockets ----------------------------------------------------------------------

constexpr int AF_UNIX_ = 1, AF_INET_ = 2, AF_INET6_ = 10, AF_NETLINK_ = 16;
constexpr int SOCK_STREAM_ = 1, SOCK_DGRAM_ = 2, SOCK_SEQPACKET_ = 5;
constexpr int SOCK_NONBLOCK_ = 04000, SOCK_CLOEXEC_ = 02000000;

class SocketFile : public FileObject {
public:
    SocketFile(int domain, int type) : domain_(domain), type_(type & 0xf) {}
    ~SocketFile() override {
        if (out_) {
            std::lock_guard lock(out_->mu);
            out_->writer_closed = true;
        }
        if (in_) {
            std::lock_guard lock(in_->mu);
            in_->reader_closed = true;
        }
        IoNotify();
    }

    void ConnectPair(std::shared_ptr<StreamBuf> in, std::shared_ptr<StreamBuf> out) {
        in_ = std::move(in);
        out_ = std::move(out);
    }
    void MakeLogd() { logd_ = true; }
    bool connected() const { return logd_ || in_ || out_; }

    s64 Read(void* buf, u64 n) override {
        if (!in_)
            return logd_ ? -EAGAIN_ : -ENOTCONN_;
        return StreamRead(*in_, buf, n, status_flags & O_NONBLOCK_);
    }
    s64 Write(const void* buf, u64 n) override {
        if (logd_)
            return LogdWrite(static_cast<const u8*>(buf), n);
        if (!out_)
            return -ENOTCONN_;
        return StreamWrite(*out_, buf, n);
    }
    u32 Poll() override {
        u32 ev = 0;
        if (in_) {
            std::lock_guard lock(in_->mu);
            if (!in_->data.empty())
                ev |= kPollIn | kPollRdNorm;
            if (in_->writer_closed)
                ev |= kPollHup | kPollIn;
        }
        if (out_ || logd_)
            ev |= kPollOut | kPollWrNorm;
        return ev;
    }
    int Stat(lx::Stat* st) override {
        FileObject::Stat(st);
        st->st_mode = S_IFSOCK_ | 0777;
        return 0;
    }

private:
    s64 LogdWrite(const u8* p, u64 n) {
        // android_log_header_t {u8 id; u16 tid; u32 sec; u32 nsec} then prio, tag\0, msg\0
        if (n < 12)
            return static_cast<s64>(n);
        u8 id = p[0];
        u16 tid;
        memcpy(&tid, p + 1, 2);
        if (id == 2 || id == 5 || id == 6)  // events / stats / security: binary
            return static_cast<s64>(n);
        const u8* body = p + 11;
        u64 blen = n - 11;
        int prio = body[0];
        const char* tag = reinterpret_cast<const char*>(body + 1);
        size_t taglen = strnlen(tag, blen - 1);
        const char* msg = tag + taglen + 1;
        size_t msglen = (taglen + 2 <= blen) ? strnlen(msg, blen - 2 - taglen) : 0;
        std::string m(msg, msglen);
        while (!m.empty() && (m.back() == '\n' || m.back() == '\r'))
            m.pop_back();
        static const char kPrio[] = "??VDIWEFS";
        char pc = prio >= 0 && prio < 9 ? kPrio[prio] : '?';
        if (prio >= g_min_log_priority)
            Log("%c/%.*s(%u): %s", pc, static_cast<int>(taglen), tag, tid, m.c_str());
        return static_cast<s64>(n);
    }

public:
    static int g_min_log_priority;

private:
    int domain_;
    int type_;
    bool logd_ = false;
    std::shared_ptr<StreamBuf> in_, out_;
};
int SocketFile::g_min_log_priority = 3;  // DEBUG

std::shared_ptr<SocketFile> GetSocket(int fd, s64* err) {
    FilePtr f = Fds().Get(fd);
    if (!f) {
        *err = -EBADF_;
        return nullptr;
    }
    auto s = std::dynamic_pointer_cast<SocketFile>(f);
    if (!s)
        *err = -ENOTSOCK_;
    return s;
}

}  // namespace

s64 SysPipe2(u64 fds_addr, int flags) {
    auto b = std::make_shared<StreamBuf>();
    auto r = std::make_shared<PipeRead>(b);
    auto w = std::make_shared<PipeWrite>(b);
    r->path = "pipe:[read]";
    w->path = "pipe:[write]";
    r->status_flags = O_RDONLY_ | (flags & O_NONBLOCK_);
    w->status_flags = O_WRONLY_ | (flags & O_NONBLOCK_);
    bool cloexec = flags & O_CLOEXEC_;
    int fds[2] = {Fds().Install(r, cloexec), Fds().Install(w, cloexec)};
    if (!SafeCopyToGuest(fds_addr, fds, sizeof(fds)))
        return -EFAULT_;
    return 0;
}

s64 SysEventfd(u32 initval, int flags) {
    auto f = std::make_shared<EventFdFile>(initval, (flags & 1) != 0);
    f->path = "anon_inode:[eventfd]";
    f->status_flags = O_RDWR_ | (flags & O_NONBLOCK_);
    return Fds().Install(f, (flags & O_CLOEXEC_) != 0);
}

s64 SysEpollCreate(int flags) {
    auto f = std::make_shared<EpollFile>();
    f->path = "anon_inode:[eventpoll]";
    return Fds().Install(f, (flags & O_CLOEXEC_) != 0);
}

s64 SysEpollCtl(int epfd, int op, int fd, u64 event_addr) {
    auto ep = std::dynamic_pointer_cast<EpollFile>(Fds().Get(epfd));
    if (!ep)
        return -EINVAL_;
    FilePtr target = Fds().Get(fd);
    if (!target)
        return -EBADF_;
    struct {
        u32 events;
        u32 pad;
        u64 data;
    } ev{};
    if (op != 2 && !SafeCopyFromGuest(&ev, event_addr, sizeof(ev)))
        return -EFAULT_;
    std::lock_guard lock(ep->mu);
    switch (op) {
    case 1:  // ADD
        if (ep->items.count(fd))
            return -EEXIST_;
        ep->items[fd] = {target, ev.events, ev.data, false};
        break;
    case 2:  // DEL
        if (!ep->items.erase(fd))
            return -ENOENT_;
        break;
    case 3:  // MOD
    {
        auto it = ep->items.find(fd);
        if (it == ep->items.end())
            return -ENOENT_;
        it->second = {target, ev.events, ev.data, false};
        break;
    }
    default:
        return -EINVAL_;
    }
    IoNotify();
    return 0;
}

s64 SysEpollWait(GuestThread* t, int epfd, u64 events, int maxevents, int timeout_ms) {
    auto ep = std::dynamic_pointer_cast<EpollFile>(Fds().Get(epfd));
    if (!ep)
        return -EINVAL_;
    if (maxevents <= 0)
        return -EINVAL_;
    s64 deadline = timeout_ms < 0 ? -1 : static_cast<s64>(MonotonicNs()) + timeout_ms * 1000000ll;
    struct Out {
        u32 events;
        u32 pad;
        u64 data;
    };
    for (;;) {
        u64 gen = IoGeneration();
        std::vector<Out> out;
        {
            std::lock_guard lock(ep->mu);
            for (auto it = ep->items.begin(); it != ep->items.end() && static_cast<int>(out.size()) < maxevents;) {
                auto f = it->second.file.lock();
                if (!f) {
                    it = ep->items.erase(it);
                    continue;
                }
                if (!it->second.disabled) {
                    u32 ready = f->Poll() & (it->second.events | kPollErr | kPollHup);
                    if (ready) {
                        out.push_back({ready, 0, it->second.data});
                        if (it->second.events & (1u << 30))  // EPOLLONESHOT
                            it->second.disabled = true;
                    }
                }
                ++it;
            }
        }
        if (!out.empty()) {
            if (!SafeCopyToGuest(events, out.data(), out.size() * sizeof(Out)))
                return -EFAULT_;
            return static_cast<s64>(out.size());
        }
        if (timeout_ms == 0 || (deadline >= 0 && static_cast<s64>(MonotonicNs()) >= deadline))
            return 0;
        if (!IoWait(t, gen, deadline))
            return -EINTR_;
    }
}

s64 SysMemfdCreate(u64 name_addr, int flags) {
    std::string name;
    SafeReadString(name_addr, name, 249);
    auto f = std::make_shared<MemFdFile>();
    f->path = "/memfd:" + name + " (deleted)";
    f->inode = HashInode(f->path + std::to_string(reinterpret_cast<uintptr_t>(f.get())));
    f->status_flags = O_RDWR_;
    return Fds().Install(f, (flags & 1) != 0);  // MFD_CLOEXEC
}

s64 SysSocket(int domain, int type, int protocol) {
    if (domain == AF_INET_ || domain == AF_INET6_)
        return InetSocketCreate(domain, type, protocol);
    if (domain != AF_UNIX_) {
        RN_INFO("socket(domain %d) refused: networking not wired yet", domain);
        return -EACCES_;
    }
    auto s = std::make_shared<SocketFile>(domain, type);
    s->path = "socket:[unix]";
    s->status_flags = O_RDWR_ | ((type & SOCK_NONBLOCK_) ? O_NONBLOCK_ : 0);
    return Fds().Install(s, (type & SOCK_CLOEXEC_) != 0);
}

s64 SysSocketpair(int domain, int type, int protocol, u64 sv) {
    if (domain != AF_UNIX_)
        return -EAFNOSUPPORT_;
    auto ab = std::make_shared<StreamBuf>();
    auto ba = std::make_shared<StreamBuf>();
    auto a = std::make_shared<SocketFile>(domain, type);
    auto b = std::make_shared<SocketFile>(domain, type);
    a->ConnectPair(ba, ab);
    b->ConnectPair(ab, ba);
    a->path = b->path = "socket:[pair]";
    int nb = (type & SOCK_NONBLOCK_) ? O_NONBLOCK_ : 0;
    a->status_flags = b->status_flags = O_RDWR_ | nb;
    bool cloexec = (type & SOCK_CLOEXEC_) != 0;
    int fds[2] = {Fds().Install(a, cloexec), Fds().Install(b, cloexec)};
    return SafeCopyToGuest(sv, fds, sizeof(fds)) ? 0 : -EFAULT_;
}

s64 SysBind(int fd, u64 addr, u32 len) {
    if (InetSocket* in = AsInet(Fds().Get(fd)))
        return InetBind(in, addr, len);
    s64 err = 0;
    return GetSocket(fd, &err) ? 0 : err;
}

s64 SysListen(int fd, int backlog) {
    if (InetSocket* in = AsInet(Fds().Get(fd)))
        return InetListen(in, backlog);
    s64 err = 0;
    return GetSocket(fd, &err) ? 0 : err;
}

s64 SysAccept(GuestThread* t, int fd, u64 addr, u64 len_addr, int flags) {
    if (InetSocket* in = AsInet(Fds().Get(fd)))
        return InetAccept(t, in, addr, len_addr, flags);
    s64 err = 0;
    return GetSocket(fd, &err) ? -EAGAIN_ : err;
}

s64 SysConnect(GuestThread* t, int fd, u64 addr, u32 len) {
    if (InetSocket* in = AsInet(Fds().Get(fd)))
        return InetConnect(t, in, addr, len);
    s64 err = 0;
    auto s = GetSocket(fd, &err);
    if (!s)
        return err;
    if (len < 3)
        return -EINVAL_;
    std::vector<u8> sa(std::min<u32>(len, 110));
    if (!SafeCopyFromGuest(sa.data(), addr, sa.size()))
        return -EFAULT_;
    std::string path(reinterpret_cast<const char*>(sa.data() + 2), strnlen(reinterpret_cast<const char*>(sa.data() + 2), sa.size() - 2));
    if (path == "/dev/socket/logdw") {
        s->MakeLogd();
        return 0;
    }
    if (path == "/dev/socket/dnsproxyd") {
        // netd's DNS proxy: one NUL-terminated command per connection, answered from the host.
        auto to_guest = std::make_shared<StreamBuf>();
        auto from_guest = std::make_shared<StreamBuf>();
        s->ConnectPair(to_guest, from_guest);
        std::thread([to_guest, from_guest] {
            std::string cmd;
            for (u64 deadline = MonotonicNs() + 30000000000ull; MonotonicNs() < deadline;) {
                {
                    std::lock_guard lock(from_guest->mu);
                    auto nul = std::find(from_guest->data.begin(), from_guest->data.end(), u8{0});
                    if (nul != from_guest->data.end()) {
                        cmd.assign(from_guest->data.begin(), nul);
                        break;
                    }
                    if (from_guest->writer_closed)
                        break;
                }
                Sleep(1);
            }
            if (!cmd.empty()) {
                std::vector<u8> reply = DnsProxyReply(cmd);
                StreamWrite(*to_guest, reply.data(), reply.size());
            }
            {
                std::lock_guard lock(to_guest->mu);
                to_guest->writer_closed = true;
            }
            IoNotify();
        }).detach();
        return 0;
    }
    RN_INFO("connect(unix %s) refused", path.c_str());
    return -ECONNREFUSED_;
}

s64 SysGetsockname(int fd, u64 addr, u64 len_addr, bool peer) {
    if (InetSocket* in = AsInet(Fds().Get(fd)))
        return InetName(in, addr, len_addr, peer);
    s64 err = 0;
    if (!GetSocket(fd, &err))
        return err;
    u16 fam = AF_UNIX_;
    u32 len = 2;
    if (addr)
        SafeCopyToGuest(addr, &fam, 2);
    if (len_addr)
        SafeCopyToGuest(len_addr, &len, 4);
    return 0;
}

s64 SysSendto(GuestThread*, int fd, u64 buf, u64 len, int flags, u64 addr, u32 addrlen) {
    if (InetSocket* in = AsInet(Fds().Get(fd)))
        return InetSendto(in, buf, len, flags, addr, addrlen);
    FilePtr f = Fds().Get(fd);
    if (!f)
        return -EBADF_;
    std::vector<u8> tmp(len);
    if (len && !SafeCopyFromGuest(tmp.data(), buf, len))
        return -EFAULT_;
    return f->Write(tmp.data(), len);
}

s64 SysRecvfrom(GuestThread*, int fd, u64 buf, u64 len, int flags, u64 addr, u64 addrlen) {
    if (InetSocket* in = AsInet(Fds().Get(fd)))
        return InetRecvfrom(in, buf, len, flags, addr, addrlen);
    FilePtr f = Fds().Get(fd);
    if (!f)
        return -EBADF_;
    if (addrlen) {
        u32 zero = 0;
        SafeCopyToGuest(addrlen, &zero, 4);
    }
    return f->Read(GuestPtr<void>(buf), len);
}

s64 SysSetsockopt(int fd, int level, int name, u64 val, u32 len) {
    if (InetSocket* in = AsInet(Fds().Get(fd)))
        return InetSetsockopt(in, level, name, val, len);
    s64 err = 0;
    return GetSocket(fd, &err) ? 0 : err;
}

s64 SysGetsockopt(int fd, int level, int name, u64 val, u64 len_addr) {
    if (InetSocket* in = AsInet(Fds().Get(fd)))
        return InetGetsockopt(in, level, name, val, len_addr);
    s64 err = 0;
    if (!GetSocket(fd, &err))
        return err;
    u32 v = 0;
    if (level == 1 && name == 4)  // SO_ERROR
        v = 0;
    else if (level == 1 && (name == 7 || name == 8))  // SO_SNDBUF/SO_RCVBUF
        v = 212992;
    if (val)
        SafeCopyToGuest(val, &v, 4);
    u32 len = 4;
    if (len_addr)
        SafeCopyToGuest(len_addr, &len, 4);
    return 0;
}

s64 SysShutdown(int fd, int how) {
    if (InetSocket* in = AsInet(Fds().Get(fd)))
        return InetShutdown(in, how);
    s64 err = 0;
    return GetSocket(fd, &err) ? 0 : err;
}

s64 SysSendmsg(GuestThread*, int fd, u64 msg, int) {
    FilePtr f = Fds().Get(fd);
    if (!f)
        return -EBADF_;
    u64 hdr[7];
    if (!SafeCopyFromGuest(hdr, msg, sizeof(hdr)))
        return -EFAULT_;
    u64 iov = hdr[2], iovlen = hdr[3];
    if (iovlen > 1024)
        return -EINVAL_;
    std::vector<Iovec> v(iovlen);
    if (iovlen && !SafeCopyFromGuest(v.data(), iov, iovlen * sizeof(Iovec)))
        return -EFAULT_;
    std::vector<u8> buf;
    for (const auto& e : v) {
        size_t at = buf.size();
        buf.resize(at + e.len);
        if (e.len && !SafeCopyFromGuest(buf.data() + at, e.base, e.len))
            return -EFAULT_;
    }
    if (hdr[5])
        RN_INFO("sendmsg with ancillary data (fd passing) is not supported");
    return f->Write(buf.data(), buf.size());
}

s64 SysRecvmsg(GuestThread*, int fd, u64 msg, int) {
    FilePtr f = Fds().Get(fd);
    if (!f)
        return -EBADF_;
    u64 hdr[7];
    if (!SafeCopyFromGuest(hdr, msg, sizeof(hdr)))
        return -EFAULT_;
    u64 iov = hdr[2], iovlen = hdr[3];
    if (iovlen == 0)
        return 0;
    Iovec first;
    if (!SafeCopyFromGuest(&first, iov, sizeof(first)))
        return -EFAULT_;
    s64 r = f->Read(GuestPtr<void>(first.base), first.len);
    // No ancillary data, no address.
    u64 zero = 0;
    SafeCopyToGuest(msg + 40, &zero, 8);
    u32 zero32 = 0;
    SafeCopyToGuest(msg + 8, &zero32, 4);
    SafeCopyToGuest(msg + 48, &zero32, 4);
    return r;
}

}  // namespace rn
