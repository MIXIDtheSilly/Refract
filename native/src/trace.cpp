// --trace lib.so!symbol / lib.so+0xoffset: logs calls to a guest function and
// what it returns. The function's first instruction is replaced with
// `svc #kThunkTrace`; the displaced instruction runs from a trampoline that
// jumps back, and the return address is redirected through `svc #kThunkTraceRet`.
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "cpu.h"
#include "kernel.h"
#include "linux_abi.h"
#include "memory.h"
#include "thunks.h"
#include "trace.h"
#include "vfs.h"

namespace rn {

namespace {

struct Hook {
    std::string module;  // file name, e.g. libfoo.so
    std::string symbol;  // empty when given as an offset
    u64 offset = 0;      // resolved from the symbol on first load
    u64 symbol_add = 0;  // added to the symbol's address
    bool resolved = false;
    u64 addr = 0;        // installed at
    u64 tramp = 0;
    bool entry_only = false;  // spec ended in '~': don't hook the return (mid-function points)
    std::string label;
    u64 calls = 0;
};

struct Frame {
    size_t hook;
    u64 lr;
    u64 sp;
    bool logged;
};

// Logs the first REFRACT_TRACE_LIMIT (default 40) calls per hook, then every 500th.
bool ShouldLog(u64 n) {
    static const u64 limit = [] {
        const char* e = getenv("REFRACT_TRACE_LIMIT");
        return e ? strtoull(e, nullptr, 0) : 40ull;
    }();
    return n <= limit || n % 500 == 0;
}

std::mutex g_mu;
std::vector<Hook> g_hooks;
u64 g_page = 0;  // trampolines; slot 0 holds the return svc
size_t g_used = 32;
thread_local std::vector<Frame> t_frames;

std::string BaseName(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

// Looks `name` up in the ELF's .symtab and .dynsym; returns its st_value.
bool FindSymbol(const std::wstring& path, const std::string& name, u64* value) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 64 || memcmp(d.data(), "\x7f" "ELF", 4) != 0)
        return false;
    auto u16at = [&](size_t o) { u16 v; memcpy(&v, &d[o], 2); return v; };
    auto u32at = [&](size_t o) { u32 v; memcpy(&v, &d[o], 4); return v; };
    auto u64at = [&](size_t o) { u64 v; memcpy(&v, &d[o], 8); return v; };
    const u64 shoff = u64at(0x28);
    const u16 shentsize = u16at(0x3a), shnum = u16at(0x3c);
    if (shoff + static_cast<u64>(shnum) * shentsize > d.size())
        return false;
    for (u16 i = 0; i < shnum; ++i) {
        const size_t sh = shoff + static_cast<size_t>(i) * shentsize;
        const u32 type = u32at(sh + 4);
        if (type != 2 /*SHT_SYMTAB*/ && type != 11 /*SHT_DYNSYM*/)
            continue;
        const u64 off = u64at(sh + 0x18), size = u64at(sh + 0x20), entsize = u64at(sh + 0x38);
        const u32 link = u32at(sh + 0x28);
        const size_t strsh = shoff + static_cast<size_t>(link) * shentsize;
        const u64 stroff = u64at(strsh + 0x18), strsize = u64at(strsh + 0x20);
        if (!entsize || off + size > d.size() || stroff + strsize > d.size())
            continue;
        for (u64 e = off; e + entsize <= off + size; e += entsize) {
            const u32 n = u32at(e);
            const u64 v = u64at(e + 8);
            if (n >= strsize || !v)
                continue;
            if (!strncmp(&d[stroff + n], name.c_str(), strsize - n) && d[stroff + n + name.size()] == 0) {
                *value = v;
                return true;
            }
        }
    }
    return false;
}

bool PcRelative(u32 insn) {
    return (insn & 0x7C000000) == 0x14000000 ||  // b, bl
           (insn & 0xFF000010) == 0x54000000 ||  // b.cond
           (insn & 0x7E000000) == 0x34000000 ||  // cbz, cbnz
           (insn & 0x7E000000) == 0x36000000 ||  // tbz, tbnz
           (insn & 0x1F000000) == 0x10000000 ||  // adr, adrp
           (insn & 0x3B000000) == 0x18000000;    // ldr (literal)
}

// "0x1234" or, when it points at a short printable string, 0x1234 "text".
std::string Arg(u64 v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", v);
    std::string out = buf, str;
    if (v > 0x10000 && Mem().IsMapped(v, 1) && SafeReadString(v, str, 96) && str.size() >= 2 &&
        std::all_of(str.begin(), str.end(), [](char ch) { return ch >= 0x20 && ch < 0x7f; }))
        out += " \"" + str + "\"";
    return out;
}

// lr plus a few frame-pointer frames.
std::string Callers(CpuCore& c) {
    std::string out = DescribeAddress(c.X(30));
    u64 fp = c.X(29);
    for (int i = 0; i < 3 && fp && Mem().IsMapped(fp, 16); ++i) {
        u64 fr[2];
        if (!SafeCopyFromGuest(fr, fp, 16) || !fr[1])
            break;
        out += " < " + DescribeAddress(fr[1]);
        if (fr[0] <= fp)
            break;
        fp = fr[0];
    }
    return out;
}

u32 Svc(u32 imm) { return 0xD4000001u | (imm << 5); }

void WriteCode(u64 addr, const void* data, size_t len) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(addr), len, PAGE_EXECUTE_READWRITE, &old);
    memcpy(reinterpret_cast<void*>(addr), data, len);
    VirtualProtect(reinterpret_cast<void*>(addr), len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), len);
    CpuInvalidateCode(addr, len);
}

bool EnsurePage() {
    if (g_page)
        return true;
    s64 p = Mem().Map(0, kPageSize, lx::PROT_READ_ | lx::PROT_EXEC_, lx::MAP_PRIVATE_ | lx::MAP_ANONYMOUS_, nullptr, 0,
                      "[refract:trace]");
    if (p < 0)
        return false;
    g_page = static_cast<u64>(p);
    const u32 ret = Svc(kThunkTraceRet);
    WriteCode(g_page, &ret, 4);
    return true;
}

void Install(Hook& h, u64 base, const Vma& code) {
    const u64 addr = base + h.offset;
    if (addr < code.start || addr + 4 > code.end)
        return;
    u32 insn;
    memcpy(&insn, reinterpret_cast<void*>(addr), 4);
    // adr/adrp are rebuilt as a literal load of the address they compute.
    const bool adr = (insn & 0x1F000000) == 0x10000000;
    if (PcRelative(insn) && !adr) {
        Log("trace: %s starts with a pc-relative instruction (%08x); trace a later offset", h.label.c_str(), insn);
        h.addr = addr;  // don't retry
        return;
    }
    if (!EnsurePage() || g_used + 32 > kPageSize) {
        Log("trace: out of trampoline space");
        return;
    }
    const u64 tramp = g_page + g_used;
    g_used += 32;
    // orig; ldr x16, #12; br x16; pad; .quad addr+4
    // adr: ldr xd, #20; ldr x16, #8; br x16; .quad addr+4; .quad value
    u8 t[32] = {};
    const u64 back = addr + 4;
    if (adr) {
        const s64 imm = (static_cast<s64>(static_cast<s32>(((insn >> 5) & 0x7FFFF) << 13)) >> 11) | ((insn >> 29) & 3);
        const u64 value = (insn & 0x80000000u) ? (addr & ~0xFFFull) + (static_cast<u64>(imm) << 12) : addr + imm;
        const u32 code_words[3] = {0x580000A0u | (insn & 0x1F), 0x58000050u, 0xD61F0200u};
        memcpy(t, code_words, 12);
        memcpy(t + 12, &back, 8);  // ldr literals need only 4-byte alignment
        memcpy(t + 20, &value, 8);
    } else {
        const u32 code_words[4] = {insn, 0x58000070u, 0xD61F0200u, 0xD503201Fu /*nop*/};
        memcpy(t, code_words, 16);
        memcpy(t + 16, &back, 8);
    }
    WriteCode(tramp, t, sizeof(t));
    const u32 svc = Svc(kThunkTrace);
    WriteCode(addr, &svc, 4);
    h.addr = addr;
    h.tramp = tramp;
    Log("trace: hooked %s at 0x%llx", h.label.c_str(), addr);
}

}  // namespace

void AddTrace(const std::string& raw) {
    Hook h;
    std::string spec = raw;
    if (!spec.empty() && spec.back() == '~') {
        h.entry_only = true;
        spec.pop_back();
    }
    h.label = spec;
    size_t bang = spec.find('!'), plus = spec.find('+');
    if (bang != std::string::npos) {
        h.module = spec.substr(0, bang);
        h.symbol = spec.substr(bang + 1);
        size_t add = h.symbol.find('+');  // sym+0xN: N bytes into the function
        if (add != std::string::npos) {
            h.symbol_add = strtoull(h.symbol.c_str() + add + 1, nullptr, 0);
            h.symbol.resize(add);
        }
    } else if (plus != std::string::npos) {
        h.module = spec.substr(0, plus);
        h.offset = strtoull(spec.c_str() + plus + 1, nullptr, 0);
        h.resolved = true;
    } else {
        Log("trace: expected lib.so!symbol or lib.so+0xoffset, got %s", spec.c_str());
        return;
    }
    std::lock_guard lock(g_mu);
    g_hooks.push_back(h);
}

void TraceOnExecMapping() {
    std::lock_guard lock(g_mu);
    bool pending = false;
    for (const Hook& h : g_hooks)
        pending |= h.addr == 0;
    if (!pending)
        return;
    std::vector<Vma> vmas = Mem().Snapshot();
    for (Hook& h : g_hooks) {
        if (h.addr)
            continue;
        for (const Vma& first : vmas) {
            if (first.offset != 0 || BaseName(first.name) != h.module)
                continue;
            if (!h.resolved) {
                auto host = Fs().HostPath(first.name);
                if (!host || !FindSymbol(*host, h.symbol, &h.offset)) {
                    Log("trace: symbol %s not found in %s", h.symbol.c_str(), first.name.c_str());
                    h.addr = 1;
                    break;
                }
                h.offset += h.symbol_add;
                h.resolved = true;
            }
            for (const Vma& code : vmas)
                if ((code.prot & lx::PROT_EXEC_) && code.name == first.name)
                    Install(h, first.start, code);
            if (h.addr)
                break;
        }
    }
}

bool TraceHit(GuestThread* t, u32 imm) {
    CpuCore& c = *t->cpu;
    std::lock_guard lock(g_mu);
    if (imm == kThunkTraceRet) {
        if (t_frames.empty()) {
            Log("trace: return without a frame");
            return false;
        }
        Frame f = t_frames.back();
        t_frames.pop_back();
        if (f.logged)
            Log("trace[%d] %s returned %s", t->tid, g_hooks[f.hook].label.c_str(), Arg(c.X(0)).c_str());
        c.SetPc(f.lr);
        return true;
    }
    const u64 at = c.Pc() - 4;
    for (size_t i = 0; i < g_hooks.size(); ++i) {
        Hook& h = g_hooks[i];
        if (h.addr != at || !h.tramp)
            continue;
        const bool logged = ShouldLog(++h.calls);
        if (logged)
            Log("trace[%d] %s #%llu(%s, %s, %s, %s) from %s", t->tid, h.label.c_str(), h.calls, Arg(c.X(0)).c_str(),
                Arg(c.X(1)).c_str(), Arg(c.X(2)).c_str(), Arg(c.X(3)).c_str(), Callers(c).c_str());
        if (!h.entry_only) {
            t_frames.push_back({i, c.X(30), c.Sp(), logged});
            c.SetX(30, g_page);
        }
        c.SetPc(h.tramp);
        return true;
    }
    return false;
}

}  // namespace rn
