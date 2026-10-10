// Host-side sampling profiler (REFRACT_HOSTPROF=name,name): suspends the host threads of matching
// guest threads every millisecond and attributes their host RIP to a function (dbghelp) or to
// translated code ("[jit]"). Answers "is the time in generated code or in the runtime around it".
#include <windows.h>

#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <map>
#include <thread>
#include <unordered_map>
#include <vector>

#include "kernel.h"

namespace rn {

namespace {

std::string Describe(u64 rip, std::unordered_map<u64, std::string>& cache) {
    auto it = cache.find(rip);
    if (it != cache.end())
        return it->second;
    std::string name;
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(rip), &m)) {
        name = "[jit]";
    } else {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        wchar_t path[MAX_PATH] = L"?";
        GetModuleFileNameW(m, path, MAX_PATH);
        std::string mod = Narrow(path);
        mod = mod.substr(mod.find_last_of("\\/") + 1);
        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(), rip, &disp, sym))
            name = mod + "!" + sym->Name;
        else
            name = mod + "+?";
    }
    cache[rip] = name;
    return name;
}

}  // namespace

void StartHostProfiler(const std::string& filter) {
    std::vector<std::string> names;
    for (size_t at = 0; at <= filter.size();) {
        size_t comma = std::min(filter.find(',', at), filter.size());
        names.push_back(filter.substr(at, comma - at));
        at = comma + 1;
    }
    std::thread([names, filter] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        std::unordered_map<u64, std::string> cache;
        std::map<std::string, std::map<std::string, int>> samples;  // thread -> function -> n
        std::map<unsigned long, HANDLE> handles;
        timeBeginPeriod(1);
        for (DWORD start = GetTickCount();;) {
            Sleep(1);
            std::vector<std::pair<std::string, unsigned long>> targets;
            {
                std::lock_guard lock(Proc().threads_mu);
                for (auto& [tid, t] : Proc().threads)
                    if (t->host_tid && std::any_of(names.begin(), names.end(), [&](const std::string& n) {
                            return t->name.find(n) != std::string::npos;
                        }))
                        targets.push_back({t->name, t->host_tid});
            }
            for (auto& [name, host_tid] : targets) {
                HANDLE& h = handles[host_tid];
                if (!h)
                    h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                   host_tid);
                if (!h || SuspendThread(h) == static_cast<DWORD>(-1))
                    continue;
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL;
                bool ok = GetThreadContext(h, &ctx) != 0;
                ResumeThread(h);
                if (ok)
                    ++samples[name][Describe(ctx.Rip, cache)];
            }
            if (GetTickCount() - start < 20000)
                continue;
            start = GetTickCount();
            Log("--- host profile ('%s') ---", filter.c_str());
            for (auto& [thread, fns] : samples) {
                int total = 0;
                std::vector<std::pair<int, std::string>> top;
                for (auto& [f, n] : fns) {
                    total += n;
                    top.push_back({n, f});
                }
                std::sort(top.rbegin(), top.rend());
                Log("  thread %s: %d samples", thread.c_str(), total);
                for (size_t i = 0; i < top.size() && i < 20; ++i)
                    Log("    %5.1f%% %s", 100.0 * top[i].first / std::max(1, total), top[i].second.c_str());
            }
            samples.clear();
        }
    }).detach();
}

}  // namespace rn
