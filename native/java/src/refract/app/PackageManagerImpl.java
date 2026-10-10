package refract.app;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import java.util.ArrayList;
import java.util.List;
import refract.Runtime;

public final class PackageManagerImpl extends PackageManager {
    private static PackageManagerImpl instance;

    public static synchronized PackageManagerImpl get() {
        if (instance == null) instance = new PackageManagerImpl();
        return instance;
    }

    private PackageInfo packageInfo() {
        PackageInfo pi = new PackageInfo();
        pi.packageName = Runtime.PACKAGE;
        pi.versionCode = Runtime.VERSION_CODE;
        pi.versionName = Runtime.VERSION_NAME;
        pi.applicationInfo = ContextImpl.applicationInfo();
        pi.firstInstallTime = 1700000000000L;
        pi.lastUpdateTime = 1700000000000L;
        pi.requestedPermissions = new String[0];
        pi.activities = new ActivityInfo[] {activityInfo(new ComponentName(Runtime.PACKAGE, Runtime.ACTIVITY))};
        return pi;
    }

    private ActivityInfo activityInfo(ComponentName c) {
        ActivityInfo ai = new ActivityInfo();
        ai.name = c.getClassName();
        ai.packageName = Runtime.PACKAGE;
        ai.applicationInfo = ContextImpl.applicationInfo();
        ai.metaData = Manifest.activityMetaData();
        ai.screenOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE;
        ai.launchMode = ActivityInfo.LAUNCH_SINGLE_TASK;
        ai.configChanges = 0xffffffff;
        ai.enabled = true;
        ai.exported = true;
        ai.processName = Runtime.PACKAGE;
        return ai;
    }

    private void checkPackage(String pkg) throws NameNotFoundException {
        if (!Runtime.PACKAGE.equals(pkg)) throw new NameNotFoundException(pkg);
    }

    @Override public PackageInfo getPackageInfo(String pkg, int flags) throws NameNotFoundException {
        checkPackage(pkg);
        return packageInfo();
    }
    @Override public PackageInfo getPackageInfo(String pkg, PackageInfoFlags flags) throws NameNotFoundException {
        return getPackageInfo(pkg, 0);
    }
    @Override public ApplicationInfo getApplicationInfo(String pkg, int flags) throws NameNotFoundException {
        checkPackage(pkg);
        return ContextImpl.applicationInfo();
    }
    @Override public ApplicationInfo getApplicationInfo(String pkg, ApplicationInfoFlags flags) throws NameNotFoundException {
        return getApplicationInfo(pkg, 0);
    }
    @Override public ActivityInfo getActivityInfo(ComponentName c, int flags) throws NameNotFoundException {
        checkPackage(c.getPackageName());
        return activityInfo(c);
    }
    @Override public ActivityInfo getActivityInfo(ComponentName c, ComponentInfoFlags flags) throws NameNotFoundException {
        return getActivityInfo(c, 0);
    }
    @Override public ServiceInfo getServiceInfo(ComponentName c, int flags) throws NameNotFoundException {
        throw new NameNotFoundException(c.toString());
    }
    @Override public List<ApplicationInfo> getInstalledApplications(int flags) {
        List<ApplicationInfo> l = new ArrayList<>();
        l.add(ContextImpl.applicationInfo());
        return l;
    }
    @Override public List<PackageInfo> getInstalledPackages(int flags) {
        List<PackageInfo> l = new ArrayList<>();
        l.add(packageInfo());
        return l;
    }
    @Override public List<ResolveInfo> queryIntentActivities(Intent intent, int flags) { return new ArrayList<>(); }
    @Override public List<ResolveInfo> queryIntentServices(Intent intent, int flags) { return new ArrayList<>(); }
    @Override public List<ResolveInfo> queryBroadcastReceivers(Intent intent, int flags) { return new ArrayList<>(); }
    @Override public ResolveInfo resolveActivity(Intent intent, int flags) { return null; }
    @Override public Intent getLaunchIntentForPackage(String pkg) {
        return Runtime.PACKAGE.equals(pkg) ? new Intent(Intent.ACTION_MAIN).setClassName(pkg, Runtime.ACTIVITY) : null;
    }
    @Override public int checkPermission(String permName, String pkgName) { return PERMISSION_GRANTED; }
    @Override public String[] getPackagesForUid(int uid) { return new String[] {Runtime.PACKAGE}; }
    @Override public String getNameForUid(int uid) { return Runtime.PACKAGE; }
    @Override public String getInstallerPackageName(String pkg) { return "com.oculus.ocms"; }
    @Override public FeatureInfo[] getSystemAvailableFeatures() { return new FeatureInfo[0]; }
    @Override public CharSequence getApplicationLabel(ApplicationInfo info) { return Runtime.LABEL; }

    @Override public boolean hasSystemFeature(String name) { return hasSystemFeature(name, 0); }
    @Override public boolean hasSystemFeature(String name, int version) {
        return name.startsWith("android.hardware.vr") || name.startsWith("oculus.") || name.startsWith("com.oculus")
                || name.equals("android.hardware.vulkan.version") || name.equals("android.hardware.vulkan.level")
                || name.equals("android.hardware.opengles.aep") || name.equals("android.hardware.wifi")
                || name.equals("android.hardware.audio.output") || name.equals("android.hardware.microphone");
    }
}
