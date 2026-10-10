// refract_native: runs Android ARM64 programs directly on Windows, without an
// emulator. Guest code runs in a JIT; Linux syscalls are implemented on Win32;
// the guest's own bionic libc and linker come from a firmware dump (sysroot).
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

#include "cpu.h"
#include "elf_loader.h"
#include "files.h"
#include "kernel.h"
#include "linux_abi.h"
#include "memory.h"
#include "procfs.h"
#include "properties.h"
#include "thunks.h"
#include "vulkan_hle.h"
#include "android_hle.h"
#include "audio_hle.h"
#include "host_runtime.h"
#include "jni_bridge.h"
#include "trace.h"
#include "vfs.h"

using namespace rn;

namespace {

const char kLdConfig[] =
    "dir.system = /system/bin/\n"
    "dir.system = /data/\n"
    "dir.system = /apex/\n"
    "[system]\n"
    "additional.namespaces =\n"
    "namespace.default.isolated = false\n"
    "namespace.default.search.paths = "
    "/apex/com.android.runtime/${LIB}/bionic:/system/${LIB}:/apex/com.android.art/${LIB}:"
    "/system_ext/${LIB}:/product/${LIB}:/vendor/${LIB}:/apex/com.android.i18n/${LIB}:"
    "/apex/com.android.conscrypt/${LIB}:/apex/com.android.os.statsd/${LIB}\n";

void Usage() {
    fprintf(stderr,
            "usage: refract_native --sysroot <dump fs dir> [options] -- <guest program> [args...]\n"
            "  --sysroot DIR        Android root filesystem (system, apex, vendor, ...)\n"
            "  --symlinks FILE      symlink list (default: DIR\\refract_symlinks.txt)\n"
            "  --data DIR           host directory mounted at /data (default: %%LOCALAPPDATA%%\\Refract\\native\\data)\n"
            "  --mount G=H          mount host directory H at guest path G\n"
            "  --env K=V            add a guest environment variable\n"
            "  --cpus N             CPUs reported to the guest (default: host count, max 8)\n"
            "  --trace LIB!SYM      log calls to a guest function (or LIB+0xOFFSET; trailing ~ = entry only)\n"
            "  -v N                 verbosity (0-3)\n");
}

LONG WINAPI HostCrashFilter(EXCEPTION_POINTERS* ep) {
    auto* r = ep->ExceptionRecord;
    if (g_verbose < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    u64 addr = r->NumberParameters >= 2 ? static_cast<u64>(r->ExceptionInformation[1]) : 0;
    Log("host exception 0x%lx at %p (%s 0x%llx = %s)", r->ExceptionCode, r->ExceptionAddress,
        r->NumberParameters >= 1 && r->ExceptionInformation[0] ? "write" : "read", addr,
        DescribeAddress(addr).c_str());
    if (GuestThread* t = CurrentThread())
        Log("  while running guest thread %d at %s", t->tid, DescribeAddress(t->cpu->Pc()).c_str());
    return EXCEPTION_CONTINUE_SEARCH;
}

// Unhandled host crashes: logs the faulting module and writes refract_native_crash.dmp
// next to the exe. Chains to the previous filter (HotSpot's, once the JVM is up).
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;

LONG WINAPI CrashDumpFilter(EXCEPTION_POINTERS* ep) {
    static thread_local bool inside = false;  // the JVM's filter may chain back to this one
    if (inside)
        return EXCEPTION_CONTINUE_SEARCH;
    inside = true;
    static std::atomic<bool> once{false};
    if (!once.exchange(true)) {
        auto* r = ep->ExceptionRecord;
        HMODULE mod = nullptr;
        wchar_t name[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(r->ExceptionAddress), &mod))
            GetModuleFileNameW(mod, name, MAX_PATH);
        Log("HOST CRASH: exception 0x%lx at %p (%s+0x%llx), thread %lu", r->ExceptionCode, r->ExceptionAddress,
            Narrow(std::filesystem::path(name).filename().wstring()).c_str(),
            static_cast<unsigned long long>(reinterpret_cast<u64>(r->ExceptionAddress) - reinterpret_cast<u64>(mod)),
            GetCurrentThreadId());
        if (GuestThread* t = CurrentThread())
            Log("  while running guest thread %d at %s", t->tid, DescribeAddress(t->cpu->Pc()).c_str());
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring path = (std::filesystem::path(exe).parent_path() / L"refract_native_crash.dmp").wstring();
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                              static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                              &mei, nullptr, nullptr);
            CloseHandle(f);
            Log("  minidump: %s", Narrow(path).c_str());
        }
    }
    LONG r = g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
    inside = false;
    return r;
}

void InstallCrashDumpFilter() {
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(CrashDumpFilter);
    if (prev != CrashDumpFilter)
        g_prev_filter = prev;
}

}  // namespace

int wmain(int argc, wchar_t** wargv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(Narrow(wargv[i]));

    std::wstring sysroot, symlinks, data, guest_libs, app_dir;
    std::vector<std::pair<std::string, std::wstring>> mounts;
    std::vector<std::string> extra_env;
    std::vector<std::pair<std::string, std::string>> props;
    int cpus = 0;
    size_t i = 0;
    for (; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= args.size()) {
                Usage();
                exit(2);
            }
            return args[++i];
        };
        if (a == "--") {
            ++i;
            break;
        } else if (a == "--sysroot") {
            sysroot = Widen(next());
        } else if (a == "--symlinks") {
            symlinks = Widen(next());
        } else if (a == "--prop") {
            std::string p = next();
            size_t eq = p.find('=');
            if (eq != std::string::npos)
                props.emplace_back(p.substr(0, eq), p.substr(eq + 1));
        } else if (a == "--app") {
            app_dir = Widen(next());
        } else if (a == "--guest-libs") {
            guest_libs = Widen(next());
        } else if (a == "--data") {
            data = Widen(next());
        } else if (a == "--mount") {
            std::string m = next();
            size_t eq = m.find('=');
            if (eq == std::string::npos) {
                Usage();
                return 2;
            }
            mounts.emplace_back(m.substr(0, eq), Widen(m.substr(eq + 1)));
        } else if (a == "--env") {
            extra_env.push_back(next());
        } else if (a == "--trace") {
            AddTrace(next());
        } else if (a == "--cpus") {
            cpus = atoi(next().c_str());
        } else if (a == "-v") {
            g_verbose = atoi(next().c_str());
        } else if (!a.empty() && a[0] != '-') {
            break;
        } else {
            Usage();
            return 2;
        }
    }
    if (!app_dir.empty()) {
        if (!LoadAppConfig(app_dir))
            Fatal("cannot read app.properties in %s", Narrow(app_dir).c_str());
    }
    if (sysroot.empty() || (i >= args.size() && app_dir.empty())) {
        Usage();
        return 2;
    }
    std::vector<std::string> guest_argv(args.begin() + i, args.end());
    if (!app_dir.empty())
        guest_argv = {"/system/bin/refract_app", App().package};

    AddVectoredExceptionHandler(0, HostCrashFilter);
    InstallCrashDumpFilter();
    // The JVM replaces the top-level filter when it starts; take it back (chaining to it).
    std::thread([] {
        for (int i = 0; i < 6; ++i) {
            Sleep(5000);
            InstallCrashDumpFilter();
        }
    }).detach();
    // Windows 11 power-throttles (EcoQoS) processes without a foreground window, and this one is
    // driven from the viewer: its threads get parked on E-cores of hybrid CPUs and its timer
    // resolution request is ignored, so the game drops from ~80 to ~24 fps until the scheduler
    // moves it back. Opt out; REFRACT_ECOQOS=1 keeps the Windows default.
    if (!getenv("REFRACT_ECOQOS")) {
        PROCESS_POWER_THROTTLING_STATE pt{};
        pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        pt.StateMask = 0;
        if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt)))
            Log("power throttling opt-out failed: %lu", GetLastError());
    }
    timeBeginPeriod(1);
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (data.empty()) {
        wchar_t buf[MAX_PATH];
        GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        data = std::wstring(buf) + L"\\Refract\\native\\data";
    }
    std::error_code ec;
    std::filesystem::create_directories(data + L"\\local\\tmp", ec);

    // Process identity.
    Process& P = Proc();
    P.pid = 4000 + static_cast<int>(GetCurrentProcessId() % 20000);
    P.next_tid = P.pid + 1;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    P.ncpus = cpus > 0 ? cpus : std::min<int>(8, static_cast<int>(si.dwNumberOfProcessors));
    for (const auto& a : guest_argv) {
        P.cmdline += a;
        P.cmdline += '\0';
    }

    // Guest memory and CPU.
    if (!Mem().Init(0x1000000000ull, 0x4000000000ull) && !Mem().Init(0, 0x4000000000ull))
        Fatal("cannot reserve guest address space");
    Mem().on_code_change = [](u64 a, u64 n) { CpuInvalidateCode(a, n); };
    CpuInit(1024);
    // Refract's GPU-share layer (tools/windows_gpu_layer) shares the eye images Refract's runtime marks
    // with the PC viewer / host bridge, as it does in the emulator's host process. REFRACT_GPU_LAYER=0 skips it.
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::filesystem::path layer_dir =
            std::filesystem::path(exe).parent_path().parent_path() / L"build-windows-gpu-layer" / L"Release";
        const char* want = getenv("REFRACT_GPU_LAYER");
        std::error_code lec;
        if ((!want || strcmp(want, "0") != 0) && std::filesystem::exists(layer_dir / L"refract_gpu_layer.json", lec)) {
            auto append = [](const wchar_t* var, const std::wstring& value) {
                wchar_t old[4096] = L"";
                const DWORD n = GetEnvironmentVariableW(var, old, 4096);
                SetEnvironmentVariableW(var, n ? (std::wstring(old) + L";" + value).c_str() : value.c_str());
            };
            append(L"VK_ADD_LAYER_PATH", layer_dir.wstring());
            append(L"VK_INSTANCE_LAYERS", L"VK_LAYER_REFRACT_gpu_share");
            RN_INFO("Refract GPU-share layer: %s", Narrow(layer_dir.wstring()).c_str());
        }
    }
    InitThunks();
    RegisterVulkanHle();
    RegisterAndroidHle();
    RegisterAudioHle();
    RegisterJniHle();
    RegisterHostRuntime();

    // Filesystem.
    Vfs& fs = Fs();
    for (const char* dir : {"/system", "/apex", "/vendor", "/system_ext", "/product", "/odm"})
        fs.Mount(dir, sysroot + Widen(dir), false);
    fs.Mount("/data", data, true);
    wchar_t exe_buf[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    const std::filesystem::path exe_dir = std::filesystem::path(exe_buf).parent_path();
    // HLE stub libraries replace the firmware's own (which would need Quest hardware).
    if (guest_libs.empty())
        guest_libs = (exe_dir / L"guest" / L"lib64").wstring();
    for (const auto& entry : std::filesystem::directory_iterator(guest_libs, ec)) {
        const std::string name = Narrow(entry.path().filename().wstring());
        fs.Mount("/system/lib64/" + name, entry.path().wstring(), false);
        RN_TRACE("stub library %s", name.c_str());
    }
    for (const auto& [g, h] : mounts)
        fs.Mount(g, h, true);
    if (!app_dir.empty()) {
        const AppConfig& app = App();
        const std::filesystem::path data_dir(data);
        fs.Mount("/system/bin/refract_app", (exe_dir / L"guest" / L"bin" / L"refract_app").wstring(), false);
        fs.Mount(app.apk_guest, app.apk_host, false);
        fs.Mount(app.lib_guest, (std::filesystem::path(app.app_dir) / L"lib" / L"arm64").wstring(), false);
        const std::filesystem::path user_dir = data_dir / L"user" / L"0" / Widen(app.package);
        for (const wchar_t* sub : {L"files", L"cache", L"code_cache", L"shared_prefs", L"databases", L"no_backup"})
            std::filesystem::create_directories(user_dir / sub, ec);
        const std::filesystem::path media = data_dir / L"media" / L"0";
        for (const wchar_t* sub : {L"files", L"cache"})
            std::filesystem::create_directories(media / L"Android" / L"data" / Widen(app.package) / sub, ec);
        std::filesystem::create_directories(media / L"Android" / L"obb" / Widen(app.package), ec);
        fs.Mount("/storage/emulated/0", media.wstring(), true);
        fs.AddSymlink("/data/data", "/data/user/0");
        fs.AddSymlink("/storage/self/primary", "/storage/emulated/0");
        fs.AddSymlink("/sdcard", "/storage/emulated/0");
    }
    if (symlinks.empty())
        symlinks = sysroot + L"\\refract_symlinks.txt";
    if (!fs.LoadSymlinkList(symlinks))
        Log("warning: no symlink list at %s (generate it with scripts/native_sysroot_symlinks.py)",
            Narrow(symlinks).c_str());
    SetupSystemProperties(sysroot, data, props);
    fs.AddSymlink("/bin", "/system/bin");
    fs.AddSymlink("/etc", "/system/etc");
    if (app_dir.empty())
        fs.AddSymlink("/sdcard", "/storage/self/primary");
    SetupProcfs(kLdConfig);

    Fds().InstallAt(0, std::make_shared<StdioFile>(0), false);
    Fds().InstallAt(1, std::make_shared<StdioFile>(1), false);
    Fds().InstallAt(2, std::make_shared<StdioFile>(2), false);
    for (int fd = 0; fd < 3; ++fd)
        Fds().Get(fd)->path = "/dev/pts/0";

    // Load the program and its interpreter.
    const std::string exe_path = Vfs::Normalize(guest_argv[0]);
    P.exe_path = exe_path;
    LoadedElf exe, interp;
    if (int r = LoadElf(exe_path, &exe))
        Fatal("cannot load %s (%d)", exe_path.c_str(), r);
    const bool has_interp = !exe.interp.empty();
    if (has_interp) {
        if (int r = LoadElf(exe.interp, &interp))
            Fatal("cannot load interpreter %s (%d)", exe.interp.c_str(), r);
    }

    std::vector<std::string> envp = {
        "PATH=/product/bin:/apex/com.android.runtime/bin:/system/bin:/system/xbin:/vendor/bin",
        "ANDROID_ROOT=/system",
        "ANDROID_DATA=/data",
        "ANDROID_ART_ROOT=/apex/com.android.art",
        "ANDROID_I18N_ROOT=/apex/com.android.i18n",
        "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata",
        "ANDROID_STORAGE=/storage",
        "EXTERNAL_STORAGE=/sdcard",
        "TMPDIR=/data/local/tmp",
        "HOME=/data",
    };
    if (!app_dir.empty())
        envp.push_back("LD_LIBRARY_PATH=" + App().lib_guest);
    for (const auto& e : extra_env)
        envp.push_back(e);

    const u64 hwcap = lx::HWCAP_FP_ | lx::HWCAP_ASIMD_ | lx::HWCAP_AES_ | lx::HWCAP_PMULL_ | lx::HWCAP_SHA1_ |
                      lx::HWCAP_SHA2_ | lx::HWCAP_CRC32_ | lx::HWCAP_ASIMDDP_;
    u64 sp = SetupInitialStack(guest_argv, envp, exe, has_interp ? &interp : nullptr, exe_path, hwcap, 0);
    P.start_stack = sp;

    auto* main_thread = new GuestThread;
    main_thread->tid = P.pid;
    main_thread->name = "main";
    main_thread->cpu = CreateCpuCore();
    for (int r = 0; r < 31; ++r)
        main_thread->cpu->SetX(r, 0);
    main_thread->cpu->SetSp(sp);
    main_thread->cpu->SetPc(has_interp ? interp.entry : exe.entry);
    RegisterThread(main_thread);
    RN_INFO("starting %s (pid %d) at 0x%llx", exe_path.c_str(), P.pid, main_thread->cpu->Pc());
    atexit([] {
        GuestThread* t = CurrentThread();
        PWSTR desc = nullptr;
        std::string name;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &desc)) && desc) {
            name = Narrow(desc);
            LocalFree(desc);
        }
        Log("process exiting (host thread %lu '%s', guest thread %d)", GetCurrentThreadId(), name.c_str(),
            t ? t->tid : 0);
        void* frames[24];
        USHORT n = CaptureStackBackTrace(1, 24, frames, nullptr);
        for (USHORT i = 0; i < n; ++i) {
            HMODULE m = nullptr;
            wchar_t path[MAX_PATH] = L"?";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(frames[i]), &m))
                GetModuleFileNameW(m, path, MAX_PATH);
            Log("  %s+0x%llx", Narrow(std::filesystem::path(path).filename().wstring()).c_str(),
                static_cast<unsigned long long>(reinterpret_cast<u64>(frames[i]) - reinterpret_cast<u64>(m)));
        }
        fflush(stdout);
    });
    if (const char* sample = getenv("REFRACT_SAMPLE"))
        StartSampler(std::max(1, atoi(sample)));
    if (const char* profile = getenv("REFRACT_PROFILE"))
        StartProfiler(profile);
    if (const char* profile = getenv("REFRACT_HOSTPROF"))
        StartHostProfiler(profile);
    RunGuestThread(main_thread);
    // The main thread called exit() while others keep running: park this host thread.
    for (;;)
        Sleep(INFINITE);
}
