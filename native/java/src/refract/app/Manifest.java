package refract.app;

import android.os.Bundle;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.Properties;

/** Manifest data the installer extracted into app.properties (meta-data, components). */
public final class Manifest {
    private static Properties props;

    private Manifest() {}

    static synchronized Properties props() {
        if (props == null) {
            props = new Properties();
            String dir = System.getProperty("refract.appDir");
            if (dir != null) {
                try (var r = new InputStreamReader(new FileInputStream(dir + "\\app.properties"), StandardCharsets.UTF_8)) {
                    props.load(r);
                } catch (Exception e) {
                    refract.Runtime.log(5, "refract", "cannot read app.properties: " + e);
                }
            }
        }
        return props;
    }

    public static String get(String key, String def) { return props().getProperty(key, def); }

    /** meta-data entries with the given prefix ("meta." = application, "activity.meta." = launch activity). */
    static Bundle bundle(String prefix) {
        Bundle b = new Bundle();
        for (String k : props().stringPropertyNames()) {
            if (!k.startsWith(prefix)) continue;
            String name = k.substring(prefix.length());
            String raw = props().getProperty(k);
            int colon = raw.indexOf(':');
            String type = colon > 0 ? raw.substring(0, colon) : "string";
            String value = colon > 0 ? raw.substring(colon + 1) : raw;
            try {
                switch (type) {
                    case "ref" -> putResolved(b, name, (int) Long.decode(value).longValue());
                    case "boolean" -> b.putBoolean(name, Boolean.parseBoolean(value));
                    case "int" -> b.putInt(name, (int) Long.decode(value).longValue());
                    case "float" -> b.putFloat(name, Float.parseFloat(value));
                    default -> b.putString(name, value);
                }
            } catch (NumberFormatException e) {
                b.putString(name, value);
            }
        }
        return b;
    }

    /** The launch activity's theme (falling back to the application's) or the application's; 0 if none. */
    public static int theme(boolean activity) {
        String v = activity ? get("activity.theme", get("theme", null)) : get("theme", null);
        try {
            return v == null ? 0 : (int) Long.decode(v).longValue();
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    /** android:value="@type/name": the referenced value, typed (PackageParser.parseMetaData). */
    private static void putResolved(Bundle b, String name, int id) {
        ResourceTable.Value v = ResourceTable.get().value(id);
        if (v == null) b.putInt(name, id);
        else if (v.type == ResourceTable.TYPE_STRING) b.putString(name, v.string);
        else if (v.type == ResourceTable.TYPE_BOOLEAN) b.putBoolean(name, v.data != 0);
        else if (v.type == ResourceTable.TYPE_FLOAT) b.putFloat(name, Float.intBitsToFloat(v.data));
        else b.putInt(name, v.data);
    }

    public static Bundle metaData() { return bundle("meta."); }
    public static Bundle activityMetaData() { return bundle("activity.meta."); }
}
