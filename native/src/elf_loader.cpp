// Kernel-style ELF loading for the initial executable and its interpreter
// (the guest's own linker64 loads everything else).
#include "elf_loader.h"

#include <cstring>
#include <vector>

#include "files.h"
#include "linux_abi.h"
#include "memory.h"
#include "vfs.h"

namespace rn {

using namespace lx;

namespace {

struct Elf64_Ehdr {
    u8 e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
};

struct Elf64_Phdr {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
};

constexpr u32 PT_LOAD = 1, PT_INTERP = 3, PT_PHDR = 6;
constexpr u16 ET_EXEC = 2, ET_DYN = 3, EM_AARCH64 = 183;

int ElfProt(u32 flags) {
    int p = 0;
    if (flags & 4)
        p |= PROT_READ_;
    if (flags & 2)
        p |= PROT_WRITE_;
    if (flags & 1)
        p |= PROT_EXEC_;
    return p;
}

}  // namespace

int LoadElf(const std::string& guest_path, LoadedElf* out) {
    FilePtr f;
    int r = Fs().Open(AT_FDCWD_, guest_path, O_RDONLY_, 0, &f);
    if (r) {
        Log("cannot open %s: %d", guest_path.c_str(), r);
        return r;
    }
    Elf64_Ehdr eh;
    if (f->Pread(&eh, sizeof(eh), 0) != sizeof(eh) || memcmp(eh.e_ident, "\x7f" "ELF", 4) != 0 ||
        eh.e_ident[4] != 2) {
        Log("%s is not a 64-bit ELF", guest_path.c_str());
        return -ENOEXEC_;
    }
    if (eh.e_machine != EM_AARCH64 || (eh.e_type != ET_DYN && eh.e_type != ET_EXEC)) {
        Log("%s: unsupported machine %u / type %u", guest_path.c_str(), eh.e_machine, eh.e_type);
        return -ENOEXEC_;
    }
    std::vector<Elf64_Phdr> ph(eh.e_phnum);
    if (f->Pread(ph.data(), ph.size() * sizeof(Elf64_Phdr), eh.e_phoff) !=
        static_cast<s64>(ph.size() * sizeof(Elf64_Phdr)))
        return -ENOEXEC_;

    u64 lo = ~0ull, hi = 0;
    for (const auto& p : ph) {
        if (p.p_type == PT_INTERP) {
            std::string interp(p.p_filesz, '\0');
            f->Pread(interp.data(), p.p_filesz, p.p_offset);
            out->interp = interp.c_str();
        }
        if (p.p_type != PT_LOAD)
            continue;
        lo = std::min(lo, PageDown(p.p_vaddr));
        hi = std::max(hi, PageUp(p.p_vaddr + p.p_memsz));
    }
    if (lo >= hi)
        return -ENOEXEC_;

    // Reserve the whole span, then map segments over it.
    u64 base;
    if (eh.e_type == ET_DYN) {
        s64 res = Mem().Map(0, hi - lo, PROT_NONE_, MAP_PRIVATE_ | MAP_ANONYMOUS_, nullptr, 0, "");
        if (res < 0)
            return static_cast<int>(res);
        base = static_cast<u64>(res) - lo;
    } else {
        base = 0;
    }

    for (const auto& p : ph) {
        if (p.p_type != PT_LOAD)
            continue;
        const u64 seg_start = base + PageDown(p.p_vaddr);
        const u64 file_end = base + p.p_vaddr + p.p_filesz;
        const u64 mem_end = base + PageUp(p.p_vaddr + p.p_memsz);
        const u64 file_map_end = PageUp(file_end);
        const int prot = ElfProt(p.p_flags);
        if (p.p_filesz) {
            s64 m = Mem().Map(seg_start, file_map_end - seg_start, PROT_READ_ | PROT_WRITE_,
                              MAP_PRIVATE_ | MAP_FIXED_, f.get(), PageDown(p.p_offset), "");
            if (m < 0)
                return static_cast<int>(m);
            // Zero the tail of the last file page (start of .bss).
            if (p.p_memsz > p.p_filesz && file_map_end > file_end)
                memset(GuestPtr<void>(file_end), 0, file_map_end - file_end);
        }
        u64 anon_start = p.p_filesz ? file_map_end : seg_start;
        if (mem_end > anon_start) {
            s64 m = Mem().Map(anon_start, mem_end - anon_start, PROT_READ_ | PROT_WRITE_,
                              MAP_PRIVATE_ | MAP_FIXED_ | MAP_ANONYMOUS_, nullptr, 0, "[anon:.bss]");
            if (m < 0)
                return static_cast<int>(m);
        }
        Mem().Protect(seg_start, mem_end - seg_start, prot);
    }

    out->path = guest_path;
    out->base = base;
    out->load_bias = base;
    out->entry = base + eh.e_entry;
    out->phnum = eh.e_phnum;
    out->phent = eh.e_phentsize;
    out->phdr = 0;
    for (const auto& p : ph)
        if (p.p_type == PT_PHDR)
            out->phdr = base + p.p_vaddr;
    if (!out->phdr) {
        for (const auto& p : ph) {
            if (p.p_type == PT_LOAD && p.p_offset <= eh.e_phoff &&
                eh.e_phoff < p.p_offset + p.p_filesz) {
                out->phdr = base + p.p_vaddr + (eh.e_phoff - p.p_offset);
                break;
            }
        }
    }
    RN_INFO("loaded %s at 0x%llx (entry 0x%llx)", guest_path.c_str(), base, out->entry);
    return 0;
}

u64 SetupInitialStack(const std::vector<std::string>& argv, const std::vector<std::string>& envp,
                      const LoadedElf& exe, const LoadedElf* interp, const std::string& execfn,
                      u64 hwcap, u64 hwcap2) {
    constexpr u64 kStackSize = 8 << 20;
    s64 stack = Mem().Map(0, kStackSize, PROT_READ_ | PROT_WRITE_,
                          MAP_PRIVATE_ | MAP_ANONYMOUS_ | MAP_GROWSDOWN_, nullptr, 0, "[stack]");
    if (stack < 0)
        Fatal("cannot map the main stack");
    u64 top = static_cast<u64>(stack) + kStackSize;
    u64 sp = top;

    auto push_bytes = [&](const void* data, size_t n) {
        sp -= n;
        memcpy(GuestPtr<void>(sp), data, n);
        return sp;
    };
    auto push_str = [&](const std::string& s) { return push_bytes(s.c_str(), s.size() + 1); };

    u64 execfn_addr = push_str(execfn);
    std::vector<u64> envp_addr, argv_addr;
    for (auto it = envp.rbegin(); it != envp.rend(); ++it)
        envp_addr.insert(envp_addr.begin(), push_str(*it));
    for (auto it = argv.rbegin(); it != argv.rend(); ++it)
        argv_addr.insert(argv_addr.begin(), push_str(*it));
    u64 platform = push_str("aarch64");
    u8 random[16];
    for (auto& b : random)
        b = static_cast<u8>(rand());
    sp &= ~15ull;
    u64 random_addr = push_bytes(random, 16);

    std::vector<u64> aux = {
        AT_PHDR_,   exe.phdr,
        AT_PHENT_,  exe.phent,
        AT_PHNUM_,  exe.phnum,
        AT_PAGESZ_, kPageSize,
        AT_BASE_,   interp ? interp->base : 0,
        AT_FLAGS_,  0,
        AT_ENTRY_,  exe.entry,
        AT_UID_,    10100,
        AT_EUID_,   10100,
        AT_GID_,    10100,
        AT_EGID_,   10100,
        AT_SECURE_, 0,
        AT_RANDOM_, random_addr,
        AT_HWCAP_,  hwcap,
        AT_HWCAP2_, hwcap2,
        AT_CLKTCK_, 100,
        AT_EXECFN_, execfn_addr,
        AT_PLATFORM_, platform,
        AT_NULL_,   0,
    };
    std::vector<u64> words;
    words.push_back(argv.size());
    for (u64 a : argv_addr)
        words.push_back(a);
    words.push_back(0);
    for (u64 e : envp_addr)
        words.push_back(e);
    words.push_back(0);
    for (u64 a : aux)
        words.push_back(a);
    sp -= words.size() * 8;
    sp &= ~15ull;
    memcpy(GuestPtr<void>(sp), words.data(), words.size() * 8);
    return sp;
}

}  // namespace rn
