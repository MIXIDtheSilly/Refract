package android.os;

import java.util.concurrent.ConcurrentHashMap;

/** In-process system properties; nothing here is shared with the guest's property service. */
public final class SystemProperties {
    private static final ConcurrentHashMap<String, String> props = new ConcurrentHashMap<>();

    static {
        props.put("ro.product.model", "Quest 2");
        props.put("ro.product.manufacturer", "Oculus");
        props.put("ro.build.version.sdk", "32");
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
