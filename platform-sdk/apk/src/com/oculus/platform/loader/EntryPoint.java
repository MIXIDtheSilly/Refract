package com.oculus.platform.loader;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.provider.Settings;

/**
 * The class Meta's libovrplatformloader.so loads from the Horizon package. It calls load64()
 * and uses the returned address as its entry-point resolver; Refract answers with its own native
 * Platform SDK implementation (librefract_ovrplatform.so, arm64 like the games that load it).
 */
public final class EntryPoint {
    private EntryPoint() {}

    public static long load64(Context app, Context platform, int major, int minor, int patch, int unused1, int unused2) {
        // Loaded through this class's loader, i.e. from this package's native library directory.
        System.loadLibrary("refract_ovrplatform");
        String pkg = app.getPackageName();
        long versionCode = 0;
        String versionName = "";
        try {
            PackageInfo info = app.getPackageManager().getPackageInfo(pkg, 0);
            versionCode = info.getLongVersionCode();
            if (info.versionName != null) versionName = info.versionName;
        } catch (Exception ignored) {
        }
        String deviceId = Settings.Secure.getString(app.getContentResolver(), Settings.Secure.ANDROID_ID);
        return nativeLoad(pkg, versionCode, versionName, deviceId, major, minor, patch);
    }

    private static native long nativeLoad(String pkg, long versionCode, String versionName, String deviceId,
                                          int major, int minor, int patch);
}
