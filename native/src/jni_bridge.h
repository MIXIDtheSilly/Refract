#pragma once

#include <jni.h>

#include <string>

#include "kernel.h"

namespace rn {

void RegisterJniHle();
void JniAttachHostVm(JavaVM* vm, JNIEnv* env);
JNIEnv* HostJniEnv();
u64 GuestJavaVm();
u64 GuestJniEnv(GuestThread* t);
// Registers a guest function as the implementation of a Java native method.
bool BindGuestNative(JNIEnv* env, jclass cls, const char* name, const char* sig, u64 guest_fn);
// "(II)V"-style descriptor of a method ID (via JVMTI); false if unknown.
bool JavaMethodDescriptor(JNIEnv* env, jmethodID m, std::string* out);

}  // namespace rn
