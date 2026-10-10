// The Android app process: JVM hosting, thread adoption and the host side of the
// guest launcher (refract_app, see guest/refract_app.c).
//
// Startup: refract_app's main() registers its helper functions (dlopen, ...) and
// calls refract_host_run(), which creates the JVM on that same thread and runs
// refract.Launcher (the Activity lifecycle) there; it never returns while the app
// runs. Java threads that call guest native methods are "adopted": a guest service
// thread pthread_create()s a thread whose clone() is handed to the waiting host
// thread instead of starting a new one, so it gets a real bionic thread (TLS,
// stack) and parks in refract_host_adopt_ready() between calls.
#include "host_runtime.h"

#include <windows.h>

#include <jni.h>
#include <jvmti.h>

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

#include "android_hle.h"
#include "files.h"
#include "jni_bridge.h"
#include "linux_abi.h"
#include "thunks.h"
#include "vfs.h"

namespace rn {

namespace {

struct GuestHelpers {
    u64 dlopen;
    u64 dlsym;
    u64 dlerror;
    u64 dlclose;
    u64 malloc;
    u64 free;
};
GuestHelpers g_helpers{};
JavaVM* g_vm = nullptr;
jvmtiEnv* g_jvmti = nullptr;

// --- adoption ---------------------------------------------------------------------

struct AdoptRequest {
    u64 token = 0;
    GuestThread* child = nullptr;
    std::mutex mu;
    std::condition_variable cv;
};

std::mutex g_svc_mu;
std::condition_variable g_svc_cv;
std::deque<AdoptRequest*> g_svc_queue;
std::map<u64, AdoptRequest*> g_pending;
u64 g_next_token = 1;
bool g_service_running = false;

// --- librefract_host.so -------------------------------------------------------------

void H_init(u64 helpers, int argc, u64 argv) {
    SafeCopyFromGuest(&g_helpers, helpers, sizeof(g_helpers));
    RN_INFO("guest launcher ready (dlopen at 0x%llx)", g_helpers.dlopen);
}

u64 H_service_wait() {
    std::unique_lock lock(g_svc_mu);
    g_service_running = true;
    g_svc_cv.notify_all();
    g_svc_cv.wait(lock, [] { return !g_svc_queue.empty(); });
    AdoptRequest* r = g_svc_queue.front();
    g_svc_queue.pop_front();
    CurrentThread()->adopt_token = r->token;
    return r->token;
}

void H_adopt_ready(u64 token) {
    GuestThread* t = CurrentThread();
    t->return_to_host = true;  // park: return to the adopting host thread
}

void H_log(const char* msg) { Log("guest: %s", msg); }

int H_run();

// --- JVM natives for refract.Runtime --------------------------------------------------

std::string JStr(JNIEnv* env, jstring s) {
    if (!s)
        return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    env->ReleaseStringUTFChars(s, c);
    return r;
}

jlong JNICALL N_dlopen(JNIEnv* env, jclass, jstring path) {
    std::string p = JStr(env, path);
    GuestThread* t = EnsureGuestThread();
    u64 h = CallGuest(t, g_helpers.dlopen, {reinterpret_cast<u64>(p.c_str()), 2 /*RTLD_NOW*/});
    RN_INFO("dlopen(%s) = 0x%llx", p.c_str(), h);
    return static_cast<jlong>(h);
}

jlong JNICALL N_dlsym(JNIEnv* env, jclass, jlong handle, jstring name) {
    std::string n = JStr(env, name);
    return static_cast<jlong>(CallGuest(EnsureGuestThread(), g_helpers.dlsym, {static_cast<u64>(handle),
                                                                               reinterpret_cast<u64>(n.c_str())}));
}

jstring JNICALL N_dlerror(JNIEnv* env, jclass) {
    u64 p = CallGuest(EnsureGuestThread(), g_helpers.dlerror, {});
    std::string s;
    if (p)
        SafeReadString(p, s, 4096);
    return env->NewStringUTF(s.c_str());
}

jint JNICALL N_callJniOnLoad(JNIEnv* env, jclass, jlong fn) {
    GuestThread* t = EnsureGuestThread();
    u64 vm = GuestJavaVm();
    jint r = static_cast<jint>(CallGuest(t, static_cast<u64>(fn), {vm, 0}));
    RN_INFO("JNI_OnLoad -> 0x%x", r);
    return r;
}

// Names of exported Java_* functions in a guest ELF (resolved by the Java side).
jobjectArray JNICALL N_javaExports(JNIEnv* env, jclass, jstring path) {
    std::vector<std::string> names;
    FilePtr f;
    if (Fs().Open(lx::AT_FDCWD_, JStr(env, path), lx::O_RDONLY_, 0, &f) == 0) {
        struct Shdr {
            u32 name, type;
            u64 flags, addr, offset, size;
            u32 link, info;
            u64 align, entsize;
        };
        struct Sym {
            u32 name;
            u8 info, other;
            u16 shndx;
            u64 value, size;
        };
        u8 eh[64];
        if (f->Pread(eh, 64, 0) == 64) {
            u64 shoff;
            u16 shnum;
            memcpy(&shoff, eh + 40, 8);
            memcpy(&shnum, eh + 60, 2);
            std::vector<Shdr> sh(shnum);
            f->Pread(sh.data(), shnum * sizeof(Shdr), shoff);
            for (const auto& s : sh) {
                if (s.type != 11 /*SHT_DYNSYM*/ || s.link >= shnum)
                    continue;
                const Shdr& strs = sh[s.link];
                std::vector<char> str(strs.size + 1, 0);
                f->Pread(str.data(), strs.size, strs.offset);
                std::vector<Sym> syms(s.size / sizeof(Sym));
                f->Pread(syms.data(), syms.size() * sizeof(Sym), s.offset);
                for (const auto& y : syms) {
                    if (y.shndx == 0 || (y.info & 0xf) != 2 /*STT_FUNC*/ || y.name >= strs.size)
                        continue;
                    const char* n = str.data() + y.name;
                    if (!strncmp(n, "Java_", 5))
                        names.push_back(n);
                }
            }
        }
    }
    jobjectArray arr = env->NewObjectArray(static_cast<jsize>(names.size()), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < names.size(); ++i)
        env->SetObjectArrayElement(arr, static_cast<jsize>(i), env->NewStringUTF(names[i].c_str()));
    return arr;
}

jboolean JNICALL N_bind(JNIEnv* env, jclass, jclass cls, jstring name, jstring sig, jlong fn) {
    return BindGuestNative(env, cls, JStr(env, name).c_str(), JStr(env, sig).c_str(), static_cast<u64>(fn)) ? JNI_TRUE
                                                                                                          : JNI_FALSE;
}

void JNICALL N_log(JNIEnv* env, jclass, jint prio, jstring tag, jstring msg) {
    static const char kPrio[] = "??VDIWEFS";
    char p = prio >= 0 && prio < 9 ? kPrio[prio] : '?';
    Log("%c/%s(java): %s", p, JStr(env, tag).c_str(), JStr(env, msg).c_str());
}

jstring JNICALL N_hostPath(JNIEnv* env, jclass, jstring guest) {
    std::string g = JStr(env, guest);
    if (g.empty() || g[0] != '/')
        return guest;
    auto h = Fs().HostPath(g);
    if (!h) {
        // Not existing yet: map through the parent directory.
        std::string parent = g.substr(0, g.rfind('/'));
        auto hp = parent.empty() ? std::nullopt : Fs().HostPath(parent);
        if (!hp)
            return guest;
        h = *hp + L"\\" + Widen(g.substr(g.rfind('/') + 1));
    }
    return env->NewStringUTF(Narrow(*h).c_str());
}

jstring JNICALL N_guestPath(JNIEnv* env, jclass, jstring host) {
    std::string h = JStr(env, host);
    std::string g = Fs().GuestPathForHost(Widen(h));
    return env->NewStringUTF(g.empty() ? h.c_str() : g.c_str());
}

bool RegisterRuntimeNatives(JNIEnv* env) {
    jclass c = env->FindClass("refract/Runtime");
    if (!c) {
        env->ExceptionDescribe();
        return false;
    }
    JNINativeMethod m[] = {
        {const_cast<char*>("dlopen"), const_cast<char*>("(Ljava/lang/String;)J"), reinterpret_cast<void*>(&N_dlopen)},
        {const_cast<char*>("dlsym"), const_cast<char*>("(JLjava/lang/String;)J"), reinterpret_cast<void*>(&N_dlsym)},
        {const_cast<char*>("dlerror"), const_cast<char*>("()Ljava/lang/String;"), reinterpret_cast<void*>(&N_dlerror)},
        {const_cast<char*>("callJniOnLoad"), const_cast<char*>("(J)I"), reinterpret_cast<void*>(&N_callJniOnLoad)},
        {const_cast<char*>("javaExports"), const_cast<char*>("(Ljava/lang/String;)[Ljava/lang/String;"),
         reinterpret_cast<void*>(&N_javaExports)},
        {const_cast<char*>("bind"), const_cast<char*>("(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;J)Z"),
         reinterpret_cast<void*>(&N_bind)},
        {const_cast<char*>("log"), const_cast<char*>("(ILjava/lang/String;Ljava/lang/String;)V"),
         reinterpret_cast<void*>(&N_log)},
        {const_cast<char*>("hostPath"), const_cast<char*>("(Ljava/lang/String;)Ljava/lang/String;"),
         reinterpret_cast<void*>(&N_hostPath)},
        {const_cast<char*>("guestPath"), const_cast<char*>("(Ljava/lang/String;)Ljava/lang/String;"),
         reinterpret_cast<void*>(&N_guestPath)},
    };
    if (env->RegisterNatives(c, m, sizeof(m) / sizeof(m[0])) != JNI_OK) {
        env->ExceptionDescribe();
        return false;
    }
    return true;
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path().wstring();
}

std::wstring FindJdk() {
    wchar_t buf[MAX_PATH];
    if (GetEnvironmentVariableW(L"REFRACT_JAVA_HOME", buf, MAX_PATH))
        return buf;
    if (GetEnvironmentVariableW(L"JAVA_HOME", buf, MAX_PATH) &&
        std::filesystem::exists(std::filesystem::path(buf) / L"bin" / L"server" / L"jvm.dll"))
        return buf;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(L"C:\\Program Files\\Java", ec))
        if (std::filesystem::exists(e.path() / L"bin" / L"server" / L"jvm.dll"))
            return e.path().wstring();
    return {};
}

int H_run() {
    GuestThread* main_thread = CurrentThread();
    {
        // The service thread must be up before any adoption.
        std::unique_lock lock(g_svc_mu);
        g_svc_cv.wait_for(lock, std::chrono::seconds(5), [] { return g_service_running; });
    }
    const AppConfig& app = App();
    std::wstring jdk = FindJdk();
    if (jdk.empty())
        Fatal("no JDK found (install one or set REFRACT_JAVA_HOME)");
    SetDllDirectoryW((jdk + L"\\bin").c_str());
    HMODULE jvm = LoadLibraryW((jdk + L"\\bin\\server\\jvm.dll").c_str());
    if (!jvm)
        Fatal("cannot load jvm.dll from %s (%lu)", Narrow(jdk).c_str(), GetLastError());
    using CreateFn = jint(JNICALL*)(JavaVM**, void**, void*);
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(jvm, "JNI_CreateJavaVM"));

    const std::wstring shim = ExeDir() + L"\\java\\refract-android.jar";
    const std::wstring app_jar = app.app_dir + L"\\classes.jar";
    std::vector<std::string> opts = {
        "-Djava.class.path=" + Narrow(shim) + ";" + Narrow(app_jar),
        "-Dfile.encoding=UTF-8",
        "-Dstdout.encoding=UTF-8",
        "-Djava.awt.headless=true",
        "--enable-native-access=ALL-UNNAMED",
        "-XX:-CreateCoredumpOnCrash",
        "-Xss4m",
        "-Drefract.package=" + app.package,
        "-Drefract.activity=" + app.activity,
        "-Drefract.label=" + app.label,
        "-Drefract.apk=" + app.apk_guest,
        "-Drefract.nativeLibraryDir=" + app.lib_guest,
        "-Drefract.dataDir=" + app.data_guest,
        "-Drefract.externalDir=" + app.external_guest,
        "-Drefract.versionCode=" + std::to_string(app.version_code),
        "-Drefract.versionName=" + app.version_name,
        "-Drefract.application=" + app.application,
        "-Drefract.targetSdk=" + std::to_string(app.target_sdk),
        "-Drefract.appDir=" + Narrow(app.app_dir),
        "-Drefract.apkHost=" + Narrow(app.apk_host),
    };
    if (const char* extra = getenv("REFRACT_JVM_OPTS")) {
        std::string s = extra;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t sp = s.find(' ', pos);
            if (sp == std::string::npos)
                sp = s.size();
            if (sp > pos)
                opts.push_back(s.substr(pos, sp - pos));
            pos = sp + 1;
        }
    }
    std::vector<JavaVMOption> jopts;
    for (auto& o : opts)
        jopts.push_back({const_cast<char*>(o.c_str()), nullptr});
    JavaVMInitArgs args{};
    args.version = JNI_VERSION_10;
    args.nOptions = static_cast<jint>(jopts.size());
    args.options = jopts.data();
    args.ignoreUnrecognized = JNI_FALSE;
    JNIEnv* env = nullptr;
    jint r = create(&g_vm, reinterpret_cast<void**>(&env), &args);
    if (r != JNI_OK)
        Fatal("JNI_CreateJavaVM failed (%d)", r);
    g_vm->GetEnv(reinterpret_cast<void**>(&g_jvmti), JVMTI_VERSION_1_2);
    JniAttachHostVm(g_vm, env);
    RN_INFO("JVM started (%s)", Narrow(jdk).c_str());
    if (!RegisterRuntimeNatives(env))
        Fatal("refract.Runtime is missing from %s", Narrow(shim).c_str());
    if (!RegisterAndroidNatives(env))
        Fatal("refract.view.NativeWindows is missing from %s", Narrow(shim).c_str());

    jclass launcher = env->FindClass("refract/Launcher");
    jmethodID main = launcher ? env->GetStaticMethodID(launcher, "main", "([Ljava/lang/String;)V") : nullptr;
    if (!main) {
        env->ExceptionDescribe();
        Fatal("refract.Launcher.main not found");
    }
    jobjectArray jargs = env->NewObjectArray(0, env->FindClass("java/lang/String"), nullptr);
    env->CallStaticVoidMethod(launcher, main, jargs);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        return 1;
    }
    (void)main_thread;
    return 0;
}

}  // namespace

bool JavaMethodDescriptor(JNIEnv*, jmethodID m, std::string* out) {
    if (!g_jvmti)
        return false;
    char *name = nullptr, *sig = nullptr, *generic = nullptr;
    if (g_jvmti->GetMethodName(m, &name, &sig, &generic) != JVMTI_ERROR_NONE)
        return false;
    *out = sig ? sig : "";
    g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(name));
    g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(sig));
    if (generic)
        g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(generic));
    return true;
}

AppConfig& App() {
    static AppConfig a;
    return a;
}

bool LoadAppConfig(const std::wstring& app_dir) {
    std::ifstream in(std::filesystem::path(app_dir) / L"app.properties");
    if (!in)
        return false;
    AppConfig& a = App();
    a.app_dir = app_dir;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#')
            continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "package")
            a.package = v;
        else if (k == "activity")
            a.activity = v;
        else if (k == "label")
            a.label = v;
        else if (k == "apk")
            a.apk_host = Widen(v);
        else if (k == "application")
            a.application = v;
        else if (k == "targetSdk")
            a.target_sdk = atoi(v.c_str());
        else if (k == "versionCode")
            a.version_code = atoi(v.c_str());
        else if (k == "versionName")
            a.version_name = v;
    }
    if (a.apk_host.empty())
        a.apk_host = app_dir + L"\\base.apk";
    a.apk_guest = "/data/app/" + a.package + "/base.apk";
    a.lib_guest = "/data/app/" + a.package + "/lib/arm64";
    a.data_guest = "/data/user/0/" + a.package;
    a.external_guest = "/storage/emulated/0/Android/data/" + a.package;
    return !a.package.empty() && !a.activity.empty();
}

GuestThread* EnsureGuestThread() {
    if (GuestThread* t = CurrentThread())
        return t;
    AdoptRequest req;
    {
        std::lock_guard lock(g_svc_mu);
        req.token = g_next_token++;
        g_pending[req.token] = &req;
        g_svc_queue.push_back(&req);
    }
    g_svc_cv.notify_all();
    GuestThread* child;
    {
        std::unique_lock lock(req.mu);
        req.cv.wait(lock, [&] { return req.child != nullptr; });
        child = req.child;
    }
    child->host_tid = GetCurrentThreadId();
    child->name = Format("host-%lu", GetCurrentThreadId());
    SetCurrentThread(child);
    // Run the new bionic thread until it parks in refract_host_adopt_ready().
    extern bool RunGuestUntilReturn(GuestThread * t);
    RunGuestUntilReturn(child);
    SetCurrentThread(child);
    RN_TRACE("adopted host thread %lu as guest thread %d", GetCurrentThreadId(), child->tid);
    return child;
}

bool CompleteAdoption(u64 token, GuestThread* child) {
    AdoptRequest* req = nullptr;
    {
        std::lock_guard lock(g_svc_mu);
        auto it = g_pending.find(token);
        if (it == g_pending.end())
            return false;
        req = it->second;
        g_pending.erase(it);
    }
    {
        std::lock_guard lock(req->mu);
        req->child = child;
    }
    req->cv.notify_all();
    return true;
}

void RegisterHostRuntime() {
    RegisterHostLibrary(kLibRefract, "librefract_host.so",
                        {
                            {"refract_host_init", &Wrap<&H_init>},
                            {"refract_host_run", &Wrap<&H_run>},
                            {"refract_host_service_wait", &Wrap<&H_service_wait>},
                            {"refract_host_adopt_ready", &Wrap<&H_adopt_ready>},
                            {"refract_host_log", &Wrap<&H_log>},
                        });
}

}  // namespace rn
