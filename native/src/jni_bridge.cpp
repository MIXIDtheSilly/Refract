#include <algorithm>
#include <map>
#include <mutex>
// JNI between guest (ARM64) native code and the host JVM (HotSpot).
//
// Guest -> JVM: the guest sees a JNIEnv/JavaVM whose function tables point at
// stubs in librefract_jni.so; each stub lands in a host handler here that calls
// the real HotSpot JNIEnv for the current host thread. JNI handles (jobject,
// jmethodID, ...) are host values the guest only passes around. Varargs and
// va_list forms are decoded with the AAPCS64 rules using the method signature
// recorded when the guest looked the method up.
//
// JVM -> guest: RegisterNatives (and Java_* exports, bound by the Java side) get
// a small x64 stub per method that enters RefractNativeEntry (native_entry.asm),
// which calls NativeDispatch to run the guest function with AAPCS64 arguments.
#include <windows.h>

#include <jni.h>

#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "host_runtime.h"
#include "jni_bridge.h"
#include "linux_abi.h"
#include "memory.h"
#include "thunks.h"
#include "vfs.h"

namespace rn {

namespace {

#include "gen/jni_tables.inc"

JavaVM* g_vm = nullptr;
thread_local JNIEnv* tls_env = nullptr;

// Guest-memory structures.
std::mutex g_guest_mu;
u64 g_guest_pool = 0, g_guest_pool_used = 0, g_guest_pool_size = 0;
u64 g_guest_jni_table = 0;
u64 g_guest_jvm_table = 0;
u64 g_guest_vm = 0;

u64 GuestAlloc(u64 n) {
    std::lock_guard lock(g_guest_mu);
    n = (n + 15) & ~15ull;
    if (g_guest_pool_used + n > g_guest_pool_size) {
        const u64 size = 1 << 20;
        s64 p = Mem().Map(0, size, lx::PROT_READ_ | lx::PROT_WRITE_, lx::MAP_PRIVATE_ | lx::MAP_ANONYMOUS_, nullptr, 0,
                          "[refract:jni]");
        if (p < 0)
            Fatal("cannot map JNI structures");
        g_guest_pool = static_cast<u64>(p);
        g_guest_pool_used = 0;
        g_guest_pool_size = size;
    }
    u64 r = g_guest_pool + g_guest_pool_used;
    g_guest_pool_used += n;
    return r;
}

bool EnsureGuestTables() {
    if (g_guest_vm)
        return true;
    const size_t n_jni = sizeof(kJniTable) / sizeof(kJniTable[0]);
    const size_t n_jvm = sizeof(kJvmTable) / sizeof(kJvmTable[0]);
    std::vector<u64> jni(n_jni), jvm(n_jvm);
    for (size_t i = 0; i < n_jni; ++i) {
        if (!kJniTable[i])
            continue;
        jni[i] = StubAddress(kLibJni, kJniTable[i]);
        if (!jni[i]) {
            Log("librefract_jni.so is not loaded (missing %s)", kJniTable[i]);
            return false;
        }
    }
    for (size_t i = 0; i < n_jvm; ++i)
        if (kJvmTable[i])
            jvm[i] = StubAddress(kLibJni, kJvmTable[i]);
    u64 jni_table = GuestAlloc(n_jni * 8);
    u64 jvm_table = GuestAlloc(n_jvm * 8);
    SafeCopyToGuest(jni_table, jni.data(), n_jni * 8);
    SafeCopyToGuest(jvm_table, jvm.data(), n_jvm * 8);
    u64 vm = GuestAlloc(16);
    SafeCopyToGuest(vm, &jvm_table, 8);
    g_guest_jni_table = jni_table;
    g_guest_jvm_table = jvm_table;
    g_guest_vm = vm;
    return true;
}

JNIEnv* Env() {
    if (!tls_env && g_vm) {
        if (g_vm->GetEnv(reinterpret_cast<void**>(&tls_env), JNI_VERSION_1_6) != JNI_OK) {
            g_vm->AttachCurrentThread(reinterpret_cast<void**>(&tls_env), nullptr);
            RN_INFO("auto-attached thread %lu to the JVM", GetCurrentThreadId());
        }
    }
    return tls_env;
}

// --- method signatures ------------------------------------------------------------

struct ParsedSig {
    std::string params;  // one char per parameter: ZBCSIJFDL
    char ret = 'V';
};

ParsedSig ParseSig(const char* desc) {
    ParsedSig s;
    const char* p = desc;
    if (*p == '(')
        ++p;
    while (*p && *p != ')') {
        char c = *p;
        if (c == '[') {
            while (*p == '[')
                ++p;
            if (*p == 'L')
                while (*p && *p != ';')
                    ++p;
            ++p;
            s.params += 'L';
            continue;
        }
        if (c == 'L') {
            while (*p && *p != ';')
                ++p;
            ++p;
            s.params += 'L';
            continue;
        }
        s.params += c;
        ++p;
    }
    if (*p == ')')
        ++p;
    s.ret = (*p == '[') ? 'L' : *p;
    return s;
}

std::shared_mutex g_sig_mu;
std::unordered_map<jmethodID, ParsedSig> g_sigs;

void NoteSig(jmethodID m, const char* desc) {
    if (!m || !desc)
        return;
    ParsedSig s = ParseSig(desc);
    std::unique_lock lock(g_sig_mu);
    g_sigs[m] = std::move(s);
}

// Signature of a method ID the guest got some other way (FromReflectedMethod).
bool SigFor(jmethodID m, ParsedSig* out) {
    {
        std::shared_lock lock(g_sig_mu);
        auto it = g_sigs.find(m);
        if (it != g_sigs.end()) {
            *out = it->second;
            return true;
        }
    }
    std::string desc;
    if (JavaMethodDescriptor(Env(), m, &desc)) {
        NoteSig(m, desc.c_str());
        *out = ParseSig(desc.c_str());
        return true;
    }
    Log("JNI: unknown method ID %p (no signature)", static_cast<void*>(m));
    return false;
}

// --- argument decoding ---------------------------------------------------------------

struct GuestVaList {
    u64 stack;
    u64 gr_top;
    u64 vr_top;
    s32 gr_offs;
    s32 vr_offs;
};

u64 VaInt(GuestVaList& va) {
    u64 v = 0;
    if (va.gr_offs < 0) {
        SafeCopyFromGuest(&v, va.gr_top + static_cast<s64>(va.gr_offs), 8);
        va.gr_offs += 8;
        return v;
    }
    SafeCopyFromGuest(&v, va.stack, 8);
    va.stack += 8;
    return v;
}

double VaDouble(GuestVaList& va) {
    double d = 0;
    if (va.vr_offs < 0) {
        SafeCopyFromGuest(&d, va.vr_top + static_cast<s64>(va.vr_offs), 8);
        va.vr_offs += 16;
        return d;
    }
    SafeCopyFromGuest(&d, va.stack, 8);
    va.stack += 8;
    return d;
}

template <typename IntFn, typename DblFn>
void DecodeArgs(const ParsedSig& sig, jvalue* out, IntFn next_int, DblFn next_double) {
    for (size_t i = 0; i < sig.params.size(); ++i) {
        jvalue& v = out[i];
        switch (sig.params[i]) {
        case 'Z': v.z = static_cast<jboolean>(next_int()); break;
        case 'B': v.b = static_cast<jbyte>(next_int()); break;
        case 'C': v.c = static_cast<jchar>(next_int()); break;
        case 'S': v.s = static_cast<jshort>(next_int()); break;
        case 'I': v.i = static_cast<jint>(next_int()); break;
        case 'J': v.j = static_cast<jlong>(next_int()); break;
        case 'F': v.f = static_cast<jfloat>(next_double()); break;  // promoted to double
        case 'D': v.d = next_double(); break;
        default: v.l = reinterpret_cast<jobject>(next_int()); break;
        }
    }
}

jmethodID g_find_library_mid = nullptr;  // ClassLoader.findLibrary: the JVM's loader knows nothing of the app's .so files
jmethodID g_load_class_mid = nullptr;    // ClassLoader.loadClass: ART accepts "a/b/C" names, HotSpot only "a.b.C"
std::mutex g_mid_names_mu;
std::map<jmethodID, std::string> g_mid_names;
void NoteName(jmethodID m, const char* name, const char* sig) {
    if (m && !strcmp(name, "findLibrary"))
        g_find_library_mid = m;
    if (m && !strcmp(name, "loadClass") && !strcmp(sig, "(Ljava/lang/String;)Ljava/lang/Class;"))
        g_load_class_mid = m;
    // Native code quitting the app explains an otherwise silent exit.
    for (const char* q : {"exit", "halt", "finish", "finishAffinity", "killProcess", "quit", "finishAndRemoveTask"})
        if (!strcmp(name, q)) {
            GuestThread* t = CurrentThread();
            Log("JNI lookup of %s%s by guest thread %d at %s", name, sig, t ? t->tid : 0,
                t ? DescribeAddress(t->cpu->X(30)).c_str() : "?");
        }
    if (g_verbose < 3 || !m)
        return;
    std::lock_guard lk(g_mid_names_mu);
    g_mid_names[m] = std::string(name) + sig;
}
std::string NameOf(jmethodID m) {
    std::lock_guard lk(g_mid_names_mu);
    auto it = g_mid_names.find(m);
    return it == g_mid_names.end() ? "?" : it->second;
}

enum CallKind { kVirtual, kNonvirtual, kStatic };
enum CallForm { kVarargs, kVaList, kArray };

void LogPendingException(const char* where);

template <CallKind K, CallForm F, char R>
void JniCallInner(GuestThread* t) {
    ArgReader rd(t);
    rd.NextInt();  // guest JNIEnv*
    jobject obj = nullptr;
    jclass cls = nullptr;
    if constexpr (K == kVirtual) {
        obj = reinterpret_cast<jobject>(rd.NextInt());
    } else if constexpr (K == kNonvirtual) {
        obj = reinterpret_cast<jobject>(rd.NextInt());
        cls = reinterpret_cast<jclass>(rd.NextInt());
    } else {
        cls = reinterpret_cast<jclass>(rd.NextInt());
    }
    jmethodID mid = reinterpret_cast<jmethodID>(rd.NextInt());
    if (g_verbose >= 3)
        Log("JNI call %s tid %d", NameOf(mid).c_str(), t->tid);
    jvalue buf[256];
    const jvalue* args = buf;
    if constexpr (F == kArray) {
        args = reinterpret_cast<const jvalue*>(rd.NextInt());
    } else {
        ParsedSig sig;
        SigFor(mid, &sig);
        if (sig.params.size() > 256)
            Fatal("JNI call with %zu arguments", sig.params.size());
        if constexpr (F == kVaList) {
            GuestVaList va{};
            SafeCopyFromGuest(&va, rd.NextInt(), sizeof(va));
            DecodeArgs(sig, buf, [&] { return VaInt(va); }, [&] { return VaDouble(va); });
        } else {
            DecodeArgs(sig, buf, [&] { return rd.NextInt(); }, [&] { return rd.NextDouble(); });
        }
    }
    JNIEnv* e = Env();
#define RN_CALL(Type)                                                     \
    (K == kVirtual      ? e->Call##Type##MethodA(obj, mid, args)          \
     : K == kNonvirtual ? e->CallNonvirtual##Type##MethodA(obj, cls, mid, args) \
                        : e->CallStatic##Type##MethodA(cls, mid, args))
    if constexpr (R == 'V') {
        if (K == kVirtual)
            e->CallVoidMethodA(obj, mid, args);
        else if (K == kNonvirtual)
            e->CallNonvirtualVoidMethodA(obj, cls, mid, args);
        else
            e->CallStaticVoidMethodA(cls, mid, args);
    } else if constexpr (R == 'L') {
        if (K == kVirtual && mid == g_find_library_mid && g_find_library_mid) {
            const char* lib = args[0].l ? e->GetStringUTFChars(static_cast<jstring>(args[0].l), nullptr) : nullptr;
            std::string path = lib ? App().lib_guest + "/lib" + lib + ".so" : "";
            if (lib)
                e->ReleaseStringUTFChars(static_cast<jstring>(args[0].l), lib);
            Vfs::Node node;
            SetReturn(t, !path.empty() && Fs().Resolve(path, true, &node) == 0 ? e->NewStringUTF(path.c_str())
                                                                               : nullptr);
            return;
        }
        if (K == kVirtual && mid == g_load_class_mid && g_load_class_mid && args[0].l) {
            const char* n = e->GetStringUTFChars(static_cast<jstring>(args[0].l), nullptr);
            std::string dotted = n ? n : "";
            if (n)
                e->ReleaseStringUTFChars(static_cast<jstring>(args[0].l), n);
            if (dotted.find('/') != std::string::npos) {
                std::replace(dotted.begin(), dotted.end(), '/', '.');
                jvalue a{};
                a.l = e->NewStringUTF(dotted.c_str());
                SetReturn(t, e->CallObjectMethodA(obj, mid, &a));
                e->DeleteLocalRef(a.l);
                return;
            }
        }
        SetReturn(t, RN_CALL(Object));
    } else if constexpr (R == 'Z') {
        SetReturn(t, RN_CALL(Boolean));
    } else if constexpr (R == 'B') {
        SetReturn(t, RN_CALL(Byte));
    } else if constexpr (R == 'C') {
        SetReturn(t, RN_CALL(Char));
    } else if constexpr (R == 'S') {
        SetReturn(t, RN_CALL(Short));
    } else if constexpr (R == 'I') {
        SetReturn(t, RN_CALL(Int));
    } else if constexpr (R == 'J') {
        SetReturn(t, RN_CALL(Long));
    } else if constexpr (R == 'F') {
        SetReturn(t, RN_CALL(Float));
    } else if constexpr (R == 'D') {
        SetReturn(t, RN_CALL(Double));
    }
#undef RN_CALL
}

// Java exceptions escaping a call from guest code usually explain what the guest does next
// (C++ wrappers such as the OpenXR loader's turn them into C++ exceptions).
template <CallKind K, CallForm F, char R>
void JniCall(GuestThread* t) {
    JniCallInner<K, F, R>(t);
    if (g_verbose >= 1 && Env()->ExceptionCheck())
        LogPendingException("Java exception in a guest JNI call");
}

template <CallForm F>
void JniNewObject(GuestThread* t) {
    ArgReader rd(t);
    rd.NextInt();
    jclass cls = reinterpret_cast<jclass>(rd.NextInt());
    jmethodID mid = reinterpret_cast<jmethodID>(rd.NextInt());
    jvalue buf[256];
    const jvalue* args = buf;
    if constexpr (F == kArray) {
        args = reinterpret_cast<const jvalue*>(rd.NextInt());
    } else {
        ParsedSig sig;
        SigFor(mid, &sig);
        if constexpr (F == kVaList) {
            GuestVaList va{};
            SafeCopyFromGuest(&va, rd.NextInt(), sizeof(va));
            DecodeArgs(sig, buf, [&] { return VaInt(va); }, [&] { return VaDouble(va); });
        } else {
            DecodeArgs(sig, buf, [&] { return rd.NextInt(); }, [&] { return rd.NextDouble(); });
        }
    }
    SetReturn(t, Env()->NewObjectA(cls, mid, args));
}

// --- plain JNI functions (first argument is the guest JNIEnv*, ignored) ---------

#define E Env()

jint J_GetVersion(u64) { return JNI_VERSION_1_6; }
jclass J_DefineClass(u64, const char* name, jobject, const jbyte*, jsize) {
    Log("JNI DefineClass(%s) is not supported", name ? name : "?");
    return nullptr;
}
jclass J_FindClass(u64, const char* name) {
    jclass c = E->FindClass(name);
    if (!c)
        RN_INFO("JNI FindClass(%s) failed", name);
    return c;
}
jmethodID J_FromReflectedMethod(u64, jobject m) { return E->FromReflectedMethod(m); }
jfieldID J_FromReflectedField(u64, jobject f) { return E->FromReflectedField(f); }
jobject J_ToReflectedMethod(u64, jclass c, jmethodID m, jboolean s) { return E->ToReflectedMethod(c, m, s); }
jclass J_GetSuperclass(u64, jclass c) { return E->GetSuperclass(c); }
jboolean J_IsAssignableFrom(u64, jclass a, jclass b) { return E->IsAssignableFrom(a, b); }
jobject J_ToReflectedField(u64, jclass c, jfieldID f, jboolean s) { return E->ToReflectedField(c, f, s); }
// Logs exceptions guest code throws (they usually explain a failed library load).
void LogThrowable(jthrowable t, const char* what = "guest JNI Throw") {
    if (g_verbose < 1 || !t)
        return;
    jclass sw = E->FindClass("java/io/StringWriter"), pw = E->FindClass("java/io/PrintWriter");
    jobject w = E->NewObject(sw, E->GetMethodID(sw, "<init>", "()V"));
    jobject p = E->NewObject(pw, E->GetMethodID(pw, "<init>", "(Ljava/io/Writer;)V"), w);
    E->CallVoidMethod(t, E->GetMethodID(E->FindClass("java/lang/Throwable"), "printStackTrace", "(Ljava/io/PrintWriter;)V"), p);
    auto s = static_cast<jstring>(E->CallObjectMethod(w, E->GetMethodID(sw, "toString", "()Ljava/lang/String;")));
    if (E->ExceptionCheck())
        E->ExceptionClear();
    if (s) {
        const char* c = E->GetStringUTFChars(s, nullptr);
        Log("%s: %s", what, c);
        E->ReleaseStringUTFChars(s, c);
    }
}
void LogPendingException(const char* where) {
    jthrowable ex = E->ExceptionOccurred();
    E->ExceptionClear();
    LogThrowable(ex, where);
    E->Throw(ex);
    E->DeleteLocalRef(ex);
}
jint J_Throw(u64, jthrowable t) {
    LogThrowable(t);
    return E->Throw(t);
}
jint J_ThrowNew(u64, jclass c, const char* msg) {
    if (g_verbose >= 1)
        Log("guest JNI ThrowNew: %s", msg ? msg : "");
    return E->ThrowNew(c, msg);
}
jthrowable J_ExceptionOccurred(u64) { return E->ExceptionOccurred(); }
void J_ExceptionDescribe(u64) { E->ExceptionDescribe(); }
void J_ExceptionClear(u64) { E->ExceptionClear(); }
void J_FatalError(u64, const char* msg) { Fatal("JNI FatalError from guest: %s", msg ? msg : "?"); }
jint J_PushLocalFrame(u64, jint n) { return E->PushLocalFrame(n); }
jobject J_PopLocalFrame(u64, jobject r) { return E->PopLocalFrame(r); }
jobject J_NewGlobalRef(u64, jobject o) { return E->NewGlobalRef(o); }
void J_DeleteGlobalRef(u64, jobject o) { E->DeleteGlobalRef(o); }
void J_DeleteLocalRef(u64, jobject o) { E->DeleteLocalRef(o); }
jboolean J_IsSameObject(u64, jobject a, jobject b) { return E->IsSameObject(a, b); }
jobject J_NewLocalRef(u64, jobject o) { return E->NewLocalRef(o); }
jint J_EnsureLocalCapacity(u64, jint n) { return E->EnsureLocalCapacity(n); }
jobject J_AllocObject(u64, jclass c) { return E->AllocObject(c); }
jclass J_GetObjectClass(u64, jobject o) { return E->GetObjectClass(o); }
jboolean J_IsInstanceOf(u64, jobject o, jclass c) {
    if (!c) {  // HotSpot crashes on a null class (ART aborts with a JNI error)
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("JNI: IsInstanceOf with a null class; returning false");
        }
        return JNI_FALSE;
    }
    return E->IsInstanceOf(o, c);
}
jmethodID J_GetMethodID(u64, jclass c, const char* name, const char* sig) {
    jmethodID m = E->GetMethodID(c, name, sig);
    NoteName(m, name, sig);
    if (m)
        NoteSig(m, sig);
    else
        RN_INFO("JNI GetMethodID(%s%s) failed", name, sig);
    return m;
}
// System.load/loadLibrary looked up from native code (Unity loads plugins this way) must
// reach refract.Runtime, which knows guest paths; the JDK's would reject "/data/app/...".
jmethodID RuntimeLoadMethod(jclass c, const char* name, const char* sig) {
    if (strcmp(sig, "(Ljava/lang/String;)V") || (strcmp(name, "load") && strcmp(name, "loadLibrary")))
        return nullptr;
    static jclass system = static_cast<jclass>(E->NewGlobalRef(E->FindClass("java/lang/System")));
    if (!E->IsSameObject(c, system))
        return nullptr;
    static jclass runtime = static_cast<jclass>(E->NewGlobalRef(E->FindClass("refract/Runtime")));
    return E->GetStaticMethodID(runtime, name, sig);
}

jmethodID J_GetStaticMethodID(u64, jclass c, const char* name, const char* sig) {
    jmethodID m = RuntimeLoadMethod(c, name, sig);
    if (!m)
        m = E->GetStaticMethodID(c, name, sig);
    NoteName(m, name, sig);
    if (m)
        NoteSig(m, sig);
    else
        RN_INFO("JNI GetStaticMethodID(%s%s) failed", name, sig);
    return m;
}
jfieldID J_GetFieldID(u64, jclass c, const char* name, const char* sig) {
    jfieldID f = E->GetFieldID(c, name, sig);
    if (!f)
        RN_INFO("JNI GetFieldID(%s %s) failed", name, sig);
    return f;
}
jfieldID J_GetStaticFieldID(u64, jclass c, const char* name, const char* sig) {
    jfieldID f = E->GetStaticFieldID(c, name, sig);
    if (!f)
        RN_INFO("JNI GetStaticFieldID(%s %s) failed", name, sig);
    return f;
}

#define RN_FIELD_TYPES(X) \
    X(Object, jobject)    \
    X(Boolean, jboolean)  \
    X(Byte, jbyte)        \
    X(Char, jchar)        \
    X(Short, jshort)      \
    X(Int, jint)          \
    X(Long, jlong)        \
    X(Float, jfloat)      \
    X(Double, jdouble)

#define RN_DEF_FIELD(Name, T)                                                                         \
    T J_Get##Name##Field(u64, jobject o, jfieldID f) { return E->Get##Name##Field(o, f); }            \
    void J_Set##Name##Field(u64, jobject o, jfieldID f, T v) { E->Set##Name##Field(o, f, v); }        \
    T J_GetStatic##Name##Field(u64, jclass c, jfieldID f) { return E->GetStatic##Name##Field(c, f); } \
    void J_SetStatic##Name##Field(u64, jclass c, jfieldID f, T v) { E->SetStatic##Name##Field(c, f, v); }
RN_FIELD_TYPES(RN_DEF_FIELD)
#undef RN_DEF_FIELD

jstring J_NewString(u64, const jchar* s, jsize n) { return E->NewString(s, n); }
jsize J_GetStringLength(u64, jstring s) { return E->GetStringLength(s); }
const jchar* J_GetStringChars(u64, jstring s, jboolean* c) { return E->GetStringChars(s, c); }
void J_ReleaseStringChars(u64, jstring s, const jchar* p) { E->ReleaseStringChars(s, p); }
jstring J_NewStringUTF(u64, const char* s) {
    if (g_verbose >= 3)
        Log("JNI NewStringUTF \"%s\"", s ? s : "(null)");
    return E->NewStringUTF(s);
}
jsize J_GetStringUTFLength(u64, jstring s) { return E->GetStringUTFLength(s); }
const char* J_GetStringUTFChars(u64, jstring s, jboolean* c) { return E->GetStringUTFChars(s, c); }
void J_ReleaseStringUTFChars(u64, jstring s, const char* p) { E->ReleaseStringUTFChars(s, p); }
jsize J_GetArrayLength(u64, jarray a) { return E->GetArrayLength(a); }
jobjectArray J_NewObjectArray(u64, jsize n, jclass c, jobject init) { return E->NewObjectArray(n, c, init); }
jobject J_GetObjectArrayElement(u64, jobjectArray a, jsize i) { return E->GetObjectArrayElement(a, i); }
void J_SetObjectArrayElement(u64, jobjectArray a, jsize i, jobject v) { E->SetObjectArrayElement(a, i, v); }

#define RN_PRIM_TYPES(X)          \
    X(Boolean, jboolean, jbooleanArray) \
    X(Byte, jbyte, jbyteArray)    \
    X(Char, jchar, jcharArray)    \
    X(Short, jshort, jshortArray) \
    X(Int, jint, jintArray)       \
    X(Long, jlong, jlongArray)    \
    X(Float, jfloat, jfloatArray) \
    X(Double, jdouble, jdoubleArray)

#define RN_DEF_ARRAY(Name, T, A)                                                                                  \
    A J_New##Name##Array(u64, jsize n) { return E->New##Name##Array(n); }                                         \
    T* J_Get##Name##ArrayElements(u64, A a, jboolean* c) { return E->Get##Name##ArrayElements(a, c); }            \
    void J_Release##Name##ArrayElements(u64, A a, T* p, jint m) { E->Release##Name##ArrayElements(a, p, m); }     \
    void J_Get##Name##ArrayRegion(u64, A a, jsize s, jsize n, T* b) { E->Get##Name##ArrayRegion(a, s, n, b); }    \
    void J_Set##Name##ArrayRegion(u64, A a, jsize s, jsize n, const T* b) { E->Set##Name##ArrayRegion(a, s, n, b); }
RN_PRIM_TYPES(RN_DEF_ARRAY)
#undef RN_DEF_ARRAY

jint J_RegisterNatives(u64, jclass c, const void* methods, jint n) {
    struct GuestNative {
        u64 name, sig, fn;
    };
    jint result = JNI_OK;
    for (jint i = 0; i < n; ++i) {
        GuestNative m;
        if (!SafeCopyFromGuest(&m, reinterpret_cast<u64>(methods) + i * sizeof(GuestNative), sizeof(m)))
            return JNI_ERR;
        std::string name, sig;
        SafeReadString(m.name, name);
        SafeReadString(m.sig, sig);
        if (!BindGuestNative(E, c, name.c_str(), sig.c_str(), m.fn))
            result = JNI_ERR;
    }
    return result;
}
jint J_UnregisterNatives(u64, jclass c) { return E->UnregisterNatives(c); }
jint J_MonitorEnter(u64, jobject o) { return E->MonitorEnter(o); }
jint J_MonitorExit(u64, jobject o) { return E->MonitorExit(o); }
jint J_GetJavaVM(u64, u64 out) {
    if (!EnsureGuestTables())
        return JNI_ERR;
    return SafeCopyToGuest(out, &g_guest_vm, 8) ? JNI_OK : JNI_ERR;
}
void J_GetStringRegion(u64, jstring s, jsize a, jsize n, jchar* b) { E->GetStringRegion(s, a, n, b); }
void J_GetStringUTFRegion(u64, jstring s, jsize a, jsize n, char* b) { E->GetStringUTFRegion(s, a, n, b); }
void* J_GetPrimitiveArrayCritical(u64, jarray a, jboolean* c) { return E->GetPrimitiveArrayCritical(a, c); }
void J_ReleasePrimitiveArrayCritical(u64, jarray a, void* p, jint m) { E->ReleasePrimitiveArrayCritical(a, p, m); }
const jchar* J_GetStringCritical(u64, jstring s, jboolean* c) { return E->GetStringCritical(s, c); }
void J_ReleaseStringCritical(u64, jstring s, const jchar* p) { E->ReleaseStringCritical(s, p); }
jweak J_NewWeakGlobalRef(u64, jobject o) { return E->NewWeakGlobalRef(o); }
void J_DeleteWeakGlobalRef(u64, jweak o) { E->DeleteWeakGlobalRef(o); }
jboolean J_ExceptionCheck(u64) { return E->ExceptionCheck(); }
jobject J_NewDirectByteBuffer(u64, void* p, jlong n) { return E->NewDirectByteBuffer(p, n); }
void* J_GetDirectBufferAddress(u64, jobject b) { return E->GetDirectBufferAddress(b); }
jlong J_GetDirectBufferCapacity(u64, jobject b) { return E->GetDirectBufferCapacity(b); }
jint J_GetObjectRefType(u64, jobject o) { return static_cast<jint>(E->GetObjectRefType(o)); }

#undef E

// --- JavaVM (invoke interface) -----------------------------------------------------

jint V_DestroyJavaVM(u64) { return JNI_ERR; }

jint AttachImpl(u64 p_env, u64 args, bool daemon) {
    GuestThread* t = CurrentThread();
    if (!t || !EnsureGuestTables())
        return JNI_ERR;
    JNIEnv* e = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK) {
        JavaVMAttachArgs a{JNI_VERSION_1_6, nullptr, nullptr};
        std::string name = t->name.empty() ? Format("guest-%d", t->tid) : t->name;
        if (args) {
            struct GuestAttachArgs {
                s32 version;
                s32 pad;
                u64 name;
                u64 group;
            } ga{};
            SafeCopyFromGuest(&ga, args, sizeof(ga));
            if (ga.name)
                SafeReadString(ga.name, name, 64);
        }
        a.name = const_cast<char*>(name.c_str());
        jint r = daemon ? g_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&e), &a)
                        : g_vm->AttachCurrentThread(reinterpret_cast<void**>(&e), &a);
        if (r != JNI_OK)
            return r;
        RN_TRACE("guest thread %d attached to the JVM as %s", t->tid, name.c_str());
    }
    tls_env = e;
    u64 genv = GuestJniEnv(t);
    return SafeCopyToGuest(p_env, &genv, 8) ? JNI_OK : JNI_ERR;
}

jint V_AttachCurrentThread(u64, u64 p_env, u64 args) { return AttachImpl(p_env, args, false); }
jint V_AttachCurrentThreadAsDaemon(u64, u64 p_env, u64 args) { return AttachImpl(p_env, args, true); }
jint V_DetachCurrentThread(u64) {
    jint r = g_vm->DetachCurrentThread();
    tls_env = nullptr;
    return r;
}
jint V_GetEnv(u64, u64 p_env, jint version) {
    GuestThread* t = CurrentThread();
    JNIEnv* e = nullptr;
    if (!t || g_vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK) {
        u64 zero = 0;
        SafeCopyToGuest(p_env, &zero, 8);
        return JNI_EDETACHED;
    }
    tls_env = e;
    u64 genv = GuestJniEnv(t);
    SafeCopyToGuest(p_env, &genv, 8);
    return JNI_OK;
}

// --- native methods implemented in the guest ----------------------------------------

struct MethodInfo {
    u64 guest_fn;
    ParsedSig sig;
    std::string name;
};

std::mutex g_tramp_mu;
u8* g_tramp_pool = nullptr;
size_t g_tramp_used = 0;
std::unordered_map<std::string, void*> g_tramps;  // "fn:sig" -> stub

extern "C" void RefractNativeEntry();

void* MakeTrampoline(u64 guest_fn, const char* sig, const char* name) {
    std::lock_guard lock(g_tramp_mu);
    std::string key = Format("%llx:%s", guest_fn, sig);
    auto it = g_tramps.find(key);
    if (it != g_tramps.end())
        return it->second;
    if (!g_tramp_pool || g_tramp_used + 32 > 65536) {
        g_tramp_pool = static_cast<u8*>(VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        g_tramp_used = 0;
    }
    auto* mi = new MethodInfo{guest_fn, ParseSig(sig), name};
    u8* p = g_tramp_pool + g_tramp_used;
    g_tramp_used += 32;
    u64 mi_addr = reinterpret_cast<u64>(mi);
    u64 entry = reinterpret_cast<u64>(&RefractNativeEntry);
    p[0] = 0x49;
    p[1] = 0xBA;  // mov r10, imm64
    memcpy(p + 2, &mi_addr, 8);
    p[10] = 0x48;
    p[11] = 0xB8;  // mov rax, imm64
    memcpy(p + 12, &entry, 8);
    p[20] = 0xFF;
    p[21] = 0xE0;  // jmp rax
    FlushInstructionCache(GetCurrentProcess(), p, 32);
    g_tramps[key] = p;
    return p;
}

}  // namespace

extern "C" u64 NativeDispatch(void* info, u64* args, const u64* xmm) {
    auto* mi = static_cast<MethodInfo*>(info);
    GuestThread* t = EnsureGuestThread();
    tls_env = reinterpret_cast<JNIEnv*>(args[0]);
    GuestCallArgs ga;
    int nx = 0, nv = 0;
    ga.x[nx++] = GuestJniEnv(t);
    ga.x[nx++] = args[1];  // jobject this / jclass
    int pos = 2;
    for (char c : mi->sig.params) {
        const bool fp = c == 'F' || c == 'D';
        u64 raw = (fp && pos < 4) ? xmm[pos * 2] : args[pos];
        ++pos;
        switch (c) {
        case 'Z': raw &= 0xff; break;
        case 'B': raw = static_cast<u64>(static_cast<s64>(static_cast<int8_t>(raw))); break;
        case 'C': raw &= 0xffff; break;
        case 'S': raw = static_cast<u64>(static_cast<s64>(static_cast<int16_t>(raw))); break;
        case 'I': raw = static_cast<u64>(static_cast<s64>(static_cast<int32_t>(raw))); break;
        case 'F': raw &= 0xffffffffull; break;
        default: break;
        }
        if (fp) {
            if (nv < 8)
                ga.v[nv++] = {raw, 0};
            else
                ga.stack.push_back(raw);
        } else {
            if (nx < 8)
                ga.x[nx++] = raw;
            else
                ga.stack.push_back(raw);
        }
    }
    CpuContext r;
    CallGuestRegs(t, mi->guest_fn, ga, &r);
    switch (mi->sig.ret) {
    case 'V': return 0;
    case 'F': return r.v[0][0] & 0xffffffffull;
    case 'D': return r.v[0][0];
    case 'Z': return r.x[0] & 0xff;
    case 'B': return static_cast<u64>(static_cast<s64>(static_cast<int8_t>(r.x[0])));
    case 'C': return r.x[0] & 0xffff;
    case 'S': return static_cast<u64>(static_cast<s64>(static_cast<int16_t>(r.x[0])));
    case 'I': return static_cast<u64>(static_cast<s64>(static_cast<int32_t>(r.x[0])));
    default: return r.x[0];
    }
}

bool BindGuestNative(JNIEnv* env, jclass cls, const char* name, const char* sig, u64 guest_fn) {
    void* stub = MakeTrampoline(guest_fn, sig, name);
    JNINativeMethod m{const_cast<char*>(name), const_cast<char*>(sig), stub};
    if (env->RegisterNatives(cls, &m, 1) != JNI_OK) {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        Log("RegisterNatives(%s%s) failed", name, sig);
        return false;
    }
    RN_TRACE("bound native %s%s -> guest 0x%llx", name, sig, guest_fn);
    return true;
}

void JniAttachHostVm(JavaVM* vm, JNIEnv* env) {
    g_vm = vm;
    tls_env = env;
}

JNIEnv* HostJniEnv() { return Env(); }

u64 GuestJavaVm() {
    EnsureGuestTables();
    return g_guest_vm;
}

u64 GuestJniEnv(GuestThread* t) {
    if (!t->guest_env) {
        if (!EnsureGuestTables())
            return 0;
        u64 env = GuestAlloc(16);
        SafeCopyToGuest(env, &g_guest_jni_table, 8);
        t->guest_env = env;
    }
    return t->guest_env;
}

void RegisterJniHle() {
    std::map<std::string, ThunkFn> m;
#define RN_REG(name) m["refract_jni_" #name] = &Wrap<&J_##name>;
    RN_REG(GetVersion) RN_REG(DefineClass) RN_REG(FindClass) RN_REG(FromReflectedMethod)
    RN_REG(FromReflectedField) RN_REG(ToReflectedMethod) RN_REG(GetSuperclass) RN_REG(IsAssignableFrom)
    RN_REG(ToReflectedField) RN_REG(Throw) RN_REG(ThrowNew) RN_REG(ExceptionOccurred) RN_REG(ExceptionDescribe)
    RN_REG(ExceptionClear) RN_REG(FatalError) RN_REG(PushLocalFrame) RN_REG(PopLocalFrame) RN_REG(NewGlobalRef)
    RN_REG(DeleteGlobalRef) RN_REG(DeleteLocalRef) RN_REG(IsSameObject) RN_REG(NewLocalRef)
    RN_REG(EnsureLocalCapacity) RN_REG(AllocObject) RN_REG(GetObjectClass) RN_REG(IsInstanceOf)
    RN_REG(GetMethodID) RN_REG(GetStaticMethodID) RN_REG(GetFieldID) RN_REG(GetStaticFieldID)
    RN_REG(NewString) RN_REG(GetStringLength) RN_REG(GetStringChars) RN_REG(ReleaseStringChars)
    RN_REG(NewStringUTF) RN_REG(GetStringUTFLength) RN_REG(GetStringUTFChars) RN_REG(ReleaseStringUTFChars)
    RN_REG(GetArrayLength) RN_REG(NewObjectArray) RN_REG(GetObjectArrayElement) RN_REG(SetObjectArrayElement)
    RN_REG(RegisterNatives) RN_REG(UnregisterNatives) RN_REG(MonitorEnter) RN_REG(MonitorExit)
    RN_REG(GetJavaVM) RN_REG(GetStringRegion) RN_REG(GetStringUTFRegion) RN_REG(GetPrimitiveArrayCritical)
    RN_REG(ReleasePrimitiveArrayCritical) RN_REG(GetStringCritical) RN_REG(ReleaseStringCritical)
    RN_REG(NewWeakGlobalRef) RN_REG(DeleteWeakGlobalRef) RN_REG(ExceptionCheck) RN_REG(NewDirectByteBuffer)
    RN_REG(GetDirectBufferAddress) RN_REG(GetDirectBufferCapacity) RN_REG(GetObjectRefType)
#define RN_REG_FIELD(Name, T) \
    RN_REG(Get##Name##Field) RN_REG(Set##Name##Field) RN_REG(GetStatic##Name##Field) RN_REG(SetStatic##Name##Field)
    RN_FIELD_TYPES(RN_REG_FIELD)
#define RN_REG_ARRAY(Name, T, A)                                                                    \
    RN_REG(New##Name##Array) RN_REG(Get##Name##ArrayElements) RN_REG(Release##Name##ArrayElements) \
    RN_REG(Get##Name##ArrayRegion) RN_REG(Set##Name##ArrayRegion)
    RN_PRIM_TYPES(RN_REG_ARRAY)
#undef RN_REG
#define RN_REG_CALL(Type, R)                                                                           \
    m["refract_jni_Call" #Type "Method"] = &JniCall<kVirtual, kVarargs, R>;                           \
    m["refract_jni_Call" #Type "MethodV"] = &JniCall<kVirtual, kVaList, R>;                           \
    m["refract_jni_Call" #Type "MethodA"] = &JniCall<kVirtual, kArray, R>;                            \
    m["refract_jni_CallNonvirtual" #Type "Method"] = &JniCall<kNonvirtual, kVarargs, R>;              \
    m["refract_jni_CallNonvirtual" #Type "MethodV"] = &JniCall<kNonvirtual, kVaList, R>;              \
    m["refract_jni_CallNonvirtual" #Type "MethodA"] = &JniCall<kNonvirtual, kArray, R>;               \
    m["refract_jni_CallStatic" #Type "Method"] = &JniCall<kStatic, kVarargs, R>;                      \
    m["refract_jni_CallStatic" #Type "MethodV"] = &JniCall<kStatic, kVaList, R>;                      \
    m["refract_jni_CallStatic" #Type "MethodA"] = &JniCall<kStatic, kArray, R>;
    RN_REG_CALL(Object, 'L') RN_REG_CALL(Boolean, 'Z') RN_REG_CALL(Byte, 'B') RN_REG_CALL(Char, 'C')
    RN_REG_CALL(Short, 'S') RN_REG_CALL(Int, 'I') RN_REG_CALL(Long, 'J') RN_REG_CALL(Float, 'F')
    RN_REG_CALL(Double, 'D') RN_REG_CALL(Void, 'V')
#undef RN_REG_CALL
    m["refract_jni_NewObject"] = &JniNewObject<kVarargs>;
    m["refract_jni_NewObjectV"] = &JniNewObject<kVaList>;
    m["refract_jni_NewObjectA"] = &JniNewObject<kArray>;
    m["refract_jvm_DestroyJavaVM"] = &Wrap<&V_DestroyJavaVM>;
    m["refract_jvm_AttachCurrentThread"] = &Wrap<&V_AttachCurrentThread>;
    m["refract_jvm_DetachCurrentThread"] = &Wrap<&V_DetachCurrentThread>;
    m["refract_jvm_GetEnv"] = &Wrap<&V_GetEnv>;
    m["refract_jvm_AttachCurrentThreadAsDaemon"] = &Wrap<&V_AttachCurrentThreadAsDaemon>;
    RegisterHostLibrary(kLibJni, "librefract_jni.so", std::move(m));
}

}  // namespace rn
