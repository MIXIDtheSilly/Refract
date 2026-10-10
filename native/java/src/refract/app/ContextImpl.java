package refract.app;

import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.Resources;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.Executor;
import refract.Runtime;

/** The process's Context implementation (ActivityThread's ContextImpl on Android). */
public class ContextImpl extends Context {
    private static ApplicationInfo appInfo;
    private static final Map<String, SharedPreferencesImpl> prefs = new HashMap<>();
    private static final Map<String, Object> services = new HashMap<>();
    private static Context applicationContext;

    public static void setApplicationContext(Context c) { applicationContext = c; }

    public static synchronized ApplicationInfo applicationInfo() {
        if (appInfo == null) {
            ApplicationInfo ai = new ApplicationInfo();
            ai.packageName = Runtime.PACKAGE;
            ai.processName = Runtime.PACKAGE;
            ai.name = System.getProperty("refract.application", null);
            ai.className = ai.name;
            ai.sourceDir = Runtime.APK;
            ai.publicSourceDir = Runtime.APK;
            ai.nativeLibraryDir = Runtime.NATIVE_LIBRARY_DIR;
            ai.dataDir = Runtime.DATA_DIR;
            ai.uid = 10100;
            ai.enabled = true;
            ai.targetSdkVersion = Integer.getInteger("refract.targetSdk", 32);
            ai.minSdkVersion = 29;
            ai.flags = ApplicationInfo.FLAG_HAS_CODE | ApplicationInfo.FLAG_INSTALLED;
            ai.metaData = Manifest.metaData();
            appInfo = ai;
        }
        return appInfo;
    }

    static File dir(String guestPath) {
        File hostDir = new File(Runtime.hostPath(guestPath));
        if (!hostDir.exists()) hostDir.mkdirs();
        return new GuestFile(guestPath);
    }

    @Override public String getPackageName() { return Runtime.PACKAGE; }
    @Override public String getOpPackageName() { return Runtime.PACKAGE; }
    public String getBasePackageName() { return Runtime.PACKAGE; }
    @Override public ApplicationInfo getApplicationInfo() { return applicationInfo(); }
    @Override public String getPackageCodePath() { return Runtime.APK; }
    @Override public String getPackageResourcePath() { return Runtime.APK; }
    @Override public File getDataDir() { return dir(Runtime.DATA_DIR); }
    @Override public File getFilesDir() { return dir(Runtime.DATA_DIR + "/files"); }
    @Override public File getCacheDir() { return dir(Runtime.DATA_DIR + "/cache"); }
    @Override public File getCodeCacheDir() { return dir(Runtime.DATA_DIR + "/code_cache"); }
    @Override public File getNoBackupFilesDir() { return dir(Runtime.DATA_DIR + "/no_backup"); }
    @Override public File getDir(String name, int mode) { return dir(Runtime.DATA_DIR + "/app_" + name); }
    @Override public File getDatabasePath(String name) { return new File(dir(Runtime.DATA_DIR + "/databases"), name); }
    @Override public File getFileStreamPath(String name) { return new File(getFilesDir(), name); }
    @Override public File getExternalFilesDir(String type) {
        return dir(Runtime.EXTERNAL_DIR + "/files" + (type == null ? "" : "/" + type));
    }
    @Override public File[] getExternalFilesDirs(String type) { return new File[] {getExternalFilesDir(type)}; }
    @Override public File getExternalCacheDir() { return dir(Runtime.EXTERNAL_DIR + "/cache"); }
    @Override public File[] getExternalCacheDirs() { return new File[] {getExternalCacheDir()}; }
    @Override public File getObbDir() { return dir("/storage/emulated/0/Android/obb/" + Runtime.PACKAGE); }
    @Override public File[] getObbDirs() { return new File[] {getObbDir()}; }
    @Override public File[] getExternalMediaDirs() { return new File[] {dir("/storage/emulated/0/Android/media/" + Runtime.PACKAGE)}; }

    @Override public FileInputStream openFileInput(String name) throws FileNotFoundException {
        return new FileInputStream(Runtime.hostPath(Runtime.DATA_DIR + "/files/" + name));
    }
    @Override public FileOutputStream openFileOutput(String name, int mode) throws FileNotFoundException {
        getFilesDir();
        return new FileOutputStream(Runtime.hostPath(Runtime.DATA_DIR + "/files/" + name), (mode & MODE_APPEND) != 0);
    }
    @Override public boolean deleteFile(String name) { return new File(Runtime.hostPath(Runtime.DATA_DIR + "/files/" + name)).delete(); }
    @Override public String[] fileList() {
        String[] l = new File(Runtime.hostPath(Runtime.DATA_DIR + "/files")).list();
        return l == null ? new String[0] : l;
    }

    @Override public AssetManager getAssets() { return AssetManager.refractInstance(); }
    @Override public Resources getResources() { return Resources.getSystem(); }
    private Resources.Theme theme;
    @Override public synchronized Resources.Theme getTheme() {
        if (theme == null) {
            theme = Resources.getSystem().newTheme();
            int id = Manifest.theme(false);
            if (id == 0) id = Resources.getSystem().getIdentifier("Theme.DeviceDefault", "style", "android");
            if (id != 0) theme.applyStyle(id, true);
        }
        return theme;
    }
    @Override public PackageManager getPackageManager() { return PackageManagerImpl.get(); }
    @Override public ContentResolver getContentResolver() { return new ContentResolver(this) {}; }
    @Override public Looper getMainLooper() { return Looper.getMainLooper(); }
    @Override public Executor getMainExecutor() {
        Handler h = new Handler(Looper.getMainLooper());
        return h::post;
    }
    @Override public Context getApplicationContext() { return applicationContext != null ? applicationContext : this; }
    private static ClassLoader classLoader;
    @Override public synchronized ClassLoader getClassLoader() {
        if (classLoader == null)
            classLoader = new dalvik.system.PathClassLoader(Runtime.APK, Runtime.NATIVE_LIBRARY_DIR, ContextImpl.class.getClassLoader());
        return classLoader;
    }
    @Override public Display getDisplay() { return refract.view.WindowManagerImpl.get().getDefaultDisplay(); }
    @Override public Context createConfigurationContext(android.content.res.Configuration c) { return this; }
    @Override public Context createDisplayContext(Display d) { return this; }
    @Override public Context createPackageContext(String pkg, int flags) { return this; }
    @Override public Context createDeviceProtectedStorageContext() { return this; }
    @Override public Context createAttributionContext(String tag) { return this; }
    @Override public boolean isDeviceProtectedStorage() { return false; }
    @Override public boolean isRestricted() { return false; }

    @Override public synchronized SharedPreferences getSharedPreferences(String name, int mode) {
        return prefs.computeIfAbsent(name, n -> new SharedPreferencesImpl(Runtime.DATA_DIR + "/shared_prefs/" + n + ".xml"));
    }
    @Override public boolean deleteSharedPreferences(String name) {
        synchronized (this) {
            prefs.remove(name);
        }
        return new File(Runtime.hostPath(Runtime.DATA_DIR + "/shared_prefs/" + name + ".xml")).delete();
    }

    @Override public int checkSelfPermission(String permission) { return PackageManager.PERMISSION_GRANTED; }
    @Override public int checkPermission(String permission, int pid, int uid) { return PackageManager.PERMISSION_GRANTED; }
    @Override public int checkCallingOrSelfPermission(String permission) { return PackageManager.PERMISSION_GRANTED; }
    @Override public int checkCallingPermission(String permission) { return PackageManager.PERMISSION_GRANTED; }
    @Override public void enforceCallingOrSelfPermission(String permission, String message) {}

    @Override public void startActivity(Intent intent) { Runtime.log(4, "refract", "startActivity ignored: " + intent); }
    @Override public void startActivity(Intent intent, Bundle options) { startActivity(intent); }
    @Override public ComponentName startService(Intent service) {
        Runtime.log(4, "refract", "startService ignored: " + service);
        return null;
    }
    @Override public void sendBroadcast(Intent intent) {}
    @Override public void sendBroadcast(Intent intent, String permission) {}
    @Override public Intent registerReceiver(android.content.BroadcastReceiver r, IntentFilter f) { return null; }
    @Override public Intent registerReceiver(android.content.BroadcastReceiver r, IntentFilter f, int flags) { return null; }
    @Override public void unregisterReceiver(android.content.BroadcastReceiver r) {}

    @Override public synchronized Object getSystemService(String name) {
        Object s = services.get(name);
        if (s == null) {
            s = SystemServices.create(name, this);
            if (s != null) services.put(name, s);
            else Runtime.log(5, "refract", "getSystemService(" + name + ") is not available");
        }
        return s;
    }

    @Override public String getSystemServiceName(Class<?> serviceClass) { return SystemServices.nameOf(serviceClass); }
}
