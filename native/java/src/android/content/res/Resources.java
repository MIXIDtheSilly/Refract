package android.content.res;

import android.util.AttributeSet;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import java.util.HashMap;
import java.util.Map;
import refract.app.ResourceTable;

/**
 * Configuration and metrics are real; ids, values, styles and XML files come from
 * the APK's and framework-res.apk's resources (refract.app.ResourceTable).
 */
public class Resources {
    public static final int ID_NULL = 0;
    private static Resources system;

    private final AssetManager mAssets;
    private final DisplayMetrics mMetrics;
    private final Configuration mConfig;

    public Resources(AssetManager assets, DisplayMetrics metrics, Configuration config) {
        mAssets = assets;
        mMetrics = metrics;
        mConfig = config;
    }

    public static synchronized Resources getSystem() {
        if (system == null) {
            DisplayMetrics m = new DisplayMetrics();
            refract.view.WindowManagerImpl.get().getDefaultDisplay().getMetrics(m);
            Configuration c = new Configuration();
            c.setToDefaults();
            c.orientation = Configuration.ORIENTATION_LANDSCAPE;
            c.screenWidthDp = m.widthPixels;
            c.screenHeightDp = m.heightPixels;
            c.smallestScreenWidthDp = Math.min(m.widthPixels, m.heightPixels);
            c.densityDpi = m.densityDpi;
            c.uiMode = Configuration.UI_MODE_TYPE_VR_HEADSET | Configuration.UI_MODE_NIGHT_NO;
            c.touchscreen = Configuration.TOUCHSCREEN_NOTOUCH;
            c.keyboard = Configuration.KEYBOARD_NOKEYS;
            c.navigation = Configuration.NAVIGATION_NONAV;
            c.screenLayout = Configuration.SCREENLAYOUT_SIZE_LARGE | Configuration.SCREENLAYOUT_LONG_YES;
            c.setLocale(java.util.Locale.getDefault());
            system = new Resources(AssetManager.refractInstance(), m, c);
        }
        return system;
    }

    private NotFoundException missing(int id) {
        return new NotFoundException("Resource ID #0x" + Integer.toHexString(id));
    }

    public final AssetManager getAssets() { return mAssets; }
    public Configuration getConfiguration() { return mConfig; }
    public DisplayMetrics getDisplayMetrics() { return mMetrics; }
    public void updateConfiguration(Configuration config, DisplayMetrics metrics) {}
    private static ResourceTable table() { return ResourceTable.get(); }

    private String[] nameParts(int id) {
        String n = table().name(id);
        if (n == null)
            throw missing(id);
        int colon = n.indexOf(':'), slash = n.indexOf('/');
        return new String[] {n.substring(0, colon), n.substring(colon + 1, slash), n.substring(slash + 1)};
    }

    private ResourceTable.Value valueOf(int id) {
        ResourceTable.Value v = table().value(id);
        if (v == null)
            throw missing(id);
        return v;
    }

    public int getIdentifier(String name, String defType, String defPackage) {
        return table().identifier(name, defType, defPackage);
    }
    public String getResourceName(int resid) throws NotFoundException {
        String n = table().name(resid);
        if (n == null)
            throw missing(resid);
        return n;
    }
    public String getResourcePackageName(int resid) throws NotFoundException { return nameParts(resid)[0]; }
    public String getResourceTypeName(int resid) throws NotFoundException { return nameParts(resid)[1]; }
    public String getResourceEntryName(int resid) throws NotFoundException { return nameParts(resid)[2]; }
    public CharSequence getText(int id) throws NotFoundException { return getString(id); }
    public CharSequence getText(int id, CharSequence def) {
        ResourceTable.Value v = table().value(id);
        return v != null && v.string != null ? v.string : def;
    }
    // Unresolvable strings read as empty rather than aborting the app.
    public String getString(int id) throws NotFoundException {
        ResourceTable.Value v = table().value(id);
        return v != null && v.string != null ? v.string : "";
    }
    public String getString(int id, Object... formatArgs) throws NotFoundException {
        return String.format(getString(id), formatArgs);
    }
    public String[] getStringArray(int id) throws NotFoundException {
        ResourceTable.Bag b = table().bag(id);
        if (b == null)
            throw missing(id);
        String[] out = new String[b.values.length];
        for (int i = 0; i < out.length; ++i) {
            Resolved r = resolve(b.values[i], null);
            out[i] = r != null ? r.string : null;
        }
        return out;
    }
    public int[] getIntArray(int id) throws NotFoundException {
        ResourceTable.Bag b = table().bag(id);
        if (b == null)
            throw missing(id);
        int[] out = new int[b.values.length];
        for (int i = 0; i < out.length; ++i) {
            Resolved r = resolve(b.values[i], null);
            out[i] = r != null ? r.data : 0;
        }
        return out;
    }
    public boolean getBoolean(int id) throws NotFoundException { return valueOf(id).data != 0; }
    public int getInteger(int id) throws NotFoundException { return valueOf(id).data; }
    public float getFloat(int id) { return Float.intBitsToFloat(valueOf(id).data); }
    public float getDimension(int id) throws NotFoundException {
        return TypedValue.complexToDimension(valueOf(id).data, mMetrics);
    }
    public int getDimensionPixelSize(int id) throws NotFoundException {
        return TypedValue.complexToDimensionPixelSize(valueOf(id).data, mMetrics);
    }
    public int getDimensionPixelOffset(int id) throws NotFoundException {
        return TypedValue.complexToDimensionPixelOffset(valueOf(id).data, mMetrics);
    }
    public float getFraction(int id, int base, int pbase) {
        return TypedValue.complexToFraction(valueOf(id).data, base, pbase);
    }
    public int getColor(int id) throws NotFoundException { return valueOf(id).data; }
    public int getColor(int id, Resources.Theme theme) throws NotFoundException { return valueOf(id).data; }
    public ColorStateList getColorStateList(int id) throws NotFoundException { return null; }
    public ColorStateList getColorStateList(int id, Resources.Theme theme) throws NotFoundException { return null; }
    /**
     * Nothing is drawn by Refract's views, so drawables are placeholders of the right class:
     * colors become ColorDrawables, XML drawables the class of their root tag (AppCompat
     * checks that its vector test drawable is a VectorDrawable), the rest transparent ColorDrawables.
     */
    public android.graphics.drawable.Drawable getDrawable(int id) throws NotFoundException { return getDrawable(id, null); }
    public android.graphics.drawable.Drawable getDrawable(int id, Resources.Theme theme) throws NotFoundException {
        ResourceTable.Value v = table().value(id);
        if (v == null)
            throw missing(id);
        if (v.type >= ResourceTable.TYPE_FIRST_COLOR && v.type <= ResourceTable.TYPE_LAST_COLOR)
            return new android.graphics.drawable.ColorDrawable(v.data);
        if (v.string != null && v.string.endsWith(".xml")) {
            try (XmlResourceParser p = getXml(id)) {
                int type;
                while ((type = p.next()) != XmlResourceParser.START_TAG && type != XmlResourceParser.END_DOCUMENT) {}
                switch (type == XmlResourceParser.START_TAG ? p.getName() : "") {
                    case "vector": return new android.graphics.drawable.VectorDrawable();
                    case "animated-vector": return new android.graphics.drawable.AnimatedVectorDrawable();
                    case "selector": return new android.graphics.drawable.StateListDrawable();
                    case "shape": return new android.graphics.drawable.GradientDrawable();
                    case "ripple": return new android.graphics.drawable.RippleDrawable(null, null, null);
                    default: break;
                }
            } catch (Exception e) {
                // placeholder below
            }
        }
        return new android.graphics.drawable.ColorDrawable(0);
    }
    public void getValue(int id, TypedValue outValue, boolean resolveRefs) throws NotFoundException {
        ResourceTable.Value v = resolveRefs ? table().value(id) : table().rawValue(id);
        if (v == null)
            throw missing(id);
        Resolved r = resolve(v, null);
        r.fill(outValue);
        if (outValue.resourceId == 0)
            outValue.resourceId = id;
    }
    public java.io.InputStream openRawResource(int id) throws NotFoundException {
        return new java.io.ByteArrayInputStream(file(id));
    }
    public AssetFileDescriptor openRawResourceFd(int id) throws NotFoundException { throw missing(id); }
    public XmlResourceParser getXml(int id) throws NotFoundException { return new refract.app.BinaryXml(file(id)); }
    public XmlResourceParser getLayout(int id) throws NotFoundException { return getXml(id); }
    public TypedArray obtainTypedArray(int id) throws NotFoundException {
        ResourceTable.Bag b = table().bag(id);
        if (b == null)
            throw missing(id);
        TypedArray a = new TypedArray(this, b.values.length);
        for (int i = 0; i < b.values.length; ++i)
            a.set(i, resolve(b.values[i], null));
        return a;
    }
    public TypedArray obtainAttributes(AttributeSet set, int[] attrs) {
        return obtain(null, set, attrs, 0, 0);
    }
    public final Theme newTheme() { return new Theme(); }

    /** The bytes of a file resource (layout, xml, raw). */
    private byte[] file(int id) {
        ResourceTable.Value v = table().value(id);
        byte[] b = v != null && v.string != null ? table().file(v.string, (id >>> 24) == 0x01) : null;
        if (b == null)
            throw missing(id);
        return b;
    }

    // --- attribute resolution ---------------------------------------------------------

    /** A fully resolved value: type/data/string plus the resource it came from. */
    static final class Resolved {
        final int type, data, resourceId;
        final String string;
        Resolved(int type, int data, String string, int resourceId) {
            this.type = type;
            this.data = data;
            this.string = string;
            this.resourceId = resourceId;
        }
        void fill(TypedValue out) {
            out.type = type;
            out.data = data;
            out.string = string;
            out.resourceId = resourceId;
            out.assetCookie = 0;
            out.changingConfigurations = 0;
            out.density = 0;
        }
    }

    /** Follows ?attr (through the theme) and @reference until a plain value or a bag. */
    static Resolved resolve(ResourceTable.Value v, Theme theme) {
        int resId = 0;
        for (int depth = 0; v != null && depth < 32; ++depth) {
            int t = v.type == 0x07 ? ResourceTable.TYPE_REFERENCE : v.type == 0x08 ? ResourceTable.TYPE_ATTRIBUTE : v.type;
            if (t == ResourceTable.TYPE_ATTRIBUTE) {
                if (theme == null)
                    return null;
                v = theme.mAttrs.get(v.data);
                continue;
            }
            if (t == ResourceTable.TYPE_REFERENCE) {
                if (v.data == 0)
                    return null;  // @null
                resId = v.data;
                ResourceTable.Value target = table().rawValue(v.data);
                if (target == null)
                    return new Resolved(t, v.data, null, resId);  // a style or other bag
                v = target;
                continue;
            }
            if (t == ResourceTable.TYPE_NULL)
                return v.data == 1 ? new Resolved(t, 1, null, resId) : null;  // @empty / undefined
            return new Resolved(t, v.data, v.string, resId);
        }
        return null;
    }

    /** obtainStyledAttributes: XML attribute, then its style="", then the default style, then the theme. */
    TypedArray obtain(Theme theme, AttributeSet set, int[] attrs, int defStyleAttr, int defStyleRes) {
        Map<Integer, ResourceTable.Value> xml = set instanceof refract.app.BinaryXml bx ? bx.refractValues() : Map.of();
        int style = set != null ? set.getStyleAttribute() : 0;
        if (style != 0 && set instanceof refract.app.BinaryXml bx) {
            Resolved r = resolve(bx.refractStyleValue(), theme);
            style = r != null ? r.resourceId : 0;
        }
        Map<Integer, ResourceTable.Value> styleValues = style != 0 ? table().styleAttributes(style) : Map.of();
        int def = 0;
        if (defStyleAttr != 0 && theme != null) {
            Resolved r = resolve(theme.mAttrs.get(defStyleAttr), theme);
            if (r != null && r.type == ResourceTable.TYPE_REFERENCE)
                def = r.data;
        }
        if (def == 0)
            def = defStyleRes;
        Map<Integer, ResourceTable.Value> defValues = def != 0 ? table().styleAttributes(def) : Map.of();
        TypedArray a = new TypedArray(this, attrs.length);
        for (int i = 0; i < attrs.length; ++i) {
            int attr = attrs[i];
            ResourceTable.Value v = xml.get(attr);
            if (v == null) v = styleValues.get(attr);
            if (v == null) v = defValues.get(attr);
            if (v == null && theme != null) v = theme.mAttrs.get(attr);
            a.set(i, resolve(v, theme));
        }
        return a;
    }

    public static class NotFoundException extends RuntimeException {
        public NotFoundException() {}
        public NotFoundException(String name) { super(name); }
        public NotFoundException(String name, Exception cause) { super(name, cause); }
    }

    /** A theme: the attributes of the styles applied to it. */
    public final class Theme {
        final HashMap<Integer, ResourceTable.Value> mAttrs = new HashMap<>();

        public void applyStyle(int resid, boolean force) {
            for (Map.Entry<Integer, ResourceTable.Value> e : table().styleAttributes(resid).entrySet())
                if (force || !mAttrs.containsKey(e.getKey()))
                    mAttrs.put(e.getKey(), e.getValue());
        }
        public void setTo(Theme other) {
            mAttrs.clear();
            mAttrs.putAll(other.mAttrs);
        }
        public void rebase() {}
        public TypedArray obtainStyledAttributes(int[] attrs) { return obtain(this, null, attrs, 0, 0); }
        public TypedArray obtainStyledAttributes(int resid, int[] attrs) throws NotFoundException {
            return obtain(this, null, attrs, 0, resid);
        }
        public TypedArray obtainStyledAttributes(AttributeSet set, int[] attrs, int defStyleAttr, int defStyleRes) {
            return obtain(this, set, attrs, defStyleAttr, defStyleRes);
        }
        public boolean resolveAttribute(int resid, TypedValue outValue, boolean resolveRefs) {
            ResourceTable.Value v = mAttrs.get(resid);
            if (v == null)
                return false;
            Resolved r;
            if (resolveRefs) {
                r = resolve(v, this);
            } else {
                for (int depth = 0; v != null && v.type == ResourceTable.TYPE_ATTRIBUTE && depth < 32; ++depth)
                    v = mAttrs.get(v.data);
                r = v == null ? null : new Resolved(v.type, v.data, v.string, 0);
            }
            if (r == null)
                return false;
            r.fill(outValue);
            return true;
        }
        public android.graphics.drawable.Drawable getDrawable(int id) throws NotFoundException {
            return Resources.this.getDrawable(id, this);
        }
        public int getChangingConfigurations() { return 0; }
        public int getExplicitStyle(AttributeSet set) { return set != null ? set.getStyleAttribute() : 0; }
        public Resources getResources() { return Resources.this; }
    }
}
