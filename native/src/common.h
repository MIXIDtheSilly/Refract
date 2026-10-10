// Shared helpers for the native (no-emulator) runtime.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rn {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

constexpr u64 kPageSize = 4096;
constexpr u64 PageDown(u64 v) { return v & ~(kPageSize - 1); }
constexpr u64 PageUp(u64 v) { return (v + kPageSize - 1) & ~(kPageSize - 1); }

// 0 = errors only, 1 = info, 2 = every syscall, 3 = very noisy.
extern int g_verbose;

void Log(const char* fmt, ...);
[[noreturn]] void Fatal(const char* fmt, ...);
std::string Format(const char* fmt, ...);

#define RN_INFO(...)                         \
    do {                                     \
        if (::rn::g_verbose >= 1)            \
            ::rn::Log(__VA_ARGS__);          \
    } while (0)
#define RN_TRACE(...)                        \
    do {                                     \
        if (::rn::g_verbose >= 2)            \
            ::rn::Log(__VA_ARGS__);          \
    } while (0)

// Wide/narrow conversion for Win32 calls.
std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);

// Guest pointers are host pointers (the guest address space is identity mapped).
template <typename T>
inline T* GuestPtr(u64 addr) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(addr));
}

// Copies that survive a bad guest pointer (return false instead of crashing).
bool SafeCopyFromGuest(void* dst, u64 src, size_t n);
bool SafeCopyToGuest(u64 dst, const void* src, size_t n);
// Reads a NUL-terminated guest string (max bytes); false on fault.
bool SafeReadString(u64 addr, std::string& out, size_t max = 4096);

}  // namespace rn
