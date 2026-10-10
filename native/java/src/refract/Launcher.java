package refract;

import android.app.Activity;
import android.app.Application;
import android.app.RefractActivityThread;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Looper;
import refract.app.ContextImpl;

/** Process entry point (called by refract_native on the app's main thread). */
public final class Launcher {
    private Launcher() {}

    public static Application application;
    public static Activity activity;

    public static void main(String[] args) throws Throwable {
        Thread.currentThread().setName("main");
        Looper.prepareMainLooper();
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
            Runtime.log(6, "AndroidRuntime", "FATAL EXCEPTION: " + t.getName());
            e.printStackTrace();
            java.lang.Runtime.getRuntime().halt(1);
        });

        ContextImpl base = new ContextImpl();
        String appClass = System.getProperty("refract.application", "");
        application = appClass.isEmpty() ? new Application()
                : (Application) Class.forName(appClass).getDeclaredConstructor().newInstance();
        RefractActivityThread.attachApplication(application, base);
        ContextImpl.setApplicationContext(application);
        Runtime.log(4, "refract", "starting " + Runtime.PACKAGE + "/" + Runtime.ACTIVITY);
        application.onCreate();

        ComponentName component = new ComponentName(Runtime.PACKAGE, Runtime.ACTIVITY);
        Intent intent = new Intent(Intent.ACTION_MAIN);
        intent.addCategory(Intent.CATEGORY_LAUNCHER);
        intent.setComponent(component);
        activity = (Activity) Class.forName(Runtime.ACTIVITY).getDeclaredConstructor().newInstance();
        RefractActivityThread.launch(activity, base, application, intent, component);
        Runtime.log(4, "refract", "activity resumed; entering the main loop");
        Looper.loop();
    }
}
