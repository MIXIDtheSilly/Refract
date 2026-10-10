// Host functions called from guest stub libraries.
//
// A stub library is an ARM64 .so whose exports look like
//     name: movz x16, #index ; svc #lib_id ; ret
// The svc stops the JIT; DispatchThunk runs the host handler registered under
// that library and function name, which reads AAPCS64 arguments from the guest
// registers and writes the result back. The library's constructor registers its
// name table (svc #kThunkRegisterLib) so the host can map index -> handler and
// name -> stub address (needed for vkGetInstanceProcAddr and friends).
#pragma once

#include <bit>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "kernel.h"

namespace rn {

// svc immediates below 0x100 are runtime control operations.
enum : u32 {
    kThunkSyscall = 0,
    kThunkReturn = 1,       // CallGuest return trampoline
    kThunkRegisterLib = 2,  // x0 lib id, x1 names**, x2 stub base, x3 count
    kThunkTrace = 3,        // --trace function entry (trace.cpp)
    kThunkTraceRet = 4,     // --trace function return
    kThunkFirstLib = 0x10,
};

// Fixed library ids (shared with tools/gen_stubs.py).
enum : u32 {
    kLibVulkan = 0x10,
    kLibAndroid = 0x12,
    kLibJni = 0x13,
    kLibEGL = 0x14,
    kLibGLES = 0x15,
    kLibAAudio = 0x16,
    kLibOpenSLES = 0x17,
    kLibNativeWindow = 0x18,
    kLibRefract = 0x19,  // runtime helpers for guest-side glue code
    kLibOvrPlatform = 0x1a,
    kLibMediaNdk = 0x1b,
    kLibGLESv1 = 0x1c,
    kLibGLESv2 = 0x1d,
    kLibCamera2 = 0x1e,
    kLibJniGraphics = 0x1f,
    kLibSync = 0x20,
    kLibBinderNdk = 0x21,
    kLibAMidi = 0x22,
    kLibCount = 0x30,
};

using ThunkFn = void (*)(GuestThread*);

// Registers host handlers for a stub library (call before the guest loads it).
void RegisterHostLibrary(u32 lib_id, const char* soname, std::map<std::string, ThunkFn> handlers);
// Guest address of a stub (after the stub library registered itself), or 0.
u64 StubAddress(u32 lib_id, const std::string& name);

// A guest call with explicit register contents (AAPCS64).
struct GuestCallArgs {
    u64 x[8] = {};
    Vec128 v[8] = {};
    std::vector<u64> stack;  // 8-byte stack slots
};
void CallGuestRegs(GuestThread* t, u64 fn, const GuestCallArgs& a, CpuContext* result);

// Calls a guest function on the current guest thread and returns x0.
u64 CallGuest(GuestThread* t, u64 fn, std::initializer_list<u64> args);
// Same, returning the full register state (for float/double/struct results).
void CallGuestFull(GuestThread* t, u64 fn, std::initializer_list<u64> args, CpuContext* result);
void InitThunks();

// --- AAPCS64 argument marshalling -------------------------------------------

class ArgReader {
public:
    explicit ArgReader(GuestThread* t) : t_(t), nsaa_(t->cpu->Sp()) {}

    template <typename T>
    T Get() {
        if constexpr (std::is_floating_point_v<T>) {
            if (nsrn_ < 8) {
                Vec128 v = t_->cpu->V(nsrn_++);
                if constexpr (sizeof(T) == 4)
                    return std::bit_cast<float>(static_cast<u32>(v[0]));
                else
                    return std::bit_cast<double>(v[0]);
            }
            u64 raw = Stack();
            if constexpr (sizeof(T) == 4)
                return std::bit_cast<float>(static_cast<u32>(raw));
            else
                return std::bit_cast<double>(raw);
        } else {
            static_assert(std::is_integral_v<T> || std::is_pointer_v<T> || std::is_enum_v<T>,
                          "unsupported argument type");
            u64 raw = ngrn_ < 8 ? t_->cpu->X(ngrn_++) : Stack();
            if constexpr (std::is_pointer_v<T>)
                return reinterpret_cast<T>(static_cast<uintptr_t>(raw));
            else
                return static_cast<T>(raw);
        }
    }

    // Raw access for variadic or hand-decoded calls.
    u64 NextInt() { return ngrn_ < 8 ? t_->cpu->X(ngrn_++) : Stack(); }
    double NextDouble() { return Get<double>(); }
    u64 StackPointer() const { return nsaa_; }

private:
    u64 Stack() {
        u64 v = 0;
        SafeCopyFromGuest(&v, nsaa_, 8);
        nsaa_ += 8;
        return v;
    }
    GuestThread* t_;
    int ngrn_ = 0;
    int nsrn_ = 0;
    u64 nsaa_;
};

template <typename T>
void SetReturn(GuestThread* t, T v) {
    if constexpr (std::is_same_v<T, float>) {
        t->cpu->SetV(0, {std::bit_cast<u32>(v), 0});
    } else if constexpr (std::is_same_v<T, double>) {
        t->cpu->SetV(0, {std::bit_cast<u64>(v), 0});
    } else if constexpr (std::is_pointer_v<T>) {
        t->cpu->SetX(0, reinterpret_cast<u64>(v));
    } else if constexpr (std::is_enum_v<T>) {
        t->cpu->SetX(0, static_cast<u64>(static_cast<std::underlying_type_t<T>>(v)));
    } else {
        static_assert(std::is_integral_v<T>, "unsupported return type");
        // Sign-extend signed results into x0 (callers may read x0 or w0).
        if constexpr (std::is_signed_v<T>)
            t->cpu->SetX(0, static_cast<u64>(static_cast<s64>(v)));
        else
            t->cpu->SetX(0, static_cast<u64>(v));
    }
}

template <typename F>
struct FnTraits;
template <typename R, typename... A>
struct FnTraits<R (*)(A...)> {
    using Ret = R;
    using Args = std::tuple<A...>;
};

// Wrap<&f> turns an ordinary host function into a ThunkFn.
template <auto F>
void Wrap(GuestThread* t) {
    using Traits = FnTraits<decltype(F)>;
    using R = typename Traits::Ret;
    ArgReader rd(t);
    auto call = [&]<typename... A>(std::tuple<A...>*) {
        // Braced init evaluates left to right, matching AAPCS64 allocation order.
        std::tuple<A...> args{rd.Get<A>()...};
        if constexpr (std::is_void_v<R>)
            std::apply(F, args);
        else
            SetReturn<R>(t, std::apply(F, args));
    };
    call(static_cast<typename Traits::Args*>(nullptr));
}

}  // namespace rn
