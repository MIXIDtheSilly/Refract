// libandroid.so / libnativewindow.so for the guest (host implementations).
#pragma once

#include <jni.h>

#include "common.h"

namespace rn {

void RegisterAndroidHle();
// Registers refract.view.NativeWindows natives (after the JVM starts).
bool RegisterAndroidNatives(JNIEnv* env);
// HWND behind a guest ANativeWindow*, or nullptr.
void* NativeWindowHwnd(const void* window);

}  // namespace rn
