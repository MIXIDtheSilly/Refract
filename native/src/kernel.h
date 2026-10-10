// The emulated Linux process: guest threads, signals, futexes and syscalls.
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "common.h"
#include "cpu.h"
#include "linux_abi.h"

namespace rn {

struct GuestThread {
    int tid = 0;
    std::unique_ptr<CpuCore> cpu;
    u64 clear_child_tid = 0;
    std::atomic<u64> sigmask{0};
    std::atomic<u64> pending{0};  // bit (sig-1)
    u64 altstack_sp = 0;
    u64 altstack_size = 0;
    int altstack_flags = 2;  // SS_DISABLE
    void* wake_event = nullptr;   // auto-reset Win32 event: futex/sleep/poll wakeups
    void* sleep_timer = nullptr;  // high-resolution waitable timer
    void* host_handle = nullptr;
    unsigned long host_tid = 0;
    std::string name;
    bool exiting = false;
    // Set by a host handler to make the current host->guest call return now
    // (used to park adopted threads).
    bool return_to_host = false;
    // Service thread: the next clone() hands the new thread to this adoption request.
    u64 adopt_token = 0;
    // Guest JNIEnv for this thread (0 until JNI is used).
    u64 guest_env = 0;
    // rt_sigsuspend: the signal frame must save the pre-suspend mask.
    bool in_sigsuspend = false;
    u64 sigsuspend_old = 0;
    // REFRACT_SAMPLE: the sampler asks running threads to log where they are; threads
    // inside a syscall are logged by the sampler itself (their registers are stable).
    std::atomic<bool> sample_req{false};
    std::atomic<bool> profile_req{false};
    std::atomic<int> in_syscall{-1};
    // Futex statistics for the REFRACT_PROFILE report: waits, time blocked, waits ended by a
    // wake, summed wake-call-to-running latency, waits satisfied while spinning.
    std::atomic<u64> fx_waits{0}, fx_blocked_ns{0}, fx_woken{0}, fx_wake_ns{0}, fx_spun{0};
    std::atomic<u64> fx_ecore{0};  // waits that ended on a low-efficiency-class (E) core

    // True when an unblocked signal is pending (interrupts blocking syscalls).
    bool SignalPending() const { return (pending.load() & ~sigmask.load()) != 0; }
};

struct Process {
    int pid = 0;
    std::string exe_path;   // guest path of the main executable
    std::string cmdline;    // NUL separated
    std::string package;    // Android package name (for /proc/self/cmdline)
    u64 start_stack = 0;    // main thread's initial SP (/proc/self/stat startstack)
    int ncpus = 8;

    std::mutex threads_mu;
    std::map<int, GuestThread*> threads;
    std::atomic<int> next_tid{0};

    std::mutex sig_mu;
    lx::KSigaction sigactions[65] = {};
};

Process& Proc();
GuestThread* CurrentThread();
void SetCurrentThread(GuestThread* t);

// Creates the per-thread Win32 objects and registers the thread.
void RegisterThread(GuestThread* t);
// Runs guest code on the calling host thread until the guest thread exits.
void RunGuestThread(GuestThread* t);
// Logs where a guest thread is (pc and frame-pointer backtrace).
void LogThreadSample(GuestThread* t, int syscall);
// REFRACT_SAMPLE=N: log every guest thread's location every N seconds.
void StartSampler(int seconds);
// REFRACT_PROFILE: samples threads whose name contains `filter` every 50 ms and
// logs the most frequent backtraces every 30 s.
void StartProfiler(const std::string& filter);
void StartHostProfiler(const std::string& filter);  // hostprof.cpp
void RecordProfileSample(GuestThread* t, int syscall);

// Syscall entry (svc #0): reads x8/x0..x5 and writes x0.
void HandleSyscall(GuestThread* t);

// Emulates an instruction the translator can't run (LSE atomics, ...).
// Returns true when handled; PC must already point past the instruction.
bool EmulateInstruction(GuestThread* t, u64 pc);

// Signals
void SendSignal(GuestThread* target, int sig);
void DeliverPendingSignals(GuestThread* t);
// Synchronous fault (SIGSEGV/SIGILL/SIGBUS/SIGTRAP) raised by the current instruction.
void RaiseFault(GuestThread* t, int sig, int code, u64 addr, u64 pc);
s64 SysRtSigreturn(GuestThread* t);
[[noreturn]] void CrashReport(GuestThread* t, int sig, u64 addr, u64 pc, const char* why);
std::string DescribeAddress(u64 addr);

// Futex
s64 FutexWait(GuestThread* t, u64 uaddr, u32 val, u32 bitset, s64 deadline_ns, bool realtime);
s64 FutexWake(u64 uaddr, int n, u32 bitset);
s64 FutexRequeue(u64 uaddr, int nwake, u64 uaddr2, int nreq, bool cmp, u32 val);
s64 FutexWakeOp(u64 uaddr, int nwake, u64 uaddr2, int nwake2, u32 op);
// Wake a thread blocked in FutexWait/sleep/poll (for signals).
void InterruptThread(GuestThread* t);

// Clocks (ns)
u64 MonotonicNs();
u64 RealtimeNs();
// Blocks until deadline (monotonic ns) or interruption; returns false if interrupted.
bool SleepUntil(GuestThread* t, u64 deadline_ns);

// Thunk dispatch for svc #imm != 0 (host functions called from guest stubs).
u64 SigReturnTrampoline();
bool DispatchThunk(GuestThread* t, u32 imm);

}  // namespace rn
