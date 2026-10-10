package android.view;

import android.content.Context;

/** The state behind Window's final accessors (Refract's window is refract.view.WindowImpl). */
public abstract class Window {
    private final Context mContext;
    private final WindowManager.LayoutParams mWindowAttributes = new WindowManager.LayoutParams();
    private Callback mCallback;
    private boolean mIsActive;

    public Window(Context context) {
        mContext = context;
    }

    public final Context getContext() { return mContext; }
    public final WindowManager.LayoutParams getAttributes() { return mWindowAttributes; }
    public void setCallback(Callback callback) { mCallback = callback; }
    public final Callback getCallback() { return mCallback; }
    public final boolean isActive() { return mIsActive; }

    protected final void refractSetActive(boolean active) { mIsActive = active; }

    public interface Callback {
        boolean dispatchKeyEvent(KeyEvent event);
        boolean dispatchTouchEvent(MotionEvent event);
        void onWindowFocusChanged(boolean hasFocus);
        void onAttachedToWindow();
        void onDetachedFromWindow();
    }
}
