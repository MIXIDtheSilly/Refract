package android.view;

import android.content.Context;
import android.util.AttributeSet;
import java.util.ArrayList;

/** Children list plus FrameLayout-like layout (every child fills the parent). */
public abstract class ViewGroup extends View implements ViewParent, ViewManager {
    final ArrayList<View> mChildren = new ArrayList<>();

    public ViewGroup(Context context) { super(context); }
    public ViewGroup(Context context, AttributeSet attrs) { super(context); }
    public ViewGroup(Context context, AttributeSet attrs, int defStyleAttr) { super(context); }
    public ViewGroup(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context); }

    public int getChildCount() { return mChildren.size(); }
    public View getChildAt(int index) { return index >= 0 && index < mChildren.size() ? mChildren.get(index) : null; }
    public int indexOfChild(View child) { return mChildren.indexOf(child); }

    public void addView(View child) { addView(child, -1, child.getLayoutParams()); }
    public void addView(View child, int index) { addView(child, index, child.getLayoutParams()); }
    public void addView(View child, int width, int height) { addView(child, -1, new LayoutParams(width, height)); }
    public void addView(View child, LayoutParams params) { addView(child, -1, params); }

    public void addView(View child, int index, LayoutParams params) {
        if (child.mParent != null) throw new IllegalStateException("The specified child already has a parent.");
        if (params == null) params = generateDefaultLayoutParams();
        child.mLayoutParams = params;
        child.mParent = this;
        if (index < 0 || index > mChildren.size()) mChildren.add(child);
        else mChildren.add(index, child);
        if (mAttached) refractAttachTree(child);
        if (getWidth() > 0) child.layout(0, 0, getWidth(), getHeight());
        onViewAdded(child);
    }

    public void updateViewLayout(View view, LayoutParams params) { view.setLayoutParams(params); }

    public void removeView(View view) {
        if (!mChildren.remove(view)) return;
        if (view.mAttached) refractDetachTree(view);
        view.mParent = null;
        onViewRemoved(view);
    }

    public void removeViewAt(int index) { removeView(getChildAt(index)); }
    public void removeViewInLayout(View view) { removeView(view); }
    public void removeAllViews() {
        for (View v : new ArrayList<>(mChildren)) removeView(v);
    }
    public void removeAllViewsInLayout() { removeAllViews(); }
    public void bringChildToFront(View child) {
        if (mChildren.remove(child)) mChildren.add(child);
    }
    public void onViewAdded(View child) {}
    public void onViewRemoved(View child) {}

    protected LayoutParams generateDefaultLayoutParams() {
        return new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT);
    }
    public LayoutParams generateLayoutParams(AttributeSet attrs) { return generateDefaultLayoutParams(); }
    protected boolean checkLayoutParams(LayoutParams p) { return p != null; }

    @Override
    protected void onLayout(boolean changed, int l, int t, int r, int b) {
        for (View c : mChildren) c.layout(0, 0, r - l, b - t);
    }

    @Override
    View findViewTraversal(int id) {
        if (id == mId) return this;
        for (View c : mChildren) {
            View v = c.findViewTraversal(id);
            if (v != null) return v;
        }
        return null;
    }

    static void refractAttachTree(View v) {
        v.refractDispatchAttached();
        if (v instanceof ViewGroup g)
            for (View c : new ArrayList<>(g.mChildren)) refractAttachTree(c);
    }

    static void refractDetachTree(View v) {
        if (v instanceof ViewGroup g)
            for (View c : new ArrayList<>(g.mChildren)) refractDetachTree(c);
        v.refractDispatchDetached();
    }

    @Override
    public void dispatchWindowFocusChanged(boolean hasFocus) {
        super.dispatchWindowFocusChanged(hasFocus);
        for (View c : new ArrayList<>(mChildren)) c.dispatchWindowFocusChanged(hasFocus);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent ev) {
        for (int i = mChildren.size() - 1; i >= 0; i--)
            if (mChildren.get(i).dispatchTouchEvent(ev)) return true;
        return super.dispatchTouchEvent(ev);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        for (int i = mChildren.size() - 1; i >= 0; i--)
            if (mChildren.get(i).dispatchKeyEvent(event)) return true;
        return super.dispatchKeyEvent(event);
    }

    public void requestChildFocus(View child, View focused) {}
    public void recomputeViewAttributes(View child) {}
    public void clearChildFocus(View child) {}
    public void requestDisallowInterceptTouchEvent(boolean disallowIntercept) {}
    public boolean onInterceptTouchEvent(MotionEvent ev) { return false; }
    public void setClipChildren(boolean clip) {}
    public void setClipToPadding(boolean clip) {}
    public void setDescendantFocusability(int focusability) {}

    /** Called by refract's window when the whole tree is shown. */
    public static void refractAttachRoot(View root) { refractAttachTree(root); }
    public static void refractDetachRoot(View root) { refractDetachTree(root); }
    public static void refractFocusRoot(View root, boolean focus) {
        root.dispatchWindowFocusChanged(focus);
        if (root.mObserver != null) root.mObserver.refractDispatchFocus(focus);
    }

    public static class LayoutParams {
        public static final int FILL_PARENT = -1;
        public static final int MATCH_PARENT = -1;
        public static final int WRAP_CONTENT = -2;
        public int width;
        public int height;

        public LayoutParams(int width, int height) {
            this.width = width;
            this.height = height;
        }

        public LayoutParams(LayoutParams source) {
            this.width = source.width;
            this.height = source.height;
        }
    }
}
