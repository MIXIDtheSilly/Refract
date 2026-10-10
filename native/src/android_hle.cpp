// libandroid.so / libnativewindow.so for the guest: native windows (a Win32
// window), assets (through the Java AssetManager), configuration, ALooper and
// AChoreographer. Everything else exported by the real libraries resolves to a
// stub that logs once and returns 0 (see thunks.cpp).
#include "android_hle.h"

#include <windows.h>

#include <jni.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "files.h"
#include "host_runtime.h"
#include "jni_bridge.h"
#include "net.h"
#include "thunks.h"

namespace rn {

namespace {

// --- native window ---------------------------------------------------------------

constexpr u32 kWindowMagic = 0x574e4652;  // "RFNW"

struct HostNativeWindow {
    u32 magic = kWindowMagic;
    HWND hwnd = nullptr;
    int width = 0, height = 0;
    int buffer_width = 0, buffer_height = 0;
    int format = 1;  // WINDOW_FORMAT_RGBA_8888
    std::atomic<int> refs{1};
};

HostNativeWindow* AsWindow(const void* p) {
    auto* w = static_cast<HostNativeWindow*>(const_cast<void*>(p));
    return (w && w->magic == kWindowMagic) ? w : nullptr;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        Log("window closed: exiting");
        fflush(stdout);
        ExitProcess(0);
    case WM_SIZE: {
        auto* w = reinterpret_cast<HostNativeWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (w) {
            w->width = LOWORD(lp);
            w->height = HIWORD(lp);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

HostNativeWindow* CreateHostWindow(int width, int height, const std::string& title) {
    auto* w = new HostNativeWindow;
    w->width = width;
    w->height = height;
    std::mutex mu;
    std::condition_variable cv;
    bool ready = false;
    std::thread([&, w] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"RefractNativeWindow";
        RegisterClassW(&wc);
        RECT r{0, 0, width, height};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, Widen(title + " - Refract").c_str(), WS_OVERLAPPEDWINDOW,
                                    CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                                    wc.hInstance, nullptr);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
        w->hwnd = hwnd;  // shown once something renders to it (VR apps never do)
        {
            std::lock_guard lock(mu);
            ready = true;
        }
        cv.notify_all();
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }).detach();
    std::unique_lock lock(mu);
    cv.wait(lock, [&] { return ready; });
    RN_INFO("native window %dx%d created", width, height);
    return w;
}

jlong JNICALL N_createNativeWindow(JNIEnv* env, jclass, jint w, jint h, jstring title) {
    const char* t = env->GetStringUTFChars(title, nullptr);
    std::string s = t ? t : "app";
    env->ReleaseStringUTFChars(title, t);
    return reinterpret_cast<jlong>(CreateHostWindow(w, h, s));
}

void* A_ANativeWindow_fromSurface(u64, jobject surface) {
    JNIEnv* env = HostJniEnv();
    jclass c = env->GetObjectClass(surface);
    jfieldID f = env->GetFieldID(c, "mNativeObject", "J");
    if (!f) {
        env->ExceptionClear();
        return nullptr;
    }
    auto* w = AsWindow(reinterpret_cast<void*>(env->GetLongField(surface, f)));
    if (w)
        w->refs++;
    return w;
}
void A_ANativeWindow_acquire(void* p) {
    if (auto* w = AsWindow(p))
        w->refs++;
}
void A_ANativeWindow_release(void* p) {
    if (auto* w = AsWindow(p))
        w->refs--;
}
s32 A_ANativeWindow_getWidth(void* p) {
    auto* w = AsWindow(p);
    return w ? (w->buffer_width ? w->buffer_width : w->width) : -22;
}
s32 A_ANativeWindow_getHeight(void* p) {
    auto* w = AsWindow(p);
    return w ? (w->buffer_height ? w->buffer_height : w->height) : -22;
}
s32 A_ANativeWindow_getFormat(void* p) {
    auto* w = AsWindow(p);
    return w ? w->format : -22;
}
s32 A_ANativeWindow_setBuffersGeometry(void* p, s32 width, s32 height, s32 format) {
    auto* w = AsWindow(p);
    if (!w)
        return -22;
    w->buffer_width = width;
    w->buffer_height = height;
    if (format)
        w->format = format;
    return 0;
}
s32 A_ANativeWindow_lock(void*, void*, void*) { return -38; }
s32 A_ANativeWindow_unlockAndPost(void*) { return -38; }
s32 A_Zero() { return 0; }

// --- assets ---------------------------------------------------------------------------

struct HostAsset {
    std::vector<u8> data;
    s64 pos = 0;
};
struct HostAssetDir {
    std::vector<std::string> names;
    size_t next = 0;
};
u8 g_asset_manager_tag;

jclass AssetsClass(JNIEnv* env) {
    static jclass cls = nullptr;
    if (!cls) {
        jclass local = env->FindClass("refract/app/Assets");
        cls = static_cast<jclass>(env->NewGlobalRef(local));
    }
    return cls;
}

void* A_AAssetManager_fromJava(u64, jobject) { return &g_asset_manager_tag; }

void* A_AAssetManager_open(void*, const char* name, s32) {
    JNIEnv* env = HostJniEnv();
    jclass c = AssetsClass(env);
    jmethodID m = env->GetStaticMethodID(c, "read", "(Ljava/lang/String;)[B");
    jstring jn = env->NewStringUTF(name);
    auto arr = static_cast<jbyteArray>(env->CallStaticObjectMethod(c, m, jn));
    env->DeleteLocalRef(jn);
    if (!arr) {
        RN_TRACE("AAssetManager_open(%s): not found", name);
        return nullptr;
    }
    auto* a = new HostAsset;
    a->data.resize(env->GetArrayLength(arr));
    env->GetByteArrayRegion(arr, 0, static_cast<jsize>(a->data.size()), reinterpret_cast<jbyte*>(a->data.data()));
    env->DeleteLocalRef(arr);
    RN_TRACE("AAssetManager_open(%s): %zu bytes", name, a->data.size());
    return a;
}
s32 A_AAsset_read(HostAsset* a, void* buf, u64 count) {
    s64 left = static_cast<s64>(a->data.size()) - a->pos;
    s64 n = std::min<s64>(left, static_cast<s64>(count));
    if (n <= 0)
        return 0;
    if (!SafeCopyToGuest(reinterpret_cast<u64>(buf), a->data.data() + a->pos, n))
        return -1;
    a->pos += n;
    return static_cast<s32>(n);
}
s64 A_AAsset_seek64(HostAsset* a, s64 off, s32 whence) {
    s64 base = whence == 0 ? 0 : whence == 1 ? a->pos : static_cast<s64>(a->data.size());
    s64 p = base + off;
    if (p < 0 || p > static_cast<s64>(a->data.size()))
        return -1;
    a->pos = p;
    return p;
}
s32 A_AAsset_seek(HostAsset* a, s32 off, s32 whence) { return static_cast<s32>(A_AAsset_seek64(a, off, whence)); }
s64 A_AAsset_getLength64(HostAsset* a) { return static_cast<s64>(a->data.size()); }
s32 A_AAsset_getLength(HostAsset* a) { return static_cast<s32>(a->data.size()); }
s64 A_AAsset_getRemainingLength64(HostAsset* a) { return static_cast<s64>(a->data.size()) - a->pos; }
s32 A_AAsset_getRemainingLength(HostAsset* a) { return static_cast<s32>(a->data.size() - a->pos); }
const void* A_AAsset_getBuffer(HostAsset* a) { return a->data.data(); }
s32 A_AAsset_isAllocated(HostAsset*) { return 1; }
s32 A_AAsset_openFileDescriptor(HostAsset*, void*, void*) { return -1; }
void A_AAsset_close(HostAsset* a) { delete a; }

void* A_AAssetManager_openDir(void*, const char* dir) {
    JNIEnv* env = HostJniEnv();
    jclass c = AssetsClass(env);
    jmethodID m = env->GetStaticMethodID(c, "list", "(Ljava/lang/String;)[Ljava/lang/String;");
    jstring jd = env->NewStringUTF(dir);
    auto arr = static_cast<jobjectArray>(env->CallStaticObjectMethod(c, m, jd));
    auto* d = new HostAssetDir;
    if (arr) {
        jsize n = env->GetArrayLength(arr);
        for (jsize i = 0; i < n; ++i) {
            auto s = static_cast<jstring>(env->GetObjectArrayElement(arr, i));
            const char* u = env->GetStringUTFChars(s, nullptr);
            d->names.emplace_back(u);
            env->ReleaseStringUTFChars(s, u);
            env->DeleteLocalRef(s);
        }
    }
    return d;
}
const char* A_AAssetDir_getNextFileName(HostAssetDir* d) {
    return d->next < d->names.size() ? d->names[d->next++].c_str() : nullptr;
}
void A_AAssetDir_rewind(HostAssetDir* d) { d->next = 0; }
void A_AAssetDir_close(HostAssetDir* d) { delete d; }

// --- configuration ---------------------------------------------------------------------

struct HostConfig {
    s32 density = 160, orientation = 2, sdk = 34, ui_mode_type = 7, touchscreen = 1, keyboard = 1,
        navigation = 1, screen_size = 3, screen_long = 2, screen_width_dp = 1280, screen_height_dp = 720;
};
void* A_AConfiguration_new() { return new HostConfig; }
void A_AConfiguration_delete(HostConfig* c) { delete c; }
void A_AConfiguration_fromAssetManager(HostConfig*, void*) {}
s32 A_AConfiguration_getDensity(HostConfig* c) { return c->density; }
s32 A_AConfiguration_getOrientation(HostConfig* c) { return c->orientation; }
s32 A_AConfiguration_getSdkVersion(HostConfig* c) { return c->sdk; }
s32 A_AConfiguration_getUiModeType(HostConfig* c) { return c->ui_mode_type; }
s32 A_AConfiguration_getTouchscreen(HostConfig* c) { return c->touchscreen; }
s32 A_AConfiguration_getKeyboard(HostConfig* c) { return c->keyboard; }
s32 A_AConfiguration_getNavigation(HostConfig* c) { return c->navigation; }
s32 A_AConfiguration_getScreenSize(HostConfig* c) { return c->screen_size; }
s32 A_AConfiguration_getScreenLong(HostConfig* c) { return c->screen_long; }
s32 A_AConfiguration_getScreenWidthDp(HostConfig* c) { return c->screen_width_dp; }
s32 A_AConfiguration_getScreenHeightDp(HostConfig* c) { return c->screen_height_dp; }
void A_AConfiguration_getLanguage(HostConfig*, char* out) { SafeCopyToGuest(reinterpret_cast<u64>(out), "en", 2); }
void A_AConfiguration_getCountry(HostConfig*, char* out) { SafeCopyToGuest(reinterpret_cast<u64>(out), "US", 2); }

// --- ALooper / AChoreographer ------------------------------------------------------------

struct LooperFd {
    s32 fd, ident, events;
    u64 callback, data;
};
struct ChoreoCallback {
    u64 fn, data;
    bool is64;
    u64 due_ns;
};
struct HostLooper {
    std::mutex mu;
    std::vector<LooperFd> fds;
    std::vector<ChoreoCallback> frames;
    std::atomic<bool> woken{false};
    std::atomic<int> refs{1};
};
thread_local HostLooper* tls_looper = nullptr;
struct HostChoreographer {
    HostLooper* looper;
};
thread_local HostChoreographer* tls_choreo = nullptr;

constexpr s32 ALOOPER_POLL_WAKE = -1, ALOOPER_POLL_CALLBACK = -2, ALOOPER_POLL_TIMEOUT = -3;

void* A_ALooper_forThread() { return tls_looper; }
void* A_ALooper_prepare(s32) {
    if (!tls_looper)
        tls_looper = new HostLooper;
    return tls_looper;
}
void A_ALooper_acquire(HostLooper* l) {
    if (l)
        l->refs++;
}
void A_ALooper_release(HostLooper* l) {
    if (l)
        l->refs--;
}
void A_ALooper_wake(HostLooper* l) {
    if (!l)
        return;
    l->woken = true;
    IoNotify();
}
s32 A_ALooper_addFd(HostLooper* l, s32 fd, s32 ident, s32 events, u64 callback, u64 data) {
    if (!l)
        return -1;
    std::lock_guard lock(l->mu);
    for (auto& e : l->fds) {
        if (e.fd == fd) {
            e = {fd, ident, events, callback, data};
            return 1;
        }
    }
    l->fds.push_back({fd, ident, events, callback, data});
    IoNotify();
    return 1;
}
s32 A_ALooper_removeFd(HostLooper* l, s32 fd) {
    if (!l)
        return -1;
    std::lock_guard lock(l->mu);
    for (auto it = l->fds.begin(); it != l->fds.end(); ++it) {
        if (it->fd == fd) {
            l->fds.erase(it);
            return 1;
        }
    }
    return 0;
}

// One poll iteration; returns an ident, ALOOPER_POLL_* or 0 to keep waiting.
s32 PollOnceInner(GuestThread* t, HostLooper* l, u64 out_fd, u64 out_events, u64 out_data, bool* ran_callback) {
    if (l->woken.exchange(false))
        return ALOOPER_POLL_WAKE;
    // Due choreographer frames.
    std::vector<ChoreoCallback> due;
    {
        std::lock_guard lock(l->mu);
        u64 now = MonotonicNs();
        for (auto it = l->frames.begin(); it != l->frames.end();) {
            if (it->due_ns <= now) {
                due.push_back(*it);
                it = l->frames.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& f : due) {
        // Frame time is CLOCK_MONOTONIC ns; the 32-bit variant takes a long (also 64-bit on arm64).
        CallGuest(t, f.fn, {MonotonicNs(), f.data});
        *ran_callback = true;
    }
    std::vector<LooperFd> fds;
    {
        std::lock_guard lock(l->mu);
        fds = l->fds;
    }
    for (const auto& e : fds) {
        FilePtr f = Fds().Get(e.fd);
        if (!f)
            continue;
        u32 ready = f->Poll() & (static_cast<u32>(e.events) | kPollErr | kPollHup);
        if (!ready)
            continue;
        if (e.callback) {
            u64 keep = CallGuest(t, e.callback, {static_cast<u64>(e.fd), ready, e.data});
            if ((keep & 0xffffffff) == 0)
                A_ALooper_removeFd(l, e.fd);
            *ran_callback = true;
            continue;
        }
        u32 fd = static_cast<u32>(e.fd);
        if (out_fd)
            SafeCopyToGuest(out_fd, &fd, 4);
        if (out_events)
            SafeCopyToGuest(out_events, &ready, 4);
        if (out_data)
            SafeCopyToGuest(out_data, &e.data, 8);
        return e.ident;
    }
    return 0;
}

s32 A_ALooper_pollOnce(s32 timeout_ms, u64 out_fd, u64 out_events, u64 out_data) {
    GuestThread* t = CurrentThread();
    HostLooper* l = tls_looper;
    if (!l || !t)
        return -4;  // ALOOPER_POLL_ERROR
    s64 deadline = timeout_ms < 0 ? -1 : static_cast<s64>(MonotonicNs()) + timeout_ms * 1000000ll;
    for (;;) {
        u64 gen = IoGeneration();
        bool ran = false;
        s32 r = PollOnceInner(t, l, out_fd, out_events, out_data, &ran);
        if (r != 0)
            return r;
        if (ran)
            return ALOOPER_POLL_CALLBACK;
        if (timeout_ms == 0)
            return ALOOPER_POLL_TIMEOUT;
        s64 wait_until = deadline;
        {
            std::lock_guard lock(l->mu);
            for (const auto& f : l->frames)
                if (wait_until < 0 || static_cast<s64>(f.due_ns) < wait_until)
                    wait_until = static_cast<s64>(f.due_ns);
        }
        if (deadline >= 0 && static_cast<s64>(MonotonicNs()) >= deadline)
            return ALOOPER_POLL_TIMEOUT;
        if (!IoWait(t, gen, wait_until))
            return -1;
    }
}

s32 A_ALooper_pollAll(s32 timeout_ms, u64 out_fd, u64 out_events, u64 out_data) {
    for (;;) {
        s32 r = A_ALooper_pollOnce(timeout_ms, out_fd, out_events, out_data);
        if (r != ALOOPER_POLL_CALLBACK)
            return r;
    }
}

void* A_AChoreographer_getInstance() {
    if (!tls_looper)
        return nullptr;
    if (!tls_choreo)
        tls_choreo = new HostChoreographer{tls_looper};
    return tls_choreo;
}

u64 NextVsync(u64 delay_ns) {
    const u64 period = 1000000000ull / 90;
    u64 t = MonotonicNs() + delay_ns;
    return (t / period + 1) * period;
}

void PostFrame(HostChoreographer* c, u64 fn, u64 data, bool is64, u64 delay_ns) {
    if (!c)
        return;
    std::lock_guard lock(c->looper->mu);
    c->looper->frames.push_back({fn, data, is64, NextVsync(delay_ns)});
    IoNotify();
}
void A_AChoreographer_postFrameCallback(HostChoreographer* c, u64 fn, u64 data) { PostFrame(c, fn, data, false, 0); }
void A_AChoreographer_postFrameCallback64(HostChoreographer* c, u64 fn, u64 data) { PostFrame(c, fn, data, true, 0); }
void A_AChoreographer_postFrameCallbackDelayed(HostChoreographer* c, u64 fn, u64 data, s64 ms) {
    PostFrame(c, fn, data, false, static_cast<u64>(ms) * 1000000ull);
}
void A_AChoreographer_postFrameCallbackDelayed64(HostChoreographer* c, u64 fn, u64 data, u32 ms) {
    PostFrame(c, fn, data, true, static_cast<u64>(ms) * 1000000ull);
}

// --- misc ------------------------------------------------------------------------------------

u8 g_sensor_manager_tag;
void* A_ASensorManager_getInstance() { return &g_sensor_manager_tag; }
void* A_ASensorManager_getInstanceForPackage(const char*) { return &g_sensor_manager_tag; }
s32 A_ASensorManager_getSensorList(void*, u64 list) {
    u64 zero = 0;
    if (list)
        SafeCopyToGuest(list, &zero, 8);
    return 0;
}
void* A_Null() { return nullptr; }
s32 A_ENOSYS() { return -38; }

template <auto F>
ThunkFn W() {
    return &Wrap<F>;
}

std::map<std::string, ThunkFn> WindowThunks() {
    return {
        {"ANativeWindow_fromSurface", W<&A_ANativeWindow_fromSurface>()},
        {"ANativeWindow_acquire", W<&A_ANativeWindow_acquire>()},
        {"ANativeWindow_release", W<&A_ANativeWindow_release>()},
        {"ANativeWindow_getWidth", W<&A_ANativeWindow_getWidth>()},
        {"ANativeWindow_getHeight", W<&A_ANativeWindow_getHeight>()},
        {"ANativeWindow_getFormat", W<&A_ANativeWindow_getFormat>()},
        {"ANativeWindow_setBuffersGeometry", W<&A_ANativeWindow_setBuffersGeometry>()},
        {"ANativeWindow_lock", W<&A_ANativeWindow_lock>()},
        {"ANativeWindow_unlockAndPost", W<&A_ANativeWindow_unlockAndPost>()},
        {"ANativeWindow_setBuffersTransform", W<&A_Zero>()},
        {"ANativeWindow_setBuffersDataSpace", W<&A_Zero>()},
        {"ANativeWindow_getBuffersDataSpace", W<&A_Zero>()},
        {"ANativeWindow_setFrameRate", W<&A_Zero>()},
        {"ANativeWindow_setFrameRateWithChangeStrategy", W<&A_Zero>()},
        {"AHardwareBuffer_allocate", W<&A_ENOSYS>()},
        {"AHardwareBuffer_lock", W<&A_ENOSYS>()},
        {"AHardwareBuffer_unlock", W<&A_ENOSYS>()},
    };
}

}  // namespace

void* NativeWindowHwnd(const void* window) {
    auto* w = AsWindow(window);
    if (w && w->hwnd && !IsWindowVisible(w->hwnd))
        ShowWindowAsync(w->hwnd, SW_SHOW);
    return w ? w->hwnd : nullptr;
}

// refract.app.NativeLooper: the main thread's Java Looper runs on its ALooper, as on
// Android, so fds and choreographer callbacks native code adds to it get dispatched.
jlong JNICALL N_looperPrepare(JNIEnv*, jclass) {
    EnsureGuestThread();
    return reinterpret_cast<jlong>(A_ALooper_prepare(0));
}
jint JNICALL N_looperPollOnce(JNIEnv*, jclass, jlong, jint timeout_ms) { return A_ALooper_pollOnce(timeout_ms, 0, 0, 0); }
void JNICALL N_looperWake(JNIEnv*, jclass, jlong looper) { A_ALooper_wake(reinterpret_cast<HostLooper*>(looper)); }

bool RegisterAndroidNatives(JNIEnv* env) {
    jclass c = env->FindClass("refract/view/NativeWindows");
    if (!c)
        return false;
    JNINativeMethod m{const_cast<char*>("createNativeWindow"), const_cast<char*>("(IILjava/lang/String;)J"),
                      reinterpret_cast<void*>(&N_createNativeWindow)};
    if (env->RegisterNatives(c, &m, 1) != JNI_OK)
        return false;
    jclass l = env->FindClass("refract/app/NativeLooper");
    if (!l)
        return false;
    JNINativeMethod lm[] = {
        {const_cast<char*>("prepare"), const_cast<char*>("()J"), reinterpret_cast<void*>(&N_looperPrepare)},
        {const_cast<char*>("pollOnce"), const_cast<char*>("(JI)I"), reinterpret_cast<void*>(&N_looperPollOnce)},
        {const_cast<char*>("wake"), const_cast<char*>("(J)V"), reinterpret_cast<void*>(&N_looperWake)},
    };
    return env->RegisterNatives(l, lm, sizeof(lm) / sizeof(lm[0])) == JNI_OK;
}

void RegisterAndroidHle() {
    std::map<std::string, ThunkFn> m = WindowThunks();
    std::map<std::string, ThunkFn> extra = {
        {"AAssetManager_fromJava", W<&A_AAssetManager_fromJava>()},
        {"AAssetManager_open", W<&A_AAssetManager_open>()},
        {"AAssetManager_openDir", W<&A_AAssetManager_openDir>()},
        {"AAssetDir_getNextFileName", W<&A_AAssetDir_getNextFileName>()},
        {"AAssetDir_rewind", W<&A_AAssetDir_rewind>()},
        {"AAssetDir_close", W<&A_AAssetDir_close>()},
        {"AAsset_read", W<&A_AAsset_read>()},
        {"AAsset_seek", W<&A_AAsset_seek>()},
        {"AAsset_seek64", W<&A_AAsset_seek64>()},
        {"AAsset_getLength", W<&A_AAsset_getLength>()},
        {"AAsset_getLength64", W<&A_AAsset_getLength64>()},
        {"AAsset_getRemainingLength", W<&A_AAsset_getRemainingLength>()},
        {"AAsset_getRemainingLength64", W<&A_AAsset_getRemainingLength64>()},
        {"AAsset_getBuffer", W<&A_AAsset_getBuffer>()},
        {"AAsset_isAllocated", W<&A_AAsset_isAllocated>()},
        {"AAsset_openFileDescriptor", W<&A_AAsset_openFileDescriptor>()},
        {"AAsset_openFileDescriptor64", W<&A_AAsset_openFileDescriptor>()},
        {"AAsset_close", W<&A_AAsset_close>()},
        {"AConfiguration_new", W<&A_AConfiguration_new>()},
        {"AConfiguration_delete", W<&A_AConfiguration_delete>()},
        {"AConfiguration_fromAssetManager", W<&A_AConfiguration_fromAssetManager>()},
        {"AConfiguration_getDensity", W<&A_AConfiguration_getDensity>()},
        {"AConfiguration_getOrientation", W<&A_AConfiguration_getOrientation>()},
        {"AConfiguration_getSdkVersion", W<&A_AConfiguration_getSdkVersion>()},
        {"AConfiguration_getUiModeType", W<&A_AConfiguration_getUiModeType>()},
        {"AConfiguration_getTouchscreen", W<&A_AConfiguration_getTouchscreen>()},
        {"AConfiguration_getKeyboard", W<&A_AConfiguration_getKeyboard>()},
        {"AConfiguration_getNavigation", W<&A_AConfiguration_getNavigation>()},
        {"AConfiguration_getScreenSize", W<&A_AConfiguration_getScreenSize>()},
        {"AConfiguration_getScreenLong", W<&A_AConfiguration_getScreenLong>()},
        {"AConfiguration_getScreenWidthDp", W<&A_AConfiguration_getScreenWidthDp>()},
        {"AConfiguration_getScreenHeightDp", W<&A_AConfiguration_getScreenHeightDp>()},
        {"AConfiguration_getLanguage", W<&A_AConfiguration_getLanguage>()},
        {"AConfiguration_getCountry", W<&A_AConfiguration_getCountry>()},
        {"ALooper_forThread", W<&A_ALooper_forThread>()},
        {"ALooper_prepare", W<&A_ALooper_prepare>()},
        {"ALooper_acquire", W<&A_ALooper_acquire>()},
        {"ALooper_release", W<&A_ALooper_release>()},
        {"ALooper_wake", W<&A_ALooper_wake>()},
        {"ALooper_addFd", W<&A_ALooper_addFd>()},
        {"ALooper_removeFd", W<&A_ALooper_removeFd>()},
        {"ALooper_pollOnce", W<&A_ALooper_pollOnce>()},
        {"ALooper_pollAll", W<&A_ALooper_pollAll>()},
        {"AChoreographer_getInstance", W<&A_AChoreographer_getInstance>()},
        {"AChoreographer_postFrameCallback", W<&A_AChoreographer_postFrameCallback>()},
        {"AChoreographer_postFrameCallback64", W<&A_AChoreographer_postFrameCallback64>()},
        {"AChoreographer_postFrameCallbackDelayed", W<&A_AChoreographer_postFrameCallbackDelayed>()},
        {"AChoreographer_postFrameCallbackDelayed64", W<&A_AChoreographer_postFrameCallbackDelayed64>()},
        {"ASensorManager_getInstance", W<&A_ASensorManager_getInstance>()},
        {"ASensorManager_getInstanceForPackage", W<&A_ASensorManager_getInstanceForPackage>()},
        {"ASensorManager_getSensorList", W<&A_ASensorManager_getSensorList>()},
        {"ASensorManager_getDefaultSensor", W<&A_Null>()},
        {"AThermal_acquireManager", W<&A_Null>()},
        {"APerformanceHint_getManager", W<&A_Null>()},
    };
    m.insert(extra.begin(), extra.end());
    RegisterHostLibrary(kLibAndroid, "libandroid.so", m);
    RegisterHostLibrary(kLibNativeWindow, "libnativewindow.so", WindowThunks());
}

}  // namespace rn
