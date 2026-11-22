package android.os;

import java.util.concurrent.ConcurrentHashMap;

/** In-process system properties; nothing here is shared with the guest's property service. */
public final class SystemProperties {
    private static final ConcurrentHashMap<String, String> props = new ConcurrentHashMap<>();

    static {
        props.put("ro.product.model", "Quest 3");
        props.put("ro.product.manufacturer", "Oculus");
        props.put("ro.product.brand", "oculus");
        props.put("ro.product.device", "eureka");
        props.put("ro.product.name", "eureka");
        props.put("ro.product.board", "eureka");
        props.put("ro.hardware", "eureka");
        props.put("ro.soc.manufacturer", "Qualcomm");
        props.put("ro.soc.model", "SXR2230P");
        props.put("ro.build.id", "UP1A.231005.007.A1");
        props.put("ro.build.display.id", "UP1A.231005.007.A1");
        props.put("ro.build.version.incremental", "52433670048800520");
        props.put("ro.build.type", "user");
        props.put("ro.build.tags", "release-keys");
        props.put("ro.build.fingerprint", "oculus/eureka/eureka:14/UP1A.231005.007.A1/52433670048800520:user/abl_signing_keys:release,amss_signing_keys:release,release-keys");
        props.put("ro.system.build.fingerprint", "oculus/eureka/eureka:14/UP1A.231005.007.A1/52433670048800520:user/abl_signing_keys:release,amss_signing_keys:release,release-keys");
        for (String partition : new String[] {"system", "vendor"}) {
            String prefix = "ro.product." + partition + ".";
            props.put(prefix + "brand", "oculus");
            props.put(prefix + "device", "eureka");
            props.put(prefix + "manufacturer", "Oculus");
            props.put(prefix + "model", "Quest 3");
            props.put(prefix + "name", "eureka");
        }
        props.put("ro.product.product.brand", "oculus");
        props.put("ro.product.product.device", "eureka");
        props.put("ro.product.product.manufacturer", "Oculus");
        props.put("ro.product.product.name", "eureka");
        props.put("ro.product.product.model", "Eureka");
        props.put("ro.kernel.qemu", "0");
        props.put("ro.build.version.sdk", "34");
        props.put("ro.build.version.release", "14");
        props.put("ro.sf.lcd_density", "320");
    }

    private SystemProperties() {}

    public static String get(String key) { return get(key, ""); }

    public static String get(String key, String def) {
        String v = props.get(key);
        return v == null ? def : v;
    }

    public static int getInt(String key, int def) {
        try { return Integer.parseInt(props.get(key)); } catch (RuntimeException e) { return def; }
    }

    public static long getLong(String key, long def) {
        try { return Long.parseLong(props.get(key)); } catch (RuntimeException e) { return def; }
    }

    public static boolean getBoolean(String key, boolean def) {
        String v = props.get(key);
        if (v == null) return def;
        switch (v) {
            case "1": case "true": case "y": case "yes": case "on": return true;
            case "0": case "false": case "n": case "no": case "off": return false;
            default: return def;
        }
    }

    public static void set(String key, String val) {
        if (val == null) props.remove(key); else props.put(key, val);
    }

    public static void addChangeCallback(Runnable callback) {}
}
