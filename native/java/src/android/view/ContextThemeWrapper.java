package android.view;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.res.Configuration;
import android.content.res.Resources;

/** A context with its own theme (Activity's base class). */
public class ContextThemeWrapper extends ContextWrapper {
    private int mThemeResource;
    private Resources.Theme mTheme;
    private LayoutInflater mInflater;
    private Configuration mOverrideConfiguration;

    public ContextThemeWrapper() { super(null); }
    public ContextThemeWrapper(Context base, int themeResId) {
        super(base);
        mThemeResource = themeResId;
    }
    public ContextThemeWrapper(Context base, Resources.Theme theme) {
        super(base);
        mTheme = theme;
    }

    @Override protected void attachBaseContext(Context newBase) { super.attachBaseContext(newBase); }
    public void applyOverrideConfiguration(Configuration overrideConfiguration) { mOverrideConfiguration = overrideConfiguration; }
    @Override public Resources getResources() { return super.getResources(); }

    public void setTheme(int resid) {
        if (mThemeResource != resid) {
            mThemeResource = resid;
            initializeTheme();
        }
    }
    public void setTheme(Resources.Theme theme) { mTheme = theme; }
    public int getThemeResId() { return mThemeResource; }

    @Override public Resources.Theme getTheme() {
        if (mTheme != null) return mTheme;
        if (mThemeResource == 0)
            mThemeResource = getResources().getIdentifier("Theme.DeviceDefault", "style", "android");
        initializeTheme();
        return mTheme;
    }

    @Override public Object getSystemService(String name) {
        if (LAYOUT_INFLATER_SERVICE.equals(name)) {
            if (mInflater == null) mInflater = LayoutInflater.from(getBaseContext()).cloneInContext(this);
            return mInflater;
        }
        return getBaseContext().getSystemService(name);
    }

    protected void onApplyThemeResource(Resources.Theme theme, int resid, boolean first) { theme.applyStyle(resid, true); }

    private void initializeTheme() {
        boolean first = mTheme == null;
        if (first) {
            mTheme = getResources().newTheme();
            Resources.Theme base = getBaseContext() != null ? getBaseContext().getTheme() : null;
            if (base != null) mTheme.setTo(base);
        }
        onApplyThemeResource(mTheme, mThemeResource, first);
    }
}
