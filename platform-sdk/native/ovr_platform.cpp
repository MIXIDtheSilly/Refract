// Refract stand-in for the Meta Platform SDK implementation that Horizon OS provides on a Quest.
//
// A game's own libovrplatformloader.so asks the package com.oculus.platformsdkruntime (system
// only) or com.oculus.horizon for the class com.oculus.platform.loader.EntryPoint, calls its
// static load64(), and treats the returned long as `void* getProc(const char* name)`. It then
// resolves every ovr_* entry point through that function. The Refract platform package provides
// that class, so unmodified APKs initialise the Platform SDK against this library.
//
// Users, entitlement and the access token come from system properties (debug.refract.platform.*).
// A game is only reported as entitled when debug.refract.platform.owned.<package> is 1; purchases
// (IAP/DLC) are never granted. Everything not handled here resolves to a logged stub.
#include "ovr_tables.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <jni.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>

#define LOG_TAG "Refract-OVRPlatform"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

extern "C" char refract_ovr_stub_pool[];
extern "C" uint64_t refract_ovr_stub_dispatch(uint32_t index);

namespace {

using namespace refract::ovr;

constexpr uint32_t kStubCount = 4096;  // must match stub_pool.S
constexpr uint32_t kStubSize = 8;

// Message.MessageType values the hand-written functions post.
constexpr uint32_t kPlatformInitializeAndroidAsynchronous = 450037684u;
constexpr uint32_t kPlatformInitializeWithAccessToken = 896085803u;
constexpr uint32_t kPlatformInitializeStandaloneOculus = 1375260172u;

constexpr int kInitSuccess = 0;
constexpr int kLaunchTypeNormal = 1;
constexpr int kPresenceOnline = 1;
constexpr int kAgeCategoryAdult = 3;
constexpr int kErrorNotSupported = 1;
constexpr int kErrorNotEntitled = 2;

std::string property(const std::string& name, const char* fallback = "") {
    char value[PROP_VALUE_MAX] = {};
    return __system_property_get(name.c_str(), value) > 0 ? value : fallback;
}

struct User {
    uint64_t id = 0;
    std::string oculusId;
    std::string displayName;
};

struct Error {
    int code = 0;
    int httpCode = 0;
    std::string message;
};

// Every message handle, and every per-message payload handle (PlatformInitialize, UserProof,
// ApplicationVersion, OrgScopedID, ...), is a Message*: the payload getters read its fields.
struct Message {
    uint32_t type = 0;
    uint64_t request = 0;
    bool isError = false;
    Error error;
    std::string text;          // ovr_Message_GetString, a UserProof nonce
    const User* user = nullptr;
    int initResult = kInitSuccess;
};

// Any array handle: all arrays Refract hands out are empty, and the generic stubs answer
// GetSize/GetElement/HasNextPage on it with 0.
alignas(16) unsigned char g_emptyObject[256];

struct App {
    std::string package;
    int64_t versionCode = 0;
    std::string versionName;
} g_app;

User g_self;
std::string g_accessToken;
bool g_entitled = false;
bool g_verbose = false;

std::mutex g_queueMutex;
std::deque<Message*> g_queue;
std::atomic<uint64_t> g_nextRequest{1};

std::mutex g_usersMutex;
std::map<uint64_t, std::unique_ptr<User>> g_users;

uint64_t post(uint32_t type, Message* message) {
    message->type = type;
    message->request = g_nextRequest.fetch_add(1);
    const uint64_t request = message->request;
    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_queue.push_back(message);
    return request;
}

uint64_t postSuccess(uint32_t type) { return post(type, new Message()); }

uint64_t postError(uint32_t type, int code, const std::string& text) {
    auto* message = new Message();
    message->isError = true;
    message->error.code = code;
    message->error.message = text;
    return post(type, message);
}

uint32_t requestType(const char* function) {
    for (const auto& entry : kRequests) {
        if (std::strcmp(entry.name, function) == 0) return entry.type;
    }
    LOGW("no message type for %s", function);
    return 0;
}

const User* userFor(uint64_t id) {
    if (id == 0 || id == g_self.id) return &g_self;
    std::lock_guard<std::mutex> lock(g_usersMutex);
    auto& user = g_users[id];
    if (!user) {
        user = std::make_unique<User>();
        user->id = id;
        user->oculusId = "Player" + std::to_string(id % 100000);
        user->displayName = user->oculusId;
    }
    return user.get();
}

std::string randomHex(size_t bytes) {
    static std::mutex mutex;
    static std::mt19937_64 rng{std::random_device{}()};
    static const char digits[] = "0123456789abcdef";
    std::lock_guard<std::mutex> lock(mutex);
    std::string out;
    for (size_t i = 0; i < bytes; ++i) {
        const auto byte = static_cast<unsigned>(rng() & 0xff);
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    return out;
}

const Message* msg(const void* handle) { return static_cast<const Message*>(handle); }

// ---- ovr_* functions implemented by hand -------------------------------------------------

#define OVR_EXPORT extern "C" __attribute__((visibility("hidden")))

// Initialisation: always succeeds; entitlement is a separate request.
OVR_EXPORT int ovr_PlatformInitializeAndroid(const char* appId, jobject, JNIEnv*) {
    LOGI("ovr_PlatformInitializeAndroid(app %s) for %s", appId ? appId : "?", g_app.package.c_str());
    return kInitSuccess;
}
OVR_EXPORT int ovr_PlatformInitializeAndroidWithOptions(const char* appId, jobject activity, JNIEnv* env) {
    return ovr_PlatformInitializeAndroid(appId, activity, env);
}
OVR_EXPORT int ovr_PlatformAndroidCheckInitialization() { return kInitSuccess; }
OVR_EXPORT uint64_t ovr_PlatformInitializeAndroidAsynchronous(const char* appId, jobject, JNIEnv*) {
    LOGI("ovr_PlatformInitializeAndroidAsynchronous(app %s) for %s", appId ? appId : "?", g_app.package.c_str());
    return postSuccess(kPlatformInitializeAndroidAsynchronous);
}
OVR_EXPORT uint64_t ovr_PlatformInitializeAndroidAsynchronousWithOptions(const char* appId, jobject activity, JNIEnv* env) {
    return ovr_PlatformInitializeAndroidAsynchronous(appId, activity, env);
}
OVR_EXPORT bool ovr_UnityInitWrapper(const char* appId) {
    LOGI("ovr_UnityInitWrapper(app %s) for %s", appId ? appId : "?", g_app.package.c_str());
    return true;
}
OVR_EXPORT uint64_t ovr_UnityInitWrapperAsynchronous(const char* appId) {
    LOGI("ovr_UnityInitWrapperAsynchronous(app %s) for %s", appId ? appId : "?", g_app.package.c_str());
    return postSuccess(kPlatformInitializeAndroidAsynchronous);
}
// What the loader's exported ovr_UnityInitWrapperAsynchronous forwards to (after finding the activity).
OVR_EXPORT uint64_t ovr_UnityInitWrapperInternalAsynchronous(const char* appId) {
    return ovr_UnityInitWrapperAsynchronous(appId);
}
OVR_EXPORT bool ovr_UnityInitWrapperStandalone() { return true; }
OVR_EXPORT void ovr_UnityInitGlobals() {}
OVR_EXPORT uint64_t ovr_PlatformInitializeWithAccessToken() { return postSuccess(kPlatformInitializeWithAccessToken); }
OVR_EXPORT uint64_t ovr_PlatformInitializeWithAccessTokenAndOptions() { return postSuccess(kPlatformInitializeWithAccessToken); }
OVR_EXPORT uint64_t ovr_Platform_InitializeStandaloneOculus() { return postSuccess(kPlatformInitializeStandaloneOculus); }
OVR_EXPORT bool ovr_IsPlatformInitialized() { return true; }

// Message queue.
OVR_EXPORT void* ovr_PopMessage() {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_queue.empty()) return nullptr;
    Message* message = g_queue.front();
    g_queue.pop_front();
    if (g_verbose) LOGI("message type %u request %" PRIu64 "%s", message->type, message->request, message->isError ? " (error)" : "");
    return message;
}
OVR_EXPORT void ovr_FreeMessage(void* message) { delete static_cast<Message*>(message); }
OVR_EXPORT uint32_t ovr_Message_GetType(const void* m) { return m ? msg(m)->type : 0; }
OVR_EXPORT uint64_t ovr_Message_GetRequestID(const void* m) { return m ? msg(m)->request : 0; }
OVR_EXPORT bool ovr_Message_IsError(const void* m) { return m && msg(m)->isError; }
OVR_EXPORT const void* ovr_Message_GetError(const void* m) { return m && msg(m)->isError ? &msg(m)->error : nullptr; }
OVR_EXPORT const char* ovr_Message_GetString(const void* m) { return m ? msg(m)->text.c_str() : ""; }
OVR_EXPORT const void* ovr_Message_GetNativeMessage(const void* m) { return m; }
OVR_EXPORT const void* ovr_Message_GetUser(const void* m) { return m && msg(m)->user ? msg(m)->user : &g_self; }
OVR_EXPORT const void* ovr_Message_GetPlatformInitialize(const void* m) { return m; }
OVR_EXPORT const void* ovr_Message_GetUserProof(const void* m) { return m; }
OVR_EXPORT const void* ovr_Message_GetOrgScopedID(const void* m) { return m; }
OVR_EXPORT const void* ovr_Message_GetApplicationVersion(const void* m) { return m; }
OVR_EXPORT const void* ovr_Message_GetUserAccountAgeCategory(const void* m) { return m; }
OVR_EXPORT const void* ovr_Message_GetAchievementUpdate(const void* m) { return m; }
OVR_EXPORT const void* ovr_Message_GetLeaderboardUpdateStatus(const void* m) { return m; }

OVR_EXPORT int ovr_Error_GetCode(const void* e) { return e ? static_cast<const Error*>(e)->code : 0; }
OVR_EXPORT int ovr_Error_GetHttpCode(const void* e) { return e ? static_cast<const Error*>(e)->httpCode : 0; }
OVR_EXPORT const char* ovr_Error_GetMessage(const void* e) { return e ? static_cast<const Error*>(e)->message.c_str() : ""; }
OVR_EXPORT const char* ovr_Error_GetDisplayableMessage(const void* e) { return ovr_Error_GetMessage(e); }

OVR_EXPORT int ovr_PlatformInitialize_GetResult(const void* m) { return m ? msg(m)->initResult : kInitSuccess; }
OVR_EXPORT const char* ovr_UserProof_GetNonce(const void* m) { return m ? msg(m)->text.c_str() : ""; }
OVR_EXPORT uint64_t ovr_OrgScopedID_GetID(const void*) { return g_self.id; }
OVR_EXPORT int ovr_UserAccountAgeCategory_GetAgeCategory(const void*) { return kAgeCategoryAdult; }

// The logged-in user.
OVR_EXPORT uint64_t ovr_GetLoggedInUserID() { return g_self.id; }
OVR_EXPORT const char* ovr_GetLoggedInUserLocale() { return "en_US"; }
OVR_EXPORT uint64_t ovr_User_GetLoggedInUser() {
    auto* message = new Message();
    message->user = &g_self;
    return post(requestType("ovr_User_GetLoggedInUser"), message);
}
OVR_EXPORT uint64_t ovr_User_Get(uint64_t id) {
    auto* message = new Message();
    message->user = userFor(id);
    return post(requestType("ovr_User_Get"), message);
}
OVR_EXPORT uint64_t ovr_User_GetAccessToken() {
    auto* message = new Message();
    message->text = g_accessToken;
    return post(requestType("ovr_User_GetAccessToken"), message);
}
OVR_EXPORT uint64_t ovr_User_GetUserProof() {
    auto* message = new Message();
    message->text = randomHex(16);
    return post(requestType("ovr_User_GetUserProof"), message);
}
OVR_EXPORT uint64_t ovr_User_GetOrgScopedID(uint64_t) { return postSuccess(requestType("ovr_User_GetOrgScopedID")); }
OVR_EXPORT uint64_t ovr_UserAgeCategory_Get() { return postSuccess(requestType("ovr_UserAgeCategory_Get")); }

OVR_EXPORT uint64_t ovr_User_GetID(const void* u) { return u ? static_cast<const User*>(u)->id : 0; }
OVR_EXPORT const char* ovr_User_GetOculusID(const void* u) { return u ? static_cast<const User*>(u)->oculusId.c_str() : ""; }
OVR_EXPORT const char* ovr_User_GetDisplayName(const void* u) { return u ? static_cast<const User*>(u)->displayName.c_str() : ""; }
OVR_EXPORT int ovr_User_GetPresenceStatus(const void*) { return kPresenceOnline; }
OVR_EXPORT const void* ovr_User_GetManagedInfo(const void*) { return nullptr; }

// Entitlement: only for games the user has declared they own.
OVR_EXPORT uint64_t ovr_Entitlement_GetIsViewerEntitled() {
    const uint32_t type = requestType("ovr_Entitlement_GetIsViewerEntitled");
    if (g_entitled) {
        LOGI("entitlement check for %s: owned", g_app.package.c_str());
        return postSuccess(type);
    }
    LOGW("entitlement check for %s: not in the owned list (setprop debug.refract.platform.owned.%s 1)",
         g_app.package.c_str(), g_app.package.c_str());
    return postError(type, kErrorNotEntitled, "Refract: this game is not marked as owned");
}

// Purchases are never granted: the viewer owns nothing and checkout is unavailable.
OVR_EXPORT uint64_t ovr_IAP_LaunchCheckoutFlow() {
    return postError(requestType("ovr_IAP_LaunchCheckoutFlow"), kErrorNotSupported, "Refract: purchases are not available");
}
OVR_EXPORT uint64_t ovr_IAP_ConsumePurchase() {
    return postError(requestType("ovr_IAP_ConsumePurchase"), kErrorNotSupported, "Refract: purchases are not available");
}

// Application details come from the game's own PackageInfo.
OVR_EXPORT uint64_t ovr_Application_GetVersion() { return postSuccess(requestType("ovr_Application_GetVersion")); }
OVR_EXPORT int ovr_ApplicationVersion_GetCurrentCode(const void*) { return static_cast<int>(g_app.versionCode); }
OVR_EXPORT const char* ovr_ApplicationVersion_GetCurrentName(const void*) { return g_app.versionName.c_str(); }
OVR_EXPORT int ovr_ApplicationVersion_GetLatestCode(const void*) { return static_cast<int>(g_app.versionCode); }
OVR_EXPORT const char* ovr_ApplicationVersion_GetLatestName(const void*) { return g_app.versionName.c_str(); }
OVR_EXPORT const void* ovr_ApplicationLifecycle_GetLaunchDetails() { return g_emptyObject; }
OVR_EXPORT int ovr_LaunchDetails_GetLaunchType(const void*) { return kLaunchTypeNormal; }
OVR_EXPORT const void* ovr_LaunchDetails_GetUsers(const void*) { return g_emptyObject; }

// Microphone: 48 kHz mono capture from the device's default input (on the emulator, the Windows
// default recording device when it runs with -allow-host-audio). Reads never block: they return
// whatever has been captured so far. Needs the game's RECORD_AUDIO permission, as on a Quest.
constexpr int32_t kMicRate = 48000;
constexpr size_t kMicBufferSamples = kMicRate;  // one second

struct Microphone {
    AAudioStream* stream = nullptr;
    std::mutex mutex;
};

AAudioStream* openMicrophone() {
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return nullptr;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setSampleRate(builder, kMicRate);
    // No VOICE_COMMUNICATION preset: on the emulator it cut the level 5-10x.
    AAudioStreamBuilder_setBufferCapacityInFrames(builder, static_cast<int32_t>(kMicBufferSamples));
    AAudioStream* stream = nullptr;
    const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK) {
        LOGW("microphone open failed: %s (does the game have RECORD_AUDIO?)", AAudio_convertResultToText(result));
        return nullptr;
    }
    LOGI("microphone opened: %d Hz, %d channel(s)", AAudioStream_getSampleRate(stream), AAudioStream_getChannelCount(stream));
    return stream;
}

size_t readMicrophone(void* handle, float* out, size_t count) {
    auto* mic = static_cast<Microphone*>(handle);
    if (!mic || !out || count == 0) return 0;
    std::lock_guard<std::mutex> lock(mic->mutex);
    if (!mic->stream) return 0;
    const aaudio_result_t read = AAudioStream_read(mic->stream, out, static_cast<int32_t>(count), 0);
    return read > 0 ? static_cast<size_t>(read) : 0;
}

OVR_EXPORT void* ovr_Microphone_Create() { return new Microphone(); }
OVR_EXPORT void ovr_Microphone_Destroy(void* handle) {
    auto* mic = static_cast<Microphone*>(handle);
    if (!mic) return;
    if (mic->stream) AAudioStream_close(mic->stream);
    delete mic;
}
OVR_EXPORT void ovr_Microphone_Start(void* handle) {
    auto* mic = static_cast<Microphone*>(handle);
    if (!mic) return;
    std::lock_guard<std::mutex> lock(mic->mutex);
    if (!mic->stream) mic->stream = openMicrophone();
    if (mic->stream) AAudioStream_requestStart(mic->stream);
}
OVR_EXPORT void ovr_Microphone_Stop(void* handle) {
    auto* mic = static_cast<Microphone*>(handle);
    if (!mic) return;
    std::lock_guard<std::mutex> lock(mic->mutex);
    if (mic->stream) AAudioStream_requestStop(mic->stream);
}
OVR_EXPORT size_t ovr_Microphone_GetOutputBufferMaxSize(void*) { return kMicBufferSamples; }
OVR_EXPORT size_t ovr_Microphone_GetNumSamplesAvailable(void* handle) {
    auto* mic = static_cast<Microphone*>(handle);
    if (!mic) return 0;
    std::lock_guard<std::mutex> lock(mic->mutex);
    if (!mic->stream) return 0;
    const int64_t available = AAudioStream_getFramesWritten(mic->stream) - AAudioStream_getFramesRead(mic->stream);
    return available > 0 ? static_cast<size_t>(available) : 0;
}
OVR_EXPORT size_t ovr_Microphone_GetPCMFloat(void* handle, float* out, size_t count) { return readMicrophone(handle, out, count); }
OVR_EXPORT size_t ovr_Microphone_ReadData(void* handle, float* out, size_t count) { return readMicrophone(handle, out, count); }
OVR_EXPORT size_t ovr_Microphone_GetPCM(void* handle, int16_t* out, size_t count) {
    if (!out) return 0;
    float samples[1024];
    size_t total = 0;
    while (total < count) {
        const size_t want = std::min(count - total, sizeof(samples) / sizeof(samples[0]));
        const size_t got = readMicrophone(handle, samples, want);
        for (size_t i = 0; i < got; ++i) {
            out[total + i] = static_cast<int16_t>(std::clamp(samples[i], -1.0f, 1.0f) * 32767.0f);
        }
        total += got;
        if (got < want) break;
    }
    return total;
}
OVR_EXPORT void ovr_Microphone_SetAcceptableRecordingDelayHint(void*, size_t) {}

// debug.refract.platform.mic_selftest=1: when a game loads this library, record 2 s through the
// functions above on a background thread and log the level every half second.
void microphoneSelfTest() {
    std::thread([] {
        void* mic = ovr_Microphone_Create();
        ovr_Microphone_Start(mic);
        static float samples[kMicBufferSamples];
        for (int i = 1; i <= 4; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const size_t available = ovr_Microphone_GetNumSamplesAvailable(mic);
            const size_t got = ovr_Microphone_GetPCMFloat(mic, samples, kMicBufferSamples);
            float peak = 0;
            for (size_t s = 0; s < got; ++s) peak = std::max(peak, std::fabs(samples[s]));
            LOGI("mic selftest %.1fs: available %zu read %zu peak %.4f", i * 0.5, available, got, peak);
        }
        ovr_Microphone_Stop(mic);
        ovr_Microphone_Destroy(mic);
    }).detach();
}

#undef OVR_EXPORT

// Requests that succeed with no payload or an empty list instead of the default error:
// social/achievement/presence calls whose failure some games treat as fatal.
constexpr const char* kSucceedingRequests[] = {
    "ovr_User_GetLoggedInUserFriends",
    "ovr_User_GetBlockedUsers",
    "ovr_User_GetUserCapabilities",
    "ovr_User_GetSdkAccounts",
    "ovr_User_GetLinkedAccounts",
    "ovr_IAP_GetViewerPurchases",
    "ovr_IAP_GetViewerPurchasesDurableCache",
    "ovr_IAP_GetProductsBySKU",
    "ovr_AssetFile_GetList",
    "ovr_Application_GetInstalledApplications",
    "ovr_Achievements_GetAllDefinitions",
    "ovr_Achievements_GetAllProgress",
    "ovr_Achievements_GetDefinitionsByName",
    "ovr_Achievements_GetProgressByName",
    "ovr_Achievements_Unlock",
    "ovr_Achievements_AddCount",
    "ovr_Achievements_AddFields",
    "ovr_Leaderboard_Get",
    "ovr_Leaderboard_GetEntries",
    "ovr_Leaderboard_GetEntriesAfterRank",
    "ovr_Leaderboard_GetEntriesByIds",
    "ovr_Leaderboard_WriteEntry",
    "ovr_Leaderboard_WriteEntryWithSupplementaryMetric",
    "ovr_RichPresence_Set",
    "ovr_RichPresence_Clear",
    "ovr_RichPresence_GetDestinations",
    "ovr_GroupPresence_Set",
    "ovr_GroupPresence_Clear",
    "ovr_GroupPresence_SetDeeplinkMessageOverride",
    "ovr_GroupPresence_SetDestination",
    "ovr_GroupPresence_SetIsJoinable",
    "ovr_GroupPresence_SetLobbySession",
    "ovr_GroupPresence_SetMatchSession",
    "ovr_GroupPresence_GetInvitableUsers",
    "ovr_GroupPresence_GetSentInvites",
    "ovr_ApplicationLifecycle_GetRegisteredPIDs",
    "ovr_ApplicationLifecycle_RegisterSessionKey",
    "ovr_Notification_MarkAsRead",
    "ovr_PushNotification_Register",
};

#define FN(name) {#name, reinterpret_cast<void*>(&name)}
const std::unordered_map<std::string, void*>& implemented() {
    static const std::unordered_map<std::string, void*> table = {
        FN(ovr_PlatformInitializeAndroid), FN(ovr_PlatformInitializeAndroidWithOptions),
        FN(ovr_PlatformAndroidCheckInitialization), FN(ovr_PlatformInitializeAndroidAsynchronous),
        FN(ovr_PlatformInitializeAndroidAsynchronousWithOptions), FN(ovr_UnityInitWrapper),
        FN(ovr_UnityInitWrapperAsynchronous), FN(ovr_UnityInitWrapperInternalAsynchronous), FN(ovr_UnityInitWrapperStandalone), FN(ovr_UnityInitGlobals),
        FN(ovr_PlatformInitializeWithAccessToken), FN(ovr_PlatformInitializeWithAccessTokenAndOptions),
        FN(ovr_Platform_InitializeStandaloneOculus), FN(ovr_IsPlatformInitialized),
        FN(ovr_PopMessage), FN(ovr_FreeMessage), FN(ovr_Message_GetType), FN(ovr_Message_GetRequestID),
        FN(ovr_Message_IsError), FN(ovr_Message_GetError), FN(ovr_Message_GetString),
        FN(ovr_Message_GetNativeMessage), FN(ovr_Message_GetUser), FN(ovr_Message_GetPlatformInitialize),
        FN(ovr_Message_GetUserProof), FN(ovr_Message_GetOrgScopedID), FN(ovr_Message_GetApplicationVersion),
        FN(ovr_Message_GetUserAccountAgeCategory), FN(ovr_Message_GetAchievementUpdate),
        FN(ovr_Message_GetLeaderboardUpdateStatus),
        FN(ovr_Error_GetCode), FN(ovr_Error_GetHttpCode), FN(ovr_Error_GetMessage), FN(ovr_Error_GetDisplayableMessage),
        FN(ovr_PlatformInitialize_GetResult), FN(ovr_UserProof_GetNonce), FN(ovr_OrgScopedID_GetID),
        FN(ovr_UserAccountAgeCategory_GetAgeCategory),
        FN(ovr_GetLoggedInUserID), FN(ovr_GetLoggedInUserLocale), FN(ovr_User_GetLoggedInUser), FN(ovr_User_Get),
        FN(ovr_User_GetAccessToken), FN(ovr_User_GetUserProof), FN(ovr_User_GetOrgScopedID),
        FN(ovr_UserAgeCategory_Get), FN(ovr_User_GetID), FN(ovr_User_GetOculusID), FN(ovr_User_GetDisplayName),
        FN(ovr_User_GetPresenceStatus), FN(ovr_User_GetManagedInfo),
        FN(ovr_Entitlement_GetIsViewerEntitled), FN(ovr_IAP_LaunchCheckoutFlow), FN(ovr_IAP_ConsumePurchase),
        FN(ovr_Application_GetVersion), FN(ovr_ApplicationVersion_GetCurrentCode),
        FN(ovr_ApplicationVersion_GetCurrentName), FN(ovr_ApplicationVersion_GetLatestCode),
        FN(ovr_ApplicationVersion_GetLatestName), FN(ovr_ApplicationLifecycle_GetLaunchDetails),
        FN(ovr_LaunchDetails_GetLaunchType), FN(ovr_LaunchDetails_GetUsers),
        FN(ovr_Microphone_Create), FN(ovr_Microphone_Destroy), FN(ovr_Microphone_Start), FN(ovr_Microphone_Stop),
        FN(ovr_Microphone_GetOutputBufferMaxSize), FN(ovr_Microphone_GetNumSamplesAvailable),
        FN(ovr_Microphone_GetPCMFloat), FN(ovr_Microphone_ReadData), FN(ovr_Microphone_GetPCM),
        FN(ovr_Microphone_SetAcceptableRecordingDelayHint),
    };
    return table;
}
#undef FN

// ---- Generic stubs -----------------------------------------------------------------------

enum class StubKind : uint8_t { Zero, EmptyString, EmptyObject, FailingRequest, SucceedingRequest };

struct Stub {
    std::string name;
    StubKind kind = StubKind::Zero;
    uint32_t type = 0;
    std::atomic<bool> called{false};
};

std::mutex g_stubMutex;
Stub g_stubs[kStubCount];
uint32_t g_stubsUsed = 0;
std::unordered_map<std::string, uint32_t> g_stubByName;

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

StubKind classify(const std::string& name, uint32_t& type) {
    for (const auto& entry : kRequests) {
        if (name == entry.name) {
            type = entry.type;
            for (const char* ok : kSucceedingRequests) {
                if (name == ok) return StubKind::SucceedingRequest;
            }
            return StubKind::FailingRequest;
        }
    }
    for (const char* getter : kStringGetters) {
        if (name == getter) return StubKind::EmptyString;
    }
    // Array/list payloads must not be null: C# wrappers and games iterate them.
    if (name.rfind("ovr_Message_Get", 0) == 0 && endsWith(name, "Array")) return StubKind::EmptyObject;
    return StubKind::Zero;
}

void* stubFor(const char* name) {
    std::lock_guard<std::mutex> lock(g_stubMutex);
    auto found = g_stubByName.find(name);
    uint32_t index;
    if (found != g_stubByName.end()) {
        index = found->second;
    } else if (g_stubsUsed < kStubCount) {
        index = g_stubsUsed++;
        Stub& stub = g_stubs[index];
        stub.name = name;
        stub.kind = classify(stub.name, stub.type);
        g_stubByName.emplace(stub.name, index);
    } else {
        LOGW("stub pool exhausted; %s shares the last stub", name);
        index = kStubCount - 1;
    }
    return refract_ovr_stub_pool + index * kStubSize;
}

void* getProc(const char* name) {
    if (!name) return nullptr;
    const auto& table = implemented();
    auto found = table.find(name);
    if (found != table.end()) return found->second;
    if (g_verbose) LOGI("resolve %s -> stub", name);
    return stubFor(name);
}

}  // namespace

extern "C" __attribute__((visibility("hidden"))) uint64_t refract_ovr_stub_dispatch(uint32_t index) {
    Stub& stub = g_stubs[index < kStubCount ? index : kStubCount - 1];
    if (!stub.called.exchange(true) || g_verbose) LOGI("unimplemented %s called", stub.name.c_str());
    switch (stub.kind) {
        case StubKind::Zero: return 0;
        case StubKind::EmptyString: return reinterpret_cast<uint64_t>("");
        case StubKind::EmptyObject: return reinterpret_cast<uint64_t>(g_emptyObject);
        case StubKind::SucceedingRequest: return postSuccess(stub.type);
        case StubKind::FailingRequest:
            return postError(stub.type, kErrorNotSupported, "Refract: " + stub.name + " is not available");
    }
    return 0;
}

extern "C" JNIEXPORT jlong JNICALL Java_com_oculus_platform_loader_EntryPoint_nativeLoad(
    JNIEnv* env, jclass, jstring package, jlong versionCode, jstring versionName, jstring deviceId,
    jint major, jint minor, jint patch) {
    auto text = [env](jstring s) {
        if (!s) return std::string();
        const char* chars = env->GetStringUTFChars(s, nullptr);
        std::string out = chars ? chars : "";
        if (chars) env->ReleaseStringUTFChars(s, chars);
        return out;
    };
    g_app.package = text(package);
    g_app.versionCode = versionCode;
    g_app.versionName = text(versionName);
    g_verbose = property("debug.refract.platform.verbose") == "1";
    g_entitled = property("debug.refract.platform.owned." + g_app.package) == "1";

    // The user id defaults to a stable value derived from the device's ANDROID_ID.
    uint64_t id = std::strtoull(property("debug.refract.platform.user_id").c_str(), nullptr, 10);
    if (id == 0) {
        id = std::strtoull(text(deviceId).c_str(), nullptr, 16) & 0x000fffffffffffffull;
        if (id == 0) id = 1000000000000001ull;
    }
    g_self.id = id;
    g_self.oculusId = property("debug.refract.platform.user_name", "Refract");
    g_self.displayName = property("debug.refract.platform.display_name", g_self.oculusId.c_str());
    g_accessToken = property("debug.refract.platform.access_token");
    if (g_accessToken.empty()) g_accessToken = "Refract" + std::to_string(g_self.id);

    LOGI("Platform SDK %d.%d.%d loader attached for %s %s (%" PRId64 "); user %" PRIu64 " '%s'; owned: %s",
         major, minor, patch, g_app.package.c_str(), g_app.versionName.c_str(), g_app.versionCode, g_self.id,
         g_self.oculusId.c_str(), g_entitled ? "yes" : "no");
    if (property("debug.refract.platform.mic_selftest") == "1") microphoneSelfTest();
    return reinterpret_cast<jlong>(&getProc);
}
