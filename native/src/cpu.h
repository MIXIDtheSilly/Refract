// One ARM64 CPU context per guest thread. The translator behind it is replaceable
// (Dynarmic now; Digitalis is the intended faster backend).
#pragma once

#include <array>
#include <memory>

#include "common.h"

namespace rn {

using Vec128 = std::array<u64, 2>;

struct StopInfo {
    enum Kind { None, Svc, Exception, Interpret, Fault, External } kind = None;
    u32 imm = 0;        // SVC immediate
    u64 pc = 0;         // instruction that stopped (Exception/Interpret/Fault)
    int exception = 0;  // backend-specific exception code
    u64 fault_addr = 0;
    bool fault_write = false;
};

// Full register state, used for signal frames and new threads.
struct CpuContext {
    u64 x[31];
    u64 sp;
    u64 pc;
    u32 pstate;
    u32 fpcr;
    u32 fpsr;
    Vec128 v[32];
};

class CpuCore {
public:
    virtual ~CpuCore() = default;

    // Runs guest code until something needs the kernel.
    virtual StopInfo Run() = 0;

    virtual u64 X(int i) const = 0;
    virtual void SetX(int i, u64 v) = 0;
    virtual u64 Sp() const = 0;
    virtual void SetSp(u64 v) = 0;
    virtual u64 Pc() const = 0;
    virtual void SetPc(u64 v) = 0;
    virtual Vec128 V(int i) const = 0;
    virtual void SetV(int i, Vec128 v) = 0;
    virtual void GetContext(CpuContext* c) const = 0;
    virtual void SetContext(const CpuContext& c) = 0;
    virtual u64& Tpidr() = 0;
    virtual void ClearExclusive() = 0;

    // Thread-safe: make Run() return soon with StopInfo::External.
    virtual void RequestStop() = 0;
};

// Backend setup: max_cores bounds concurrently existing cores (exclusive monitor).
void CpuInit(size_t max_cores);
std::unique_ptr<CpuCore> CreateCpuCore();
// Drop translated code for [addr, addr+len) in every core.
void CpuInvalidateCode(u64 addr, u64 len);
// Emulates an untranslatable instruction from inside the JIT (emulate.cpp); false = leave Run().
bool EmulateInJit(u64 pc);
// Runs the current thread's syscall (svc #0) from inside the JIT (thread.cpp): 0 = not handled
// here (leave Run()), 1 = done, 2 = done but the thread needs the run loop (signal, exit, ...).
int SyscallInJit();

}  // namespace rn
