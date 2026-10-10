// Guest-side launcher for an Android app under refract_native (the role of
// app_process/zygote). It hands the host its libdl entry points, starts the
// thread-adoption service, then lets the host run the app's Java side (JVM on
// the host) on this thread.
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

struct RefractGuestHelpers {
    void* dlopen;
    void* dlsym;
    void* dlerror;
    void* dlclose;
    void* malloc;
    void* free;
};

// librefract_host.so (host functions)
void refract_host_init(const struct RefractGuestHelpers* helpers, int argc, char** argv);
int refract_host_run(void);
long refract_host_service_wait(void);
void refract_host_adopt_ready(long token);

// Runs on a host thread that adopted this bionic thread. It parks inside
// refract_host_adopt_ready; the host calls guest functions from that state.
static void* adopted_entry(void* token) {
    refract_host_adopt_ready((long)token);
    return NULL;
}

static void* service_loop(void* arg) {
    (void)arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 4 << 20);
    for (;;) {
        long token = refract_host_service_wait();
        pthread_t th;
        pthread_create(&th, &attr, adopted_entry, (void*)token);
    }
    return NULL;
}

// Keep libjni's stubs (the JNIEnv tables point into it) loaded.
extern void refract_jni_GetVersion(void);
void* volatile refract_keep_jni = (void*)&refract_jni_GetVersion;

int main(int argc, char** argv) {
    static const struct RefractGuestHelpers helpers = {
        (void*)&dlopen, (void*)&dlsym, (void*)&dlerror, (void*)&dlclose, (void*)&malloc, (void*)&free,
    };
    refract_host_init(&helpers, argc, argv);
    pthread_t service;
    pthread_create(&service, NULL, service_loop, NULL);
    pthread_setname_np(service, "refract-adopt");
    return refract_host_run();
}
