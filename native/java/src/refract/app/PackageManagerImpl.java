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

    private static android.content.pm.Signature[] signers;

    /** The APK's signer certificates (v1 signature: META-INF/*.RSA|DSA|EC), as PackageParser reports them. */
    static synchronized android.content.pm.Signature[] signers() {
        if (signers != null) return signers;
        List<android.content.pm.Signature> out = new ArrayList<>();
        try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(System.getProperty("refract.apkHost"))) {
            for (var it = z.entries(); it.hasMoreElements(); ) {
                java.util.zip.ZipEntry e = it.nextElement();
                String n = e.getName().toUpperCase(java.util.Locale.ROOT);
                if (!n.startsWith("META-INF/") || n.indexOf('/', 9) >= 0
                        || !(n.endsWith(".RSA") || n.endsWith(".DSA") || n.endsWith(".EC"))) continue;
                try (java.io.InputStream in = z.getInputStream(e)) {
                    var certs = java.security.cert.CertificateFactory.getInstance("X.509").generateCertificates(in);
                    if (!certs.isEmpty()) out.add(new android.content.pm.Signature(certs.iterator().next().getEncoded()));
                }
            }
        } catch (Exception ex) {
            Runtime.log(5, "refract", "APK signature: " + ex);
        }
        if (out.isEmpty()) Runtime.log(5, "refract", "APK has no v1 signature; reporting no signers");
        return signers = out.toArray(new android.content.pm.Signature[0]);
    }

    private PackageInfo packageInfo(int flags) {
        PackageInfo pi = packageInfo();
        if ((flags & GET_SIGNATURES) != 0) pi.signatures = signers();
        if ((flags & GET_SIGNING_CERTIFICATES) != 0) pi.signingInfo = new android.content.pm.SigningInfo(signers());
        return pi;
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

    /** Refract's stand-ins for system packages, installed on the emulator as their own APKs (the
     *  Platform SDK's com.oculus.horizon, the OpenXR driver loader's com.oculus.systemdriver); here
     *  their code is part of the app (tools/install_apk.py). */
    private static final String[][] STAND_INS = {
        {"com.oculus.horizon", "0.1-refract"},
        {"com.oculus.systemdriver", "0.1"},
    };

    private static PackageInfo standIn(String pkg) {
        for (String[] s : STAND_INS) {
            if (!s[0].equals(pkg)) continue;
            PackageInfo pi = new PackageInfo();
            pi.packageName = pkg;
            pi.versionCode = 1;
            pi.versionName = s[1];
            ApplicationInfo ai = new ApplicationInfo(ContextImpl.applicationInfo());
            ai.packageName = pkg;
            ai.processName = pkg;
            ai.flags |= ApplicationInfo.FLAG_SYSTEM;
            pi.applicationInfo = ai;
            pi.firstInstallTime = 1700000000000L;
            pi.lastUpdateTime = 1700000000000L;
            return pi;
        }
        return null;
    }

    @Override public PackageInfo getPackageInfo(String pkg, int flags) throws NameNotFoundException {
        PackageInfo standIn = standIn(pkg);
        if (standIn != null) return standIn;
        checkPackage(pkg);
        return packageInfo(flags);
    }
    @Override public PackageInfo getPackageInfo(String pkg, PackageInfoFlags flags) throws NameNotFoundException {
        return getPackageInfo(pkg, flags == null ? 0 : (int) flags.getValue());
    }
    @Override public ApplicationInfo getApplicationInfo(String pkg, int flags) throws NameNotFoundException {
        PackageInfo standIn = standIn(pkg);
        if (standIn != null) return standIn.applicationInfo;
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
    @Override public android.content.pm.InstallSourceInfo getInstallSourceInfo(String pkg) throws NameNotFoundException {
        if (!Runtime.PACKAGE.equals(pkg)) throw new NameNotFoundException(pkg);
        return new android.content.pm.InstallSourceInfo("com.oculus.ocms", null, "com.oculus.ocms");
    }
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
