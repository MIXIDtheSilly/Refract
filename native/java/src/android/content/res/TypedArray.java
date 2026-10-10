package android.content.res;

import android.util.TypedValue;
import refract.app.ResourceTable;

/** Attribute values resolved by Resources.obtain (null = attribute not set). */
public class TypedArray implements AutoCloseable {
    private final Resources mResources;
    private final Resources.Resolved[] mValues;

    public TypedArray() { this(Resources.getSystem(), 0); }

    TypedArray(Resources res, int length) {
        mResources = res;
        mValues = new Resources.Resolved[length];
    }

    void set(int index, Resources.Resolved v) { mValues[index] = v; }

    private Resources.Resolved at(int index) {
        return index >= 0 && index < mValues.length ? mValues[index] : null;
    }
    private static boolean isColor(int type) {
        return type >= ResourceTable.TYPE_FIRST_COLOR && type <= ResourceTable.TYPE_LAST_COLOR;
    }
    private static boolean isInt(int type) {
        return type >= ResourceTable.TYPE_INT_DEC && type <= ResourceTable.TYPE_LAST_COLOR;
    }

    public int length() { return mValues.length; }
    public int getIndexCount() {
        int n = 0;
        for (Resources.Resolved v : mValues)
            if (v != null) ++n;
        return n;
    }
    public int getIndex(int at) {
        for (int i = 0; i < mValues.length; ++i)
            if (mValues[i] != null && at-- == 0) return i;
        return -1;
    }
    public Resources getResources() { return mResources; }

    public CharSequence getText(int index) { return getString(index); }
    public String getString(int index) {
        Resources.Resolved v = at(index);
        if (v == null || v.type == ResourceTable.TYPE_NULL) return null;
        if (v.string != null) return v.string;
        return v.type == ResourceTable.TYPE_REFERENCE ? null : TypedValue.coerceToString(v.type, v.data);
    }
    public String getNonResourceString(int index) {
        Resources.Resolved v = at(index);
        return v != null && v.resourceId == 0 ? v.string : null;
    }
    public CharSequence[] getTextArray(int index) {
        Resources.Resolved v = at(index);
        return v != null && v.resourceId != 0 ? mResources.getStringArray(v.resourceId) : null;
    }
    public boolean getBoolean(int index, boolean defValue) {
        Resources.Resolved v = at(index);
        return v != null && isInt(v.type) ? v.data != 0 : defValue;
    }
    public int getInt(int index, int defValue) {
        Resources.Resolved v = at(index);
        if (v == null) return defValue;
        if (isInt(v.type)) return v.data;
        if (v.type == ResourceTable.TYPE_STRING && v.string != null) {
            try { return Integer.decode(v.string); } catch (NumberFormatException e) { return defValue; }
        }
        return defValue;
    }
    public int getInteger(int index, int defValue) { return getInt(index, defValue); }
    public float getFloat(int index, float defValue) {
        Resources.Resolved v = at(index);
        if (v == null) return defValue;
        if (v.type == ResourceTable.TYPE_FLOAT) return Float.intBitsToFloat(v.data);
        if (isInt(v.type)) return v.data;
        return defValue;
    }
    public int getColor(int index, int defValue) {
        Resources.Resolved v = at(index);
        return v != null && isInt(v.type) ? v.data : defValue;
    }
    public ColorStateList getColorStateList(int index) { return null; }
    public float getDimension(int index, float defValue) {
        Resources.Resolved v = at(index);
        return v != null && v.type == ResourceTable.TYPE_DIMENSION
                ? TypedValue.complexToDimension(v.data, mResources.getDisplayMetrics()) : defValue;
    }
    public int getDimensionPixelOffset(int index, int defValue) {
        Resources.Resolved v = at(index);
        return v != null && v.type == ResourceTable.TYPE_DIMENSION
                ? TypedValue.complexToDimensionPixelOffset(v.data, mResources.getDisplayMetrics()) : defValue;
    }
    public int getDimensionPixelSize(int index, int defValue) {
        Resources.Resolved v = at(index);
        return v != null && v.type == ResourceTable.TYPE_DIMENSION
                ? TypedValue.complexToDimensionPixelSize(v.data, mResources.getDisplayMetrics()) : defValue;
    }
    public int getLayoutDimension(int index, int defValue) {
        Resources.Resolved v = at(index);
        if (v == null) return defValue;
        if (isInt(v.type)) return v.data;
        if (v.type == ResourceTable.TYPE_DIMENSION)
            return TypedValue.complexToDimensionPixelSize(v.data, mResources.getDisplayMetrics());
        return defValue;
    }
    public int getLayoutDimension(int index, String name) { return getLayoutDimension(index, 0); }
    public float getFraction(int index, int base, int pbase, float defValue) {
        Resources.Resolved v = at(index);
        return v != null && v.type == ResourceTable.TYPE_FRACTION
                ? TypedValue.complexToFraction(v.data, base, pbase) : defValue;
    }
    public int getResourceId(int index, int defValue) {
        Resources.Resolved v = at(index);
        return v != null && v.type != ResourceTable.TYPE_NULL && v.resourceId != 0 ? v.resourceId : defValue;
    }
    public int getSourceResourceId(int index, int defValue) { return defValue; }
    public android.graphics.drawable.Drawable getDrawable(int index) {
        Resources.Resolved v = at(index);
        if (v == null || v.type == ResourceTable.TYPE_NULL) return null;
        return new android.graphics.drawable.ColorDrawable(isColor(v.type) ? v.data : 0);
    }
    public android.graphics.Typeface getFont(int index) { return null; }
    public boolean getValue(int index, TypedValue outValue) {
        Resources.Resolved v = at(index);
        if (v == null) {
            outValue.type = ResourceTable.TYPE_NULL;
            return false;
        }
        v.fill(outValue);
        return true;
    }
    public int getType(int index) {
        Resources.Resolved v = at(index);
        return v == null ? ResourceTable.TYPE_NULL : v.type;
    }
    public boolean hasValue(int index) {
        Resources.Resolved v = at(index);
        return v != null && v.type != ResourceTable.TYPE_NULL;
    }
    public boolean hasValueOrEmpty(int index) { return at(index) != null; }
    public TypedValue peekValue(int index) {
        Resources.Resolved v = at(index);
        if (v == null) return null;
        TypedValue t = new TypedValue();
        v.fill(t);
        return t;
    }
    public String getPositionDescription() { return "<internal>"; }
    public void recycle() {}
    public void close() {}
    public int getChangingConfigurations() { return 0; }
    public String toString() { return "TypedArray[" + mValues.length + "]"; }
}
