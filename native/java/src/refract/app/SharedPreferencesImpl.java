package refract.app;

import android.content.SharedPreferences;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import refract.Runtime;

/** SharedPreferences stored in Android's XML format under the app's shared_prefs. */
final class SharedPreferencesImpl implements SharedPreferences {
    private final File file;
    private final Map<String, Object> map = new HashMap<>();
    private final List<OnSharedPreferenceChangeListener> listeners = new ArrayList<>();

    SharedPreferencesImpl(String guestPath) {
        file = new File(Runtime.hostPath(guestPath));
        load();
    }

    private static final Pattern ENTRY = Pattern.compile(
            "<(string|int|long|float|boolean)\\s+name=\"([^\"]*)\"(?:\\s+value=\"([^\"]*)\"\\s*/>|>([^<]*)</string>)");

    private void load() {
        if (!file.exists()) return;
        try {
            String xml = Files.readString(file.toPath(), StandardCharsets.UTF_8);
            Matcher m = ENTRY.matcher(xml);
            while (m.find()) {
                String type = m.group(1), name = unescape(m.group(2));
                String value = m.group(3) != null ? m.group(3) : unescape(m.group(4));
                switch (type) {
                    case "string" -> map.put(name, value);
                    case "int" -> map.put(name, Integer.parseInt(value));
                    case "long" -> map.put(name, Long.parseLong(value));
                    case "float" -> map.put(name, Float.parseFloat(value));
                    case "boolean" -> map.put(name, Boolean.parseBoolean(value));
                    default -> { }
                }
            }
        } catch (Exception e) {
            Runtime.log(5, "SharedPreferences", "cannot read " + file + ": " + e);
        }
    }

    private static String escape(String s) {
        return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;");
    }

    private static String unescape(String s) {
        return s.replace("&quot;", "\"").replace("&gt;", ">").replace("&lt;", "<").replace("&amp;", "&");
    }

    private synchronized void save() {
        StringBuilder sb = new StringBuilder("<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n");
        for (Map.Entry<String, Object> e : map.entrySet()) {
            String n = escape(e.getKey());
            Object v = e.getValue();
            if (v instanceof String s) sb.append("    <string name=\"").append(n).append("\">").append(escape(s)).append("</string>\n");
            else if (v instanceof Integer) sb.append("    <int name=\"").append(n).append("\" value=\"").append(v).append("\" />\n");
            else if (v instanceof Long) sb.append("    <long name=\"").append(n).append("\" value=\"").append(v).append("\" />\n");
            else if (v instanceof Float) sb.append("    <float name=\"").append(n).append("\" value=\"").append(v).append("\" />\n");
            else if (v instanceof Boolean) sb.append("    <boolean name=\"").append(n).append("\" value=\"").append(v).append("\" />\n");
        }
        sb.append("</map>\n");
        try {
            file.getParentFile().mkdirs();
            Files.writeString(file.toPath(), sb.toString(), StandardCharsets.UTF_8);
        } catch (Exception e) {
            Runtime.log(5, "SharedPreferences", "cannot write " + file + ": " + e);
        }
    }

    @Override public synchronized Map<String, ?> getAll() { return new HashMap<>(map); }
    @Override public synchronized String getString(String key, String def) {
        Object v = map.get(key);
        return v instanceof String s ? s : def;
    }
    @SuppressWarnings("unchecked")
    @Override public synchronized Set<String> getStringSet(String key, Set<String> def) {
        Object v = map.get(key);
        return v instanceof Set ? (Set<String>) v : def;
    }
    @Override public synchronized int getInt(String key, int def) {
        Object v = map.get(key);
        return v instanceof Integer i ? i : def;
    }
    @Override public synchronized long getLong(String key, long def) {
        Object v = map.get(key);
        return v instanceof Long l ? l : def;
    }
    @Override public synchronized float getFloat(String key, float def) {
        Object v = map.get(key);
        return v instanceof Float f ? f : def;
    }
    @Override public synchronized boolean getBoolean(String key, boolean def) {
        Object v = map.get(key);
        return v instanceof Boolean b ? b : def;
    }
    @Override public synchronized boolean contains(String key) { return map.containsKey(key); }
    @Override public Editor edit() { return new EditorImpl(); }
    @Override public void registerOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener l) { listeners.add(l); }
    @Override public void unregisterOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener l) { listeners.remove(l); }

    private final class EditorImpl implements Editor {
        private final Map<String, Object> changes = new HashMap<>();
        private final Set<String> removed = new HashSet<>();
        private boolean clear;

        @Override public Editor putString(String k, String v) { changes.put(k, v); return this; }
        @Override public Editor putStringSet(String k, Set<String> v) { changes.put(k, v == null ? null : new HashSet<>(v)); return this; }
        @Override public Editor putInt(String k, int v) { changes.put(k, v); return this; }
        @Override public Editor putLong(String k, long v) { changes.put(k, v); return this; }
        @Override public Editor putFloat(String k, float v) { changes.put(k, v); return this; }
        @Override public Editor putBoolean(String k, boolean v) { changes.put(k, v); return this; }
        @Override public Editor remove(String k) { removed.add(k); return this; }
        @Override public Editor clear() { clear = true; return this; }

        @Override public boolean commit() {
            synchronized (SharedPreferencesImpl.this) {
                if (clear) map.clear();
                for (String k : removed) map.remove(k);
                for (Map.Entry<String, Object> e : changes.entrySet()) {
                    if (e.getValue() == null) map.remove(e.getKey());
                    else map.put(e.getKey(), e.getValue());
                }
            }
            save();
            for (OnSharedPreferenceChangeListener l : new ArrayList<>(listeners))
                for (String k : changes.keySet()) l.onSharedPreferenceChanged(SharedPreferencesImpl.this, k);
            return true;
        }

        @Override public void apply() { commit(); }
    }
}
