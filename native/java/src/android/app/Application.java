package android.app;

import android.content.ComponentCallbacks2;
import android.content.ContextWrapper;
import android.content.res.Configuration;
import android.os.Bundle;
import java.util.ArrayList;

public class Application extends ContextWrapper implements ComponentCallbacks2 {
    private final ArrayList<ActivityLifecycleCallbacks> mActivityLifecycleCallbacks = new ArrayList<>();

    public Application() { super(null); }

    public void onCreate() {}
    public void onTerminate() {}
    public void onConfigurationChanged(Configuration newConfig) {}
    public void onLowMemory() {}
    public void onTrimMemory(int level) {}

    public void registerActivityLifecycleCallbacks(ActivityLifecycleCallbacks callback) {
        synchronized (mActivityLifecycleCallbacks) {
            mActivityLifecycleCallbacks.add(callback);
        }
    }
    public void unregisterActivityLifecycleCallbacks(ActivityLifecycleCallbacks callback) {
        synchronized (mActivityLifecycleCallbacks) {
            mActivityLifecycleCallbacks.remove(callback);
        }
    }
    ActivityLifecycleCallbacks[] refractCallbacks() {
        synchronized (mActivityLifecycleCallbacks) {
            return mActivityLifecycleCallbacks.toArray(new ActivityLifecycleCallbacks[0]);
        }
    }

    public void registerOnProvideAssistDataListener(OnProvideAssistDataListener callback) {}
    public void unregisterOnProvideAssistDataListener(OnProvideAssistDataListener callback) {}
    public static String getProcessName() { return refract.Runtime.PACKAGE; }

    public interface ActivityLifecycleCallbacks {
        default void onActivityPreCreated(Activity activity, Bundle savedInstanceState) {}
        void onActivityCreated(Activity activity, Bundle savedInstanceState);
        default void onActivityPostCreated(Activity activity, Bundle savedInstanceState) {}
        default void onActivityPreStarted(Activity activity) {}
        void onActivityStarted(Activity activity);
        default void onActivityPostStarted(Activity activity) {}
        default void onActivityPreResumed(Activity activity) {}
        void onActivityResumed(Activity activity);
        default void onActivityPostResumed(Activity activity) {}
        default void onActivityPrePaused(Activity activity) {}
        void onActivityPaused(Activity activity);
        default void onActivityPostPaused(Activity activity) {}
        default void onActivityPreStopped(Activity activity) {}
        void onActivityStopped(Activity activity);
        default void onActivityPostStopped(Activity activity) {}
        default void onActivityPreSaveInstanceState(Activity activity, Bundle outState) {}
        void onActivitySaveInstanceState(Activity activity, Bundle outState);
        default void onActivityPostSaveInstanceState(Activity activity, Bundle outState) {}
        default void onActivityPreDestroyed(Activity activity) {}
        void onActivityDestroyed(Activity activity);
        default void onActivityPostDestroyed(Activity activity) {}
    }

    public interface OnProvideAssistDataListener {
        void onProvideAssistData(Activity activity, Bundle data);
    }
}
