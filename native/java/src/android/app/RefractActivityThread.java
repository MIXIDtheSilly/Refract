package android.app;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;

/** Drives the Activity lifecycle (ActivityThread's role); lives in android.app for access. */
public final class RefractActivityThread {
    private RefractActivityThread() {}

    public static void attachApplication(Application app, Context base) {
        app.refractAttach(base);
    }

    public static void launch(Activity a, Context base, Application app, Intent intent, ComponentName component) {
        a.refractAttach(base, app, intent, component);
        int theme = refract.app.Manifest.theme(true);
        if (theme != 0) a.setTheme(theme);
        a.refractDispatch(c -> c.onActivityPreCreated(a, null));
        a.onCreate((Bundle) null);
        a.refractDispatch(c -> c.onActivityPostCreated(a, null));
        a.mStopped = false;
        a.refractDispatch(c -> c.onActivityPreStarted(a));
        a.onStart();
        a.refractDispatch(c -> c.onActivityPostStarted(a));
        a.onPostCreate((Bundle) null);
        a.refractDispatch(c -> c.onActivityPreResumed(a));
        a.onResume();
        a.mResumed = true;
        a.onPostResume();
        a.refractDispatch(c -> c.onActivityPostResumed(a));
        a.mWindow.show();
        a.onAttachedToWindow();
        a.mWindow.setFocus(true);
    }

    public static void pause(Activity a) {
        if (!a.mResumed) return;
        a.mWindow.setFocus(false);
        a.mResumed = false;
        a.refractDispatch(c -> c.onActivityPrePaused(a));
        a.onPause();
        a.refractDispatch(c -> c.onActivityPostPaused(a));
    }

    public static void resume(Activity a) {
        if (a.mResumed) return;
        a.refractDispatch(c -> c.onActivityPreResumed(a));
        a.onResume();
        a.mResumed = true;
        a.onPostResume();
        a.refractDispatch(c -> c.onActivityPostResumed(a));
        a.mWindow.setFocus(true);
    }

    public static void destroy(Activity a) {
        pause(a);
        if (!a.mStopped) {
            a.mStopped = true;
            a.refractDispatch(c -> c.onActivityPreStopped(a));
            a.onStop();
            a.refractDispatch(c -> c.onActivityPostStopped(a));
        }
        a.refractDispatch(c -> c.onActivityPreDestroyed(a));
        a.onDestroy();
        a.refractDispatch(c -> c.onActivityPostDestroyed(a));
        refract.Runtime.log(4, "refract", "activity destroyed; exiting");
        java.lang.Runtime.getRuntime().halt(0);
    }
}
