// Native half of com.oculus.systemdriver.DriverLoader: forwards Meta's loader to libopenxr_runtime.so.
#include <dlfcn.h>
#include <jni.h>
#include <android/log.h>
#include <stdint.h>

JNIEXPORT void JNICALL
Java_com_oculus_systemdriver_DriverLoader_setContext(JNIEnv *env, jclass cls,
                                                      jobject context) {
  (void)cls;
  JavaVM *vm = 0;
  if ((*env)->GetJavaVM(env, &vm) != JNI_OK || !vm) return;
  void *runtime = dlopen("libopenxr_runtime.so", RTLD_NOW | RTLD_LOCAL);
  typedef void (*SetContext)(JavaVM *, jobject);
  SetContext set_context = runtime ? (SetContext)dlsym(runtime, "refract_set_android_context") : 0;
  if (set_context) set_context(vm, context);
  __android_log_print(ANDROID_LOG_INFO, "RefractDriverLoader", "context bridge: %s",
                      set_context ? "ready" : "unavailable");
}

JNIEXPORT jlong JNICALL
Java_com_oculus_systemdriver_DriverLoader_getProcAddr(JNIEnv *env, jclass cls,
                                                       jstring name) {
  (void)cls;
  const char *symbol = (*env)->GetStringUTFChars(env, name, 0);
  if (!symbol) return 0;
  void *runtime = dlopen("libopenxr_runtime.so", RTLD_NOW | RTLD_LOCAL);
  void *address = runtime ? dlsym(runtime, symbol) : 0;
  __android_log_print(ANDROID_LOG_INFO, "RefractDriverLoader", "%s -> %p (%s)",
                      symbol, address, runtime ? "loaded" : dlerror());
  (*env)->ReleaseStringUTFChars(env, name, symbol);
  return (jlong)(uintptr_t)address;
}
