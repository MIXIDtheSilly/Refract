package refract.app;

import android.content.Context;
import android.content.res.ColorStateList;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;

/** Bodies of Context's final methods (ShimBuilder routes them here). */
public final class ContextSupport {
    private ContextSupport() {}

    private static Resources.Theme theme(Context c) {
        Resources.Theme t = c.getTheme();
        return t != null ? t : Resources.getSystem().newTheme();
    }

    public static CharSequence getText(Context c, int id) { return c.getResources().getText(id); }
    public static String getString(Context c, int id) { return c.getResources().getString(id); }
    public static String getString(Context c, int id, Object[] args) { return c.getResources().getString(id, args); }
    public static int getColor(Context c, int id) { return c.getResources().getColor(id, c.getTheme()); }
    public static Drawable getDrawable(Context c, int id) { return c.getResources().getDrawable(id, c.getTheme()); }
    public static ColorStateList getColorStateList(Context c, int id) { return c.getResources().getColorStateList(id, c.getTheme()); }

    public static TypedArray obtainStyledAttributes(Context c, int[] attrs) {
        return theme(c).obtainStyledAttributes(attrs);
    }
    public static TypedArray obtainStyledAttributes(Context c, int resid, int[] attrs) {
        return theme(c).obtainStyledAttributes(resid, attrs);
    }
    public static TypedArray obtainStyledAttributes(Context c, AttributeSet set, int[] attrs) {
        return theme(c).obtainStyledAttributes(set, attrs, 0, 0);
    }
    public static TypedArray obtainStyledAttributes(Context c, AttributeSet set, int[] attrs, int defStyleAttr, int defStyleRes) {
        return theme(c).obtainStyledAttributes(set, attrs, defStyleAttr, defStyleRes);
    }

    public static Object getSystemService(Context c, Class<?> serviceClass) {
        String name = c.getSystemServiceName(serviceClass);
        return name != null ? c.getSystemService(name) : null;
    }
}
