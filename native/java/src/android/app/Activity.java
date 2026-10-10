package android.app;

import android.content.ComponentName;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;
import refract.view.WindowImpl;

// Implements Window.Callback and KeyEvent.Callback through the merged android.jar class
// (declaring them here would require every callback method).
public class Activity extends android.view.ContextThemeWrapper {
    public static final int RESULT_OK = -1;
    public static final int RESULT_CANCELED = 0;
    public static final int RESULT_FIRST_USER = 1;

    Application mApplication;
    Intent mIntent;
    WindowImpl mWindow;
    ComponentName mComponent;
    boolean mFinished;
    boolean mResumed;
    boolean mStopped = true;
    int mRequestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE;
    Handler mHandler;
    final OnBackInvokedDispatcher mBackDispatcher = new OnBackInvokedDispatcher() {
        public void registerOnBackInvokedCallback(int priority, OnBackInvokedCallback callback) {}
        public void unregisterOnBackInvokedCallback(OnBackInvokedCallback callback) {}
    };

    public Activity() {}

    /** Called by RefractActivityThread before onCreate. */
    void refractAttach(Context base, Application app, Intent intent, ComponentName component) {
        refractAttach(base);
        mApplication = app;
        mIntent = intent;
        mComponent = component;
        mHandler = new Handler(Looper.getMainLooper());
        mWindow = new WindowImpl(this);
        mWindow.setCallback((Window.Callback) (Object) this);
    }

    protected void onCreate(Bundle savedInstanceState) {}
    protected void onPostCreate(Bundle savedInstanceState) {}
    protected void onStart() {}
    protected void onRestart() {}
    protected void onResume() {}
    protected void onPostResume() {}
    protected void onPause() {}
    protected void onStop() {}
    protected void onDestroy() {}
    protected void onNewIntent(Intent intent) {}
    protected void onSaveInstanceState(Bundle outState) {}
    protected void onRestoreInstanceState(Bundle savedInstanceState) {}
    public void onWindowFocusChanged(boolean hasFocus) {}
    public void onAttachedToWindow() {}
    public void onDetachedFromWindow() {}
    public void onConfigurationChanged(android.content.res.Configuration newConfig) {}
    public void onLowMemory() {}
    public void onTrimMemory(int level) {}
    public void onUserInteraction() {}
    public void onContentChanged() {}
    public void onWindowAttributesChanged(WindowManager.LayoutParams params) {}
    public void onBackPressed() { finish(); }
    public void onMultiWindowModeChanged(boolean isInMultiWindowMode) {}
    public void onPictureInPictureModeChanged(boolean isInPictureInPictureMode) {}
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {}

    public Window getWindow() { return mWindow; }
    public WindowManager getWindowManager() { return mWindow.getWindowManager(); }
    public void setContentView(View view) { mWindow.setContentView(view); }
    public void setContentView(View view, ViewGroup.LayoutParams params) { mWindow.setContentView(view, params); }
    public void setContentView(int layoutResID) { mWindow.setContentView(layoutResID); }
    public void addContentView(View view, ViewGroup.LayoutParams params) { mWindow.addContentView(view, params); }
    public <T extends View> T findViewById(int id) { return mWindow.findViewById(id); }
    public View getCurrentFocus() { return mWindow.getDecorView(); }
    public final boolean requestWindowFeature(int featureId) { return true; }

    public Intent getIntent() { return mIntent; }
    public void setIntent(Intent newIntent) { mIntent = newIntent; }
    public final Application getApplication() { return mApplication; }
    public ComponentName getComponentName() { return mComponent; }
    public String getLocalClassName() { return getClass().getName(); }
    public ComponentName getCallingActivity() { return null; }
    public String getCallingPackage() { return null; }
    public boolean isTaskRoot() { return true; }
    public int getTaskId() { return 1; }
    public boolean isFinishing() { return mFinished; }
    public boolean isDestroyed() { return false; }
    public boolean isChangingConfigurations() { return false; }
    public boolean isInMultiWindowMode() { return false; }
    public boolean isInPictureInPictureMode() { return false; }
    public boolean hasWindowFocus() { return mResumed; }
    public final boolean isChild() { return false; }
    public final Activity getParent() { return null; }
    public int getRequestedOrientation() { return mRequestedOrientation; }
    public void setRequestedOrientation(int orientation) { mRequestedOrientation = orientation; }
    public void setTitle(CharSequence title) {}
    public void setTitle(int titleId) {}
    public ActionBar getActionBar() { return null; }
    public FragmentManager getFragmentManager() { return null; }
    public OnBackInvokedDispatcher getOnBackInvokedDispatcher() { return mBackDispatcher; }

    public final void runOnUiThread(Runnable action) {
        if (Looper.myLooper() == Looper.getMainLooper()) action.run();
        else mHandler.post(action);
    }

    public void finish() {
        if (mFinished) return;
        mFinished = true;
        refract.Runtime.log(4, "Activity", "finish() -> closing the app");
        mHandler.post(() -> RefractActivityThread.destroy(this));
    }
    public void finishAffinity() { finish(); }
    public void finishAndRemoveTask() { finish(); }
    public boolean moveTaskToBack(boolean nonRoot) { return false; }

    public void startActivity(Intent intent) { refract.Runtime.log(4, "Activity", "startActivity ignored: " + intent); }
    public void startActivity(Intent intent, Bundle options) { startActivity(intent); }
    public void startActivityForResult(Intent intent, int requestCode) { startActivity(intent); }
    public void startActivityForResult(Intent intent, int requestCode, Bundle options) { startActivity(intent); }
    public final void setResult(int resultCode) {}
    public final void setResult(int resultCode, Intent data) {}

    public final void requestPermissions(String[] permissions, int requestCode) {
        int[] granted = new int[permissions.length];
        mHandler.post(() -> onRequestPermissionsResult(requestCode, permissions, granted));
    }
    public boolean shouldShowRequestPermissionRationale(String permission) { return false; }

    @Override
    public Object getSystemService(String name) {
        if (WINDOW_SERVICE.equals(name)) return getWindowManager();
        return super.getSystemService(name);
    }

    // Window.Callback
    public boolean dispatchKeyEvent(KeyEvent event) { return mWindow.getDecorView().dispatchKeyEvent(event); }
    public boolean dispatchTouchEvent(MotionEvent ev) { return mWindow.getDecorView().dispatchTouchEvent(ev); }
    public boolean dispatchGenericMotionEvent(MotionEvent ev) { return false; }
    public boolean dispatchTrackballEvent(MotionEvent ev) { return false; }
    public boolean dispatchKeyShortcutEvent(KeyEvent event) { return false; }
    public boolean onKeyDown(int keyCode, KeyEvent event) { return false; }
    public boolean onKeyLongPress(int keyCode, KeyEvent event) { return false; }
    public boolean onKeyUp(int keyCode, KeyEvent event) { return false; }
    public boolean onKeyMultiple(int keyCode, int repeatCount, KeyEvent event) { return false; }
    public boolean onTouchEvent(MotionEvent event) { return false; }
    public boolean onGenericMotionEvent(MotionEvent event) { return false; }
}
