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
        a.onCreate((Bundle) null);
        a.mStopped = false;
        a.onStart();
        a.onPostCreate((Bundle) null);
        a.onResume();
        a.mResumed = true;
        a.onPostResume();
        a.mWindow.show();
        a.onAttachedToWindow();
        a.mWindow.setFocus(true);
    }

    public static void pause(Activity a) {
        if (!a.mResumed) return;
        a.mWindow.setFocus(false);
        a.mResumed = false;
        a.onPause();
    }

    public static void resume(Activity a) {
        if (a.mResumed) return;
        a.onResume();
        a.mResumed = true;
        a.onPostResume();
        a.mWindow.setFocus(true);
    }

    public static void destroy(Activity a) {
        pause(a);
        if (!a.mStopped) {
            a.mStopped = true;
            a.onStop();
        }
        a.onDestroy();
        refract.Runtime.log(4, "refract", "activity destroyed; exiting");
        java.lang.Runtime.getRuntime().halt(0);
    }
}
