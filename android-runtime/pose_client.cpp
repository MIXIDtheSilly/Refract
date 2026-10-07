#include "pose_client.h"
#include "host_address.h"
#include "perf_stats.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#if defined(__ANDROID__)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <jni.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <unistd.h>
#endif

namespace refract::runtime {

namespace {

constexpr uint16_t kDefaultBridgePort = 38490;

void log_pose_client(const char* message)
{
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "Refract.PoseClient", "%s", message);
#else
    std::fprintf(stderr, "Refract.PoseClient: %s\n", message);
#endif
}

#if defined(__ANDROID__)
bool connect_with_timeout(int socket, const sockaddr_in& address, int* out_error)
{
    timeval timeout{};
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    if (setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        *out_error = errno;
        return false;
    }

    const int result = connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (result == 0) {
        return true;
    }
    *out_error = errno;
    return false;
}

void log_connect_failure(const char* host, int error)
{
    char message[160]{};
    std::snprintf(
        message,
        sizeof(message),
        "connect failed host=%s port=%u errno=%d (%s)",
        host,
        static_cast<unsigned>(kDefaultBridgePort),
        error,
        std::strerror(error));
    log_pose_client(message);
}
#endif

} // namespace

refract::protocol::PoseFrame PoseClient::latest_pose_frame()
{
    static refract::protocol::PerfStats stats("pose-query");
    refract::protocol::PerfScope scope(stats);
    // Unreal polls from both game and render threads. A TCP record and its
    // decoder must have one reader, including connection/close operations.
    std::lock_guard lock(mutex_);
#if defined(__ANDROID__)
    // Emulator: host is 10.0.2.2. Waydroid (debug.refract.host_addr): host is that address. Real device
    // (debug.refract.direct_host=1): host is reached via `adb reverse tcp:38490`; the broker path blocks
    // each query on the next pushed frame.
    static const bool direct = [] {
        if (direct_host_address()) return true;
        char value[PROP_VALUE_MAX]{};
        __system_property_get("debug.refract.direct_host", value);
        return std::strcmp(value, "1") == 0;
    }();
    if (emulator_ || direct) {
        // Games query ~16 times a frame, and each recv costs a syscall (plus an ACK through the emulator's
        // virtual NIC) on the game thread. The server sends at most every 4 ms, so drain at most once a ms.
        const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        if (now >= next_drain_ns_ && ensure_emulator_connected()) {
            read_available_frames();
            next_drain_ns_ = now + 1'000'000;
        }
        return latest_;
    }
#endif
    if (query_pose_broker()) {
        return latest_;
    }
    if (ensure_connected()) {
        read_available_frames();
    }
    return latest_;
}

#if defined(__ANDROID__)
void PoseClient::set_android_context(JavaVM* vm, jobject context)
{
    std::lock_guard lock(mutex_);
    if (vm == nullptr || context == nullptr) {
        return;
    }

    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
        return;
    }

    java_vm_ = vm;
    if (android_context_ != nullptr) {
        env->DeleteGlobalRef(android_context_);
    }
    android_context_ = env->NewGlobalRef(context);
    log_pose_client("captured Android context for pose broker");
}

int PoseClient::open_image_transport_fd()
{
    std::lock_guard lock(mutex_);
    return open_provider_stream_locked(
        "content://org.khronos.openxr.system_runtime_broker/openxr/1/image/stream", image_provider_client_);
}

// Opens a relayed stream FD from the runtime's broker provider and keeps the provider client until
// the next stream of the same kind replaces it. Called with mutex_ held.
int PoseClient::open_provider_stream_locked(const char* uriString, jobject& heldClient)
{
    if (!java_vm_ || !android_context_) return -1;
    JNIEnv* env = nullptr;
    bool detach = false;
    const jint status = java_vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        if (java_vm_->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return -1;
        detach = true;
    } else if (status != JNI_OK || !env) {
        return -1;
    }

    int fd = -1;
    jclass contextClass = env->GetObjectClass(android_context_);
    jclass uriClass = env->FindClass("android/net/Uri");
    jstring uriText = env->NewStringUTF(uriString);
    jstring mode = env->NewStringUTF("rw");
    if (!env->ExceptionCheck() && contextClass && uriClass && uriText && mode) {
        jmethodID getResolver = env->GetMethodID(contextClass, "getContentResolver", "()Landroid/content/ContentResolver;");
        jmethodID parse = env->GetStaticMethodID(uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
        if (!env->ExceptionCheck() && getResolver && parse) {
            jobject resolver = env->CallObjectMethod(android_context_, getResolver);
            jobject uri = env->CallStaticObjectMethod(uriClass, parse, uriText);
            if (!env->ExceptionCheck() && resolver && uri) {
                // A stable provider client (unlike a one-off openFileDescriptor) makes Android rank the
                // relaying runtime process with this foreground app instead of freezing it as cached.
                jclass resolverClass = env->GetObjectClass(resolver);
                jmethodID acquire = env->GetMethodID(resolverClass, "acquireContentProviderClient",
                    "(Landroid/net/Uri;)Landroid/content/ContentProviderClient;");
                jobject client = !env->ExceptionCheck() && acquire ? env->CallObjectMethod(resolver, acquire, uri) : nullptr;
                if (!env->ExceptionCheck() && client) {
                    jclass clientClass = env->GetObjectClass(client);
                    jmethodID open = env->GetMethodID(clientClass, "openFile",
                        "(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;");
                    jmethodID close = env->GetMethodID(clientClass, "close", "()V");
                    jobject descriptor = !env->ExceptionCheck() && open ? env->CallObjectMethod(client, open, uri, mode) : nullptr;
                    if (!env->ExceptionCheck() && descriptor) {
                        jclass descriptorClass = env->GetObjectClass(descriptor);
                        jmethodID detachFd = env->GetMethodID(descriptorClass, "detachFd", "()I");
                        if (!env->ExceptionCheck() && detachFd) fd = env->CallIntMethod(descriptor, detachFd);
                        env->DeleteLocalRef(descriptorClass);
                        env->DeleteLocalRef(descriptor);
                    }
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    if (fd >= 0 && close) {
                        if (heldClient) {
                            env->CallVoidMethod(heldClient, close);
                            env->DeleteGlobalRef(heldClient);
                        }
                        heldClient = env->NewGlobalRef(client);
                    } else if (close) {
                        env->CallVoidMethod(client, close);
                    }
                    env->DeleteLocalRef(clientClass);
                    env->DeleteLocalRef(client);
                }
                env->DeleteLocalRef(resolverClass);
            }
            if (uri) env->DeleteLocalRef(uri);
            if (resolver) env->DeleteLocalRef(resolver);
        }
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        __android_log_print(ANDROID_LOG_INFO, "Refract.PoseClient", "provider FD request failed: %s", uriString);
    }
    if (mode) env->DeleteLocalRef(mode);
    if (uriText) env->DeleteLocalRef(uriText);
    if (uriClass) env->DeleteLocalRef(uriClass);
    if (contextClass) env->DeleteLocalRef(contextClass);
    if (detach) java_vm_->DetachCurrentThread();
    return fd;
}
#endif

bool PoseClient::query_pose_broker()
{
#if !defined(__ANDROID__)
    return false;
#else
    if (java_vm_ == nullptr || android_context_ == nullptr) {
        return false;
    }
    if (broker_retry_countdown_ > 0) {
        --broker_retry_countdown_;
        return false;
    }

    JNIEnv* env = nullptr;
    bool detach = false;
    jint envResult = java_vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (envResult == JNI_EDETACHED) {
        if (java_vm_->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) {
            broker_retry_countdown_ = 90;
            return false;
        }
        detach = true;
    } else if (envResult != JNI_OK || env == nullptr) {
        broker_retry_countdown_ = 90;
        return false;
    }

    bool ok = false;
    jobject cursor = nullptr;
    jstring uriString = nullptr;
    jobject uri = nullptr;
    jobject resolver = nullptr;

    do {
        jclass contextClass = env->GetObjectClass(android_context_);
        jmethodID getContentResolver = env->GetMethodID(contextClass, "getContentResolver", "()Landroid/content/ContentResolver;");
        resolver = env->CallObjectMethod(android_context_, getContentResolver);
        if (env->ExceptionCheck() || resolver == nullptr) {
            env->ExceptionClear();
            break;
        }

        jclass uriClass = env->FindClass("android/net/Uri");
        jmethodID parse = env->GetStaticMethodID(uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
        uriString = env->NewStringUTF("content://org.khronos.openxr.system_runtime_broker/openxr/1/pose/latest");
        uri = env->CallStaticObjectMethod(uriClass, parse, uriString);
        if (env->ExceptionCheck() || uri == nullptr) {
            env->ExceptionClear();
            break;
        }

        jclass resolverClass = env->GetObjectClass(resolver);
        jmethodID query = env->GetMethodID(
            resolverClass,
            "query",
            "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;");
        cursor = env->CallObjectMethod(resolver, query, uri, nullptr, nullptr, nullptr, nullptr);
        if (env->ExceptionCheck() || cursor == nullptr) {
            env->ExceptionClear();
            break;
        }

        jclass cursorClass = env->GetObjectClass(cursor);
        jmethodID moveToFirst = env->GetMethodID(cursorClass, "moveToFirst", "()Z");
        if (!env->CallBooleanMethod(cursor, moveToFirst)) {
            break;
        }

        jmethodID getLong = env->GetMethodID(cursorClass, "getLong", "(I)J");
        jmethodID getFloat = env->GetMethodID(cursorClass, "getFloat", "(I)F");
        latest_.sequence = static_cast<uint64_t>(env->CallLongMethod(cursor, getLong, 0));
        latest_.version = 1; // Legacy cursor exposes only head/controller poses.
        latest_.monotonic_time_ns = static_cast<uint64_t>(env->CallLongMethod(cursor, getLong, 1));
        latest_.hmd.x = env->CallFloatMethod(cursor, getFloat, 2);
        latest_.hmd.y = env->CallFloatMethod(cursor, getFloat, 3);
        latest_.hmd.z = env->CallFloatMethod(cursor, getFloat, 4);
        latest_.hmd.qx = env->CallFloatMethod(cursor, getFloat, 5);
        latest_.hmd.qy = env->CallFloatMethod(cursor, getFloat, 6);
        latest_.hmd.qz = env->CallFloatMethod(cursor, getFloat, 7);
        latest_.hmd.qw = env->CallFloatMethod(cursor, getFloat, 8);
        latest_.left_controller.x = env->CallFloatMethod(cursor, getFloat, 9);
        latest_.left_controller.y = env->CallFloatMethod(cursor, getFloat, 10);
        latest_.left_controller.z = env->CallFloatMethod(cursor, getFloat, 11);
        latest_.left_controller.qx = env->CallFloatMethod(cursor, getFloat, 12);
        latest_.left_controller.qy = env->CallFloatMethod(cursor, getFloat, 13);
        latest_.left_controller.qz = env->CallFloatMethod(cursor, getFloat, 14);
        latest_.left_controller.qw = env->CallFloatMethod(cursor, getFloat, 15);
        latest_.right_controller.x = env->CallFloatMethod(cursor, getFloat, 16);
        latest_.right_controller.y = env->CallFloatMethod(cursor, getFloat, 17);
        latest_.right_controller.z = env->CallFloatMethod(cursor, getFloat, 18);
        latest_.right_controller.qx = env->CallFloatMethod(cursor, getFloat, 19);
        latest_.right_controller.qy = env->CallFloatMethod(cursor, getFloat, 20);
        latest_.right_controller.qz = env->CallFloatMethod(cursor, getFloat, 21);
        latest_.right_controller.qw = env->CallFloatMethod(cursor, getFloat, 22);
        ok = true;
    } while (false);

    if (cursor != nullptr) {
        jclass cursorClass = env->GetObjectClass(cursor);
        jmethodID close = env->GetMethodID(cursorClass, "close", "()V");
        env->CallVoidMethod(cursor, close);
    }

    if (!ok) {
        broker_retry_countdown_ = 30;
    }

    if (detach) {
        java_vm_->DetachCurrentThread();
    }
    return ok;
#endif
}

bool PoseClient::ensure_connected()
{
#if !defined(__ANDROID__)
    return false;
#else
    if (socket_ >= 0) {
        return true;
    }
    if (retry_countdown_ > 0) {
        --retry_countdown_;
        return false;
    }
    retry_countdown_ = 90;

    constexpr const char* kCandidateHosts[] = {
        "172.26.32.1",
        "127.0.0.1",
        "192.168.240.1",
        "10.0.2.2",
    };

    for (const char* host : kCandidateHosts) {
        int candidate = socket(AF_INET, SOCK_STREAM, 0);
        if (candidate < 0) {
            continue;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kDefaultBridgePort);
        if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
            close(candidate);
            continue;
        }

        int connect_error = 0;
        if (connect_with_timeout(candidate, address, &connect_error)) {
            socket_ = candidate;
            char message[96]{};
            std::snprintf(message, sizeof(message), "connected to host bridge host=%s", host);
            log_pose_client(message);
            return true;
        }

        log_connect_failure(host, connect_error);
        close(candidate);
    }

    log_pose_client("host bridge unavailable; using fallback pose");
    return false;
#endif
}

bool PoseClient::ensure_emulator_connected()
{
#if !defined(__ANDROID__)
    return false;
#else
    const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if (socket_ >= 0) {
        if (!connecting_) { return true; }
        pollfd descriptor{socket_, POLLOUT, 0};
        const int ready = poll(&descriptor, 1, 0);
        if (ready < 0 && errno == EINTR) { return false; }
        if (ready == 0 && now < next_connect_ns_) { return false; }
        int error = 0;
        socklen_t length = sizeof(error);
        if (ready > 0 && getsockopt(socket_, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0) {
            connecting_ = false;
            log_pose_client("nonblocking emulator pose stream connected");
            return true;
        }
        close_socket();
        next_connect_ns_ = now + 1'000'000'000;
        return false;
    }
    if (now < next_connect_ns_) { return false; }
    next_connect_ns_ = now + 1'000'000'000;
    if (!socket_permitted_) { return open_relayed_pose_stream_locked(); }
    socket_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (socket_ < 0) {
        if (errno == EPERM || errno == EACCES) {
            // No INTERNET permission (e.g. AC Nexus): the runtime APK relays the stream instead.
            socket_permitted_ = false;
            log_pose_client("app may not open sockets; using the broker's relayed pose stream");
            return open_relayed_pose_stream_locked();
        }
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kDefaultBridgePort);
    const char* host = direct_host_address();
    if (inet_pton(AF_INET, host ? host : emulator_ ? "10.0.2.2" : "127.0.0.1", &address.sin_addr) != 1) {
        log_pose_client("debug.refract.host_addr is not an IPv4 address");
        close_socket();
        return false;
    }
    if (connect(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) { return true; }
    if (errno == EINPROGRESS) { connecting_ = true; return false; }
    close_socket();
    return false;
#endif
}

bool PoseClient::open_relayed_pose_stream_locked()
{
#if !defined(__ANDROID__)
    return false;
#else
    socket_ = open_provider_stream_locked(
        "content://org.khronos.openxr.system_runtime_broker/openxr/1/pose/stream", pose_provider_client_);
    if (socket_ < 0) { return false; }
    connecting_ = false;
    log_pose_client("relayed pose stream opened");
    return true;
#endif
}

void PoseClient::read_available_frames()
{
#if !defined(__ANDROID__)
    return;
#else
    unsigned char bytes[4096];
    // Bound the drain even if the producer is continuously writing.
    for (int batch = 0; batch < 64; ++batch) {
        const ssize_t received = recv(socket_, bytes, sizeof(bytes), MSG_DONTWAIT);
        if (received > 0) {
            const uint64_t before = latest_.sequence;
            if (decoder_.append(bytes, static_cast<size_t>(received), latest_)) {
                static refract::protocol::FrameIntervals arrivals("pose-arrival");
                if (latest_.sequence != before) arrivals.record();
                continue;
            }
            log_pose_client("invalid pose record");
        } else if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else if (received < 0 && errno == EINTR) {
            continue;
        }
        close_socket();
        return;
    }
#endif
}

void PoseClient::close_socket()
{
    connecting_ = false;
    decoder_.reset();
#if defined(__ANDROID__)
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
#else
    socket_ = -1;
#endif
}

PoseClient& pose_client()
{
    static PoseClient client;
    return client;
}

} // namespace refract::runtime
