// Generated /proc, /sys and /dev entries, plus the linker configuration.
#include "procfs.h"

#include <windows.h>

#include <cctype>
#include <string>

#include "files.h"
#include "kernel.h"
#include "memory.h"
#include "vfs.h"

namespace rn {

namespace {

// "/proc/1234/x" or "/proc/self/x" or "/proc/thread-self/x" -> "x" (empty for the dir)
std::optional<std::string> SelfRelative(const std::string& p) {
    const std::string pid = std::to_string(Proc().pid);
    for (const std::string& prefix :
         {std::string("/proc/self"), std::string("/proc/thread-self"), "/proc/" + pid}) {
        if (p == prefix)
            return std::string();
        if (p.size() > prefix.size() && p.compare(0, prefix.size(), prefix) == 0 && p[prefix.size()] == '/')
            return p.substr(prefix.size() + 1);
    }
    return std::nullopt;
}

std::string ThreadName(int tid) {
    std::lock_guard lock(Proc().threads_mu);
    auto it = Proc().threads.find(tid);
    if (it == Proc().threads.end())
        return {};
    return it->second->name;
}

std::string StatLine(int tid, const std::string& comm) {
    u64 start_ticks = 100;
    return Format("%d (%s) R 1 %d %d 0 -1 4194624 0 0 0 0 0 0 0 0 20 0 %zu 0 %llu 0 0 "
                  "18446744073709551615 0 0 0 0 0 0 0 4612 1640 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                  tid, comm.c_str(), Proc().pid, Proc().pid, Proc().threads.size(), start_ticks);
}

std::optional<std::string> ProcSelf(const std::string& rel) {
    Process& P = Proc();
    std::string comm = P.package.empty() ? "app_process64" : P.package;
    if (comm.size() > 15)
        comm = comm.substr(comm.size() - 15);
    if (rel == "maps" || rel == "smaps")
        return Mem().ProcMaps();
    if (rel == "cmdline")
        return P.cmdline;
    if (rel == "comm")
        return comm + "\n";
    if (rel == "stat")
        return StatLine(P.pid, comm);
    if (rel == "status")
        return Format("Name:\t%s\nUmask:\t0077\nState:\tR (running)\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t1\n"
                      "TracerPid:\t0\nUid:\t10100\t10100\t10100\t10100\nGid:\t10100\t10100\t10100\t10100\n"
                      "FDSize:\t256\nVmPeak:\t 4000000 kB\nVmSize:\t 4000000 kB\nVmRSS:\t  500000 kB\n"
                      "Threads:\t%zu\nSigQ:\t0/27000\nSigPnd:\t0000000000000000\nCpus_allowed:\tff\n"
                      "Cpus_allowed_list:\t0-%d\n",
                      comm.c_str(), P.pid, P.pid, P.threads.size(), P.ncpus - 1);
    if (rel == "environ" || rel == "auxv" || rel == "mountinfo" || rel == "mounts")
        return std::string();
    if (rel == "oom_score_adj" || rel == "oom_adj")
        return std::string("0\n");
    if (rel == "statm")
        return std::string("1000000 125000 10000 100 0 100000 0\n");
    if (rel.rfind("task/", 0) == 0) {
        // task/<tid>/{comm,stat,status}
        size_t slash = rel.find('/', 5);
        if (slash == std::string::npos)
            return std::nullopt;
        int tid = atoi(rel.substr(5, slash - 5).c_str());
        std::string leaf = rel.substr(slash + 1);
        std::string name = ThreadName(tid);
        if (name.empty() && tid != P.pid)
            return std::nullopt;
        if (name.empty())
            name = comm;
        if (leaf == "comm")
            return name + "\n";
        if (leaf == "stat")
            return StatLine(tid, name);
        if (leaf == "status")
            return Format("Name:\t%s\nPid:\t%d\nTgid:\t%d\n", name.c_str(), tid, P.pid);
        return std::nullopt;
    }
    return std::nullopt;
}

std::string CpuInfo(int ncpus) {
    std::string s;
    for (int i = 0; i < ncpus; ++i) {
        s += Format("processor\t: %d\nBogoMIPS\t: 38.40\n"
                    "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 asimddp\n"
                    "CPU implementer\t: 0x51\nCPU architecture: 8\nCPU variant\t: 0x%d\n"
                    "CPU part\t: 0x%s\nCPU revision\t: %d\n\n",
                    i, i < 4 ? 7 : 1, i < 4 ? "803" : "804", i < 4 ? 12 : 14);
    }
    s += "Hardware\t: Qualcomm Technologies, Inc KONA\n";
    return s;
}

std::string MemInfo() {
    MEMORYSTATUSEX ms{sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    u64 total = ms.ullTotalPhys / 1024, avail = ms.ullAvailPhys / 1024;
    return Format("MemTotal:       %llu kB\nMemFree:        %llu kB\nMemAvailable:   %llu kB\n"
                  "Buffers:               0 kB\nCached:          1000000 kB\nSwapCached:            0 kB\n"
                  "SwapTotal:             0 kB\nSwapFree:              0 kB\n",
                  total, avail, avail);
}

}  // namespace

void SetupProcfs(const std::string& ld_config) {
    Vfs& fs = Fs();
    fs.AddGenerator("/proc", [](const std::string& p) -> std::optional<std::string> {
        int ncpus = Proc().ncpus;
        if (auto rel = SelfRelative(p)) {
            if (rel->empty())
                return std::nullopt;  // directory
            return ProcSelf(*rel);
        }
        if (p == "/proc/cpuinfo")
            return CpuInfo(ncpus);
        if (p == "/proc/meminfo")
            return MemInfo();
        if (p == "/proc/version")
            return std::string("Linux version 4.19.191-refract (refract@native) #1 SMP PREEMPT\n");
        if (p == "/proc/stat") {
            std::string s = "cpu  1000 0 1000 100000 0 0 0 0 0 0\n";
            for (int i = 0; i < ncpus; ++i)
                s += Format("cpu%d 100 0 100 10000 0 0 0 0 0 0\n", i);
            return s;
        }
        if (p == "/proc/sys/kernel/random/boot_id")
            return std::string("6f2c1a0e-8a5c-4b1e-9d1c-5e0d6c7a9b10\n");
        if (p == "/proc/sys/kernel/pid_max")
            return std::string("32768\n");
        if (p == "/proc/sys/vm/overcommit_memory")
            return std::string("1\n");
        if (p == "/proc/loadavg")
            return std::string("1.00 1.00 1.00 2/500 4242\n");
        return std::nullopt;
    });
    fs.AddLinkResolver([](const std::string& p) -> std::optional<std::string> {
        auto rel = SelfRelative(p);
        if (!rel)
            return std::nullopt;
        if (*rel == "exe")
            return Proc().exe_path;
        if (*rel == "cwd")
            return Fs().Cwd();
        if (rel->rfind("fd/", 0) == 0) {
            const std::string num = rel->substr(3);
            if (num.empty() || !std::isdigit(static_cast<unsigned char>(num[0])))
                return std::nullopt;
            FilePtr f = Fds().Get(atoi(num.c_str()));
            if (!f)
                return std::nullopt;
            return f->path;
        }
        return std::nullopt;
    });

    fs.AddGenerator("/sys/devices/system/cpu", [](const std::string& p) -> std::optional<std::string> {
        int n = Proc().ncpus;
        std::string range = Format("0-%d\n", n - 1);
        if (p == "/sys/devices/system/cpu/possible" || p == "/sys/devices/system/cpu/present" ||
            p == "/sys/devices/system/cpu/online")
            return range;
        int cpu;
        char leaf[128];
        if (sscanf(p.c_str(), "/sys/devices/system/cpu/cpu%d/%127s", &cpu, leaf) == 2 && cpu < n) {
            std::string l = leaf;
            const char* freq = cpu < 4 ? "1804800" : cpu < 7 ? "2419200" : "2841600";
            if (l == "cpufreq/cpuinfo_max_freq" || l == "cpufreq/scaling_max_freq" ||
                l == "cpufreq/scaling_cur_freq" || l == "cpufreq/cpuinfo_cur_freq")
                return std::string(freq) + "\n";
            if (l == "cpufreq/cpuinfo_min_freq")
                return std::string("300000\n");
            if (l == "online")
                return std::string("1\n");
            if (l == "topology/physical_package_id")
                return std::string("0\n");
            if (l == "topology/core_id")
                return Format("%d\n", cpu);
            if (l == "regs/identification/midr_el1")
                return std::string(cpu < 4 ? "0x00000000517f803c\n" : "0x00000000411fd0d0\n");
        }
        return std::nullopt;
    });

    fs.AddDevice("/dev/null", [] { return std::make_shared<DevFile>(DevFile::Null); });
    fs.AddDevice("/dev/zero", [] { return std::make_shared<DevFile>(DevFile::Zero); });
    fs.AddDevice("/dev/urandom", [] { return std::make_shared<DevFile>(DevFile::Random); });
    fs.AddDevice("/dev/random", [] { return std::make_shared<DevFile>(DevFile::Random); });
    fs.AddDevice("/dev/tty", [] { return std::make_shared<StdioFile>(1); });

    fs.AddFile("/linkerconfig/ld.config.txt", [ld_config] { return ld_config; });
}

}  // namespace rn
