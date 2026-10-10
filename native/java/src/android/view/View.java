package android.view;

import android.content.Context;
import android.graphics.Rect;
import android.os.Handler;
import android.os.Looper;
import android.util.AttributeSet;
import java.util.ArrayList;

/** Minimal view: tree structure, layout bounds, attach/focus dispatch and posting. */
public class View {
    public static final int VISIBLE = 0;
    public static final int INVISIBLE = 4;
    public static final int GONE = 8;
    public static final int NO_ID = -1;

    Context mContext;
    ViewParent mParent;
    ViewGroup.LayoutParams mLayoutParams;
    int mId = NO_ID;
    int mVisibility = VISIBLE;
    int mLeft, mTop, mRight, mBottom;
    boolean mAttached;
    boolean mWindowFocus;
    boolean mFocusable = true;
    Object mTag;
    ViewTreeObserver mObserver;
    OnTouchListener mTouchListener;
    OnKeyListener mKeyListener;
    OnClickListener mClickListener;
    OnGenericMotionListener mGenericListener;
    OnSystemUiVisibilityChangeListener mSystemUiListener;
    OnAttachStateChangeListener[] mAttachListeners = new OnAttachStateChangeListener[0];
    int mSystemUiVisibility;

    static Handler sMainHandler;

    static Handler mainHandler() {
        if (sMainHandler == null) sMainHandler = new Handler(Looper.getMainLooper());
        return sMainHandler;
    }

    public View(Context context) { mContext = context; }
    public View(Context context, AttributeSet attrs) { this(context); }
    public View(Context context, AttributeSet attrs, int defStyleAttr) { this(context); }
    public View(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { this(context); }

    public final Context getContext() { return mContext; }
    public int getId() { return mId; }
    public void setId(int id) { mId = id; }
    public final ViewParent getParent() { return mParent; }
    public View getRootView() {
        View v = this;
        while (v.mParent instanceof View p) v = p;
        return v;
    }
    public final <T extends View> T findViewById(int id) { return (T) findViewTraversal(id); }
    View findViewTraversal(int id) { return id == mId ? this : null; }
    public Object getTag() { return mTag; }
    public void setTag(Object tag) { mTag = tag; }

    public ViewGroup.LayoutParams getLayoutParams() { return mLayoutParams; }
    public void setLayoutParams(ViewGroup.LayoutParams params) { mLayoutParams = params; requestLayout(); }

    public int getVisibility() { return mVisibility; }
    public void setVisibility(int visibility) {
        int old = mVisibility;
        mVisibility = visibility;
        if (old != visibility) onVisibilityChanged(this, visibility);
    }
    protected void onVisibilityChanged(View changedView, int visibility) {}
    public boolean isShown() { return mVisibility == VISIBLE && mAttached; }

    public final int getWidth() { return mRight - mLeft; }
    public final int getHeight() { return mBottom - mTop; }
    public final int getLeft() { return mLeft; }
    public final int getTop() { return mTop; }
    public final int getRight() { return mRight; }
    public final int getBottom() { return mBottom; }
    public final int getMeasuredWidth() { return getWidth(); }
    public final int getMeasuredHeight() { return getHeight(); }
    public void getDrawingRect(Rect r) { r.set(0, 0, getWidth(), getHeight()); }
    public void getWindowVisibleDisplayFrame(Rect r) {
        View root = getRootView();
        r.set(0, 0, root.getWidth(), root.getHeight());
    }
    public boolean getGlobalVisibleRect(Rect r) { r.set(mLeft, mTop, mRight, mBottom); return true; }
    public void getLocationOnScreen(int[] out) { out[0] = mLeft; out[1] = mTop; }
    public void getLocationInWindow(int[] out) { out[0] = mLeft; out[1] = mTop; }

    public void layout(int l, int t, int r, int b) {
        boolean changed = l != mLeft || t != mTop || r != mRight || b != mBottom;
        int ow = getWidth(), oh = getHeight();
        mLeft = l;
        mTop = t;
        mRight = r;
        mBottom = b;
        if (changed) onSizeChanged(r - l, b - t, ow, oh);
        onLayout(changed, l, t, r, b);
        if (changed && mObserver != null) mObserver.refractDispatchGlobalLayout();
    }
    protected void onLayout(boolean changed, int l, int t, int r, int b) {}
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {}
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {}
    public final void measure(int widthMeasureSpec, int heightMeasureSpec) { onMeasure(widthMeasureSpec, heightMeasureSpec); }
    protected final void setMeasuredDimension(int w, int h) {}
    public void requestLayout() {}
    public void forceLayout() {}
    public void invalidate() {}
    public void postInvalidate() {}
    public void postInvalidateOnAnimation() {}

    public boolean post(Runnable action) { return mainHandler().post(action); }
    public boolean postDelayed(Runnable action, long delayMillis) { return mainHandler().postDelayed(action, delayMillis); }
    public void postOnAnimation(Runnable action) { mainHandler().post(action); }
    public void postOnAnimationDelayed(Runnable action, long delayMillis) { mainHandler().postDelayed(action, delayMillis); }
    public boolean removeCallbacks(Runnable action) { mainHandler().removeCallbacks(action); return true; }
    public Handler getHandler() { return mAttached ? mainHandler() : null; }

    public ViewTreeObserver getViewTreeObserver() {
        View root = getRootView();
        if (root.mObserver == null) root.mObserver = new ViewTreeObserver();
        return root.mObserver;
    }

    // Focus
    public boolean requestFocus() { return true; }
    public final boolean requestFocus(int direction) { return true; }
    public boolean requestFocus(int direction, Rect previouslyFocusedRect) { return true; }
    public void setFocusable(boolean focusable) { mFocusable = focusable; }
    public void setFocusable(int focusable) { mFocusable = focusable != 0; }
    public void setFocusableInTouchMode(boolean focusable) {}
    public boolean isFocusable() { return mFocusable; }
    public boolean isFocused() { return mWindowFocus; }
    public boolean hasFocus() { return mWindowFocus; }
    public boolean hasWindowFocus() { return mWindowFocus; }
    public void clearFocus() {}
    public void onWindowFocusChanged(boolean hasWindowFocus) {}
    public void dispatchWindowFocusChanged(boolean hasFocus) {
        mWindowFocus = hasFocus;
        onWindowFocusChanged(hasFocus);
    }

    // Attachment
    public boolean isAttachedToWindow() { return mAttached; }
    protected void onAttachedToWindow() {}
    protected void onDetachedFromWindow() {}
    void refractDispatchAttached() {
        mAttached = true;
        onAttachedToWindow();
        for (OnAttachStateChangeListener l : mAttachListeners) l.onViewAttachedToWindow(this);
    }
    void refractDispatchDetached() {
        onDetachedFromWindow();
        for (OnAttachStateChangeListener l : mAttachListeners) l.onViewDetachedFromWindow(this);
        mAttached = false;
    }
    public void addOnAttachStateChangeListener(OnAttachStateChangeListener l) {
        ArrayList<OnAttachStateChangeListener> list = new ArrayList<>(java.util.List.of(mAttachListeners));
        list.add(l);
        mAttachListeners = list.toArray(new OnAttachStateChangeListener[0]);
    }
    public void removeOnAttachStateChangeListener(OnAttachStateChangeListener l) {
        ArrayList<OnAttachStateChangeListener> list = new ArrayList<>(java.util.List.of(mAttachListeners));
        list.remove(l);
        mAttachListeners = list.toArray(new OnAttachStateChangeListener[0]);
    }

    // Input listeners (no input is delivered yet; VR input comes through OpenXR)
    public void setOnTouchListener(OnTouchListener l) { mTouchListener = l; }
    public void setOnKeyListener(OnKeyListener l) { mKeyListener = l; }
    public void setOnClickListener(OnClickListener l) { mClickListener = l; }
    public void setOnGenericMotionListener(OnGenericMotionListener l) { mGenericListener = l; }
    public boolean onTouchEvent(MotionEvent event) { return false; }
    public boolean onGenericMotionEvent(MotionEvent event) { return false; }
    public boolean onKeyDown(int keyCode, KeyEvent event) { return false; }
    public boolean onKeyUp(int keyCode, KeyEvent event) { return false; }
    public boolean onKeyMultiple(int keyCode, int count, KeyEvent event) { return false; }
    public boolean dispatchTouchEvent(MotionEvent event) {
        if (mTouchListener != null && mTouchListener.onTouch(this, event)) return true;
        return onTouchEvent(event);
    }
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (mKeyListener != null && mKeyListener.onKey(this, event.getKeyCode(), event)) return true;
        return event.getAction() == KeyEvent.ACTION_DOWN ? onKeyDown(event.getKeyCode(), event)
                                                         : onKeyUp(event.getKeyCode(), event);
    }
    public boolean performClick() {
        if (mClickListener != null) {
            mClickListener.onClick(this);
            return true;
        }
        return false;
    }

    public void setSystemUiVisibility(int visibility) {
        mSystemUiVisibility = visibility;
        if (mSystemUiListener != null) mSystemUiListener.onSystemUiVisibilityChange(visibility);
    }
    public int getSystemUiVisibility() { return mSystemUiVisibility; }
    public void setOnSystemUiVisibilityChangeListener(OnSystemUiVisibilityChangeListener l) { mSystemUiListener = l; }
    public void setKeepScreenOn(boolean keepScreenOn) {}
    public void setBackgroundColor(int color) {}
    public void setAlpha(float alpha) {}
    public void setWillNotDraw(boolean willNotDraw) {}
    public Display getDisplay() { return refract.view.WindowManagerImpl.get().getDefaultDisplay(); }
    public WindowInsets getRootWindowInsets() { return null; }
    public android.os.IBinder getWindowToken() { return null; }
    public android.content.res.Resources getResources() { return mContext.getResources(); }

    // Nested types this file uses (same members as android.jar).
    public interface OnTouchListener {
        boolean onTouch(View v, MotionEvent event);
    }
    public interface OnKeyListener {
        boolean onKey(View v, int keyCode, KeyEvent event);
    }
    public interface OnClickListener {
        void onClick(View v);
    }
    public interface OnGenericMotionListener {
        boolean onGenericMotion(View v, MotionEvent event);
    }
    public interface OnSystemUiVisibilityChangeListener {
        void onSystemUiVisibilityChange(int visibility);
    }
    public interface OnAttachStateChangeListener {
        void onViewAttachedToWindow(View v);
        void onViewDetachedFromWindow(View v);
    }
}
