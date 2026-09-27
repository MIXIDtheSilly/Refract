#pragma once
// Polls the emulator over adb about once a second: overall Android CPU use and
// the game's busiest threads (from /proc/stat and /proc/<pid>/task/*/stat).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

struct ThreadUse {
    std::string name;
    double percent = 0;  // Of one core.
};

struct AndroidSnapshot {
    bool valid = false;
    int cores = 0;
    double cpuPercent = 0;  // Of all cores.
    std::vector<ThreadUse> threads;  // Busiest first.
};

class AndroidStats {
public:
    AndroidStats(std::string adb, std::string serial, std::string package)
        : adb_(std::move(adb)), serial_(std::move(serial)), package_(std::move(package)) {}

    void start()
    {
        std::thread([this] {
            for (;;) {
                if (enabled) poll();
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }).detach();
    }

    AndroidSnapshot snapshot()
    {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }

    std::atomic<bool> enabled{true};

private:
    // Runs a command without a console window and returns its standard output.
    static bool run(std::string command, std::string& output)
    {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &inherit, 0)) return false;
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        // Inherit only the output pipe. If this adb call starts the adb server, that server lives on and
        // would otherwise keep every inheritable handle, including the viewer's port 38491 listener.
        SIZE_T attributeSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        std::vector<char> attributeStorage(attributeSize);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) ||
            !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &write, sizeof(write),
                                       nullptr, nullptr)) {
            CloseHandle(read);
            CloseHandle(write);
            return false;
        }
        STARTUPINFOEXA startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = write;
        startup.StartupInfo.hStdError = write;
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        const bool started = CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE,
                                            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                            &startup.StartupInfo, &process);
        DeleteProcThreadAttributeList(attributes);
        CloseHandle(write);
        if (started) {
            char buffer[4096];
            DWORD n = 0;
            while (ReadFile(read, buffer, sizeof(buffer), &n, nullptr) && n) output.append(buffer, n);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hProcess);
            CloseHandle(process.hThread);
        }
        CloseHandle(read);
        return started;
    }

    void poll()
    {
        std::string output;
        const std::string command = "\"" + adb_ + "\" -s " + serial_ + " shell \"grep '^cpu' /proc/stat; p=$(pidof " +
            package_ + "); [ -n \\\"$p\\\" ] && cat /proc/$p/task/*/stat\"";
        const auto now = std::chrono::steady_clock::now();
        if (!run(command, output)) return;

        AndroidSnapshot next;
        unsigned long long total = 0, idle = 0;
        std::map<int, std::pair<std::string, unsigned long long>> threads;  // tid -> (name, cpu ticks)
        std::istringstream lines(output);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.rfind("cpu", 0) == 0) {
                if (line.size() > 3 && line[3] != ' ') { ++next.cores; continue; }
                std::istringstream fields(line.substr(3));
                unsigned long long value[8]{};
                for (auto& v : value) fields >> v;
                for (auto v : value) total += v;
                idle = value[3] + value[4];  // idle + iowait
                continue;
            }
            const auto open = line.find('('), close = line.rfind(')');
            if (open == std::string::npos || close == std::string::npos || close < open) continue;
            std::istringstream fields(line.substr(close + 2));
            std::string field[13];
            for (auto& f : field) fields >> f;  // state ... utime (index 11), stime (index 12)
            if (field[12].empty()) continue;
            threads[std::atoi(line.c_str())] = {line.substr(open + 1, close - open - 1),
                                                std::strtoull(field[11].c_str(), nullptr, 10) + std::strtoull(field[12].c_str(), nullptr, 10)};
        }
        if (!total) return;

        const double seconds = std::chrono::duration<double>(now - previousAt_).count();
        if (previousTotal_ && total > previousTotal_) {
            next.cpuPercent = 100.0 * (1.0 - double(idle - previousIdle_) / double(total - previousTotal_));
            for (const auto& [tid, thread] : threads) {
                const auto found = previousThreads_.find(tid);
                if (found == previousThreads_.end() || thread.second < found->second) continue;
                const double percent = double(thread.second - found->second) / seconds;  // 100 ticks per second.
                if (percent >= 1.0) next.threads.push_back({thread.first, percent});
            }
            std::sort(next.threads.begin(), next.threads.end(), [](const auto& a, const auto& b) { return a.percent > b.percent; });
            next.valid = true;
        }
        previousTotal_ = total;
        previousIdle_ = idle;
        previousAt_ = now;
        previousThreads_.clear();
        for (const auto& [tid, thread] : threads) previousThreads_[tid] = thread.second;

        std::lock_guard lock(mutex_);
        snapshot_ = std::move(next);
    }

    std::string adb_, serial_, package_;
    std::mutex mutex_;
    AndroidSnapshot snapshot_;
    unsigned long long previousTotal_ = 0, previousIdle_ = 0;
    std::chrono::steady_clock::time_point previousAt_{};
    std::map<int, unsigned long long> previousThreads_;
};
