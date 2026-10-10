// Guest threads, futexes, clocks and signal delivery.
#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "kernel.h"
#include "memory.h"

namespace rn {

using namespace lx;

Process& Proc() {
    static Process p;
    return p;
}

static thread_local GuestThread* tls_current = nullptr;

GuestThread* CurrentThread() { return tls_current; }
void SetCurrentThread(GuestThread* t) { tls_current = t; }

// Clocks ------------------------------------------------------------------------

static u64 QpcFreq() {
    static u64 f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return static_cast<u64>(v.QuadPart);
    }();
    return f;
}

u64 MonotonicNs() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    u64 hi;
    u64 lo = _umul128(static_cast<u64>(now.QuadPart), 1000000000ull, &hi);
    u64 rem;
    return _udiv128(hi, lo, QpcFreq(), &rem);
}

u64 RealtimeNs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    u64 t = (static_cast<u64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) * 100;
}

bool SleepUntil(GuestThread* t, u64 deadline_ns) {
    for (;;) {
        u64 now = MonotonicNs();
        if (now >= deadline_ns)
            return true;
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>((deadline_ns - now + 99) / 100);
        SetWaitableTimer(t->sleep_timer, &due, 0, nullptr, nullptr, FALSE);
        HANDLE hs[2] = {t->sleep_timer, t->wake_event};
        DWORD r = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 + 1 && t->SignalPending()) {
            CancelWaitableTimer(t->sleep_timer);
            return false;
        }
    }
}

void InterruptThread(GuestThread* t) {
    if (t->wake_event)
        SetEvent(t->wake_event);
}

// Futexes -----------------------------------------------------------------------

namespace {
struct FutexWaiter {
    u64 addr;
    u32 bitset;
    GuestThread* t;
    std::atomic<bool> woken{false};
    std::atomic<bool> sleeping{false};  // false while spinning: the waker can skip SetEvent
    u64 wake_ns = 0;                    // when the waker woke it (wake latency statistics)
};
// REFRACT_FUTEX_SPIN: microseconds a waiter polls for its wake before blocking (0 = off).
// Unity hands work between its main and job threads through futexes many times per frame, and
// a Win32 event wake costs a context switch each time. REFRACT_FUTEX_SPIN_AB=1 makes the
// profiler toggle it every report window for an A/B comparison.
const u64 g_futex_spin_cfg = [] {
    const char* v = getenv("REFRACT_FUTEX_SPIN");
    return static_cast<u64>(v ? atoi(v) : 0) * 1000;
}();
std::atomic<u64> g_futex_spin_ns{g_futex_spin_cfg};

// Logical processors that are E-cores (efficiency class below the highest) on hybrid CPUs.
bool OnEfficiencyCore() {
    static const std::vector<bool> ecore = [] {
        std::vector<bool> v;
        ULONG len = 0;
        GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
        std::vector<char> buf(len);
        auto* info = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data());
        if (!len || !GetSystemCpuSetInformation(info, len, &len, GetCurrentProcess(), 0))
            return v;
        BYTE top = 0;
        for (ULONG at = 0; at < len; at += reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data() + at)->Size)
            top = std::max(top, reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data() + at)->CpuSet.EfficiencyClass);
        for (ULONG at = 0; at < len; at += reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data() + at)->Size) {
            auto& cs = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data() + at)->CpuSet;
            if (cs.Group != 0)
                continue;
            if (v.size() <= cs.LogicalProcessorIndex)
                v.resize(cs.LogicalProcessorIndex + 1);
            v[cs.LogicalProcessorIndex] = cs.EfficiencyClass < top;
        }
        return v;
    }();
    DWORD cpu = GetCurrentProcessorNumber();
    return cpu < ecore.size() && ecore[cpu];
}
std::mutex g_futex_mu;
std::map<u64, std::deque<FutexWaiter*>> g_futex_queues;
std::atomic<u64> g_futex_timed{0}, g_futex_short{0};  // timed waits; those under 2 ms (profile report)

void RemoveWaiterLocked(FutexWaiter* w) {
    auto it = g_futex_queues.find(w->addr);
    if (it == g_futex_queues.end())
        return;
    auto& q = it->second;
    for (auto qi = q.begin(); qi != q.end(); ++qi) {
        if (*qi == w) {
            q.erase(qi);
            break;
        }
    }
    if (q.empty())
        g_futex_queues.erase(it);
}

int WakeLocked(u64 uaddr, int n, u32 bitset) {
    auto it = g_futex_queues.find(uaddr);
    if (it == g_futex_queues.end())
        return 0;
    auto& q = it->second;
    int woke = 0;
    for (auto qi = q.begin(); qi != q.end() && woke < n;) {
        FutexWaiter* w = *qi;
        if (w->bitset & bitset) {
            qi = q.erase(qi);
            w->wake_ns = MonotonicNs();
            w->woken = true;
            if (w->sleeping)
                SetEvent(w->t->wake_event);
            ++woke;
        } else {
            ++qi;
        }
    }
    if (q.empty())
        g_futex_queues.erase(it);
    return woke;
}
}  // namespace

s64 FutexWait(GuestThread* t, u64 uaddr, u32 val, u32 bitset, s64 deadline_ns, bool realtime) {
    if (bitset == 0)
        return -EINVAL_;
    if (uaddr & 3)
        return -EINVAL_;
    std::unique_lock lk(g_futex_mu);
    u32 cur;
    if (!SafeCopyFromGuest(&cur, uaddr, 4))
        return -EFAULT_;
    if (cur != val)
        return -EAGAIN_;
    FutexWaiter w;
    w.addr = uaddr;
    w.bitset = bitset;
    w.t = t;
    const u64 spin_ns = g_futex_spin_ns;
    w.sleeping = spin_ns == 0;
    g_futex_queues[uaddr].push_back(&w);
    lk.unlock();
    const u64 start = MonotonicNs();
    auto finish = [&](s64 r) {
        u64 end = MonotonicNs();
        ++t->fx_waits;
        t->fx_blocked_ns += end - start;
        if (OnEfficiencyCore())
            ++t->fx_ecore;
        if (r == 0) {
            ++t->fx_woken;
            t->fx_wake_ns += end > w.wake_ns ? end - w.wake_ns : 0;
        }
        return r;
    };
    if (!w.sleeping) {
        u64 limit = start + spin_ns;
        if (deadline_ns >= 0 && !realtime)
            limit = std::min<u64>(limit, static_cast<u64>(deadline_ns));
        while (!w.woken && !t->SignalPending() && MonotonicNs() < limit)
            YieldProcessor();
        if (w.woken) {
            ++t->fx_spun;
            lk.lock();  // the waker may still be inside WakeLocked
            return finish(0);
        }
        w.sleeping = true;  // a wake racing with this store may also SetEvent: a spurious wakeup
    }
    for (;;) {
        if (w.woken)
            break;
        if (deadline_ns >= 0) {
            // Timed waits use the high-resolution timer: a millisecond WaitForSingleObject timeout
            // turns the sub-millisecond waits of Unity's job system into 1-2 ms stalls.
            u64 now = realtime ? RealtimeNs() : MonotonicNs();
            if (now < static_cast<u64>(deadline_ns)) {
                ++g_futex_timed;
                if (static_cast<u64>(deadline_ns) - now < 2000000)
                    ++g_futex_short;
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>((static_cast<u64>(deadline_ns) - now + 99) / 100);
                SetWaitableTimer(t->sleep_timer, &due, 0, nullptr, nullptr, FALSE);
                HANDLE hs[2] = {t->wake_event, t->sleep_timer};
                WaitForMultipleObjects(2, hs, FALSE, INFINITE);
                CancelWaitableTimer(t->sleep_timer);
            }
        } else {
            WaitForSingleObject(t->wake_event, INFINITE);
        }
        lk.lock();
        if (w.woken)
            return finish(0);
        bool timed_out = false;
        if (deadline_ns >= 0) {
            u64 now = realtime ? RealtimeNs() : MonotonicNs();
            timed_out = now >= static_cast<u64>(deadline_ns);
        }
        if (timed_out || t->SignalPending()) {
            RemoveWaiterLocked(&w);
            return finish(timed_out ? -ETIMEDOUT_ : -EINTR_);
        }
        lk.unlock();
    }
    lk.lock();
    return finish(0);
}

s64 FutexWake(u64 uaddr, int n, u32 bitset) {
    if (bitset == 0)
        return -EINVAL_;
    std::lock_guard lk(g_futex_mu);
    return WakeLocked(uaddr, n, bitset);
}

s64 FutexRequeue(u64 uaddr, int nwake, u64 uaddr2, int nreq, bool cmp, u32 val) {
    std::lock_guard lk(g_futex_mu);
    if (cmp) {
        u32 cur;
        if (!SafeCopyFromGuest(&cur, uaddr, 4))
            return -EFAULT_;
        if (cur != val)
            return -EAGAIN_;
    }
    int woke = WakeLocked(uaddr, nwake, ~0u);
    int moved = 0;
    auto it = g_futex_queues.find(uaddr);
    if (it != g_futex_queues.end() && uaddr != uaddr2) {
        auto& src = it->second;
        auto& dst = g_futex_queues[uaddr2];
        while (!src.empty() && moved < nreq) {
            FutexWaiter* w = src.front();
            src.pop_front();
            w->addr = uaddr2;
            dst.push_back(w);
            ++moved;
        }
        if (src.empty())
            g_futex_queues.erase(uaddr);
        if (dst.empty())
            g_futex_queues.erase(uaddr2);
    }
    return woke + moved;
}

s64 FutexWakeOp(u64 uaddr, int nwake, u64 uaddr2, int nwake2, u32 op) {
    int optype = (op >> 28) & 0xf;
    int cmptype = (op >> 24) & 0xf;
    u32 oparg = (op >> 12) & 0xfff;
    u32 cmparg = op & 0xfff;
    if (optype & 8) {
        optype &= 7;
        oparg = 1u << (oparg & 31);
    }
    std::lock_guard lk(g_futex_mu);
    u32 old;
    for (;;) {
        if (!SafeCopyFromGuest(&old, uaddr2, 4))
            return -EFAULT_;
        u32 nv;
        switch (optype) {
        case 0: nv = oparg; break;
        case 1: nv = old + oparg; break;
        case 2: nv = old | oparg; break;
        case 3: nv = old & ~oparg; break;
        case 4: nv = old ^ oparg; break;
        default: return -ENOSYS_;
        }
        if (_InterlockedCompareExchange(GuestPtr<volatile long>(uaddr2), static_cast<long>(nv),
                                        static_cast<long>(old)) == static_cast<long>(old))
            break;
    }
    int woke = WakeLocked(uaddr, nwake, ~0u);
    bool c;
    s32 so = static_cast<s32>(old), sc = static_cast<s32>(cmparg);
    switch (cmptype) {
    case 0: c = so == sc; break;
    case 1: c = so != sc; break;
    case 2: c = so < sc; break;
    case 3: c = so <= sc; break;
    case 4: c = so > sc; break;
    case 5: c = so >= sc; break;
    default: return -ENOSYS_;
    }
    if (c)
        woke += WakeLocked(uaddr2, nwake2, ~0u);
    return woke;
}

// Threads -----------------------------------------------------------------------

void RegisterThread(GuestThread* t) {
    t->wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    t->sleep_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS);
    if (!t->sleep_timer)
        t->sleep_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    std::lock_guard lock(Proc().threads_mu);
    Proc().threads[t->tid] = t;
}

static void UnregisterThread(GuestThread* t) {
    std::lock_guard lock(Proc().threads_mu);
    Proc().threads.erase(t->tid);
}

// Runs guest code until the thread exits (returns false) or, when allow_return is
// set, until it reaches the host-call return trampoline (returns true).
static bool RunLoop(GuestThread* t, bool allow_return) {
    for (;;) {
        StopInfo s = t->cpu->Run();
        switch (s.kind) {
        case StopInfo::Svc:
            if (s.imm == 0) {
                t->in_syscall = static_cast<int>(t->cpu->X(8));
                HandleSyscall(t);
                t->in_syscall = -1;
            } else if (s.imm == 1 /*kThunkReturn*/) {
                if (allow_return)
                    return true;
                CrashReport(t, SIGSEGV_, 0, t->cpu->Pc() - 4, "return trampoline reached outside a host call");
            } else if (!DispatchThunk(t, s.imm)) {
                CrashReport(t, SIGSYS_, 0, t->cpu->Pc() - 4, "unknown host thunk");
            }
            if (t->return_to_host) {
                t->return_to_host = false;
                if (allow_return)
                    return true;
            }
            break;
        case StopInfo::Exception:
        case StopInfo::Interpret:
            if (!EmulateInstruction(t, s.pc)) {
                u32 insn = 0;
                SafeCopyFromGuest(&insn, s.pc, 4);
                if (s.kind == StopInfo::Exception && s.exception == 8 /*Breakpoint*/) {
                    RaiseFault(t, SIGTRAP_, 1, s.pc, s.pc);
                } else {
                    Log("unsupported instruction %08x at %s (%s)", insn, DescribeAddress(s.pc).c_str(),
                        s.kind == StopInfo::Interpret ? "interpreter fallback" : "exception");
                    RaiseFault(t, SIGILL_, 1, s.pc, s.pc);
                }
            }
            break;
        case StopInfo::Fault:
            RaiseFault(t, SIGSEGV_, 1, s.fault_addr, s.pc);
            break;
        default:
            break;
        }
        if (t->exiting)
            return false;
        if (t->sample_req.exchange(false))
            LogThreadSample(t, -1);
        if (t->profile_req.exchange(false))
            RecordProfileSample(t, -1);
        if (t->pending.load())
            DeliverPendingSignals(t);
    }
}

int SyscallInJit() {
    GuestThread* t = CurrentThread();
    if (!t)
        return 0;
    const u64 nr = t->cpu->X(8);
    switch (nr) {
    case 93: case 94:    // exit, exit_group
    case 133: case 139:  // rt_sigsuspend, rt_sigreturn: rewrite the whole context
    case 220: case 221: case 435:  // clone, execve, clone3
        return 0;
    default:
        break;
    }
    t->in_syscall = static_cast<int>(nr);
    HandleSyscall(t);
    t->in_syscall = -1;
    const bool attention = t->exiting || t->return_to_host || t->pending.load() || t->sample_req.load() ||
                           t->profile_req.load();
    return attention ? 2 : 1;
}

std::string Backtrace(GuestThread* t, int depth) {
    CpuContext c;
    t->cpu->GetContext(&c);
    std::string bt = DescribeAddress(c.pc) + " < " + DescribeAddress(c.x[30]);
    u64 fp = c.x[29];
    for (int i = 0; i < depth && fp; ++i) {
        u64 fr[2];
        if (!SafeCopyFromGuest(fr, fp, 16) || !fr[1])
            break;
        bt += " < " + DescribeAddress(fr[1]);
        if (fr[0] <= fp)
            break;
        fp = fr[0];
    }
    return bt;
}

namespace {
std::mutex g_profile_mu;
std::map<std::string, int> g_profile;  // "thread: backtrace" -> samples
int g_profile_total = 0;
}  // namespace

// Callers found by scanning the stack for return addresses (code without frame
// pointers breaks the fp chain): values in executable memory that follow a BL/BLR.
std::string StackScan(GuestThread* t, int depth) {
    CpuContext c;
    t->cpu->GetContext(&c);
    std::string bt = DescribeAddress(c.pc);
    std::vector<u64> stack(4096);
    u64 sp = c.sp;
    size_t n = stack.size();
    while (n && !SafeCopyFromGuest(stack.data(), sp, n * 8))
        n /= 2;
    Vma code;
    bool have = false;
    int found = 0;
    std::vector<u64> seen{c.x[30]};
    auto is_ret = [&](u64 v) {
        if (v & 3)
            return false;
        if (!have || v < code.start || v >= code.end) {
            if (!Mem().FindVma(v, &code) || !(code.prot & lx::PROT_EXEC_))
                return have = false;
            have = true;
        }
        u32 insn;
        if (v - 4 < code.start || !SafeCopyFromGuest(&insn, v - 4, 4))
            return false;
        return (insn & 0xfc000000) == 0x94000000 || (insn & 0xfffffc1f) == 0xd63f0000;
    };
    if (is_ret(c.x[30])) {
        bt += " < " + DescribeAddress(c.x[30]);
        ++found;
    }
    for (size_t i = 0; i < n && found < depth; ++i) {
        u64 v = stack[i];
        if (v < 0x10000 || std::find(seen.begin(), seen.end(), v) != seen.end() || !is_ret(v))
            continue;
        seen.push_back(v);
        bt += " < " + DescribeAddress(v);
        ++found;
    }
    return bt;
}

void RecordProfileSample(GuestThread* t, int syscall) {
    static const int depth = getenv("REFRACT_PROFILE_DEPTH") ? atoi(getenv("REFRACT_PROFILE_DEPTH")) : 6;
    std::string key = t->name + ": " + (syscall >= 0 ? Format("[syscall %d] ", syscall) : std::string()) + StackScan(t, depth);
    std::lock_guard lock(g_profile_mu);
    ++g_profile[key];
    ++g_profile_total;
}

void StartProfiler(const std::string& filter) {
    std::vector<std::string> names;
    for (size_t at = 0; at <= filter.size();) {
        size_t comma = std::min(filter.find(',', at), filter.size());
        names.push_back(filter.substr(at, comma - at));
        at = comma + 1;
    }
    // REFRACT_PROFILE_SECS: report window (default 30 s).
    const char* secs = getenv("REFRACT_PROFILE_SECS");
    int ticks = std::max(1, (secs ? atoi(secs) : 30) * 20);
    std::thread([filter, names, ticks] {
        for (int tick = 1;; ++tick) {
            Sleep(50);
            {
                std::lock_guard lock(Proc().threads_mu);
                for (auto& [tid, t] : Proc().threads) {
                    if (std::none_of(names.begin(), names.end(),
                                     [&](const std::string& n) { return t->name.find(n) != std::string::npos; }))
                        continue;
                    int sc = t->in_syscall.load();
                    if (sc >= 0) {
                        RecordProfileSample(t, sc);
                    } else {
                        t->profile_req = true;
                        t->cpu->RequestStop();
                    }
                }
            }
            if (tick % ticks)
                continue;
            std::vector<std::pair<int, std::string>> top;
            int total;
            {
                std::lock_guard lock(g_profile_mu);
                for (auto& [k, n] : g_profile)
                    top.push_back({n, k});
                total = g_profile_total;
                g_profile.clear();
                g_profile_total = 0;
            }
            std::sort(top.rbegin(), top.rend());
            Log("--- profile (%d samples, '%s'; futex timed waits %llu, %llu under 2 ms) ---", total,
                filter.c_str(), g_futex_timed.exchange(0), g_futex_short.exchange(0));
            // Per thread: samples, share blocked in syscalls, hottest leaf (pc < caller) pairs.
            struct ThreadStats {
                int samples = 0, blocked = 0;
                std::map<std::string, int> leaves;
            };
            std::map<std::string, ThreadStats> threads;
            for (auto& [n, k] : top) {
                size_t colon = k.find(": ");
                ThreadStats& ts = threads[k.substr(0, colon)];
                std::string rest = k.substr(colon + 2);
                ts.samples += n;
                if (rest.rfind("[syscall", 0) == 0)
                    ts.blocked += n;
                size_t second = rest.find(" < ");
                second = second == std::string::npos ? std::string::npos : rest.find(" < ", second + 3);
                ts.leaves[rest.substr(0, second)] += n;
            }
            for (auto& [name, ts] : threads) {
                Log("  thread %s: %d samples, %.0f%% blocked in syscalls", name.c_str(), ts.samples,
                    100.0 * ts.blocked / std::max(1, ts.samples));
                std::vector<std::pair<int, std::string>> leaves;
                for (auto& [l, n] : ts.leaves)
                    leaves.push_back({n, l});
                std::sort(leaves.rbegin(), leaves.rend());
                for (size_t i = 0; i < leaves.size() && i < 8; ++i)
                    Log("    %5.1f%% %s", 100.0 * leaves[i].first / std::max(1, ts.samples), leaves[i].second.c_str());
            }
            for (size_t i = 0; i < top.size() && i < 25; ++i)
                Log("%5.1f%% %s", 100.0 * top[i].first / std::max(1, total), top[i].second.c_str());
            // Futex waits per thread over the window (all threads), busiest first.
            struct FutexStats {
                u64 waits, blocked_ns, woken, wake_ns, spun, ecore;
                std::string name;
            };
            std::vector<FutexStats> fx;
            {
                std::lock_guard lock(Proc().threads_mu);
                for (auto& [tid, t] : Proc().threads)
                    if (u64 n = t->fx_waits.exchange(0))
                        fx.push_back({n, t->fx_blocked_ns.exchange(0), t->fx_woken.exchange(0),
                                      t->fx_wake_ns.exchange(0), t->fx_spun.exchange(0), t->fx_ecore.exchange(0), t->name});
            }
            std::sort(fx.begin(), fx.end(), [](auto& a, auto& b) { return a.waits > b.waits; });
            const double secs = ticks / 20.0;
            static const bool ab = getenv("REFRACT_FUTEX_SPIN_AB") != nullptr;
            Log("  futex spin this window: %llu us", g_futex_spin_ns.load() / 1000);
            if (ab) {
                g_futex_spin_ns = g_futex_spin_ns ? 0 : g_futex_spin_cfg;
                Log("  futex spin now %llu us", g_futex_spin_ns.load() / 1000);
            }
            for (size_t i = 0; i < fx.size() && i < 12; ++i)
                Log("  futex %-16s %7.0f waits/s, blocked %4.0f%%, avg wait %6.1f us, woken %3.0f%% "
                    "(avg wake latency %5.1f us), spun %3.0f%%, on E-cores %3.0f%%",
                    fx[i].name.c_str(), fx[i].waits / secs, fx[i].blocked_ns / (secs * 1e7),
                    fx[i].blocked_ns / 1e3 / fx[i].waits, 100.0 * fx[i].woken / fx[i].waits,
                    fx[i].woken ? fx[i].wake_ns / 1e3 / fx[i].woken : 0.0,
                    100.0 * fx[i].spun / fx[i].waits, 100.0 * fx[i].ecore / fx[i].waits);
        }
    }).detach();
}

void LogThreadSample(GuestThread* t, int syscall) {
    CpuContext c;
    t->cpu->GetContext(&c);
    std::string bt = DescribeAddress(c.pc) + " < " + DescribeAddress(c.x[30]);
    u64 fp = c.x[29];
    for (int i = 0; i < 6 && fp; ++i) {
        u64 fr[2];
        if (!SafeCopyFromGuest(fr, fp, 16) || !fr[1])
            break;
        bt += " < " + DescribeAddress(fr[1]);
        if (fr[0] <= fp)
            break;
        fp = fr[0];
    }
    if (syscall >= 0)
        Log("sample tid %d '%s' in syscall %d: %s", t->tid, t->name.c_str(), syscall, bt.c_str());
    else
        Log("sample tid %d '%s': %s", t->tid, t->name.c_str(), bt.c_str());
}

void StartSampler(int seconds) {
    std::thread([seconds] {
        for (;;) {
            Sleep(seconds * 1000);
            Log("--- thread sample ---");
            std::lock_guard lock(Proc().threads_mu);
            for (auto& [tid, t] : Proc().threads) {
                int sc = t->in_syscall.load();
                if (sc >= 0) {
                    LogThreadSample(t, sc);
                } else {
                    t->sample_req = true;
                    t->cpu->RequestStop();
                }
            }
        }
    }).detach();
}

void RunGuestThread(GuestThread* t) {
    tls_current = t;
    t->host_tid = GetCurrentThreadId();
    RunLoop(t, false);
    UnregisterThread(t);
}

bool RunGuestUntilReturn(GuestThread* t) {
    GuestThread* prev = tls_current;
    tls_current = t;
    bool r = RunLoop(t, true);
    tls_current = prev;
    return r;
}

// Signals -----------------------------------------------------------------------

namespace {
constexpr u64 Bit(int sig) { return 1ull << (sig - 1); }

// arm64 struct rt_sigframe layout
constexpr u64 kSiginfoSize = 128;
constexpr u64 kUcMcontextOff = 176;           // within ucontext
constexpr u64 kSigcontextSize = 288 + 4096;   // fault_address..pstate + __reserved
constexpr u64 kUcontextSize = kUcMcontextOff + kSigcontextSize;
constexpr u64 kFrameSize = kSiginfoSize + kUcontextSize;
constexpr u32 kFpsimdMagic = 0x46508001;

void BuildSiginfo(u8* info, int sig, int code, u64 addr) {
    memset(info, 0, kSiginfoSize);
    s32 v = sig;
    memcpy(info + 0, &v, 4);
    v = code;
    memcpy(info + 8, &v, 4);
    if (sig == SIGSEGV_ || sig == SIGBUS_ || sig == SIGILL_ || sig == SIGFPE_ || sig == SIGTRAP_) {
        memcpy(info + 16, &addr, 8);
    } else {
        s32 pid = Proc().pid;
        u32 uid = 10100;
        memcpy(info + 16, &pid, 4);
        memcpy(info + 20, &uid, 4);
    }
}

// Pushes a signal frame and redirects the thread into the handler.
bool SetupFrame(GuestThread* t, int sig, const KSigaction& act, int code, u64 addr, u64 pc_override) {
    CpuContext ctx;
    t->cpu->GetContext(&ctx);
    if (pc_override)
        ctx.pc = pc_override;
    u64 sp = ctx.sp;
    bool on_alt = t->altstack_size && sp >= t->altstack_sp && sp < t->altstack_sp + t->altstack_size;
    if ((act.flags & SA_ONSTACK_) && t->altstack_flags == 0 && !on_alt)
        sp = t->altstack_sp + t->altstack_size;
    sp -= 16;  // frame record {fp, lr}
    u64 record = sp & ~15ull;
    u64 frame = (record - kFrameSize) & ~15ull;

    std::vector<u8> buf(kFrameSize + 16, 0);
    BuildSiginfo(buf.data(), sig, code, addr);
    u8* uc = buf.data() + kSiginfoSize;
    // uc_stack
    memcpy(uc + 16, &t->altstack_sp, 8);
    s32 ssflags = on_alt ? 1 : t->altstack_flags;
    memcpy(uc + 24, &ssflags, 4);
    memcpy(uc + 32, &t->altstack_size, 8);
    u64 oldmask = t->sigmask.load();
    u64 savedmask = oldmask;
    if (t->in_sigsuspend) {
        savedmask = t->sigsuspend_old;
        t->in_sigsuspend = false;
    }
    memcpy(uc + 40, &savedmask, 8);
    u8* mc = uc + kUcMcontextOff;
    memcpy(mc + 0, &addr, 8);
    memcpy(mc + 8, ctx.x, 31 * 8);
    memcpy(mc + 256, &ctx.sp, 8);
    memcpy(mc + 264, &ctx.pc, 8);
    u64 pstate = ctx.pstate;
    memcpy(mc + 272, &pstate, 8);
    u8* res = mc + 288;
    u32 magic = kFpsimdMagic, size = 528;
    memcpy(res + 0, &magic, 4);
    memcpy(res + 4, &size, 4);
    memcpy(res + 8, &ctx.fpsr, 4);
    memcpy(res + 12, &ctx.fpcr, 4);
    memcpy(res + 16, ctx.v, 512);
    // terminator follows (zeros)
    u64 fp = ctx.x[29], lr = ctx.x[30];
    memcpy(buf.data() + (record - frame), &fp, 8);
    memcpy(buf.data() + (record - frame) + 8, &lr, 8);
    if (!SafeCopyToGuest(frame, buf.data(), (record + 16) - frame))
        return false;

    u64 restorer = (act.flags & SA_RESTORER_) && act.restorer ? act.restorer : SigReturnTrampoline();
    t->cpu->SetX(0, static_cast<u64>(sig));
    t->cpu->SetX(1, frame);
    t->cpu->SetX(2, frame + kSiginfoSize);
    t->cpu->SetX(29, record);
    t->cpu->SetX(30, restorer);
    t->cpu->SetSp(frame);
    t->cpu->SetPc(act.handler);
    t->cpu->ClearExclusive();

    u64 newmask = oldmask | act.mask;
    if (!(act.flags & SA_NODEFER_))
        newmask |= Bit(sig);
    newmask &= ~(Bit(SIGKILL_) | Bit(SIGSTOP_));
    t->sigmask = newmask;
    return true;
}

bool DefaultIgnored(int sig) {
    return sig == SIGCHLD_ || sig == SIGURG_ || sig == SIGWINCH_ || sig == SIGCONT_;
}
}  // namespace

s64 SysRtSigreturn(GuestThread* t) {
    u64 frame = t->cpu->Sp();
    std::vector<u8> buf(kFrameSize);
    if (!SafeCopyFromGuest(buf.data(), frame, kFrameSize))
        CrashReport(t, SIGSEGV_, frame, t->cpu->Pc(), "bad signal frame");
    const u8* uc = buf.data() + kSiginfoSize;
    const u8* mc = uc + kUcMcontextOff;
    CpuContext ctx;
    t->cpu->GetContext(&ctx);
    memcpy(ctx.x, mc + 8, 31 * 8);
    memcpy(&ctx.sp, mc + 256, 8);
    memcpy(&ctx.pc, mc + 264, 8);
    u64 pstate;
    memcpy(&pstate, mc + 272, 8);
    ctx.pstate = static_cast<u32>(pstate);
    const u8* res = mc + 288;
    u32 magic;
    memcpy(&magic, res, 4);
    if (magic == kFpsimdMagic) {
        memcpy(&ctx.fpsr, res + 8, 4);
        memcpy(&ctx.fpcr, res + 12, 4);
        memcpy(ctx.v, res + 16, 512);
    }
    t->cpu->SetContext(ctx);
    t->cpu->ClearExclusive();
    u64 mask;
    memcpy(&mask, uc + 40, 8);
    t->sigmask = mask & ~(Bit(SIGKILL_) | Bit(SIGSTOP_));
    return 0;
}

void SendSignal(GuestThread* target, int sig) {
    if (sig <= 0 || sig > 64)
        return;
    target->pending.fetch_or(Bit(sig));
    if (!(target->sigmask.load() & Bit(sig))) {
        InterruptThread(target);
        target->cpu->RequestStop();
    }
}

void DeliverPendingSignals(GuestThread* t) {
    for (;;) {
        u64 deliverable = t->pending.load() & ~t->sigmask.load();
        if (!deliverable)
            return;
        unsigned long idx;
        _BitScanForward64(&idx, deliverable);
        int sig = static_cast<int>(idx) + 1;
        t->pending.fetch_and(~Bit(sig));
        KSigaction act;
        {
            std::lock_guard lock(Proc().sig_mu);
            act = Proc().sigactions[sig];
            if (act.handler > SIG_IGN_ && (act.flags & SA_RESETHAND_))
                Proc().sigactions[sig].handler = SIG_DFL_;
        }
        if (act.handler == SIG_IGN_)
            continue;
        if (act.handler == SIG_DFL_) {
            if (DefaultIgnored(sig))
                continue;
            CrashReport(t, sig, 0, t->cpu->Pc(), "fatal signal (default action)");
        }
        if (!SetupFrame(t, sig, act, -6 /*SI_TKILL*/, 0, 0))
            CrashReport(t, SIGSEGV_, t->cpu->Sp(), t->cpu->Pc(), "cannot push signal frame");
        return;  // one frame at a time; the rest are delivered after it runs
    }
}

void RaiseFault(GuestThread* t, int sig, int code, u64 addr, u64 pc) {
    KSigaction act;
    {
        std::lock_guard lock(Proc().sig_mu);
        act = Proc().sigactions[sig];
    }
    bool blocked = (t->sigmask.load() & Bit(sig)) != 0;
    if (act.handler <= SIG_IGN_ || blocked)
        CrashReport(t, sig, addr, pc, "fault");
    Log("guest fault: signal %d addr 0x%llx pc %s -> guest handler", sig, addr,
        DescribeAddress(pc).c_str());
    static thread_local int depth = 0;
    if (++depth > 8)
        CrashReport(t, sig, addr, pc, "fault inside the guest's fault handler");
    if (!SetupFrame(t, sig, act, code, addr, pc))
        CrashReport(t, sig, addr, pc, "cannot push signal frame");
    depth = 0;
}

std::string DescribeAddress(u64 addr) {
    Vma v;
    if (Mem().FindVma(addr, &v)) {
        if (!v.name.empty())
            return Format("0x%llx (%s+0x%llx)", addr, v.name.c_str(), addr - v.start + v.offset);
        return Format("0x%llx (anon 0x%llx+0x%llx)", addr, v.start, addr - v.start);
    }
    return Format("0x%llx (unmapped)", addr);
}

void CrashReport(GuestThread* t, int sig, u64 addr, u64 pc, const char* why) {
    static std::mutex crash_mu;
    std::lock_guard lock(crash_mu);
    CpuContext c;
    t->cpu->GetContext(&c);
    Log("==== guest crash: %s, signal %d, thread %d (%s) ====", why, sig, t->tid, t->name.c_str());
    Log("pc   %s", DescribeAddress(pc).c_str());
    Log("lr   %s", DescribeAddress(c.x[30]).c_str());
    Log("addr 0x%llx  sp 0x%llx  tpidr 0x%llx", addr, c.sp, t->cpu->Tpidr());
    for (int i = 0; i < 31; i += 4) {
        std::string line;
        for (int j = i; j < i + 4 && j < 31; ++j)
            line += Format("x%-2d %016llx  ", j, c.x[j]);
        Log("%s", line.c_str());
    }
    // Frame-pointer backtrace.
    u64 fp = c.x[29];
    for (int depth = 0; depth < 32 && fp; ++depth) {
        u64 rec[2];
        if (!SafeCopyFromGuest(rec, fp, 16))
            break;
        if (!rec[1])
            break;
        Log("  #%02d %s", depth, DescribeAddress(rec[1]).c_str());
        if (rec[0] <= fp)
            break;
        fp = rec[0];
    }
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 128 + sig);
    for (;;) {
    }
}

}  // namespace rn
