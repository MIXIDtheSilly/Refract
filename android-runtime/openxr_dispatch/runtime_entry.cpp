#include "openxr_minimal.h"
#include "session.h"

REFRACT_XR_EXPORT int refract_runtime_probe()
{
    return refract::runtime::session_probe();
}

REFRACT_XR_EXPORT XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(
    const XrNegotiateLoaderInfo* loaderInfo,
    XrNegotiateRuntimeRequest* runtimeRequest)
{
    return refract::runtime::negotiate_loader_runtime_interface(loaderInfo, runtimeRequest);
}
