package android.os;

import java.io.Serializable;
import java.util.ArrayList;

public final class Bundle extends BaseBundle implements Cloneable, Parcelable {
    public static final Bundle EMPTY = new Bundle();
    public static final Parcelable.Creator<Bundle> CREATOR = null;
    private ClassLoader mClassLoader;

    public Bundle() {}
    public Bundle(ClassLoader loader) { mClassLoader = loader; }
    public Bundle(int capacity) {}
    public Bundle(Bundle b) { if (b != null) mMap.putAll(b.mMap); }
    public Bundle(PersistableBundle b) { if (b != null) mMap.putAll(b.mMap); }

    public static Bundle forPair(String key, String value) {
        Bundle b = new Bundle();
        b.putString(key, value);
        return b;
    }

    public void setClassLoader(ClassLoader loader) { mClassLoader = loader; }
    public ClassLoader getClassLoader() { return mClassLoader; }
    public Object clone() { return new Bundle(this); }
    public Bundle deepCopy() { return new Bundle(this); }
    public void clear() { mMap.clear(); }
    public void remove(String key) { mMap.remove(key); }
    public void putAll(Bundle b) { mMap.putAll(b.mMap); }
    public boolean hasFileDescriptors() { return false; }

    public void putByte(String key, byte v) { mMap.put(key, v); }
    public void putChar(String key, char v) { mMap.put(key, v); }
    public void putShort(String key, short v) { mMap.put(key, v); }
    public void putFloat(String key, float v) { mMap.put(key, v); }
    public void putCharSequence(String key, CharSequence v) { mMap.put(key, v); }
    public void putParcelable(String key, Parcelable v) { mMap.put(key, v); }
    public void putSize(String key, android.util.Size v) { mMap.put(key, v); }
    public void putSizeF(String key, android.util.SizeF v) { mMap.put(key, v); }
    public void putParcelableArray(String key, Parcelable[] v) { mMap.put(key, v); }
    public void putParcelableArrayList(String key, ArrayList<? extends Parcelable> v) { mMap.put(key, v); }
    public void putIntegerArrayList(String key, ArrayList<Integer> v) { mMap.put(key, v); }
    public void putStringArrayList(String key, ArrayList<String> v) { mMap.put(key, v); }
    public void putCharSequenceArrayList(String key, ArrayList<CharSequence> v) { mMap.put(key, v); }
    public void putSerializable(String key, Serializable v) { mMap.put(key, v); }
    public void putByteArray(String key, byte[] v) { mMap.put(key, v); }
    public void putShortArray(String key, short[] v) { mMap.put(key, v); }
    public void putCharArray(String key, char[] v) { mMap.put(key, v); }
    public void putFloatArray(String key, float[] v) { mMap.put(key, v); }
    public void putCharSequenceArray(String key, CharSequence[] v) { mMap.put(key, v); }
    public void putBundle(String key, Bundle v) { mMap.put(key, v); }
    public void putBinder(String key, IBinder v) { mMap.put(key, v); }

    public byte getByte(String key) { return getByte(key, (byte) 0); }
    public Byte getByte(String key, byte def) {
        Object o = mMap.get(key);
        return o instanceof Number n ? n.byteValue() : def;
    }
    public char getChar(String key) { return getChar(key, (char) 0); }
    public char getChar(String key, char def) {
        Object o = mMap.get(key);
        return o instanceof Character c ? c : def;
    }
    public short getShort(String key) { return getShort(key, (short) 0); }
    public short getShort(String key, short def) {
        Object o = mMap.get(key);
        return o instanceof Number n ? n.shortValue() : def;
    }
    public float getFloat(String key) { return getFloat(key, 0f); }
    public float getFloat(String key, float def) {
        Object o = mMap.get(key);
        return o instanceof Number n ? n.floatValue() : def;
    }
    public CharSequence getCharSequence(String key) { return typed(key, CharSequence.class, null); }
    public CharSequence getCharSequence(String key, CharSequence def) { return typed(key, CharSequence.class, def); }
    public android.util.Size getSize(String key) { return typed(key, android.util.Size.class, null); }
    public android.util.SizeF getSizeF(String key) { return typed(key, android.util.SizeF.class, null); }
    public Bundle getBundle(String key) { return typed(key, Bundle.class, null); }
    @SuppressWarnings("unchecked")
    public <T extends Parcelable> T getParcelable(String key) { return (T) mMap.get(key); }
    @SuppressWarnings("unchecked")
    public <T> T getParcelable(String key, Class<T> clazz) { return (T) typed(key, clazz, null); }
    public Parcelable[] getParcelableArray(String key) { return typed(key, Parcelable[].class, null); }
    @SuppressWarnings("unchecked")
    public <T extends Parcelable> ArrayList<T> getParcelableArrayList(String key) { return (ArrayList<T>) typed(key, ArrayList.class, null); }
    public Serializable getSerializable(String key) { return typed(key, Serializable.class, null); }
    @SuppressWarnings("unchecked")
    public ArrayList<Integer> getIntegerArrayList(String key) { return (ArrayList<Integer>) typed(key, ArrayList.class, null); }
    @SuppressWarnings("unchecked")
    public ArrayList<String> getStringArrayList(String key) { return (ArrayList<String>) typed(key, ArrayList.class, null); }
    @SuppressWarnings("unchecked")
    public ArrayList<CharSequence> getCharSequenceArrayList(String key) { return (ArrayList<CharSequence>) typed(key, ArrayList.class, null); }
    public byte[] getByteArray(String key) { return typed(key, byte[].class, null); }
    public short[] getShortArray(String key) { return typed(key, short[].class, null); }
    public char[] getCharArray(String key) { return typed(key, char[].class, null); }
    public float[] getFloatArray(String key) { return typed(key, float[].class, null); }
    public CharSequence[] getCharSequenceArray(String key) { return typed(key, CharSequence[].class, null); }
    public IBinder getBinder(String key) { return typed(key, IBinder.class, null); }

    public int describeContents() { return 0; }
    public void writeToParcel(Parcel parcel, int flags) {}
    public void readFromParcel(Parcel parcel) {}

    @Override
    public String toString() { return "Bundle" + mMap; }
}
