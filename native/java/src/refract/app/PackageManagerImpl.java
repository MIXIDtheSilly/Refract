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
import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.security.cert.CertificateException;
import java.security.cert.CertificateFactory;
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

    private static final byte[] APK_SIG_BLOCK_MAGIC = {
        'A', 'P', 'K', ' ', 'S', 'i', 'g', ' ', 'B', 'l', 'o', 'c', 'k', ' ', '4', '2'
    };
    private static final int APK_SIG_V2_ID = 0x7109871a;
    private static final int APK_SIG_V3_ID = 0xf05368c0;
    private static final int APK_SIG_V31_ID = 0x1b93ad61;

    /** The APK's current signer certificates, as PackageParser reports them. */
    static synchronized android.content.pm.Signature[] signers() {
        if (signers != null) return signers;
        List<android.content.pm.Signature> out = new ArrayList<>();
        String apkPath = System.getProperty("refract.apkHost");
        if (apkPath == null || apkPath.isEmpty()) {
            Runtime.log(5, "refract", "APK signature: refract.apkHost is not set");
            return signers = new android.content.pm.Signature[0];
        }
        File apk = new File(apkPath);
        try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(apk)) {
            for (var it = z.entries(); it.hasMoreElements(); ) {
                java.util.zip.ZipEntry e = it.nextElement();
                String n = e.getName().toUpperCase(java.util.Locale.ROOT);
                if (!n.startsWith("META-INF/") || n.indexOf('/', 9) >= 0
                        || !(n.endsWith(".RSA") || n.endsWith(".DSA") || n.endsWith(".EC"))) continue;
                try (java.io.InputStream in = z.getInputStream(e)) {
                    var certs = CertificateFactory.getInstance("X.509").generateCertificates(in);
                    if (!certs.isEmpty()) out.add(new android.content.pm.Signature(certs.iterator().next().getEncoded()));
                }
            }
        } catch (IOException | CertificateException ex) {
            Runtime.log(5, "refract", "APK signature: " + ex);
        }
        if (out.isEmpty()) {
            try {
                out.addAll(signingBlockSigners(apk));
            } catch (IOException | CertificateException ex) {
                Runtime.log(5, "refract", "APK signing block: " + ex);
            }
        }
        if (out.isEmpty()) Runtime.log(5, "refract", "APK has no supported signature; reporting no signers");
        return signers = out.toArray(new android.content.pm.Signature[0]);
    }

    private static List<android.content.pm.Signature> signingBlockSigners(File apk)
            throws IOException, CertificateException {
        try (RandomAccessFile file = new RandomAccessFile(apk, "r")) {
            long fileLength = file.length();
            int tailLength = (int) Math.min(fileLength, 22 + 0xffffL);
            byte[] tail = new byte[tailLength];
            file.seek(fileLength - tailLength);
            file.readFully(tail);

            int eocd = -1;
            for (int i = tail.length - 22; i >= 0; i--) {
                if (readIntLE(tail, i) == 0x06054b50
                        && i + 22 + readUnsignedShortLE(tail, i + 20) == tail.length) {
                    eocd = i;
                    break;
                }
            }
            if (eocd < 0) throw new IOException("ZIP end of central directory not found");
            long centralDirectoryOffset = Integer.toUnsignedLong(readIntLE(tail, eocd + 16));
            if (centralDirectoryOffset == 0xffffffffL)
                throw new IOException("ZIP64 central directory is not supported");
            if (centralDirectoryOffset < 24 || centralDirectoryOffset > fileLength)
                throw new IOException("Invalid ZIP central directory offset");

            file.seek(centralDirectoryOffset - 24);
            byte[] footer = new byte[24];
            file.readFully(footer);
            for (int i = 0; i < APK_SIG_BLOCK_MAGIC.length; i++) {
                if (footer[i + 8] != APK_SIG_BLOCK_MAGIC[i]) return new ArrayList<>();
            }
            long blockSize = readLongLE(footer, 0);
            long blockStart = centralDirectoryOffset - blockSize - 8;
            if (blockSize < 24 || blockStart < 0)
                throw new IOException("Invalid APK signing block size");
            file.seek(blockStart);
            if (readLongLE(file) != blockSize) throw new IOException("APK signing block size mismatch");

            long pairOffset = blockStart + 8;
            long pairsEnd = centralDirectoryOffset - 24;
            int selectedPriority = 0;
            List<android.content.pm.Signature> selected = new ArrayList<>();
            while (pairOffset < pairsEnd) {
                if (pairsEnd - pairOffset < 8) throw new IOException("Truncated APK signing block pair");
                file.seek(pairOffset);
                long pairLength = readLongLE(file);
                if (pairLength < 4 || pairLength > pairsEnd - pairOffset - 8)
                    throw new IOException("Invalid APK signing block pair length");
                int id = readIntLE(file);
                long valueLength = pairLength - 4;
                int priority = signingSchemePriority(id);
                if (priority > 0) {
                    if (valueLength > Integer.MAX_VALUE)
                        throw new IOException("APK signer data is too large");
                    byte[] value = new byte[(int) valueLength];
                    file.readFully(value);
                    if (priority > selectedPriority) {
                        selected = parseSigningSchemeSigners(value);
                        selectedPriority = priority;
                    }
                }
                pairOffset += 8 + pairLength;
            }
            return selected;
        }
    }

    private static int signingSchemePriority(int id) {
        if (id == APK_SIG_V31_ID) return 3;
        if (id == APK_SIG_V3_ID) return 2;
        if (id == APK_SIG_V2_ID) return 1;
        return 0;
    }

    private static List<android.content.pm.Signature> parseSigningSchemeSigners(byte[] value)
            throws IOException, CertificateException {
        ByteBuffer block = ByteBuffer.wrap(value).order(ByteOrder.LITTLE_ENDIAN);
        ByteBuffer signers = lengthPrefixedSlice(block);
        if (block.hasRemaining()) throw new IOException("Trailing APK signer data");

        List<android.content.pm.Signature> out = new ArrayList<>();
        CertificateFactory certificates = CertificateFactory.getInstance("X.509");
        while (signers.hasRemaining()) {
            ByteBuffer signer = lengthPrefixedSlice(signers);
            ByteBuffer signedData = lengthPrefixedSlice(signer);
            lengthPrefixedSlice(signer);
            lengthPrefixedSlice(signer);
            if (signer.hasRemaining()) throw new IOException("Trailing APK signer fields");

            lengthPrefixedSlice(signedData);
            ByteBuffer certs = lengthPrefixedSlice(signedData);
            if (!certs.hasRemaining()) throw new IOException("APK signer has no certificate");
            ByteBuffer certData = lengthPrefixedSlice(certs);
            byte[] encoded = new byte[certData.remaining()];
            certData.get(encoded);
            java.security.cert.Certificate certificate =
                    certificates.generateCertificate(new ByteArrayInputStream(encoded));
            out.add(new android.content.pm.Signature(certificate.getEncoded()));
        }
        return out;
    }

    private static ByteBuffer lengthPrefixedSlice(ByteBuffer source) throws IOException {
        if (source.remaining() < 4) throw new IOException("Truncated length-prefixed APK signer field");
        long length = Integer.toUnsignedLong(source.getInt());
        if (length > source.remaining()) throw new IOException("Invalid APK signer field length");
        ByteBuffer slice = source.slice().order(ByteOrder.LITTLE_ENDIAN);
        slice.limit((int) length);
        source.position(source.position() + (int) length);
        return slice;
    }

    private static int readIntLE(byte[] data, int offset) {
        return (data[offset] & 0xff) | ((data[offset + 1] & 0xff) << 8)
                | ((data[offset + 2] & 0xff) << 16) | ((data[offset + 3] & 0xff) << 24);
    }

    private static int readUnsignedShortLE(byte[] data, int offset) {
        return (data[offset] & 0xff) | ((data[offset + 1] & 0xff) << 8);
    }

    private static long readLongLE(byte[] data, int offset) {
        return ByteBuffer.wrap(data, offset, 8).order(ByteOrder.LITTLE_ENDIAN).getLong();
    }

    private static long readLongLE(RandomAccessFile file) throws IOException {
        byte[] data = new byte[8];
        file.readFully(data);
        return readLongLE(data, 0);
    }

    private static int readIntLE(RandomAccessFile file) throws IOException {
        byte[] data = new byte[4];
        file.readFully(data);
        return readIntLE(data, 0);
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
    /** The launch activity answers intents that match its MAIN filter (UE4 asks for com.oculus.intent.category.VR
     *  to find out it is a VR app); nothing else is installed. */
    @Override public List<ResolveInfo> queryIntentActivities(Intent intent, int flags) {
        List<ResolveInfo> l = new ArrayList<>();
        if (intent == null) return l;
        ComponentName c = intent.getComponent();
        String pkg = c != null ? c.getPackageName() : intent.getPackage();
        if (pkg != null && !Runtime.PACKAGE.equals(pkg)) return l;
        if (c != null && !Runtime.ACTIVITY.equals(c.getClassName())) return l;
        if (c == null) {
            if (intent.getAction() != null && !Intent.ACTION_MAIN.equals(intent.getAction())) return l;
            List<String> filter = java.util.Arrays.asList(Manifest.get("activity.categories", "").split(","));
            if (intent.getCategories() != null)
                for (String cat : intent.getCategories())
                    if (!filter.contains(cat)) return l;
        }
        ResolveInfo ri = new ResolveInfo();
        ri.activityInfo = activityInfo(new ComponentName(Runtime.PACKAGE, Runtime.ACTIVITY));
        ri.match = 0x108000;  // IntentFilter.MATCH_CATEGORY_EMPTY | MATCH_ADJUSTMENT_NORMAL
        l.add(ri);
        return l;
    }
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
    /** A Quest 2's features with versions (product/etc/permissions in the firmware dump). */
    private static final Object[][] FEATURES = {
        {"android.hardware.vulkan.version", 4198400}, {"android.hardware.vulkan.level", 1},
        {"android.hardware.vulkan.compute", 0}, {"android.hardware.opengles.aep", 0},
        {"android.hardware.vr.headtracking", 1}, {"android.hardware.vr.high_performance", 0},
        {"android.hardware.wifi", 0}, {"android.hardware.audio.output", 0}, {"android.hardware.microphone", 0},
        {"oculus.software.mic_sharing", 0}, {"oculus.software.xrsp", 0},
    };

    @Override public FeatureInfo[] getSystemAvailableFeatures() {
        FeatureInfo[] r = new FeatureInfo[FEATURES.length + 1];
        for (int i = 0; i < FEATURES.length; i++) {
            r[i] = new FeatureInfo();
            r[i].name = (String) FEATURES[i][0];
            r[i].version = (Integer) FEATURES[i][1];
        }
        r[FEATURES.length] = new FeatureInfo();
        r[FEATURES.length].reqGlEsVersion = 0x00030002;  // OpenGL ES 3.2, as ActivityManager reports
        return r;
    }
    @Override public CharSequence getApplicationLabel(ApplicationInfo info) { return Runtime.LABEL; }

    @Override public boolean hasSystemFeature(String name) { return hasSystemFeature(name, 0); }
    @Override public boolean hasSystemFeature(String name, int version) {
        for (Object[] f : FEATURES)
            if (f[0].equals(name)) return version <= (Integer) f[1];
        return name.startsWith("android.hardware.vr") || name.startsWith("oculus.") || name.startsWith("com.oculus")
                || name.equals("android.hardware.vulkan.version") || name.equals("android.hardware.vulkan.level")
                || name.equals("android.hardware.opengles.aep") || name.equals("android.hardware.wifi")
                || name.equals("android.hardware.audio.output") || name.equals("android.hardware.microphone");
    }
}
