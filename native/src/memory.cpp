#include "memory.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "files.h"
#include "linux_abi.h"

namespace rn {

using namespace lx;

AddressSpace& Mem() {
    static AddressSpace instance;
    return instance;
}

u32 ToWinProtect(int prot) {
    // The host never executes guest code (the JIT reads it), so EXEC maps to READ.
    if (prot & PROT_WRITE_)
        return PAGE_READWRITE;
    if (prot & (PROT_READ_ | PROT_EXEC_))
        return PAGE_READONLY;
    return PAGE_NOACCESS;
}

bool AddressSpace::Init(u64 arena_base, u64 arena_size) {
    void* p = VirtualAlloc(reinterpret_cast<void*>(arena_base), arena_size, MEM_RESERVE,
                           PAGE_NOACCESS);
    if (!p) {
        Log("reserving the guest arena at 0x%llx (+0x%llx) failed: %lu", arena_base, arena_size,
            GetLastError());
        return false;
    }
    base_ = reinterpret_cast<u64>(p);
    size_ = arena_size;
    RN_INFO("guest arena 0x%llx-0x%llx", base_, base_ + size_);
    return true;
}

bool AddressSpace::RangeFreeLocked(u64 addr, u64 len) const {
    if (!InArena(addr, len))
        return false;
    auto it = vmas_.upper_bound(addr);
    if (it != vmas_.begin()) {
        auto prev = std::prev(it);
        if (prev->second.end > addr)
            return false;
    }
    if (it != vmas_.end() && it->first < addr + len)
        return false;
    return true;
}

u64 AddressSpace::FindFreeLocked(u64 len, u64 hint) const {
    if (hint && RangeFreeLocked(PageDown(hint), len))
        return PageDown(hint);
    // Top-down first fit, like Linux's mmap_base allocator.
    u64 top = base_ + size_;
    for (auto it = vmas_.rbegin(); it != vmas_.rend(); ++it) {
        const Vma& v = it->second;
        if (top >= v.end && top - v.end >= len)
            return top - len;
        top = std::min(top, v.start);
    }
    if (top >= base_ && top - base_ >= len)
        return top - len;
    return 0;
}

void AddressSpace::SplitAtLocked(u64 addr) {
    auto it = vmas_.upper_bound(addr);
    if (it == vmas_.begin())
        return;
    --it;
    Vma& v = it->second;
    if (addr <= v.start || addr >= v.end)
        return;
    Vma tail = v;
    tail.start = addr;
    if (v.name.empty() || v.name[0] != '[')
        tail.offset = v.offset + (addr - v.start);
    v.end = addr;
    vmas_.emplace(addr, tail);
}

void AddressSpace::UnmapLocked(u64 addr, u64 len, bool* had_exec) {
    SplitAtLocked(addr);
    SplitAtLocked(addr + len);
    auto it = vmas_.lower_bound(addr);
    while (it != vmas_.end() && it->first < addr + len) {
        if (it->second.prot & PROT_EXEC_)
            *had_exec = true;
        it = vmas_.erase(it);
    }
    if (InArena(addr, len))
        VirtualFree(reinterpret_cast<void*>(addr), len, MEM_DECOMMIT);
}

bool AddressSpace::CommitLocked(u64 addr, u64 len, int prot) {
    void* p = VirtualAlloc(reinterpret_cast<void*>(addr), len, MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        Log("commit 0x%llx+0x%llx failed: %lu", addr, len, GetLastError());
        return false;
    }
    DWORD old;
    if (ToWinProtect(prot) != PAGE_READWRITE)
        VirtualProtect(p, len, ToWinProtect(prot), &old);
    return true;
}

void AddressSpace::MergeLocked() {
    // Cheap pass: merge anonymous neighbours with identical attributes.
    for (auto it = vmas_.begin(); it != vmas_.end();) {
        auto next = std::next(it);
        if (next == vmas_.end())
            break;
        Vma& a = it->second;
        const Vma& b = next->second;
        if (a.end == b.start && a.prot == b.prot && a.flags == b.flags && a.inode == 0 &&
            b.inode == 0 && a.name == b.name && (a.flags & MAP_ANONYMOUS_)) {
            a.end = b.end;
            vmas_.erase(next);
            continue;
        }
        it = next;
    }
}

s64 AddressSpace::Map(u64 addr, u64 len, int prot, int flags, FileObject* file, u64 offset,
                      const std::string& name) {
    if (len == 0)
        return -EINVAL_;
    len = PageUp(len);
    if (offset & (kPageSize - 1))
        return -EINVAL_;
    std::lock_guard lock(mu_);
    u64 target;
    if (flags & (MAP_FIXED_ | MAP_FIXED_NOREPLACE_)) {
        if (addr & (kPageSize - 1))
            return -EINVAL_;
        if (!InArena(addr, len)) {
            Log("mmap: fixed address 0x%llx+0x%llx is outside the guest arena", addr, len);
            return -ENOMEM_;
        }
        if (!RangeFreeLocked(addr, len)) {
            if (flags & MAP_FIXED_NOREPLACE_)
                return -EEXIST_;
            bool had_exec = false;
            UnmapLocked(addr, len, &had_exec);
            if (had_exec && on_code_change)
                on_code_change(addr, len);
        }
        target = addr;
    } else {
        target = FindFreeLocked(len, addr);
        if (!target)
            return -ENOMEM_;
    }

    if (file || prot != PROT_NONE_) {
        if (!CommitLocked(target, len, PROT_READ_ | PROT_WRITE_))
            return -ENOMEM_;
        if (file) {
            s64 n = file->Pread(reinterpret_cast<void*>(target), len, offset);
            if (n < 0) {
                VirtualFree(reinterpret_cast<void*>(target), len, MEM_DECOMMIT);
                return n;
            }
        }
        DWORD old;
        if (ToWinProtect(prot) != PAGE_READWRITE)
            VirtualProtect(reinterpret_cast<void*>(target), len, ToWinProtect(prot), &old);
    }

    Vma v;
    v.start = target;
    v.end = target + len;
    v.prot = prot;
    v.flags = flags & (MAP_SHARED_ | MAP_PRIVATE_ | MAP_ANONYMOUS_ | MAP_GROWSDOWN_);
    v.offset = file ? offset : 0;
    if (file) {
        v.name = file->path;
        v.inode = file->inode ? file->inode : 1;
    } else {
        v.name = name;
        v.flags |= MAP_ANONYMOUS_;
    }
    vmas_[target] = v;
    if ((prot & PROT_EXEC_) && on_code_change)
        on_code_change(target, len);
    return static_cast<s64>(target);
}

int AddressSpace::Unmap(u64 addr, u64 len) {
    if ((addr & (kPageSize - 1)) || len == 0)
        return -EINVAL_;
    len = PageUp(len);
    std::lock_guard lock(mu_);
    bool had_exec = false;
    UnmapLocked(addr, len, &had_exec);
    if (had_exec && on_code_change)
        on_code_change(addr, len);
    return 0;
}

int AddressSpace::Protect(u64 addr, u64 len, int prot) {
    if (addr & (kPageSize - 1))
        return -EINVAL_;
    len = PageUp(len);
    if (len == 0)
        return 0;
    std::lock_guard lock(mu_);
    if (!IsMapped(addr, len))
        return -ENOMEM_;
    SplitAtLocked(addr);
    SplitAtLocked(addr + len);
    bool exec_changed = false;
    for (auto it = vmas_.lower_bound(addr); it != vmas_.end() && it->first < addr + len; ++it) {
        Vma& v = it->second;
        if ((v.prot | prot) & PROT_EXEC_)
            exec_changed = true;
        v.prot = prot;
    }
    void* p = reinterpret_cast<void*>(addr);
    DWORD old;
    if (prot != PROT_NONE_) {
        // Commit any never-touched pages (PROT_NONE reservations becoming usable).
        if (!VirtualAlloc(p, len, MEM_COMMIT, PAGE_READWRITE))
            return -ENOMEM_;
        VirtualProtect(p, len, ToWinProtect(prot), &old);
    } else {
        // VirtualProtect fails on uncommitted pages, so walk committed runs.
        u64 cur = addr;
        while (cur < addr + len) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof(mbi)))
                break;
            u64 run_end = std::min<u64>(reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize,
                                        addr + len);
            if (mbi.State == MEM_COMMIT)
                VirtualProtect(reinterpret_cast<void*>(cur), run_end - cur, PAGE_NOACCESS, &old);
            cur = run_end;
        }
    }
    MergeLocked();
    if (exec_changed && on_code_change)
        on_code_change(addr, len);
    return 0;
}

s64 AddressSpace::Remap(u64 old_addr, u64 old_len, u64 new_len, int flags, u64 new_addr) {
    constexpr int MREMAP_MAYMOVE = 1, MREMAP_FIXED = 2;
    if (old_addr & (kPageSize - 1))
        return -EINVAL_;
    old_len = PageUp(old_len);
    new_len = PageUp(new_len);
    if (new_len == 0)
        return -EINVAL_;
    std::lock_guard lock(mu_);
    auto it = vmas_.upper_bound(old_addr);
    if (it == vmas_.begin())
        return -EFAULT_;
    --it;
    if (it->second.end < old_addr + old_len || it->second.start > old_addr)
        return -EFAULT_;
    Vma src = it->second;
    if ((flags & MREMAP_FIXED) && !(flags & MREMAP_MAYMOVE))
        return -EINVAL_;
    if (!(flags & MREMAP_FIXED) && new_len <= old_len) {
        if (new_len < old_len) {
            bool had_exec = false;
            UnmapLocked(old_addr + new_len, old_len - new_len, &had_exec);
        }
        return static_cast<s64>(old_addr);
    }
    // Grow in place when the tail is free.
    if (!(flags & MREMAP_FIXED) && it->second.end == old_addr + old_len &&
        RangeFreeLocked(old_addr + old_len, new_len - old_len)) {
        if (src.prot != PROT_NONE_ &&
            !CommitLocked(old_addr + old_len, new_len - old_len, src.prot))
            return -ENOMEM_;
        SplitAtLocked(old_addr + old_len);
        Vma tail = src;
        tail.start = old_addr + old_len;
        tail.end = old_addr + new_len;
        vmas_[tail.start] = tail;
        MergeLocked();
        return static_cast<s64>(old_addr);
    }
    if (!(flags & MREMAP_MAYMOVE))
        return -ENOMEM_;
    u64 dst;
    if (flags & MREMAP_FIXED) {
        if ((new_addr & (kPageSize - 1)) || !InArena(new_addr, new_len))
            return -EINVAL_;
        if (new_addr < old_addr + old_len && old_addr < new_addr + new_len)
            return -EINVAL_;
        bool had_exec = false;
        UnmapLocked(new_addr, new_len, &had_exec);
        if (had_exec && on_code_change)
            on_code_change(new_addr, new_len);
        dst = new_addr;
    } else {
        dst = FindFreeLocked(new_len, 0);
        if (!dst)
            return -ENOMEM_;
    }
    const u64 copy_len = std::min(old_len, new_len);
    if (!CommitLocked(dst, new_len, PROT_READ_ | PROT_WRITE_))
        return -ENOMEM_;
    // Copy only committed source pages.
    u64 cur = old_addr;
    while (cur < old_addr + copy_len) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof(mbi)))
            break;
        u64 run_end = std::min<u64>(reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize,
                                    old_addr + copy_len);
        if (mbi.State == MEM_COMMIT) {
            DWORD old;
            if (mbi.Protect == PAGE_NOACCESS)
                VirtualProtect(reinterpret_cast<void*>(cur), run_end - cur, PAGE_READONLY, &old);
            memcpy(reinterpret_cast<void*>(dst + (cur - old_addr)), reinterpret_cast<void*>(cur),
                   run_end - cur);
        }
        cur = run_end;
    }
    DWORD old;
    if (ToWinProtect(src.prot) != PAGE_READWRITE)
        VirtualProtect(reinterpret_cast<void*>(dst), new_len, ToWinProtect(src.prot), &old);
    bool had_exec = false;
    UnmapLocked(old_addr, old_len, &had_exec);
    Vma v = src;
    v.start = dst;
    v.end = dst + new_len;
    vmas_[dst] = v;
    if (had_exec && on_code_change)
        on_code_change(old_addr, old_len);
    return static_cast<s64>(dst);
}

int AddressSpace::Advise(u64 addr, u64 len, int advice) {
    constexpr int MADV_DONTNEED = 4, MADV_REMOVE = 9;
    if (advice != MADV_DONTNEED && advice != MADV_REMOVE)
        return 0;
    len = PageUp(len);
    std::lock_guard lock(mu_);
    // Linux semantics: private anonymous pages read back as zero.
    u64 cur = addr;
    while (cur < addr + len) {
        auto it = vmas_.upper_bound(cur);
        if (it == vmas_.begin())
            break;
        --it;
        const Vma& v = it->second;
        if (v.end <= cur) {
            // hole: skip to the next mapping
            auto next = vmas_.upper_bound(cur);
            if (next == vmas_.end())
                break;
            cur = next->first;
            continue;
        }
        u64 run_end = std::min(v.end, addr + len);
        if (v.prot != PROT_NONE_ && !(v.flags & MAP_SHARED_)) {
            VirtualFree(reinterpret_cast<void*>(cur), run_end - cur, MEM_DECOMMIT);
            CommitLocked(cur, run_end - cur, v.prot);
        }
        cur = run_end;
    }
    return 0;
}

void AddressSpace::SetName(u64 addr, u64 len, const std::string& name) {
    len = PageUp(len);
    std::lock_guard lock(mu_);
    if (!IsMapped(addr, len))
        return;
    SplitAtLocked(addr);
    SplitAtLocked(addr + len);
    for (auto it = vmas_.lower_bound(addr); it != vmas_.end() && it->first < addr + len; ++it)
        if (it->second.inode == 0)
            it->second.name = "[anon:" + name + "]";
}

bool AddressSpace::IsMapped(u64 addr, u64 len) const {
    std::lock_guard lock(mu_);
    u64 cur = addr;
    const u64 end = addr + len;
    while (cur < end) {
        auto it = vmas_.upper_bound(cur);
        if (it == vmas_.begin())
            return false;
        --it;
        if (it->second.end <= cur)
            return false;
        cur = it->second.end;
    }
    return true;
}

bool AddressSpace::FindVma(u64 addr, Vma* out) const {
    std::lock_guard lock(mu_);
    auto it = vmas_.upper_bound(addr);
    if (it == vmas_.begin())
        return false;
    --it;
    if (it->second.end <= addr)
        return false;
    *out = it->second;
    return true;
}

std::vector<Vma> AddressSpace::Snapshot() const {
    std::lock_guard lock(mu_);
    std::vector<Vma> out;
    out.reserve(vmas_.size());
    for (const auto& [start, v] : vmas_)
        out.push_back(v);
    return out;
}

std::string AddressSpace::ProcMaps() const {
    std::lock_guard lock(mu_);
    std::string out;
    for (const auto& [start, v] : vmas_) {
        char line[512];
        snprintf(line, sizeof(line), "%llx-%llx %c%c%c%c %08llx %s %llu", v.start, v.end,
                 (v.prot & PROT_READ_) ? 'r' : '-', (v.prot & PROT_WRITE_) ? 'w' : '-',
                 (v.prot & PROT_EXEC_) ? 'x' : '-', (v.flags & MAP_SHARED_) ? 's' : 'p',
                 v.offset, v.inode ? "fe:2a" : "00:00", v.inode);
        out += line;
        if (!v.name.empty()) {
            size_t pad = strlen(line) < 73 ? 73 - strlen(line) : 1;
            out.append(pad, ' ');
            out += v.name;
        }
        out += '\n';
    }
    return out;
}

}  // namespace rn
