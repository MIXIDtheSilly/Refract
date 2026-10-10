// Guest address space. Guest addresses are host addresses: guest mappings live in
// one big reserved arena where 4K pages are committed and protected individually,
// which gives Linux mmap semantics on top of Windows' 64K reservation granularity.
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "common.h"

namespace rn {

class FileObject;

struct Vma {
    u64 start = 0;
    u64 end = 0;
    int prot = 0;   // lx::PROT_*
    int flags = 0;  // lx::MAP_*
    u64 offset = 0;
    u64 inode = 0;
    std::string name;  // path, "[stack]", "[anon:...]" or empty
};

class AddressSpace {
public:
    bool Init(u64 arena_base, u64 arena_size);

    // Returns the address or a negative errno.
    s64 Map(u64 addr, u64 len, int prot, int flags, FileObject* file, u64 offset,
            const std::string& name);
    int Unmap(u64 addr, u64 len);
    int Protect(u64 addr, u64 len, int prot);
    s64 Remap(u64 old_addr, u64 old_len, u64 new_len, int flags, u64 new_addr);
    int Advise(u64 addr, u64 len, int advice);
    void SetName(u64 addr, u64 len, const std::string& name);

    bool IsMapped(u64 addr, u64 len) const;
    bool FindVma(u64 addr, Vma* out) const;
    bool InArena(u64 addr, u64 len) const {
        return addr >= base_ && addr + len <= base_ + size_ && addr + len >= addr;
    }
    std::string ProcMaps() const;
    std::vector<Vma> Snapshot() const;

    // Called with (start, len) whenever executable guest memory changes, so
    // translated code can be dropped.
    std::function<void(u64, u64)> on_code_change;

private:
    u64 FindFreeLocked(u64 len, u64 hint) const;
    bool RangeFreeLocked(u64 addr, u64 len) const;
    void SplitAtLocked(u64 addr);
    void UnmapLocked(u64 addr, u64 len, bool* had_exec);
    bool CommitLocked(u64 addr, u64 len, int prot);
    void MergeLocked();

    mutable std::recursive_mutex mu_;
    std::map<u64, Vma> vmas_;  // keyed by start
    u64 base_ = 0;
    u64 size_ = 0;
};

AddressSpace& Mem();

u32 ToWinProtect(int prot);

}  // namespace rn
