// Host functions callable from guest stub libraries via `svc #lib_id` (see thunks.h).
#include "thunks.h"

#include <mutex>
#include <vector>

#include "linux_abi.h"
#include "memory.h"
#include "trace.h"

namespace rn {

namespace {

struct HostLibrary {
    std::string soname;
    std::map<std::string, ThunkFn> by_name;
    // Filled when the guest stub library registers itself.
    std::vector<ThunkFn> by_index;
    std::vector<std::string> names;
    std::map<std::string, u64> stub_addr;
    bool registered = false;
};

std::mutex g_mu;
HostLibrary g_libs[kLibCount];
u64 g_return_trampoline = 0;

void Unimplemented(GuestThread* t, u32 lib, u64 index) {
    const HostLibrary& L = g_libs[lib];
    const char* name = index < L.names.size() ? L.names[index].c_str() : "?";
    static std::mutex warn_mu;
    static std::map<std::string, int> warned;
    std::lock_guard lock(warn_mu);
    if (warned[name]++ == 0)
        Log("unimplemented host function %s!%s (returning 0)", L.soname.c_str(), name);
    // Report "unsupported" where a zero result would mean success with an unusable handle.
    u64 result = 0;
    if (lib == kLibOpenSLES)
        result = 12;  // SL_RESULT_FEATURE_UNSUPPORTED
    else if (lib == kLibAAudio)
        result = static_cast<u64>(-889);  // AAUDIO_ERROR_UNAVAILABLE
    else if (lib == kLibMediaNdk || lib == kLibCamera2)
        result = static_cast<u64>(-10000);
    t->cpu->SetX(0, result);
}

void RegisterFromGuest(GuestThread* t) {
    const u32 lib = static_cast<u32>(t->cpu->X(0));
    const u64 names = t->cpu->X(1), base = t->cpu->X(2), count = t->cpu->X(3);
    if (lib >= kLibCount || count > 65536) {
        Log("bad stub library registration (lib 0x%x, %llu functions)", lib, count);
        return;
    }
    std::lock_guard lock(g_mu);
    HostLibrary& L = g_libs[lib];
    L.by_index.assign(count, nullptr);
    L.names.assign(count, {});
    size_t missing = 0;
    for (u64 i = 0; i < count; ++i) {
        u64 name_ptr = 0;
        SafeCopyFromGuest(&name_ptr, names + i * 8, 8);
        std::string name;
        SafeReadString(name_ptr, name, 256);
        L.names[i] = name;
        L.stub_addr[name] = base + i * 16;
        auto it = L.by_name.find(name);
        if (it != L.by_name.end())
            L.by_index[i] = it->second;
        else
            ++missing;
    }
    L.registered = true;
    RN_INFO("stub library %s registered: %llu functions, %zu without host code", L.soname.c_str(), count,
            missing);
}

}  // namespace

void RegisterHostLibrary(u32 lib_id, const char* soname, std::map<std::string, ThunkFn> handlers) {
    std::lock_guard lock(g_mu);
    HostLibrary& L = g_libs[lib_id];
    L.soname = soname;
    for (auto& [k, v] : handlers)
        L.by_name[k] = v;
}

u64 StubAddress(u32 lib_id, const std::string& name) {
    std::lock_guard lock(g_mu);
    const HostLibrary& L = g_libs[lib_id];
    auto it = L.stub_addr.find(name);
    return it == L.stub_addr.end() ? 0 : it->second;
}

bool DispatchThunk(GuestThread* t, u32 imm) {
    if (imm == kThunkRegisterLib) {
        RegisterFromGuest(t);
        return true;
    }
    if (imm == kThunkTrace || imm == kThunkTraceRet)
        return TraceHit(t, imm);
    if (imm < kThunkFirstLib || imm >= kLibCount)
        return false;
    const u64 index = t->cpu->X(16);
    ThunkFn fn = nullptr;
    {
        // by_index is only written at registration (before any call), so the
        // lock just orders that publication.
        std::lock_guard lock(g_mu);
        const HostLibrary& L = g_libs[imm];
        if (index < L.by_index.size())
            fn = L.by_index[index];
    }
    if (!fn) {
        Unimplemented(t, imm, index);
        return true;
    }
    if (g_verbose >= 3) {
        std::lock_guard lock(g_mu);
        const HostLibrary& L = g_libs[imm];
        Log("host call %s!%s tid %d", L.soname.c_str(), index < L.names.size() ? L.names[index].c_str() : "?", t->tid);
    }
    fn(t);
    return true;
}

void InitThunks() {
    static const std::pair<u32, const char*> kNames[] = {
        {kLibVulkan, "libvulkan.so"}, {kLibAndroid, "libandroid.so"},
        {kLibJni, "librefract_jni.so"}, {kLibEGL, "libEGL.so"}, {kLibGLES, "libGLESv3.so"},
        {kLibAAudio, "libaaudio.so"}, {kLibOpenSLES, "libOpenSLES.so"}, {kLibNativeWindow, "libnativewindow.so"},
        {kLibRefract, "librefract_host.so"}, {kLibOvrPlatform, "libovrplatformloader.so"},
        {kLibMediaNdk, "libmediandk.so"}, {kLibGLESv1, "libGLESv1_CM.so"}, {kLibGLESv2, "libGLESv2.so"},
        {kLibCamera2, "libcamera2ndk.so"}, {kLibJniGraphics, "libjnigraphics.so"}, {kLibSync, "libsync.so"},
        {kLibBinderNdk, "libbinder_ndk.so"}, {kLibAMidi, "libamidi.so"},
    };
    for (const auto& [id, name] : kNames)
        g_libs[id].soname = name;
    // One page holding `svc #kThunkReturn`, used as the return address of host->guest calls.
    s64 page = Mem().Map(0, kPageSize, lx::PROT_READ_ | lx::PROT_WRITE_, lx::MAP_PRIVATE_ | lx::MAP_ANONYMOUS_,
                         nullptr, 0, "[refract:trampolines]");
    if (page < 0)
        Fatal("cannot map trampoline page");
    const u32 svc_return = 0xD4000001u | (kThunkReturn << 5);
    for (u64 off = 0; off < kPageSize; off += 4)
        memcpy(GuestPtr<void>(static_cast<u64>(page) + off), &svc_return, 4);
    // The kernel's vDSO rt_sigreturn trampoline (bionic leaves sa_restorer unset on arm64).
    const u32 sigreturn_code[2] = {0xD2801168u /* movz x8, #139 */, 0xD4000001u /* svc #0 */};
    memcpy(GuestPtr<void>(static_cast<u64>(page) + 16), sigreturn_code, sizeof(sigreturn_code));
    Mem().Protect(static_cast<u64>(page), kPageSize, lx::PROT_READ_ | lx::PROT_EXEC_);
    g_return_trampoline = static_cast<u64>(page);
}

u64 SigReturnTrampoline() { return g_return_trampoline + 16; }

bool RunGuestUntilReturn(GuestThread* t);  // thread.cpp

void CallGuestFull(GuestThread* t, u64 fn, std::initializer_list<u64> args, CpuContext* result) {
    CpuContext saved;
    t->cpu->GetContext(&saved);
    size_t i = 0;
    std::vector<u64> stack_args;
    for (u64 a : args) {
        if (i < 8)
            t->cpu->SetX(static_cast<int>(i), a);
        else
            stack_args.push_back(a);
        ++i;
    }
    u64 sp = (saved.sp - stack_args.size() * 8) & ~15ull;
    if (!stack_args.empty())
        SafeCopyToGuest(sp, stack_args.data(), stack_args.size() * 8);
    t->cpu->SetSp(sp);
    t->cpu->SetX(30, g_return_trampoline);
    t->cpu->SetPc(fn);
    RunGuestUntilReturn(t);
    if (result)
        t->cpu->GetContext(result);
    u64 tpidr = t->cpu->Tpidr();
    t->cpu->SetContext(saved);
    t->cpu->Tpidr() = tpidr;
}

void CallGuestRegs(GuestThread* t, u64 fn, const GuestCallArgs& a, CpuContext* result) {
    CpuContext saved;
    t->cpu->GetContext(&saved);
    for (int i = 0; i < 8; ++i) {
        t->cpu->SetX(i, a.x[i]);
        t->cpu->SetV(i, a.v[i]);
    }
    u64 sp = (saved.sp - a.stack.size() * 8) & ~15ull;
    if (!a.stack.empty())
        SafeCopyToGuest(sp, a.stack.data(), a.stack.size() * 8);
    t->cpu->SetSp(sp);
    t->cpu->SetX(30, g_return_trampoline);
    t->cpu->SetPc(fn);
    RunGuestUntilReturn(t);
    if (result)
        t->cpu->GetContext(result);
    u64 tpidr = t->cpu->Tpidr();
    t->cpu->SetContext(saved);
    t->cpu->Tpidr() = tpidr;
}

u64 CallGuest(GuestThread* t, u64 fn, std::initializer_list<u64> args) {
    CpuContext r;
    CallGuestFull(t, fn, args, &r);
    return r.x[0];
}

}  // namespace rn
