package android.os;

import java.util.HashMap;
import java.util.Set;

public class BaseBundle {
    HashMap<String, Object> mMap = new HashMap<>();

    BaseBundle() {}

    @SuppressWarnings("unchecked")
    <T> T typed(String key, Class<T> type, T def) {
        Object o = mMap.get(key);
        if (o == null) return def;
        return type.isInstance(o) ? (T) o : def;
    }

    public int size() { return mMap.size(); }
    public boolean isEmpty() { return mMap.isEmpty(); }
    public void clear() { mMap.clear(); }
    public boolean containsKey(String key) { return mMap.containsKey(key); }
    @Deprecated public Object get(String key) { return mMap.get(key); }
    public void remove(String key) { mMap.remove(key); }
    public void putAll(PersistableBundle bundle) { mMap.putAll(bundle.mMap); }
    public Set<String> keySet() { return mMap.keySet(); }

    public void putBoolean(String key, boolean value) { mMap.put(key, value); }
    public void putInt(String key, int value) { mMap.put(key, value); }
    public void putLong(String key, long value) { mMap.put(key, value); }
    public void putDouble(String key, double value) { mMap.put(key, value); }
    public void putString(String key, String value) { mMap.put(key, value); }
    public void putBooleanArray(String key, boolean[] value) { mMap.put(key, value); }
    public void putIntArray(String key, int[] value) { mMap.put(key, value); }
    public void putLongArray(String key, long[] value) { mMap.put(key, value); }
    public void putDoubleArray(String key, double[] value) { mMap.put(key, value); }
    public void putStringArray(String key, String[] value) { mMap.put(key, value); }

    public boolean getBoolean(String key) { return getBoolean(key, false); }
    public boolean getBoolean(String key, boolean def) {
        Object o = mMap.get(key);
        if (o instanceof Boolean b) return b;
        if (o instanceof String s) return Boolean.parseBoolean(s);
        return def;
    }
    public int getInt(String key) { return getInt(key, 0); }
    public int getInt(String key, int def) {
        Object o = mMap.get(key);
        if (o instanceof Number n) return n.intValue();
        if (o instanceof Boolean b) return b ? 1 : 0;
        return def;
    }
    public long getLong(String key) { return getLong(key, 0L); }
    public long getLong(String key, long def) {
        Object o = mMap.get(key);
        return o instanceof Number n ? n.longValue() : def;
    }
    public double getDouble(String key) { return getDouble(key, 0.0); }
    public double getDouble(String key, double def) {
        Object o = mMap.get(key);
        return o instanceof Number n ? n.doubleValue() : def;
    }
    public String getString(String key) {
        Object o = mMap.get(key);
        return o == null ? null : o.toString();
    }
    public String getString(String key, String def) {
        String s = getString(key);
        return s == null ? def : s;
    }
    public boolean[] getBooleanArray(String key) { return typed(key, boolean[].class, null); }
    public int[] getIntArray(String key) { return typed(key, int[].class, null); }
    public long[] getLongArray(String key) { return typed(key, long[].class, null); }
    public double[] getDoubleArray(String key) { return typed(key, double[].class, null); }
    public String[] getStringArray(String key) { return typed(key, String[].class, null); }
}
