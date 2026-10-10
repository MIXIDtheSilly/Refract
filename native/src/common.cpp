#include "common.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <mutex>

namespace rn {

int g_verbose = 1;

static std::mutex g_log_mutex;

static void VLog(const char* fmt, va_list ap) {
    char buf[4096];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0)
        return;
    std::lock_guard lock(g_log_mutex);
    fprintf(stderr, "[rn %5lu] %s\n", GetCurrentThreadId(), buf);
    fflush(stderr);
}

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    VLog(fmt, ap);
    va_end(ap);
}

void Fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    VLog(fmt, ap);
    va_end(ap);
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 134);
    std::abort();
}

std::string Format(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[4096];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return {};
    if (static_cast<size_t>(n) < sizeof(buf))
        return std::string(buf, n);
    std::string big(n + 1, '\0');
    va_start(ap, fmt);
    vsnprintf(big.data(), big.size(), fmt, ap);
    va_end(ap);
    big.resize(n);
    return big;
}

std::wstring Widen(const std::string& s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static bool RawCopy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeCopyFromGuest(void* dst, u64 src, size_t n) {
    if (n == 0)
        return true;
    if (src == 0)
        return false;
    return RawCopy(dst, GuestPtr<void>(src), n);
}

bool SafeCopyToGuest(u64 dst, const void* src, size_t n) {
    if (n == 0)
        return true;
    if (dst == 0)
        return false;
    return RawCopy(GuestPtr<void>(dst), src, n);
}

static int RawStrnlen(const char* p, size_t max, size_t* out) {
    __try {
        size_t i = 0;
        while (i < max && p[i])
            ++i;
        *out = i;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool SafeReadString(u64 addr, std::string& out, size_t max) {
    if (addr == 0)
        return false;
    size_t len = 0;
    if (!RawStrnlen(GuestPtr<const char>(addr), max, &len))
        return false;
    out.assign(GuestPtr<const char>(addr), len);
    return true;
}

}  // namespace rn
