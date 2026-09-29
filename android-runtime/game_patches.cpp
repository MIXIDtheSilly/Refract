#include "game_patches.h"

#if defined(__ANDROID__)
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace refract::runtime {
namespace {

struct CodePatch {
    const char* name;
    const char* library;       // Basename of the loaded library.
    uintptr_t vaddr;           // Address of the patched instruction in the library's ELF.
    uint32_t before[3];        // Expected words at vaddr - 4, vaddr, vaddr + 4.
    uint32_t replacement;      // New word at vaddr.
    bool applied;
};

// Assassin's Creed Nexus VR (Unity 2021.3.12f1, libil2cpp build from 2025-04-13).
// NexusVR.AppSWManager.UpdateAppSWStatus enables Application SpaceWarp whenever its mode is Always,
// and NexusUtils.PhaseSyncHack then spin-waits every frame out to 21.25 ms (45 fps + reprojection on
// a Quest 2). Refract has no SpaceWarp, so that is a hard 45.7 fps cap with half of UnityMain spinning.
// The patch turns `cbnz x9, disabled` (mode != Always) into `b disabled`: the game runs as it does in
// its passthrough scenes (mode Never), with SpaceWarp off, its normal fixed timestep, and no spin.
CodePatch g_patches[] = {
    {"AC Nexus: Application SpaceWarp off", "libil2cpp.so", 0x4770b90,
     {0xd360fd09 /* lsr x9, x8, #32 */, 0xb50001c9 /* cbnz x9, +0x38 */, 0x72001d1f /* tst w8, #0xff */},
     0x1400000e /* b +0x38 */, false},
};

struct Search {
    const char* library;
    uintptr_t vaddr;  // Start of the three words to compare (patch vaddr - 4).
    uintptr_t bias;
    bool found;
    bool mapped;      // All three words lie in one executable segment of the library.
};

int find_library(dl_phdr_info* info, size_t, void* data)
{
    auto* search = static_cast<Search*>(data);
    const char* name = info->dlpi_name ? std::strrchr(info->dlpi_name, '/') : nullptr;
    name = name ? name + 1 : info->dlpi_name;
    if (!name || std::strcmp(name, search->library) != 0) return 0;
    search->bias = info->dlpi_addr;
    search->found = true;
    // Other games have libraries of the same name that are smaller than the patched build
    // (North Star's libil2cpp.so ends before the AC Nexus offset): reading there crashed them.
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto& segment = info->dlpi_phdr[i];
        if (segment.p_type == PT_LOAD && (segment.p_flags & PF_X) && search->vaddr >= segment.p_vaddr &&
            search->vaddr + 3 * sizeof(uint32_t) <= segment.p_vaddr + segment.p_filesz)
            search->mapped = true;
    }
    return 1;
}

void apply(CodePatch& patch)
{
    Search search{patch.library, patch.vaddr - sizeof(uint32_t), 0, false, false};
    dl_iterate_phdr(find_library, &search);
    if (!search.found) return;  // Not this game, or not loaded yet.
    if (!search.mapped) {
        patch.applied = true;  // A different build: never look again.
        return;
    }
    auto* code = reinterpret_cast<uint32_t*>(search.bias + patch.vaddr);
    if (code[0] == patch.replacement && code[-1] == patch.before[0] && code[1] == patch.before[2]) {
        patch.applied = true;
        return;
    }
    if (code[-1] != patch.before[0] || code[0] != patch.before[1] || code[1] != patch.before[2]) {
        patch.applied = true;  // A different build: never look again.
        return;
    }
    const uintptr_t pageSize = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    void* page = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(code) & ~(pageSize - 1));
    if (mprotect(page, pageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        __android_log_print(ANDROID_LOG_WARN, "Refract.Patch", "%s: cannot make code writable", patch.name);
        patch.applied = true;
        return;
    }
    code[0] = patch.replacement;
    __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(code + 1));
    mprotect(page, pageSize, PROT_READ | PROT_EXEC);
    patch.applied = true;
    __android_log_print(ANDROID_LOG_INFO, "Refract.Patch", "applied %s (%s+0x%lx)", patch.name, patch.library,
                        static_cast<unsigned long>(patch.vaddr));
}

} // namespace

void apply_game_patches()
{
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX]{};
        __system_property_get("debug.refract.game_patches", value);
        return std::strcmp(value, "0") != 0;
    }();
    if (!enabled) return;
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    for (auto& patch : g_patches) {
        if (!patch.applied) apply(patch);
    }
}

void apply_unity_quality()
{
    static const int target = [] {
        char value[PROP_VALUE_MAX]{};
        __system_property_get("debug.refract.texture_mip_limit", value);
        return std::atoi(value);
    }();
    if (target <= 0) return;
    // Unity's quality setters assert the main thread.
    char thread[16]{};
    if (prctl(PR_GET_NAME, thread) != 0 || std::strcmp(thread, "UnityMain") != 0) return;
    static int64_t nextCheckNs = 0;
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t nowNs = int64_t(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
    if (nowNs < nextCheckNs) return;
    nextCheckNs = nowNs + 1'000'000'000;

    using ResolveIcall = void* (*)(const char*);
    using GetLimit = int (*)();
    using SetLimit = void (*)(int);
    static GetLimit get = nullptr;
    static SetLimit set = nullptr;
    static int attempts = 0;
    if (!get || !set) {
        if (attempts >= 30) return;  // Not an IL2CPP Unity game with this API.
        ++attempts;
        // The runtime may live in another linker namespace, so open the game's copy by its full path.
        std::string path;
        dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) {
            const char* name = info->dlpi_name ? std::strrchr(info->dlpi_name, '/') : nullptr;
            if (!name || std::strcmp(name + 1, "libil2cpp.so") != 0) return 0;
            *static_cast<std::string*>(data) = info->dlpi_name;
            return 1;
        }, &path);
        if (path.empty()) return;  // Not loaded yet, or not IL2CPP.
        void* il2cpp = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
        auto resolve = il2cpp ? reinterpret_cast<ResolveIcall>(dlsym(il2cpp, "il2cpp_resolve_icall")) : nullptr;
        if (!resolve) {
            if (attempts == 30)
                __android_log_print(ANDROID_LOG_WARN, "Refract.Patch", "texture mip limit: cannot open %s: %s", path.c_str(), dlerror());
            return;
        }
        get = reinterpret_cast<GetLimit>(resolve("UnityEngine.QualitySettings::get_globalTextureMipmapLimit()"));
        set = reinterpret_cast<SetLimit>(resolve("UnityEngine.QualitySettings::set_globalTextureMipmapLimit(System.Int32)"));
        if (!get || !set) {
            get = reinterpret_cast<GetLimit>(resolve("UnityEngine.QualitySettings::get_masterTextureLimit()"));
            set = reinterpret_cast<SetLimit>(resolve("UnityEngine.QualitySettings::set_masterTextureLimit(System.Int32)"));
        }
        if (!get || !set) {
            if (attempts == 30)
                __android_log_print(ANDROID_LOG_WARN, "Refract.Patch", "texture mip limit: Unity icalls not found");
            return;
        }
    }
    const int current = get();
    if (current >= target) return;
    set(target);
    __android_log_print(ANDROID_LOG_INFO, "Refract.Patch", "texture mip limit %d -> %d (now %d)", current, target, get());
}

} // namespace refract::runtime

#else

namespace refract::runtime {
void apply_game_patches() {}
void apply_unity_quality() {}
} // namespace refract::runtime

#endif
