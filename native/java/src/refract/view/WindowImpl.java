package refract.view;

import android.content.Context;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;

/** PhoneWindow stand-in: a decor FrameLayout sized to the host window. */
public class WindowImpl extends Window {
    private final Context context;
    private final FrameLayout decor;
    private final WindowManager.LayoutParams attrs = getAttributes();
    private boolean shown;

    public WindowImpl(Context context) {
        super(context);
        this.context = context;
        decor = new FrameLayout(context);
        attrs.width = WindowManager.LayoutParams.MATCH_PARENT;
        attrs.height = WindowManager.LayoutParams.MATCH_PARENT;
    }

    /** Shows the window: attaches the view tree, lays it out and gives it focus. */
    public void show() {
        if (shown) return;
        shown = true;
        refractSetActive(true);
        ViewGroup.refractAttachRoot(decor);
        decor.layout(0, 0, NativeWindows.WIDTH, NativeWindows.HEIGHT);
    }

    public void setFocus(boolean focus) {
        ViewGroup.refractFocusRoot(decor, focus);
        if (getCallback() != null) getCallback().onWindowFocusChanged(focus);
    }

    public View getDecorView() { return decor; }
    public View peekDecorView() { return decor; }
    public void setContentView(View view) { setContentView(view, null); }
    public void setContentView(int layoutResID) {
        refract.Runtime.log(5, "Window", "setContentView(layout 0x" + Integer.toHexString(layoutResID) + ") ignored");
    }
    public void setContentView(View view, ViewGroup.LayoutParams params) {
        decor.removeAllViews();
        if (params != null) decor.addView(view, params);
        else decor.addView(view);
    }
    public void addContentView(View view, ViewGroup.LayoutParams params) { decor.addView(view, params); }
    public <T extends View> T findViewById(int id) { return decor.findViewById(id); }
    public void setAttributes(WindowManager.LayoutParams a) { attrs.copyFrom(a); }
    public void addFlags(int flags) { attrs.flags |= flags; }
    public void clearFlags(int flags) { attrs.flags &= ~flags; }
    public void setFlags(int flags, int mask) { attrs.flags = (attrs.flags & ~mask) | (flags & mask); }
    public void setFormat(int format) { attrs.format = format; }
    public void setLayout(int width, int height) {}
    public WindowManager getWindowManager() { return WindowManagerImpl.get(); }
    public boolean requestFeature(int featureId) { return true; }
    public boolean hasFeature(int feature) { return false; }
    public boolean isFloating() { return false; }
    public void takeSurface(android.view.SurfaceHolder.Callback2 cb) {
        refract.Runtime.log(5, "Window", "takeSurface is not supported yet");
    }
    public void takeInputQueue(android.view.InputQueue.Callback cb) {}
    public android.view.WindowInsetsController getInsetsController() { return null; }
    public void setDecorFitsSystemWindows(boolean fit) {}
    public android.view.LayoutInflater getLayoutInflater() { return null; }
    public View getCurrentFocus() { return decor; }
}
