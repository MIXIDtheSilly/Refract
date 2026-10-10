#pragma once

#include <string>
#include <vector>

#include "common.h"

namespace rn {

struct LoadedElf {
    std::string path;
    std::string interp;  // PT_INTERP, if any
    u64 base = 0;
    u64 load_bias = 0;
    u64 entry = 0;
    u64 phdr = 0;
    u16 phnum = 0;
    u16 phent = 0;
};

int LoadElf(const std::string& guest_path, LoadedElf* out);

// Builds argc/argv/envp/auxv on a new main-thread stack and returns the initial SP.
u64 SetupInitialStack(const std::vector<std::string>& argv, const std::vector<std::string>& envp,
                      const LoadedElf& exe, const LoadedElf* interp, const std::string& execfn,
                      u64 hwcap, u64 hwcap2);

}  // namespace rn
