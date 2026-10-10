// Linux arm64 syscalls implemented on Win32.
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "files.h"
#include "kernel.h"
#include "memory.h"
#include "host_runtime.h"
#include "net.h"
#include "trace.h"
#include "vfs.h"

namespace rn {

using namespace lx;

namespace {

constexpr s64 kNoReturn = INT64_MIN;  // syscall already set registers

const char* SyscallName(u64 nr);

bool ReadPath(u64 addr, std::string* out) { return SafeReadString(addr, *out, 4096); }

template <typename T>
bool Put(u64 addr, const T& v) {
    return SafeCopyToGuest(addr, &v, sizeof(T));
}
template <typename T>
bool Get(u64 addr, T* v) {
    return SafeCopyFromGuest(v, addr, sizeof(T));
}

FilePtr FdObj(int fd) { return Fds().Get(fd); }

s64 TimespecToNs(const Timespec& ts) { return ts.tv_sec * 1000000000ll + ts.tv_nsec; }
Timespec NsToTimespec(u64 ns) {
    return Timespec{static_cast<s64>(ns / 1000000000ull), static_cast<s64>(ns % 1000000000ull)};
}

u64 ThreadCpuNs(HANDLE h) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u))
        return 0;
    auto f = [](FILETIME ft) { return ((static_cast<u64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) * 100; };
    return f(k) + f(u);
}

u64 ProcessCpuNs() {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
        return 0;
    auto f = [](FILETIME ft) { return ((static_cast<u64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) * 100; };
    return f(k) + f(u);
}

s64 ClockGettime(int clk, u64 tp) {
    u64 ns;
    switch (clk) {
    case CLOCK_REALTIME_:
    case CLOCK_REALTIME_COARSE_:
    case CLOCK_REALTIME_ALARM_:
        ns = RealtimeNs();
        break;
    case CLOCK_MONOTONIC_:
    case CLOCK_MONOTONIC_RAW_:
    case CLOCK_MONOTONIC_COARSE_:
    case CLOCK_BOOTTIME_:
    case CLOCK_BOOTTIME_ALARM_:
        ns = MonotonicNs();
        break;
    case CLOCK_PROCESS_CPUTIME_ID_:
        ns = ProcessCpuNs();
        break;
    case CLOCK_THREAD_CPUTIME_ID_:
        ns = ThreadCpuNs(GetCurrentThread());
        break;
    default:
        if (clk < 0) {  // per-thread/process CPU clocks encoded by pthread_getcpuclockid
            ns = ThreadCpuNs(GetCurrentThread());
            break;
        }
        return -EINVAL_;
    }
    if (!Put(tp, NsToTimespec(ns)))
        return -EFAULT_;
    return 0;
}

GuestThread* FindThread(int tid) {
    std::lock_guard lock(Proc().threads_mu);
    auto it = Proc().threads.find(tid);
    return it == Proc().threads.end() ? nullptr : it->second;
}

// clone() for threads ---------------------------------------------------------

DWORD WINAPI GuestThreadEntry(LPVOID p) {
    auto* t = static_cast<GuestThread*>(p);
    RunGuestThread(t);
    CloseHandle(static_cast<HANDLE>(t->wake_event));
    CloseHandle(static_cast<HANDLE>(t->sleep_timer));
    if (t->host_handle)
        CloseHandle(static_cast<HANDLE>(t->host_handle));
    delete t;
    return 0;
}

s64 SysClone(GuestThread* t, u64 flags, u64 stack, u64 ptid, u64 tls, u64 ctid) {
    if (!(flags & CLONE_VM_) || !(flags & CLONE_THREAD_)) {
        Log("clone without CLONE_VM|CLONE_THREAD (fork) is not supported (flags 0x%llx)", flags);
        return -ENOSYS_;
    }
    auto* child = new GuestThread;
    child->tid = Proc().next_tid.fetch_add(1);
    child->cpu = CreateCpuCore();
    CpuContext ctx;
    t->cpu->GetContext(&ctx);
    ctx.x[0] = 0;
    if (stack)
        ctx.sp = stack;
    child->cpu->SetContext(ctx);
    child->cpu->Tpidr() = (flags & CLONE_SETTLS_) ? tls : t->cpu->Tpidr();
    child->sigmask = t->sigmask.load();
    if (flags & CLONE_CHILD_CLEARTID_)
        child->clear_child_tid = ctid;
    u32 tid32 = static_cast<u32>(child->tid);
    if (flags & CLONE_PARENT_SETTID_)
        Put(ptid, tid32);
    if (flags & CLONE_CHILD_SETTID_)
        Put(ctid, tid32);
    RegisterThread(child);
    if (t->adopt_token) {
        // Service thread creating a thread for a host thread to adopt: no new host thread.
        u64 token = t->adopt_token;
        t->adopt_token = 0;
        if (CompleteAdoption(token, child))
            return child->tid;
        Log("adoption token %llu has no waiter", token);
    }
    DWORD host_tid;
    // Generous host stack: thunks call into graphics drivers on this thread.
    HANDLE h = CreateThread(nullptr, 8 << 20, GuestThreadEntry, child, STACK_SIZE_PARAM_IS_A_RESERVATION,
                            &host_tid);
    if (!h) {
        Log("CreateThread failed: %lu", GetLastError());
        return -EAGAIN_;
    }
    child->host_handle = h;
    RN_TRACE("clone -> tid %d", child->tid);
    return child->tid;
}

void ThreadExit(GuestThread* t) {
    if (t->clear_child_tid) {
        u32 zero = 0;
        Put(t->clear_child_tid, zero);
        FutexWake(t->clear_child_tid, 1, ~0u);
    }
    t->exiting = true;
    bool last;
    {
        std::lock_guard lock(Proc().threads_mu);
        last = Proc().threads.size() <= 1;
    }
    if (last) {
        RN_INFO("last guest thread (%d) exited", t->tid);
        fflush(stdout);
        ExitProcess(0);
    }
}

// mmap ------------------------------------------------------------------------

s64 SysMmap(u64 addr, u64 len, int prot, int flags, int fd, u64 off) {
    FilePtr f;
    if (!(flags & MAP_ANONYMOUS_)) {
        f = FdObj(fd);
        if (!f)
            return -EBADF_;
        if (f->IsDirectory())
            return -ENODEV_;
    }
    s64 r = Mem().Map(addr, len, prot, flags, f.get(), off, "");
    RN_TRACE("mmap(0x%llx, 0x%llx, %d, 0x%x, %d, 0x%llx) = 0x%llx %s", addr, len, prot, flags, fd, off, r,
             f ? f->path.c_str() : "");
    return r;
}

// misc structs ----------------------------------------------------------------

s64 SysUname(u64 buf) {
    char u[6][65] = {};
    strcpy(u[0], "Linux");
    strcpy(u[1], "localhost");
    strcpy(u[2], "4.19.191-refract");
    strcpy(u[3], "#1 SMP PREEMPT");
    strcpy(u[4], "aarch64");
    strcpy(u[5], "localdomain");
    return SafeCopyToGuest(buf, u, sizeof(u)) ? 0 : -EFAULT_;
}

s64 SysSysinfo(u64 buf) {
    struct {
        s64 uptime;
        u64 loads[3];
        u64 totalram, freeram, sharedram, bufferram, totalswap, freeswap;
        u16 procs;
        u16 pad;
        u32 pad2;
        u64 totalhigh, freehigh;
        u32 mem_unit;
        u32 pad3;
    } si{};
    MEMORYSTATUSEX ms{sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    si.uptime = static_cast<s64>(GetTickCount64() / 1000);
    si.totalram = ms.ullTotalPhys;
    si.freeram = ms.ullAvailPhys;
    si.procs = 300;
    si.mem_unit = 1;
    return SafeCopyToGuest(buf, &si, sizeof(si)) ? 0 : -EFAULT_;
}

s64 SysPrlimit(int resource, u64 new_lim, u64 old_lim) {
    constexpr u64 kInf = ~0ull;
    if (old_lim) {
        u64 v[2] = {kInf, kInf};
        if (resource == 3)  // RLIMIT_STACK
            v[0] = 8 << 20;
        else if (resource == 7)  // RLIMIT_NOFILE
            v[0] = v[1] = 32768;
        else if (resource == 4)  // RLIMIT_CORE
            v[0] = 0;
        if (!SafeCopyToGuest(old_lim, v, 16))
            return -EFAULT_;
    }
    return 0;
}

// I/O -------------------------------------------------------------------------

s64 SysReadv(FilePtr f, u64 iov, int cnt, s64 off, bool positional) {
    if (cnt < 0 || cnt > 1024)
        return -EINVAL_;
    std::vector<Iovec> v(cnt);
    if (cnt && !SafeCopyFromGuest(v.data(), iov, cnt * sizeof(Iovec)))
        return -EFAULT_;
    s64 total = 0;
    for (const auto& e : v) {
        if (!e.len)
            continue;
        s64 r = positional ? f->Pread(GuestPtr<void>(e.base), e.len, off + total)
                           : f->Read(GuestPtr<void>(e.base), e.len);
        if (r < 0)
            return total ? total : r;
        total += r;
        if (static_cast<u64>(r) < e.len)
            break;
    }
    return total;
}

s64 SysWritev(FilePtr f, u64 iov, int cnt, s64 off, bool positional) {
    if (cnt < 0 || cnt > 1024)
        return -EINVAL_;
    std::vector<Iovec> v(cnt);
    if (cnt && !SafeCopyFromGuest(v.data(), iov, cnt * sizeof(Iovec)))
        return -EFAULT_;
    // Gather into one buffer: sockets (logd) need whole datagrams.
    std::vector<u8> buf;
    for (const auto& e : v) {
        size_t at = buf.size();
        buf.resize(at + e.len);
        if (e.len && !SafeCopyFromGuest(buf.data() + at, e.base, e.len))
            return -EFAULT_;
    }
    return positional ? f->Pwrite(buf.data(), buf.size(), off) : f->Write(buf.data(), buf.size());
}

s64 SysFcntl(int fd, int cmd, u64 arg) {
    FilePtr f = FdObj(fd);
    if (!f)
        return -EBADF_;
    switch (cmd) {
    case 0:     // F_DUPFD
    case 1030:  // F_DUPFD_CLOEXEC
        return Fds().Install(f, cmd == 1030, static_cast<int>(arg));
    case 1:  // F_GETFD
        return Fds().GetCloexec(fd) ? 1 : 0;
    case 2:  // F_SETFD
        Fds().SetCloexec(fd, arg & 1);
        return 0;
    case 3:  // F_GETFL
        return f->status_flags | O_LARGEFILE_;
    case 4:  // F_SETFL
        f->status_flags = (f->status_flags & ~(O_APPEND_ | O_NONBLOCK_)) |
                          static_cast<int>(arg & (O_APPEND_ | O_NONBLOCK_));
        return 0;
    case 5:  // F_GETLK
    {
        // struct flock: l_type (short) first; report unlocked.
        u16 unlck = 2;
        Put(arg, unlck);
        return 0;
    }
    case 6:
    case 7:     // F_SETLK(W)
    case 1033:  // F_ADD_SEALS
        return 0;
    case 1034:  // F_GET_SEALS
        return 0;
    case 1031:  // F_SETPIPE_SZ
    case 1032:  // F_GETPIPE_SZ
        return 65536;
    default:
        RN_INFO("fcntl(%d, %d) unsupported", fd, cmd);
        return -EINVAL_;
    }
}

s64 SysGetcwd(u64 buf, u64 size) {
    std::string cwd = Fs().Cwd();
    if (cwd.size() + 1 > size)
        return -ERANGE_;
    if (!SafeCopyToGuest(buf, cwd.c_str(), cwd.size() + 1))
        return -EFAULT_;
    return static_cast<s64>(cwd.size() + 1);
}

s64 SysStatx(int dirfd, const std::string& path, int flags, u64 buf) {
    Stat st;
    int r;
    if (path.empty() && (flags & AT_EMPTY_PATH_)) {
        FilePtr f = FdObj(dirfd);
        if (!f)
            return -EBADF_;
        r = f->Stat(&st);
    } else {
        r = Fs().Stat(dirfd, path, !(flags & AT_SYMLINK_NOFOLLOW_), &st);
    }
    if (r)
        return r;
    u8 sx[256] = {};
    u32 mask = 0x7ff;
    u32 blksize = st.st_blksize;
    u32 nlink = st.st_nlink;
    u16 mode = static_cast<u16>(st.st_mode);
    memcpy(sx + 0, &mask, 4);
    memcpy(sx + 4, &blksize, 4);
    memcpy(sx + 16, &nlink, 4);
    memcpy(sx + 20, &st.st_uid, 4);
    memcpy(sx + 24, &st.st_gid, 4);
    memcpy(sx + 28, &mode, 2);
    memcpy(sx + 32, &st.st_ino, 8);
    memcpy(sx + 40, &st.st_size, 8);
    memcpy(sx + 48, &st.st_blocks, 8);
    auto ts = [&](int off, s64 sec, u64 nsec) {
        u32 n = static_cast<u32>(nsec);
        memcpy(sx + off, &sec, 8);
        memcpy(sx + off + 8, &n, 4);
    };
    ts(64, st.st_atime_sec, st.st_atime_nsec);
    ts(80, st.st_ctime_sec, st.st_ctime_nsec);  // btime
    ts(96, st.st_ctime_sec, st.st_ctime_nsec);
    ts(112, st.st_mtime_sec, st.st_mtime_nsec);
    u32 dev_major = 254, dev_minor = 42;
    memcpy(sx + 136, &dev_major, 4);
    memcpy(sx + 140, &dev_minor, 4);
    return SafeCopyToGuest(buf, sx, sizeof(sx)) ? 0 : -EFAULT_;
}

s64 SysPpoll(GuestThread* t, u64 fds_addr, u64 nfds, u64 tsp) {
    struct PollFd {
        s32 fd;
        s16 events;
        s16 revents;
    };
    std::vector<PollFd> fds(nfds);
    if (nfds && !SafeCopyFromGuest(fds.data(), fds_addr, nfds * sizeof(PollFd)))
        return -EFAULT_;
    s64 deadline = -1;
    if (tsp) {
        Timespec ts;
        if (!Get(tsp, &ts))
            return -EFAULT_;
        deadline = static_cast<s64>(MonotonicNs()) + TimespecToNs(ts);
    }
    for (;;) {
        u64 gen = IoGeneration();
        int ready = 0;
        for (auto& p : fds) {
            p.revents = 0;
            if (p.fd < 0)
                continue;
            FilePtr f = FdObj(p.fd);
            if (!f) {
                p.revents = static_cast<s16>(kPollNval);
                ++ready;
                continue;
            }
            u32 ev = f->Poll();
            p.revents = static_cast<s16>(ev & (static_cast<u16>(p.events) | kPollErr | kPollHup));
            if (p.revents)
                ++ready;
        }
        if (ready || (deadline >= 0 && static_cast<s64>(MonotonicNs()) >= deadline)) {
            if (nfds)
                SafeCopyToGuest(fds_addr, fds.data(), nfds * sizeof(PollFd));
            return ready;
        }
        if (!IoWait(t, gen, deadline))
            return -EINTR_;
    }
}

s64 SysPselect(GuestThread* t, int nfds, u64 rd, u64 wr, u64 ex, u64 tsp) {
    // Implemented on top of ppoll's readiness checks.
    auto load = [](u64 a, std::vector<u64>& v, int n) {
        v.assign((n + 63) / 64, 0);
        return !a || SafeCopyFromGuest(v.data(), a, v.size() * 8);
    };
    std::vector<u64> r, w, e;
    if (!load(rd, r, nfds) || !load(wr, w, nfds) || !load(ex, e, nfds))
        return -EFAULT_;
    s64 deadline = -1;
    if (tsp) {
        Timespec ts;
        if (!Get(tsp, &ts))
            return -EFAULT_;
        deadline = static_cast<s64>(MonotonicNs()) + TimespecToNs(ts);
    }
    for (;;) {
        u64 gen = IoGeneration();
        std::vector<u64> ro(r.size()), wo(w.size()), eo(e.size());
        int ready = 0;
        for (int fd = 0; fd < nfds; ++fd) {
            u64 bit = 1ull << (fd % 64);
            bool want_r = rd && (r[fd / 64] & bit), want_w = wr && (w[fd / 64] & bit);
            if (!want_r && !want_w)
                continue;
            FilePtr f = FdObj(fd);
            if (!f)
                return -EBADF_;
            u32 ev = f->Poll();
            if (want_r && (ev & (kPollIn | kPollHup))) {
                ro[fd / 64] |= bit;
                ++ready;
            }
            if (want_w && (ev & kPollOut)) {
                wo[fd / 64] |= bit;
                ++ready;
            }
        }
        if (ready || (deadline >= 0 && static_cast<s64>(MonotonicNs()) >= deadline)) {
            if (rd)
                SafeCopyToGuest(rd, ro.data(), ro.size() * 8);
            if (wr)
                SafeCopyToGuest(wr, wo.data(), wo.size() * 8);
            if (ex)
                SafeCopyToGuest(ex, eo.data(), eo.size() * 8);
            return ready;
        }
        if (!IoWait(t, gen, deadline))
            return -EINTR_;
    }
}

}  // namespace

void HandleSyscall(GuestThread* t) {
    CpuCore& c = *t->cpu;
    const u64 nr = c.X(8);
    const u64 a0 = c.X(0), a1 = c.X(1), a2 = c.X(2), a3 = c.X(3), a4 = c.X(4), a5 = c.X(5);
    s64 r = -ENOSYS_;
    std::string p1, p2;
    const int fd0 = static_cast<int>(a0);

    if (g_verbose >= 3 && (nr == 98 || nr == 73 || nr == 21 || nr == 22 || nr == 63 || nr == 101 || nr == 115 ||
                           nr == 207 || nr == 212 || nr == 242 || nr == 260))
    {
        std::string bt = DescribeAddress(c.X(30));
        u64 fp = c.X(29);
        for (int i = 0; i < 6 && fp; ++i) {
            u64 fr[2];
            if (!SafeCopyFromGuest(fr, fp, 16)) break;
            bt += " < " + DescribeAddress(fr[1]);
            fp = fr[0];
        }
        Log("enter %s(0x%llx, 0x%llx, 0x%llx) tid %d lr %s", SyscallName(nr), a0, a1, a2, t->tid, bt.c_str());
    }

    switch (nr) {
    // --- files ---
    case 56:  // openat
        if (!ReadPath(a1, &p1)) {
            r = -EFAULT_;
            break;
        }
        {
            FilePtr f;
            r = Fs().Open(static_cast<int>(a0), p1, static_cast<int>(a2), static_cast<u32>(a3), &f);
            if (r == 0)
                r = Fds().Install(f, (a2 & O_CLOEXEC_) != 0);
            RN_TRACE("openat(%s, 0x%llx) = %lld", p1.c_str(), a2, r);
        }
        break;
    case 57:  // close
        r = Fds().Close(fd0);
        break;
    case 63: {  // read
        FilePtr f = FdObj(fd0);
        r = f ? f->Read(GuestPtr<void>(a1), a2) : -EBADF_;
        break;
    }
    case 64: {  // write
        FilePtr f = FdObj(fd0);
        if (!f) {
            r = -EBADF_;
            break;
        }
        std::vector<u8> buf(a2);
        if (a2 && !SafeCopyFromGuest(buf.data(), a1, a2)) {
            r = -EFAULT_;
            break;
        }
        r = f->Write(buf.data(), a2);
        break;
    }
    case 65:  // readv
    case 69:  // preadv
    {
        FilePtr f = FdObj(fd0);
        r = f ? SysReadv(f, a1, static_cast<int>(a2), static_cast<s64>(a3), nr == 69) : -EBADF_;
        break;
    }
    case 66:  // writev
    case 70:  // pwritev
    {
        FilePtr f = FdObj(fd0);
        r = f ? SysWritev(f, a1, static_cast<int>(a2), static_cast<s64>(a3), nr == 70) : -EBADF_;
        break;
    }
    case 67: {  // pread64
        FilePtr f = FdObj(fd0);
        r = f ? f->Pread(GuestPtr<void>(a1), a2, a3) : -EBADF_;
        break;
    }
    case 68: {  // pwrite64
        FilePtr f = FdObj(fd0);
        if (!f) {
            r = -EBADF_;
            break;
        }
        std::vector<u8> buf(a2);
        if (a2 && !SafeCopyFromGuest(buf.data(), a1, a2)) {
            r = -EFAULT_;
            break;
        }
        r = f->Pwrite(buf.data(), a2, a3);
        break;
    }
    case 62: {  // lseek
        FilePtr f = FdObj(fd0);
        r = f ? f->Seek(static_cast<s64>(a1), static_cast<int>(a2)) : -EBADF_;
        break;
    }
    case 80: {  // fstat
        FilePtr f = FdObj(fd0);
        if (!f) {
            r = -EBADF_;
            break;
        }
        Stat st;
        r = f->Stat(&st);
        if (!st.st_ino)
            st.st_ino = f->inode;
        if (r == 0 && !Put(a1, st))
            r = -EFAULT_;
        break;
    }
    case 79: {  // newfstatat
        if (!ReadPath(a1, &p1)) {
            r = -EFAULT_;
            break;
        }
        Stat st;
        if (p1.empty() && (a3 & AT_EMPTY_PATH_)) {
            FilePtr f = FdObj(fd0);
            r = f ? f->Stat(&st) : -EBADF_;
        } else {
            r = Fs().Stat(static_cast<int>(a0), p1, !(a3 & AT_SYMLINK_NOFOLLOW_), &st);
        }
        if (r == 0 && !Put(a2, st))
            r = -EFAULT_;
        RN_TRACE("fstatat(%s) = %lld", p1.c_str(), r);
        break;
    }
    case 291:  // statx
        if (!ReadPath(a1, &p1)) {
            r = -EFAULT_;
            break;
        }
        r = SysStatx(static_cast<int>(a0), p1, static_cast<int>(a2), a4);
        break;
    case 48:   // faccessat
    case 439:  // faccessat2
        if (!ReadPath(a1, &p1)) {
            r = -EFAULT_;
            break;
        }
        r = Fs().Access(static_cast<int>(a0), p1, static_cast<int>(a2));
        RN_TRACE("faccessat(%s) = %lld", p1.c_str(), r);
        break;
    case 78: {  // readlinkat
        if (!ReadPath(a1, &p1)) {
            r = -EFAULT_;
            break;
        }
        std::string target;
        r = Fs().Readlink(static_cast<int>(a0), p1, &target);
        if (r == 0) {
            size_t n = std::min<size_t>(target.size(), a3);
            r = SafeCopyToGuest(a2, target.data(), n) ? static_cast<s64>(n) : -EFAULT_;
        }
        RN_TRACE("readlinkat(%s) = %lld %s", p1.c_str(), r, target.c_str());
        break;
    }
    case 61: {  // getdents64
        FilePtr f = FdObj(fd0);
        r = f ? f->Getdents(GuestPtr<void>(a1), a2) : -EBADF_;
        break;
    }
    case 17:  // getcwd
        r = SysGetcwd(a0, a1);
        break;
    case 49:  // chdir
        r = ReadPath(a0, &p1) ? Fs().Chdir(p1) : -EFAULT_;
        break;
    case 50: {  // fchdir
        FilePtr f = FdObj(fd0);
        r = f ? Fs().Chdir(f->path) : -EBADF_;
        break;
    }
    case 34:  // mkdirat
        r = ReadPath(a1, &p1) ? Fs().Mkdir(static_cast<int>(a0), p1, static_cast<u32>(a2)) : -EFAULT_;
        break;
    case 35:  // unlinkat
        r = ReadPath(a1, &p1) ? Fs().Unlink(static_cast<int>(a0), p1, static_cast<int>(a2)) : -EFAULT_;
        break;
    case 38:   // renameat
    case 276:  // renameat2
        if (!ReadPath(a1, &p1) || !ReadPath(a3, &p2)) {
            r = -EFAULT_;
            break;
        }
        r = Fs().Rename(static_cast<int>(a0), p1, static_cast<int>(a2), p2);
        break;
    case 36:  // symlinkat
    case 37:  // linkat
        r = -EPERM_;
        break;
    case 46: {  // ftruncate
        FilePtr f = FdObj(fd0);
        r = f ? f->Truncate(a1) : -EBADF_;
        break;
    }
    case 45:  // truncate
        if (!ReadPath(a0, &p1)) {
            r = -EFAULT_;
            break;
        }
        {
            FilePtr f;
            r = Fs().Open(AT_FDCWD_, p1, O_WRONLY_, 0, &f);
            if (r == 0)
                r = f->Truncate(a1);
        }
        break;
    case 82:  // fsync
    case 83:  // fdatasync
    {
        FilePtr f = FdObj(fd0);
        r = f ? f->Sync() : -EBADF_;
        break;
    }
    case 81:  // sync
    case 84:  // sync_file_range
    case 32:  // flock
    case 52:  // fchmod
    case 53:  // fchmodat
    case 54:  // fchownat
    case 55:  // fchown
    case 88:  // utimensat
    case 47:  // fallocate
    case 223: // fadvise64
        r = 0;
        break;
    case 166:  // umask
        r = 077;
        break;
    case 23: {  // dup
        FilePtr f = FdObj(fd0);
        r = f ? Fds().Install(f, false) : -EBADF_;
        break;
    }
    case 24: {  // dup3
        FilePtr f = FdObj(fd0);
        if (!f)
            r = -EBADF_;
        else if (a0 == a1)
            r = -EINVAL_;
        else
            r = Fds().InstallAt(static_cast<int>(a1), f, (a2 & O_CLOEXEC_) != 0);
        break;
    }
    case 25:  // fcntl
        r = SysFcntl(fd0, static_cast<int>(a1), a2);
        break;
    case 29: {  // ioctl
        FilePtr f = FdObj(fd0);
        r = f ? f->Ioctl(a1, a2) : -EBADF_;
        break;
    }
    case 43:  // statfs
    case 44:  // fstatfs
    {
        Statfs sf{};
        sf.f_type = 0xEF53;  // ext4
        sf.f_bsize = 4096;
        sf.f_blocks = 64ull << 20;
        sf.f_bfree = sf.f_bavail = 32ull << 20;
        sf.f_files = 1 << 20;
        sf.f_ffree = 1 << 19;
        sf.f_namelen = 255;
        sf.f_frsize = 4096;
        r = Put(nr == 43 ? a1 : a1, sf) ? 0 : -EFAULT_;
        break;
    }
    case 59:  // pipe2
        r = SysPipe2(a0, static_cast<int>(a1));
        break;
    case 19:  // eventfd2
        r = SysEventfd(static_cast<u32>(a0), static_cast<int>(a1));
        break;
    case 20:  // epoll_create1
        r = SysEpollCreate(static_cast<int>(a0));
        break;
    case 21:  // epoll_ctl
        r = SysEpollCtl(static_cast<int>(a0), static_cast<int>(a1), static_cast<int>(a2), a3);
        break;
    case 22:  // epoll_pwait
        r = SysEpollWait(t, static_cast<int>(a0), a1, static_cast<int>(a2), static_cast<int>(a3));
        break;
    case 73:  // ppoll
        r = SysPpoll(t, a0, a1, a2);
        break;
    case 72:  // pselect6
        r = SysPselect(t, static_cast<int>(a0), a1, a2, a3, a4);
        break;
    case 279:  // memfd_create
        r = SysMemfdCreate(a0, static_cast<int>(a1));
        break;
    case 26:  // inotify_init1
    case 85:  // timerfd_create
        r = -ENOSYS_;
        break;

    // --- sockets ---
    case 198: r = SysSocket(static_cast<int>(a0), static_cast<int>(a1), static_cast<int>(a2)); break;
    case 199: r = SysSocketpair(static_cast<int>(a0), static_cast<int>(a1), static_cast<int>(a2), a3); break;
    case 200: r = SysBind(fd0, a1, static_cast<u32>(a2)); break;
    case 201: r = SysListen(fd0, static_cast<int>(a1)); break;
    case 202:
    case 242: r = SysAccept(t, fd0, a1, a2, nr == 242 ? static_cast<int>(a3) : 0); break;
    case 203: r = SysConnect(t, fd0, a1, static_cast<u32>(a2)); break;
    case 204: r = SysGetsockname(fd0, a1, a2, false); break;
    case 205: r = SysGetsockname(fd0, a1, a2, true); break;
    case 206: r = SysSendto(t, fd0, a1, a2, static_cast<int>(a3), a4, static_cast<u32>(a5)); break;
    case 207: r = SysRecvfrom(t, fd0, a1, a2, static_cast<int>(a3), a4, a5); break;
    case 208: r = SysSetsockopt(fd0, static_cast<int>(a1), static_cast<int>(a2), a3, static_cast<u32>(a4)); break;
    case 209: r = SysGetsockopt(fd0, static_cast<int>(a1), static_cast<int>(a2), a3, a4); break;
    case 210: r = SysShutdown(fd0, static_cast<int>(a1)); break;
    case 211: r = SysSendmsg(t, fd0, a1, static_cast<int>(a2)); break;
    case 212: r = SysRecvmsg(t, fd0, a1, static_cast<int>(a2)); break;

    // --- memory ---
    case 222:
        r = SysMmap(a0, a1, static_cast<int>(a2), static_cast<int>(a3), static_cast<int>(a4), a5);
        if (r >= 0 && (a2 & lx::PROT_EXEC_))
            TraceOnExecMapping();
        break;
    case 215:
        r = Mem().Unmap(a0, a1);
        RN_TRACE("munmap(0x%llx, 0x%llx) = %lld", a0, a1, r);
        break;
    case 226:
        r = Mem().Protect(a0, a1, static_cast<int>(a2));
        RN_TRACE("mprotect(0x%llx, 0x%llx, %lld) = %lld", a0, a1, a2, r);
        if (r >= 0 && (a2 & lx::PROT_EXEC_))
            TraceOnExecMapping();
        break;
    case 216:
        r = Mem().Remap(a0, a1, a2, static_cast<int>(a3), a4);
        break;
    case 233:
        r = Mem().Advise(a0, a1, static_cast<int>(a2));
        break;
    case 232: {  // mincore
        std::vector<u8> v(PageUp(a1) / kPageSize, 1);
        r = SafeCopyToGuest(a2, v.data(), v.size()) ? 0 : -EFAULT_;
        break;
    }
    case 214:  // brk
        r = -ENOMEM_;
        break;
    case 227:  // msync
    case 228:  // mlock
    case 229:  // munlock
    case 230:  // mlockall
    case 231:  // munlockall
    case 284:  // mlock2
        r = 0;
        break;

    // --- threads / process ---
    case 220:  // clone
        r = SysClone(t, a0, a1, a2, a3, a4);
        break;
    case 435:  // clone3: let libc fall back to clone
        r = -ENOSYS_;
        break;
    case 93:  // exit
        ThreadExit(t);
        r = 0;
        break;
    case 94:  // exit_group
        RN_INFO("guest exit_group(%lld)", static_cast<s64>(a0));
        fflush(stdout);
        fflush(stderr);
        ExitProcess(static_cast<UINT>(a0 & 0xff));
    case 96:  // set_tid_address
        t->clear_child_tid = a0;
        r = t->tid;
        break;
    case 99:   // set_robust_list
        r = 0;
        break;
    case 100:  // get_robust_list
        r = -ENOSYS_;
        break;
    case 98: {  // futex
        int op = static_cast<int>(a1) & 127;
        bool realtime = (a1 & FUTEX_CLOCK_REALTIME_) != 0;
        switch (op) {
        case FUTEX_WAIT_:
        case FUTEX_WAIT_BITSET_: {
            s64 deadline = -1;
            if (a3) {
                Timespec ts;
                if (!Get(a3, &ts)) {
                    r = -EFAULT_;
                    break;
                }
                if (op == FUTEX_WAIT_) {
                    deadline = static_cast<s64>(MonotonicNs()) + TimespecToNs(ts);
                    realtime = false;
                } else {
                    deadline = TimespecToNs(ts);
                }
            }
            r = FutexWait(t, a0, static_cast<u32>(a2), op == FUTEX_WAIT_ ? ~0u : static_cast<u32>(a5),
                          deadline, realtime);
            break;
        }
        case FUTEX_WAKE_:
            r = FutexWake(a0, static_cast<int>(a2), ~0u);
            break;
        case FUTEX_WAKE_BITSET_:
            r = FutexWake(a0, static_cast<int>(a2), static_cast<u32>(a5));
            break;
        case FUTEX_REQUEUE_:
            r = FutexRequeue(a0, static_cast<int>(a2), a4, static_cast<int>(a3), false, 0);
            break;
        case FUTEX_CMP_REQUEUE_:
            r = FutexRequeue(a0, static_cast<int>(a2), a4, static_cast<int>(a3), true,
                             static_cast<u32>(a5));
            break;
        case FUTEX_WAKE_OP_:
            r = FutexWakeOp(a0, static_cast<int>(a2), a4, static_cast<int>(a3), static_cast<u32>(a5));
            break;
        default:
            Log("futex op %d unsupported", op);
            r = -ENOSYS_;
        }
        break;
    }
    case 172:  // getpid
        r = Proc().pid;
        break;
    case 173:  // getppid
        r = 1;
        break;
    case 178:  // gettid
        r = t->tid;
        break;
    case 174:  // getuid
    case 175:  // geteuid
    case 176:  // getgid
    case 177:  // getegid
        r = 10100;
        break;
    case 148:  // getresuid
    case 150:  // getresgid
    {
        u32 id = 10100;
        r = (Put(a0, id) && Put(a1, id) && Put(a2, id)) ? 0 : -EFAULT_;
        break;
    }
    case 158:  // getgroups
        r = 0;
        break;
    case 154:  // setpgid
    case 157:  // setsid
    case 146:  // setuid etc.
    case 147:
    case 143:
    case 144:
        r = 0;
        break;
    case 155:  // getpgid
    case 156:  // getsid
        r = Proc().pid;
        break;
    case 160:
        r = SysUname(a0);
        break;
    case 179:
        r = SysSysinfo(a0);
        break;
    case 261:  // prlimit64
        r = SysPrlimit(static_cast<int>(a1), a2, a3);
        break;
    case 163:  // getrlimit
        r = SysPrlimit(static_cast<int>(a0), 0, a1);
        break;
    case 164:  // setrlimit
        r = 0;
        break;
    case 165: {  // getrusage
        u8 ru[144] = {};
        r = SafeCopyToGuest(a1, ru, sizeof(ru)) ? 0 : -EFAULT_;
        break;
    }
    case 167: {  // prctl
        int option = static_cast<int>(a0);
        switch (option) {
        case 15: {  // PR_SET_NAME
            std::string name;
            SafeReadString(a1, name, 16);
            t->name = name;
            SetThreadDescription(GetCurrentThread(), Widen("guest:" + name).c_str());
            r = 0;
            break;
        }
        case 16: {  // PR_GET_NAME
            char buf[16] = {};
            strncpy(buf, t->name.c_str(), 15);
            r = SafeCopyToGuest(a1, buf, 16) ? 0 : -EFAULT_;
            break;
        }
        case 0x53564d41:  // PR_SET_VMA
            if (a1 == 0) {  // PR_SET_VMA_ANON_NAME
                std::string name;
                if (a4)
                    SafeReadString(a4, name, 80);
                Mem().SetName(a2, a3, name);
            }
            r = 0;
            break;
        case 3:  // PR_GET_DUMPABLE
            r = 1;
            break;
        case 1:   // PR_SET_PDEATHSIG
        case 4:   // PR_SET_DUMPABLE
        case 8:   // PR_SET_KEEPCAPS
        case 22:  // PR_SET_SECCOMP
        case 29:  // PR_SET_TIMERSLACK
        case 36:  // PR_SET_CHILD_SUBREAPER
        case 38:  // PR_SET_NO_NEW_PRIVS
        case 23:  // PR_CAPBSET_READ
            r = 0;
            break;
        default:
            // PR_SET_TAGGED_ADDR_CTRL (55), PR_PAC_* and MTE: not supported, so libc
            // keeps pointers untagged.
            r = -EINVAL_;
            break;
        }
        break;
    }
    case 123: {  // sched_getaffinity
        int n = Proc().ncpus;
        u64 mask = n >= 64 ? ~0ull : ((1ull << n) - 1);
        if (a1 < 8) {
            r = -EINVAL_;
            break;
        }
        r = Put(a2, mask) ? 8 : -EFAULT_;
        break;
    }
    case 122:  // sched_setaffinity
    case 119:  // sched_setscheduler
    case 118:  // sched_setparam
    case 140:  // setpriority
        r = 0;
        break;
    case 120:  // sched_getscheduler
        r = 0;
        break;
    case 121: {  // sched_getparam
        s32 prio = 0;
        r = Put(a1, prio) ? 0 : -EFAULT_;
        break;
    }
    case 141:  // getpriority (kernel returns 20 - nice)
        r = 20;
        break;
    case 125:  // sched_get_priority_max
    case 126:  // sched_get_priority_min
        r = (a0 == 1 || a0 == 2) ? (nr == 125 ? 99 : 1) : 0;
        break;
    case 124:  // sched_yield
        SwitchToThread();
        r = 0;
        break;
    case 168: {  // getcpu
        u32 cpu = GetCurrentProcessorNumber() % Proc().ncpus, node = 0;
        if (a0)
            Put(a0, cpu);
        if (a1)
            Put(a1, node);
        r = 0;
        break;
    }
    case 283:  // membarrier
        r = a0 == 0 ? 0 : 0;
        break;
    case 278: {  // getrandom
        std::vector<u8> buf(std::min<u64>(a1, 1 << 20));
        BCryptGenRandom(nullptr, buf.data(), static_cast<ULONG>(buf.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        r = SafeCopyToGuest(a0, buf.data(), buf.size()) ? static_cast<s64>(buf.size()) : -EFAULT_;
        break;
    }
    case 90:   // capget
    {
        u8 data[24] = {};
        if (a1)
            SafeCopyToGuest(a1, data, sizeof(data));
        r = 0;
        break;
    }
    case 91:   // capset
        r = 0;
        break;
    case 117:  // ptrace
        r = -EPERM_;
        break;
    case 221:  // execve
    case 260:  // wait4
    case 95:   // waitid
        r = nr == 221 ? -EACCES_ : -ECHILD_;
        break;

    // --- time ---
    case 113:
        r = ClockGettime(static_cast<int>(a0), a1);
        break;
    case 114: {  // clock_getres
        Timespec ts{0, 1};
        r = (!a1 || Put(a1, ts)) ? 0 : -EFAULT_;
        break;
    }
    case 169: {  // gettimeofday
        u64 ns = RealtimeNs();
        s64 tv[2] = {static_cast<s64>(ns / 1000000000), static_cast<s64>((ns / 1000) % 1000000)};
        r = (!a0 || SafeCopyToGuest(a0, tv, 16)) ? 0 : -EFAULT_;
        break;
    }
    case 101:  // nanosleep
    case 115:  // clock_nanosleep
    {
        u64 req = nr == 101 ? a0 : a2;
        Timespec ts;
        if (!Get(req, &ts)) {
            r = -EFAULT_;
            break;
        }
        u64 deadline;
        if (nr == 115 && (a1 & 1)) {  // TIMER_ABSTIME
            if (a0 == CLOCK_REALTIME_) {
                s64 delta = TimespecToNs(ts) - static_cast<s64>(RealtimeNs());
                deadline = MonotonicNs() + (delta > 0 ? delta : 0);
            } else {
                deadline = TimespecToNs(ts);
            }
        } else {
            deadline = MonotonicNs() + TimespecToNs(ts);
        }
        r = SleepUntil(t, deadline) ? 0 : -EINTR_;
        break;
    }
    case 153: {  // times
        if (a0) {
            u64 tms[4] = {ProcessCpuNs() / 10000000, 0, 0, 0};
            SafeCopyToGuest(a0, tms, sizeof(tms));
        }
        r = static_cast<s64>(GetTickCount64() / 10);
        break;
    }
    case 102:  // getitimer
    case 103:  // setitimer
        r = 0;
        break;

    // --- signals ---
    case 134: {  // rt_sigaction
        int sig = static_cast<int>(a0);
        if (sig < 1 || sig > 64 || sig == SIGKILL_ || sig == SIGSTOP_) {
            r = -EINVAL_;
            break;
        }
        std::lock_guard lock(Proc().sig_mu);
        if (a2 && !Put(a2, Proc().sigactions[sig])) {
            r = -EFAULT_;
            break;
        }
        if (a1) {
            KSigaction act;
            if (!Get(a1, &act)) {
                r = -EFAULT_;
                break;
            }
            Proc().sigactions[sig] = act;
            RN_TRACE("sigaction(%d) handler 0x%llx flags 0x%llx", sig, act.handler, act.flags);
        }
        r = 0;
        break;
    }
    case 135: {  // rt_sigprocmask
        u64 old = t->sigmask.load();
        if (a2 && !Put(a2, old)) {
            r = -EFAULT_;
            break;
        }
        if (a1) {
            u64 set;
            if (!Get(a1, &set)) {
                r = -EFAULT_;
                break;
            }
            u64 nm = a0 == 0 ? (old | set) : a0 == 1 ? (old & ~set) : a0 == 2 ? set : old;
            if (a0 > 2) {
                r = -EINVAL_;
                break;
            }
            t->sigmask = nm & ~((1ull << (SIGKILL_ - 1)) | (1ull << (SIGSTOP_ - 1)));
        }
        r = 0;
        break;
    }
    case 136: {  // rt_sigpending
        u64 p = t->pending.load();
        r = Put(a0, p) ? 0 : -EFAULT_;
        break;
    }
    case 133: {  // rt_sigsuspend
        u64 mask;
        if (!Get(a0, &mask)) {
            r = -EFAULT_;
            break;
        }
        u64 old = t->sigmask.exchange(mask);
        while (!t->SignalPending())
            WaitForSingleObject(t->wake_event, INFINITE);
        t->cpu->SetX(0, static_cast<u64>(-EINTR_));
        t->sigsuspend_old = old;
        t->in_sigsuspend = true;
        DeliverPendingSignals(t);
        if (t->in_sigsuspend) {  // no handler frame was pushed
            t->in_sigsuspend = false;
            t->sigmask = old;
        }
        r = kNoReturn;
        break;
    }
    case 132: {  // sigaltstack
        if (a1) {
            u64 old[3] = {t->altstack_sp, static_cast<u64>(t->altstack_flags), t->altstack_size};
            if (!SafeCopyToGuest(a1, old, 24)) {
                r = -EFAULT_;
                break;
            }
        }
        if (a0) {
            u64 ss[3];
            if (!SafeCopyFromGuest(ss, a0, 24)) {
                r = -EFAULT_;
                break;
            }
            t->altstack_sp = ss[0];
            t->altstack_flags = static_cast<int>(ss[1]);
            t->altstack_size = ss[2];
        }
        r = 0;
        break;
    }
    case 270:  // process_vm_readv
    case 271:  // process_vm_writev
    {
        // Only our own address space exists; games use it to probe whether memory is readable.
        const int pid = static_cast<int>(a0);
        if (pid != Proc().pid && pid != 0) {
            r = -ESRCH_;
            break;
        }
        struct Iov { u64 base, len; };
        const bool write = nr == 271;
        std::vector<Iov> local(a2 > 1024 ? 0 : a2), remote(a4 > 1024 ? 0 : a4);
        if (a2 > 1024 || a4 > 1024 || !SafeCopyFromGuest(local.data(), a1, local.size() * 16) ||
            !SafeCopyFromGuest(remote.data(), a3, remote.size() * 16)) {
            r = -EFAULT_;
            break;
        }
        s64 total = 0;
        size_t ri = 0;
        u64 roff = 0;
        std::vector<u8> buf;
        for (const Iov& l : local) {
            u64 done = 0;
            while (done < l.len && ri < remote.size()) {
                u64 n = std::min<u64>(l.len - done, remote[ri].len - roff);
                buf.resize(n);
                bool ok = write ? SafeCopyFromGuest(buf.data(), l.base + done, n) &&
                                      SafeCopyToGuest(remote[ri].base + roff, buf.data(), n)
                                : SafeCopyFromGuest(buf.data(), remote[ri].base + roff, n) &&
                                      SafeCopyToGuest(l.base + done, buf.data(), n);
                if (!ok) {
                    if (total == 0)
                        total = -EFAULT_;
                    ri = remote.size();
                    break;
                }
                total += n;
                done += n;
                roff += n;
                if (roff == remote[ri].len) {
                    ++ri;
                    roff = 0;
                }
            }
            if (total < 0)
                break;
        }
        r = total;
        break;
    }
    case 139:  // rt_sigreturn
        SysRtSigreturn(t);
        r = kNoReturn;
        break;
    case 129:  // kill
    case 131:  // tgkill
    case 130:  // tkill
    case 240:  // rt_tgsigqueueinfo
    {
        const bool tg = nr == 131 || nr == 240;
        int sig = static_cast<int>(tg ? a2 : a1);
        int target = static_cast<int>(tg ? a1 : a0);
        GuestThread* dst = nullptr;
        if (nr == 129) {
            if (target != Proc().pid && target != 0 && target != -1) {
                r = -ESRCH_;
                break;
            }
            dst = FindThread(Proc().pid);
            if (!dst)
                dst = t;
        } else {
            dst = FindThread(target);
        }
        if (!dst) {
            r = -ESRCH_;
            break;
        }
        if (sig == 0) {
            r = 0;
            break;
        }
        RN_TRACE("signal %d -> thread %d (from %d)", sig, dst->tid, t->tid);
        SendSignal(dst, sig);
        r = 0;
        break;
    }

    default:
        break;
    }

    if (r == -ENOSYS_ && nr != 435 && nr != 100 && nr != 26 && nr != 85)
        Log("unimplemented syscall %llu (%s) x0=0x%llx x1=0x%llx x2=0x%llx at %s", nr, SyscallName(nr), a0, a1,
            a2, DescribeAddress(c.Pc()).c_str());
    if (g_verbose >= 3)
        Log("syscall %llu (%s)(0x%llx, 0x%llx, 0x%llx) = %lld", nr, SyscallName(nr), a0, a1, a2, r);
    if (r != kNoReturn)
        c.SetX(0, static_cast<u64>(r));
}

namespace {
const char* SyscallName(u64 nr) {
    switch (nr) {
    case 17: return "getcwd";
    case 25: return "fcntl";
    case 29: return "ioctl";
    case 56: return "openat";
    case 63: return "read";
    case 64: return "write";
    case 98: return "futex";
    case 113: return "clock_gettime";
    case 134: return "rt_sigaction";
    case 135: return "rt_sigprocmask";
    case 167: return "prctl";
    case 220: return "clone";
    case 222: return "mmap";
    case 226: return "mprotect";
    case 260: return "wait4";
    case 280: return "bpf";
    case 293: return "rseq";
    case 424: return "pidfd_send_signal";
    case 434: return "pidfd_open";
    case 436: return "close_range";
    case 441: return "epoll_pwait2";
    default: return "?";
    }
}
}  // namespace

}  // namespace rn
