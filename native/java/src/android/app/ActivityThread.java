package android.app;

import android.content.Context;

/** The hidden ActivityThread statics that libraries reach by reflection/JNI to find the app. */
public final class ActivityThread {
    private static final ActivityThread sCurrent = new ActivityThread();

    private ActivityThread() {}

    public static ActivityThread currentActivityThread() { return sCurrent; }
    public static Application currentApplication() { return refract.Launcher.application; }
    public static String currentPackageName() { return refract.Runtime.PACKAGE; }
    public static String currentOpPackageName() { return refract.Runtime.PACKAGE; }
    public static String currentProcessName() { return refract.Runtime.PACKAGE; }
    public Application getApplication() { return currentApplication(); }
    public Context getSystemContext() {
        Application app = currentApplication();
        return app != null ? app.getBaseContext() : null;
    }
    public String getProcessName() { return refract.Runtime.PACKAGE; }
}
