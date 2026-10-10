package android.content.res;

import android.util.DisplayMetrics;

/** Configuration and metrics are real; ids, names and simple values come from resources.arsc. */
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
    private static refract.app.ResourceTable table() { return refract.app.ResourceTable.get(); }

    private String[] nameParts(int id) {
        String n = table().name(id);
        if (n == null)
            throw missing(id);
        int colon = n.indexOf(':'), slash = n.indexOf('/');
        return new String[] {n.substring(0, colon), n.substring(colon + 1, slash), n.substring(slash + 1)};
    }

    private refract.app.ResourceTable.Value valueOf(int id) {
        refract.app.ResourceTable.Value v = table().value(id);
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
        refract.app.ResourceTable.Value v = table().value(id);
        return v != null && v.type == refract.app.ResourceTable.TYPE_STRING ? table().string(v.data) : def;
    }
    // Unresolvable strings read as empty rather than aborting the app.
    public String getString(int id) throws NotFoundException {
        refract.app.ResourceTable.Value v = table().value(id);
        String s = v != null && v.type == refract.app.ResourceTable.TYPE_STRING ? table().string(v.data) : null;
        return s != null ? s : "";
    }
    public String getString(int id, Object... formatArgs) throws NotFoundException {
        return String.format(getString(id), formatArgs);
    }
    public String[] getStringArray(int id) throws NotFoundException { return new String[0]; }
    public int[] getIntArray(int id) throws NotFoundException { throw missing(id); }
    public boolean getBoolean(int id) throws NotFoundException { return valueOf(id).data != 0; }
    public int getInteger(int id) throws NotFoundException { return valueOf(id).data; }
    public float getDimension(int id) throws NotFoundException { throw missing(id); }
    public int getDimensionPixelSize(int id) throws NotFoundException { throw missing(id); }
    public int getDimensionPixelOffset(int id) throws NotFoundException { throw missing(id); }
    public int getColor(int id) throws NotFoundException { return valueOf(id).data; }
    public int getColor(int id, Resources.Theme theme) throws NotFoundException { return valueOf(id).data; }
    public android.graphics.drawable.Drawable getDrawable(int id) throws NotFoundException { throw missing(id); }
    public android.graphics.drawable.Drawable getDrawable(int id, Resources.Theme theme) throws NotFoundException {
        throw missing(id);
    }
    public java.io.InputStream openRawResource(int id) throws NotFoundException { throw missing(id); }
    public AssetFileDescriptor openRawResourceFd(int id) throws NotFoundException { throw missing(id); }
    public XmlResourceParser getXml(int id) throws NotFoundException { throw missing(id); }
    public XmlResourceParser getLayout(int id) throws NotFoundException { throw missing(id); }
    public TypedArray obtainTypedArray(int id) throws NotFoundException { throw missing(id); }
    public TypedArray obtainAttributes(android.util.AttributeSet set, int[] attrs) { return null; }
    public final Theme newTheme() { return new Theme(); }

    public static class NotFoundException extends RuntimeException {
        public NotFoundException() {}
        public NotFoundException(String name) { super(name); }
        public NotFoundException(String name, Exception cause) { super(name, cause); }
    }

    /** Empty theme: every attribute is unset, so callers fall back to their defaults. */
    public final class Theme {
        public TypedArray obtainStyledAttributes(int[] attrs) { return new TypedArray(); }
        public TypedArray obtainStyledAttributes(int resid, int[] attrs) throws NotFoundException { return new TypedArray(); }
        public TypedArray obtainStyledAttributes(android.util.AttributeSet set, int[] attrs, int defStyleAttr, int defStyleRes) {
            return new TypedArray();
        }
        public void applyStyle(int resid, boolean force) {}
        public void setTo(Theme other) {}
        public boolean resolveAttribute(int resid, android.util.TypedValue outValue, boolean resolveRefs) { return false; }
        public Resources getResources() { return Resources.this; }
    }
}
