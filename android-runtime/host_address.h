#pragma once
#if defined(__ANDROID__)
#include <sys/system_properties.h>

namespace refract::runtime {

// Where the PC host bridge listens (poses on :38490, images on :38491) when the runtime connects to it
// directly, or null for the defaults (the emulator's 10.0.2.2 for poses, adb reverse on 127.0.0.1 for
// images). debug.refract.host_addr sets it; Waydroid's setup sets it to the host's side of its network
// bridge, 192.168.240.1, and setting it also turns direct connections on.
inline const char* direct_host_address()
{
    static char address[PROP_VALUE_MAX]{};
    static const bool set = __system_property_get("debug.refract.host_addr", address) > 0;
    return set ? address : nullptr;
}

} // namespace refract::runtime
#endif
