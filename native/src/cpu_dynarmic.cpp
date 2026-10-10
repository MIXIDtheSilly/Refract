// CpuCore backed by Dynarmic's A64 JIT. Guest memory is identity mapped, so the
// JIT uses fastmem with a zero base and the slow-path callbacks touch host memory
// directly (guarded against faults).
#include <string>
#include <windows.h>

#include <intrin.h>

#include <mutex>
#include <vector>

#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include "cpu.h"

namespace rn {
namespace {

using Dynarmic::HaltReason;
namespace A64 = Dynarmic::A64;

constexpr u64 kCntFrq = 19200000;  // Qualcomm generic timer frequency
constexpr HaltReason kStopHalt = HaltReason::UserDefined1;
constexpr HaltReason kExternalHalt = HaltReason::UserDefined2;

class DynCore;

std::unique_ptr<Dynarmic::ExclusiveMonitor> g_monitor;
std::mutex g_cores_mu;
std::vector<DynCore*> g_cores;  // indexed by monitor slot
u64 g_qpc_freq = 1;

template <typename T>
bool RawLoad(u64 addr, T* out) {
    __try {
        *out = *reinterpret_cast<volatile T*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool RawStore(u64 addr, T v) {
    __try {
        *reinterpret_cast<volatile T*>(addr) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RawCas8(u64 addr, u8 desired, u8 expected, bool* ok) {
    __try {
        *ok = _InterlockedCompareExchange8(reinterpret_cast<volatile char*>(addr), desired, expected) ==
              static_cast<char>(expected);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool RawCas16(u64 addr, u16 desired, u16 expected, bool* ok) {
    __try {
        *ok = _InterlockedCompareExchange16(reinterpret_cast<volatile short*>(addr), desired,
                                            expected) == static_cast<short>(expected);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool RawCas32(u64 addr, u32 desired, u32 expected, bool* ok) {
    __try {
        *ok = _InterlockedCompareExchange(reinterpret_cast<volatile long*>(addr), desired, expected) ==
              static_cast<long>(expected);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool RawCas64(u64 addr, u64 desired, u64 expected, bool* ok) {
    __try {
        *ok = _InterlockedCompareExchange64(reinterpret_cast<volatile long long*>(addr), desired,
                                            expected) == static_cast<long long>(expected);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool RawCas128(u64 addr, Vec128 desired, Vec128 expected, bool* ok) {
    __try {
        long long cmp[2] = {static_cast<long long>(expected[0]), static_cast<long long>(expected[1])};
        *ok = _InterlockedCompareExchange128(reinterpret_cast<volatile long long*>(addr),
                                             static_cast<long long>(desired[1]),
                                             static_cast<long long>(desired[0]), cmp) != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

class DynCore final : public CpuCore, public A64::UserCallbacks {
public:
    explicit DynCore(size_t slot) : slot_(slot) {
        A64::UserConfig c;
        c.callbacks = this;
        c.processor_id = slot;
        c.global_monitor = g_monitor.get();
        c.tpidr_el0 = &tpidr_;
        c.tpidrro_el0 = &tpidrro_;
        c.fastmem_pointer = 0;  // identity mapping
        c.fastmem_address_space_bits = 48;
        c.silently_mirror_fastmem = false;
        c.fastmem_exclusive_access = true;
        c.recompile_on_fastmem_failure = true;
        c.recompile_on_exclusive_fastmem_failure = true;
        c.enable_cycle_counting = false;
        c.wall_clock_cntpct = true;
        c.cntfrq_el0 = kCntFrq;
        // DZP set: DC ZVA is not hooked, so tell libc not to use it.
        c.dczid_el0 = 0x14;
        c.define_unpredictable_behaviour = true;
        c.code_cache_size = 128 * 1024 * 1024;
        // Exact ARM NaN/FMA/FPCR emulation costs some speed in float-heavy game code, but the
        // unsafe FP flags made remote players' arms sink over time in Yeeps (2026-10-10), so FP is
        // exact by default. REFRACT_JIT_FASTFP=all enables every unsafe FP flag, or a comma list of
        // reduced,nan,fpcr,fma to bisect which one matters.
        static const std::string fastfp = [] { const char* e = getenv("REFRACT_JIT_FASTFP"); return std::string(e ? e : ""); }();
        auto fp_on = [&](const char* name) {
            return fastfp == "all" || (!fastfp.empty() && (',' + fastfp + ',').find(std::string(",") + name + ",") != std::string::npos);
        };
        if (fp_on("reduced")) { c.unsafe_optimizations = true; c.optimizations |= Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP; }
        if (fp_on("nan"))     { c.unsafe_optimizations = true; c.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN; }
        if (fp_on("fpcr"))    { c.unsafe_optimizations = true; c.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue; }
        if (fp_on("fma"))     { c.unsafe_optimizations = true; c.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA; }
        // With the global monitor every STXR checks all other monitor slots inline (1023 of them
        // with CpuInit(1024)): kilobytes of code per guest atomic. Ignoring it, a store-exclusive
        // is a lock cmpxchg against the value its load-exclusive saw (ABA-tolerant, like the LSE
        // emulation already is). REFRACT_JIT_MONITOR=1 restores it.
        static const bool monitor = getenv("REFRACT_JIT_MONITOR") && *getenv("REFRACT_JIT_MONITOR") == '1';
        if (!monitor) {
            c.unsafe_optimizations = true;
            c.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        }
        jit_ = std::make_unique<A64::Jit>(c);
    }

    ~DynCore() override {
        std::lock_guard lock(g_cores_mu);
        g_cores[slot_] = nullptr;
        g_monitor->ClearProcessor(slot_);
    }

    StopInfo Run() override {
        for (;;) {
            stop_ = {};
            const HaltReason hr = jit_->Run();
            if (stop_.kind != StopInfo::None)
                return stop_;
            if (Dynarmic::Has(hr, kExternalHalt)) {
                stop_.kind = StopInfo::External;
                return stop_;
            }
            // Cache invalidation or a spurious halt: keep going.
        }
    }

    u64 X(int i) const override { return jit_->GetRegister(i); }
    void SetX(int i, u64 v) override { jit_->SetRegister(i, v); }
    u64 Sp() const override { return jit_->GetSP(); }
    void SetSp(u64 v) override { jit_->SetSP(v); }
    u64 Pc() const override { return jit_->GetPC(); }
    void SetPc(u64 v) override { jit_->SetPC(v); }
    Vec128 V(int i) const override { return jit_->GetVector(i); }
    void SetV(int i, Vec128 v) override { jit_->SetVector(i, v); }
    u64& Tpidr() override { return tpidr_; }
    void ClearExclusive() override { jit_->ClearExclusiveState(); }

    void GetContext(CpuContext* c) const override {
        auto regs = jit_->GetRegisters();
        for (int i = 0; i < 31; ++i)
            c->x[i] = regs[i];
        c->sp = jit_->GetSP();
        c->pc = jit_->GetPC();
        c->pstate = jit_->GetPstate();
        c->fpcr = jit_->GetFpcr();
        c->fpsr = jit_->GetFpsr();
        auto vecs = jit_->GetVectors();
        for (int i = 0; i < 32; ++i)
            c->v[i] = vecs[i];
    }

    void SetContext(const CpuContext& c) override {
        std::array<u64, 31> regs;
        for (int i = 0; i < 31; ++i)
            regs[i] = c.x[i];
        jit_->SetRegisters(regs);
        jit_->SetSP(c.sp);
        jit_->SetPC(c.pc);
        jit_->SetPstate(c.pstate);
        jit_->SetFpcr(c.fpcr);
        jit_->SetFpsr(c.fpsr);
        std::array<A64::Vector, 32> vecs;
        for (int i = 0; i < 32; ++i)
            vecs[i] = c.v[i];
        jit_->SetVectors(vecs);
    }

    void RequestStop() override { jit_->HaltExecution(kExternalHalt); }

    void Invalidate(u64 addr, u64 len) { jit_->InvalidateCacheRange(addr, len); }

    // --- UserCallbacks ---------------------------------------------------------

    std::optional<std::uint32_t> MemoryReadCode(A64::VAddr vaddr) override {
        u32 v;
        if (!RawLoad(vaddr, &v))
            return std::nullopt;
        return v;
    }

    template <typename T>
    T Read(A64::VAddr a) {
        T v{};
        if (!RawLoad(a, &v))
            Fault(a, false);
        return v;
    }
    template <typename T>
    void Write(A64::VAddr a, T v) {
        if (!RawStore(a, v))
            Fault(a, true);
    }

    std::uint8_t MemoryRead8(A64::VAddr a) override { return Read<u8>(a); }
    std::uint16_t MemoryRead16(A64::VAddr a) override { return Read<u16>(a); }
    std::uint32_t MemoryRead32(A64::VAddr a) override { return Read<u32>(a); }
    std::uint64_t MemoryRead64(A64::VAddr a) override { return Read<u64>(a); }
    A64::Vector MemoryRead128(A64::VAddr a) override {
        return {Read<u64>(a), Read<u64>(a + 8)};
    }
    void MemoryWrite8(A64::VAddr a, std::uint8_t v) override { Write(a, v); }
    void MemoryWrite16(A64::VAddr a, std::uint16_t v) override { Write(a, v); }
    void MemoryWrite32(A64::VAddr a, std::uint32_t v) override { Write(a, v); }
    void MemoryWrite64(A64::VAddr a, std::uint64_t v) override { Write(a, v); }
    void MemoryWrite128(A64::VAddr a, A64::Vector v) override {
        Write(a, v[0]);
        Write(a + 8, v[1]);
    }

    bool MemoryWriteExclusive8(A64::VAddr a, std::uint8_t v, std::uint8_t e) override {
        bool ok = false;
        if (!RawCas8(a, v, e, &ok))
            Fault(a, true);
        return ok;
    }
    bool MemoryWriteExclusive16(A64::VAddr a, std::uint16_t v, std::uint16_t e) override {
        bool ok = false;
        if (!RawCas16(a, v, e, &ok))
            Fault(a, true);
        return ok;
    }
    bool MemoryWriteExclusive32(A64::VAddr a, std::uint32_t v, std::uint32_t e) override {
        bool ok = false;
        if (!RawCas32(a, v, e, &ok))
            Fault(a, true);
        return ok;
    }
    bool MemoryWriteExclusive64(A64::VAddr a, std::uint64_t v, std::uint64_t e) override {
        bool ok = false;
        if (!RawCas64(a, v, e, &ok))
            Fault(a, true);
        return ok;
    }
    bool MemoryWriteExclusive128(A64::VAddr a, A64::Vector v, A64::Vector e) override {
        bool ok = false;
        if (!RawCas128(a, v, e, &ok))
            Fault(a, true);
        return ok;
    }

    void InterpreterFallback(A64::VAddr pc, size_t) override {
        // LSE atomics in the ARMv8.2 system libraries land here constantly; a full exit from Run()
        // per atomic is what made lock-heavy code slow.
        if (stop_.kind == StopInfo::None && in_jit_syscalls_ && EmulateInJit(pc))
            return;
        Stop(StopInfo::Interpret, pc);
    }

    void CallSVC(std::uint32_t swi) override {
        // Plain syscalls run right here (dynarmic has already advanced the PC); leaving Run() for
        // each clock_gettime/futex is a large share of a game thread's time.
        if (swi == 0 && stop_.kind == StopInfo::None && in_jit_syscalls_) {
            switch (SyscallInJit()) {
            case 1:
                return;
            case 2:
                stop_.kind = StopInfo::External;
                jit_->HaltExecution(kStopHalt);
                return;
            default:
                break;
            }
        }
        stop_.kind = StopInfo::Svc;
        stop_.imm = swi;
        jit_->HaltExecution(kStopHalt);
    }

    void ExceptionRaised(A64::VAddr pc, A64::Exception e) override {
        switch (e) {
        case A64::Exception::Yield:
        case A64::Exception::WaitForEvent:
        case A64::Exception::WaitForInterrupt:
        case A64::Exception::SendEvent:
        case A64::Exception::SendEventLocal:
            return;
        default:
            break;
        }
        stop_.exception = static_cast<int>(e);
        Stop(StopInfo::Exception, pc);
    }

    void InstructionCacheOperationRaised(A64::InstructionCacheOperation op,
                                         A64::VAddr value) override {
        if (op == A64::InstructionCacheOperation::InvalidateByVAToPoU)
            CpuInvalidateCode(value & ~63ull, 64);
        else
            CpuInvalidateCode(0, ~0ull);
    }

    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return ~0ull >> 4; }

    std::uint64_t GetCNTPCT() override {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        u64 hi;
        u64 lo = _umul128(static_cast<u64>(now.QuadPart), kCntFrq, &hi);
        u64 rem;
        return _udiv128(hi, lo, g_qpc_freq, &rem);
    }

private:
    void Stop(StopInfo::Kind k, u64 pc) {
        if (stop_.kind == StopInfo::None) {
            stop_.kind = k;
            stop_.pc = pc;
        }
        jit_->HaltExecution(kStopHalt);
    }
    void Fault(u64 addr, bool write) {
        if (stop_.kind == StopInfo::None) {
            stop_.kind = StopInfo::Fault;
            stop_.fault_addr = addr;
            stop_.fault_write = write;
            stop_.pc = jit_->GetPC();
        }
        jit_->HaltExecution(kStopHalt);
    }

    // REFRACT_JIT_INPLACE=0: every syscall and emulated instruction leaves Run() (debugging).
    const bool in_jit_syscalls_ = !(getenv("REFRACT_JIT_INPLACE") && *getenv("REFRACT_JIT_INPLACE") == '0');
    size_t slot_;
    u64 tpidr_ = 0;
    u64 tpidrro_ = 0;
    StopInfo stop_;
    std::unique_ptr<A64::Jit> jit_;
};

}  // namespace

void CpuInit(size_t max_cores) {
    g_monitor = std::make_unique<Dynarmic::ExclusiveMonitor>(max_cores);
    g_cores.assign(max_cores, nullptr);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_freq = static_cast<u64>(f.QuadPart);
}

std::unique_ptr<CpuCore> CreateCpuCore() {
    size_t slot = SIZE_MAX;
    {
        std::lock_guard lock(g_cores_mu);
        for (size_t i = 0; i < g_cores.size(); ++i) {
            if (!g_cores[i]) {
                slot = i;
                g_cores[i] = reinterpret_cast<DynCore*>(1);  // reserved
                break;
            }
        }
    }
    if (slot == SIZE_MAX)
        Fatal("too many guest threads (%zu)", g_cores.size());
    auto core = std::make_unique<DynCore>(slot);
    std::lock_guard lock(g_cores_mu);
    g_cores[slot] = core.get();
    return core;
}

void CpuInvalidateCode(u64 addr, u64 len) {
    std::lock_guard lock(g_cores_mu);
    for (DynCore* c : g_cores)
        if (c && c != reinterpret_cast<DynCore*>(1))
            c->Invalidate(addr, len);
}

}  // namespace rn
