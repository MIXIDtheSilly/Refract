#pragma once

#include <string>

#include "common.h"

namespace rn {

struct GuestThread;

// Registers a function to trace: "libfoo.so!symbol" or "libfoo.so+0x1234".
void AddTrace(const std::string& spec);
// Installs pending hooks once their library is mapped (call after exec mappings).
void TraceOnExecMapping();
// Handles svc #kThunkTrace / #kThunkTraceRet.
bool TraceHit(GuestThread* t, u32 imm);

}  // namespace rn
