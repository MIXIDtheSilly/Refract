package com.oculus.systemdriver;

import android.content.Context;
import android.util.Log;

import java.lang.reflect.Method;

// The entry point Meta's OpenXR loader calls in com.oculus.systemdriver: load the runtime, then look up
// xrNegotiateLoaderRuntimeInterface and friends through getProcAddr (refract_driver.c).
public final class DriverLoader {
    private static final String TAG = "RefractDriverLoader";

    private DriverLoader() {}

    public static void loadXrRuntime() {
        Log.i(TAG, "Loading Refract OpenXR runtime");
        System.loadLibrary("openxr_runtime");
        System.loadLibrary("refract_driver");
        // The runtime reaches its pose and image brokers through the game's application context.
        try {
            Class<?> activityThread = Class.forName("android.app.ActivityThread");
            Method currentApplication = activityThread.getDeclaredMethod("currentApplication");
            currentApplication.setAccessible(true);
            Context context = (Context) currentApplication.invoke(null);
            if (context != null) {
                setContext(context);
                Log.i(TAG, "Passed application context to Refract runtime");
            } else {
                Log.w(TAG, "Current application context is null");
            }
        } catch (Exception ex) {
            Log.w(TAG, "Cannot acquire application context", ex);
        }
    }

    private static native void setContext(Context context);
    public static native long getProcAddr(String name);
}
