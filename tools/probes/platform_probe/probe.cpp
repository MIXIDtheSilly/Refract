// Feasibility probe: can the official Windows Platform SDK (via the signed-in Meta Horizon Link app)
// answer identity requests for a Quest app ID? Prints results; secrets are shown truncated only.
// Usage: platform_probe.exe <app id>
#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>
#include "OVR_Platform.h"

static std::string redact(const char* s) {
    if (!s) return "(null)";
    size_t n = strlen(s);
    return std::string(s, n < 6 ? n : 6) + "... (" + std::to_string(n) + " chars)";
}

static ovrMessageHandle wait_for(ovrRequest req, DWORD timeout_ms = 15000) {
    DWORD start = GetTickCount();
    while (GetTickCount() - start < timeout_ms) {
        while (ovrMessageHandle m = ovr_PopMessage()) {
            if (ovr_Message_GetRequestID(m) == req) return m;
            ovr_FreeMessage(m);
        }
        Sleep(20);
    }
    return nullptr;
}

static bool check(const char* what, ovrMessageHandle m) {
    if (!m) { printf("%-22s TIMEOUT\n", what); return false; }
    if (ovr_Message_IsError(m)) {
        ovrErrorHandle e = ovr_Message_GetError(m);
        printf("%-22s ERROR %d: %s\n", what, ovr_Error_GetCode(e), ovr_Error_GetMessage(e));
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <app id>\n", argv[0]); return 2; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* app = argv[1];
    ovrPlatformInitializeResult r = ovr_PlatformInitializeWindows(app);
    printf("%-22s %d (%s)\n", "init", r, ovrPlatformInitializeResult_ToString(r));
    if (r != ovrPlatformInitialize_Success) return 1;

    ovrMessageHandle m = wait_for(ovr_User_GetLoggedInUser());
    if (check("GetLoggedInUser", m)) {
        ovrUserHandle u = ovr_Message_GetUser(m);
        printf("%-22s id %llu, oculus id '%s', display '%s'\n", "GetLoggedInUser",
               (unsigned long long)ovr_User_GetID(u), ovr_User_GetOculusID(u), ovr_User_GetDisplayName(u));
    }
    if (m) ovr_FreeMessage(m);

    m = wait_for(ovr_User_GetUserProof());
    if (check("GetUserProof", m))
        printf("%-22s nonce %s\n", "GetUserProof", redact(ovr_UserProof_GetNonce(ovr_Message_GetUserProof(m))).c_str());
    if (m) ovr_FreeMessage(m);

    m = wait_for(ovr_User_GetAccessToken());
    if (check("GetAccessToken", m))
        printf("%-22s token %s\n", "GetAccessToken", redact(ovr_Message_GetString(m)).c_str());
    if (m) ovr_FreeMessage(m);

    m = wait_for(ovr_Entitlement_GetIsViewerEntitled());
    if (check("IsViewerEntitled", m)) printf("%-22s entitled\n", "IsViewerEntitled");
    if (m) ovr_FreeMessage(m);

    m = wait_for(ovr_User_GetLoggedInUserFriends());
    if (check("GetLoggedInUserFriends", m)) {
        ovrUserArrayHandle a = ovr_Message_GetUserArray(m);
        printf("%-22s %zu friend(s)%s\n", "GetLoggedInUserFriends", ovr_UserArray_GetSize(a),
               ovr_UserArray_HasNextPage(a) ? " (+more pages)" : "");
    }
    if (m) ovr_FreeMessage(m);
    return 0;
}
