// Instructions Dynarmic doesn't translate. Quest system libraries are built for
// ARMv8.2, so ARMv8.1 LSE atomics (CAS, LDADD, SWP, ...) and RCpc loads show up.
// Guest memory is host memory, so host interlocked operations give the same
// atomicity. This is a slow path (each one leaves the JIT).
#include <windows.h>

#include <intrin.h>

#include "kernel.h"

namespace rn {
namespace {

u64 ReadReg(GuestThread* t, int r, bool sp_for_31) {
    if (r == 31)
        return sp_for_31 ? t->cpu->Sp() : 0;
    return t->cpu->X(r);
}

void WriteReg(GuestThread* t, int r, u64 v) {
    if (r != 31)
        t->cpu->SetX(r, v);
}

template <typename T>
T Op(int opc, T old, T operand) {
    using S = std::make_signed_t<T>;
    switch (opc) {
    case 0: return static_cast<T>(old + operand);              // LDADD
    case 1: return static_cast<T>(old & ~operand);             // LDCLR
    case 2: return static_cast<T>(old ^ operand);              // LDEOR
    case 3: return static_cast<T>(old | operand);              // LDSET
    case 4: return static_cast<S>(old) > static_cast<S>(operand) ? old : operand;  // LDSMAX
    case 5: return static_cast<S>(old) < static_cast<S>(operand) ? old : operand;  // LDSMIN
    case 6: return old > operand ? old : operand;              // LDUMAX
    case 7: return old < operand ? old : operand;              // LDUMIN
    case 8: return operand;                                    // SWP
    }
    return old;
}

template <typename T>
T HostCas(volatile T* p, T desired, T expected) {
    if constexpr (sizeof(T) == 1)
        return static_cast<T>(_InterlockedCompareExchange8(reinterpret_cast<volatile char*>(p),
                                                           static_cast<char>(desired),
                                                           static_cast<char>(expected)));
    else if constexpr (sizeof(T) == 2)
        return static_cast<T>(_InterlockedCompareExchange16(reinterpret_cast<volatile short*>(p),
                                                            static_cast<short>(desired),
                                                            static_cast<short>(expected)));
    else if constexpr (sizeof(T) == 4)
        return static_cast<T>(_InterlockedCompareExchange(reinterpret_cast<volatile long*>(p),
                                                          static_cast<long>(desired),
                                                          static_cast<long>(expected)));
    else
        return static_cast<T>(_InterlockedCompareExchange64(reinterpret_cast<volatile long long*>(p),
                                                            static_cast<long long>(desired),
                                                            static_cast<long long>(expected)));
}

template <typename T>
bool AtomicRmw(u64 addr, int opc, T operand, T* old_out) {
    __try {
        volatile T* p = reinterpret_cast<volatile T*>(addr);
        T old = *p;
        for (;;) {
            T desired = Op<T>(opc, old, operand);
            T seen = HostCas<T>(p, desired, old);
            if (seen == old)
                break;
            old = seen;
        }
        *old_out = old;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool AtomicCas(u64 addr, T expected, T desired, T* old_out) {
    __try {
        *old_out = HostCas<T>(reinterpret_cast<volatile T*>(addr), desired, expected);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool PlainLoad(u64 addr, T* out) {
    __try {
        *out = *reinterpret_cast<volatile T*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CasPair128(u64 addr, u64 e0, u64 e1, u64 d0, u64 d1, u64* o0, u64* o1) {
    __try {
        long long cmp[2] = {static_cast<long long>(e0), static_cast<long long>(e1)};
        _InterlockedCompareExchange128(reinterpret_cast<volatile long long*>(addr),
                                       static_cast<long long>(d1), static_cast<long long>(d0), cmp);
        *o0 = static_cast<u64>(cmp[0]);
        *o1 = static_cast<u64>(cmp[1]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool DoRmw(GuestThread* t, u32 insn, int opc, u64* fault) {
    const int rs = (insn >> 16) & 31, rn = (insn >> 5) & 31, rt = insn & 31;
    u64 addr = ReadReg(t, rn, true);
    T old;
    if (!AtomicRmw<T>(addr, opc, static_cast<T>(ReadReg(t, rs, false)), &old)) {
        *fault = addr;
        return false;
    }
    WriteReg(t, rt, static_cast<u64>(old));
    return true;
}

template <typename T>
bool DoCas(GuestThread* t, u32 insn, u64* fault) {
    const int rs = (insn >> 16) & 31, rn = (insn >> 5) & 31, rt = insn & 31;
    u64 addr = ReadReg(t, rn, true);
    T old;
    if (!AtomicCas<T>(addr, static_cast<T>(ReadReg(t, rs, false)), static_cast<T>(ReadReg(t, rt, false)),
                      &old)) {
        *fault = addr;
        return false;
    }
    WriteReg(t, rs, static_cast<u64>(old));
    return true;
}

template <typename T>
bool DoLoad(GuestThread* t, u64 addr, int rt, u64* fault) {
    T v;
    if (!PlainLoad<T>(addr, &v)) {
        *fault = addr;
        return false;
    }
    WriteReg(t, rt, static_cast<u64>(v));
    return true;
}

// Returns 1 handled, 0 not recognised, -1 memory fault (*fault set).
int Emulate(GuestThread* t, u32 insn, u64* fault) {
    const u32 size = insn >> 30;
    // Atomic memory operations: size 111 0 00 A R 1 Rs o3 opc 00 Rn Rt
    if ((insn & 0x3F200C00u) == 0x38200000u) {
        const int o3 = (insn >> 15) & 1, opc = (insn >> 12) & 7;
        if (o3 == 1 && opc == 4) {
            // LDAPR{B,H} (RCpc load-acquire): a plain load on x86.
            const int rn = (insn >> 5) & 31, rt = insn & 31;
            u64 addr = ReadReg(t, rn, true);
            bool ok = size == 0 ? DoLoad<u8>(t, addr, rt, fault)
                      : size == 1 ? DoLoad<u16>(t, addr, rt, fault)
                      : size == 2 ? DoLoad<u32>(t, addr, rt, fault)
                                  : DoLoad<u64>(t, addr, rt, fault);
            return ok ? 1 : -1;
        }
        if (o3 == 1 && opc != 0)
            return 0;
        const int op = o3 ? 8 : opc;
        bool ok = size == 0 ? DoRmw<u8>(t, insn, op, fault)
                  : size == 1 ? DoRmw<u16>(t, insn, op, fault)
                  : size == 2 ? DoRmw<u32>(t, insn, op, fault)
                              : DoRmw<u64>(t, insn, op, fault);
        return ok ? 1 : -1;
    }
    // CAS{A,L,AL}{B,H,}: size 0010001 L 1 Rs o0 11111 Rn Rt
    if ((insn & 0x3FA07C00u) == 0x08A07C00u) {
        bool ok = size == 0 ? DoCas<u8>(t, insn, fault)
                  : size == 1 ? DoCas<u16>(t, insn, fault)
                  : size == 2 ? DoCas<u32>(t, insn, fault)
                              : DoCas<u64>(t, insn, fault);
        return ok ? 1 : -1;
    }
    // CASP{A,L,AL}: 0 sz 0010000 L 1 Rs o0 11111 Rn Rt
    if ((insn & 0xBFA07C00u) == 0x08207C00u) {
        const bool is64 = (insn >> 30) & 1;
        const int rs = (insn >> 16) & 31, rn = (insn >> 5) & 31, rt = insn & 31;
        u64 addr = ReadReg(t, rn, true);
        u64 e0 = ReadReg(t, rs, false), e1 = ReadReg(t, rs + 1, false);
        u64 d0 = ReadReg(t, rt, false), d1 = ReadReg(t, rt + 1, false);
        if (is64) {
            u64 o0, o1;
            if (!CasPair128(addr, e0, e1, d0, d1, &o0, &o1)) {
                *fault = addr;
                return -1;
            }
            WriteReg(t, rs, o0);
            WriteReg(t, rs + 1, o1);
        } else {
            u64 exp = (e0 & 0xffffffffu) | (e1 << 32);
            u64 des = (d0 & 0xffffffffu) | (d1 << 32);
            u64 old;
            if (!AtomicCas<u64>(addr, exp, des, &old)) {
                *fault = addr;
                return -1;
            }
            WriteReg(t, rs, old & 0xffffffffu);
            WriteReg(t, rs + 1, old >> 32);
        }
        return 1;
    }
    // LDAPUR (RCpc, unscaled offset, ARMv8.4): size 011001 opc(2) 0 imm9 00 Rn Rt
    if ((insn & 0x3F200C00u) == 0x19000000u) {
        const int opc = (insn >> 22) & 3;
        const int rn = (insn >> 5) & 31, rt = insn & 31;
        s64 imm = static_cast<s64>(static_cast<s32>(((insn >> 12) & 0x1ff) << 23) >> 23);
        u64 addr = ReadReg(t, rn, true) + imm;
        if (opc == 0)
            return 0;  // STLUR: handled below
        bool ok;
        if (opc == 1) {
            ok = size == 0 ? DoLoad<u8>(t, addr, rt, fault)
                 : size == 1 ? DoLoad<u16>(t, addr, rt, fault)
                 : size == 2 ? DoLoad<u32>(t, addr, rt, fault)
                             : DoLoad<u64>(t, addr, rt, fault);
            return ok ? 1 : -1;
        }
        return 0;
    }
    return 0;
}

}  // namespace

bool EmulateInstruction(GuestThread* t, u64 pc) {
    u32 insn;
    if (!SafeCopyFromGuest(&insn, pc, 4))
        return false;
    u64 fault = 0;
    int r = Emulate(t, insn, &fault);
    if (r == 0)
        return false;
    if (r < 0) {
        t->cpu->SetPc(pc);
        RaiseFault(t, lx::SIGSEGV_, 1, fault, pc);
        return true;
    }
    t->cpu->SetPc(pc + 4);
    return true;
}

// Called from inside the JIT (its interpreter-fallback callback, guest registers flushed): emulates
// the instruction and lets the JIT continue at pc + 4 without leaving Run(). Faults and unknown
// instructions return false and take the slow path above.
bool EmulateInJit(u64 pc) {
    GuestThread* t = CurrentThread();
    u32 insn;
    if (!t || !SafeCopyFromGuest(&insn, pc, 4))
        return false;
    u64 fault = 0;
    if (Emulate(t, insn, &fault) != 1)
        return false;
    t->cpu->SetPc(pc + 4);
    static std::atomic<u64> count{0};
    if (((count.fetch_add(1) + 1) & ((1u << 24) - 1)) == 0)
        RN_INFO("JIT: %llu M atomics emulated in place", static_cast<unsigned long long>((count.load() >> 20)));
    return true;
}

}  // namespace rn
