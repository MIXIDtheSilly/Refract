package refract.view;

import android.view.Display;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;

public final class WindowManagerImpl implements WindowManager {
    private static WindowManagerImpl instance;
    private final Display display = new Display();

    public static synchronized WindowManagerImpl get() {
        if (instance == null) instance = new WindowManagerImpl();
        return instance;
    }

    @Override public Display getDefaultDisplay() { return display; }
    @Override public void removeViewImmediate(View view) {}
    @Override public void addView(View view, ViewGroup.LayoutParams params) {
        refract.Runtime.log(5, "WindowManager", "addView(" + view.getClass().getName() + ") ignored");
    }
    @Override public void updateViewLayout(View view, ViewGroup.LayoutParams params) {}
    @Override public void removeView(View view) {}
}
