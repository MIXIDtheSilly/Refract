// The Android app process: hosts the JVM, adopts host threads into the guest,
// and implements librefract_host.so (the guest launcher's host interface).
#pragma once

#include <string>

#include "kernel.h"

namespace rn {

struct AppConfig {
    std::string package;
    std::string activity;
    std::string label;
    std::wstring app_dir;    // host: installed app directory
    std::wstring apk_host;   // host path of base.apk
    std::wstring data_host;  // host directory mounted at /data
    std::string apk_guest;   // /data/app/<pkg>/base.apk
    std::string lib_guest;   // /data/app/<pkg>/lib/arm64
    std::string data_guest;  // /data/user/0/<pkg>
    std::string external_guest;  // /storage/emulated/0/Android/data/<pkg>
    std::string application;  // Application subclass from the manifest, if any
    int target_sdk = 32;
    int version_code = 0;
    std::string version_name;
};

AppConfig& App();
bool LoadAppConfig(const std::wstring& app_dir);

void RegisterHostRuntime();
// The calling host thread's guest thread, adopting it if it has none.
GuestThread* EnsureGuestThread();
// The guest launcher's dlopen/dlsym, called on the calling thread (adopted if needed).
u64 GuestDlopen(const std::string& path);
u64 GuestDlsym(u64 handle, const char* name);
// clone() on the service thread: hand the new thread to the adopting host thread.
bool CompleteAdoption(u64 token, GuestThread* child);

}  // namespace rn
