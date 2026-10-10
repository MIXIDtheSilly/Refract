package android.view;

import java.util.concurrent.CopyOnWriteArrayList;

public final class ViewTreeObserver {
    private final CopyOnWriteArrayList<OnGlobalLayoutListener> mGlobalLayout = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnPreDrawListener> mPreDraw = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnWindowFocusChangeListener> mFocus = new CopyOnWriteArrayList<>();

    public ViewTreeObserver() {}

    public boolean isAlive() { return true; }
    public void addOnGlobalLayoutListener(OnGlobalLayoutListener l) { mGlobalLayout.add(l); }
    public void removeOnGlobalLayoutListener(OnGlobalLayoutListener l) { mGlobalLayout.remove(l); }
    @Deprecated public void removeGlobalOnLayoutListener(OnGlobalLayoutListener l) { mGlobalLayout.remove(l); }
    public void addOnPreDrawListener(OnPreDrawListener l) { mPreDraw.add(l); }
    public void removeOnPreDrawListener(OnPreDrawListener l) { mPreDraw.remove(l); }
    public void addOnWindowFocusChangeListener(OnWindowFocusChangeListener l) { mFocus.add(l); }
    public void removeOnWindowFocusChangeListener(OnWindowFocusChangeListener l) { mFocus.remove(l); }

    public void dispatchOnGlobalLayout() {
        for (OnGlobalLayoutListener l : mGlobalLayout) l.onGlobalLayout();
    }

    void refractDispatchGlobalLayout() { dispatchOnGlobalLayout(); }

    void refractDispatchFocus(boolean focus) {
        for (OnWindowFocusChangeListener l : mFocus) l.onWindowFocusChanged(focus);
    }

    public boolean dispatchOnPreDraw() {
        boolean cancel = false;
        for (OnPreDrawListener l : mPreDraw) cancel |= !l.onPreDraw();
        return cancel;
    }

    public interface OnGlobalLayoutListener {
        void onGlobalLayout();
    }
    public interface OnPreDrawListener {
        boolean onPreDraw();
    }
    public interface OnWindowFocusChangeListener {
        void onWindowFocusChanged(boolean hasFocus);
    }
}
