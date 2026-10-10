package android.view;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.util.AttributeSet;
import java.util.ArrayList;

/**
 * A SurfaceView owns a host window surface: when it is attached and laid out it
 * reports surfaceCreated/surfaceChanged with a Surface backed by refract's
 * native window, and surfaceDestroyed when detached.
 */
public class SurfaceView extends View {
    final ArrayList<SurfaceHolder.Callback> mCallbacks = new ArrayList<>();
    final Surface mSurface = new Surface();
    final Rect mFrame = new Rect();
    boolean mCreated;
    int mFormat = PixelFormat.RGBA_8888;
    int mFixedWidth = -1, mFixedHeight = -1;

    final SurfaceHolder mHolder = new SurfaceHolder() {
        public void addCallback(Callback callback) {
            synchronized (mCallbacks) {
                if (!mCallbacks.contains(callback)) mCallbacks.add(callback);
            }
        }
        public void removeCallback(Callback callback) {
            synchronized (mCallbacks) {
                mCallbacks.remove(callback);
            }
        }
        public boolean isCreating() { return false; }
        public void setType(int type) {}
        public void setFixedSize(int width, int height) {
            mFixedWidth = width;
            mFixedHeight = height;
            if (mCreated) notifyChanged();
        }
        public void setSizeFromLayout() {
            mFixedWidth = mFixedHeight = -1;
        }
        public void setFormat(int format) { mFormat = format; }
        public void setKeepScreenOn(boolean screenOn) {}
        public Canvas lockCanvas() { return null; }
        public Canvas lockCanvas(Rect dirty) { return null; }
        public void unlockCanvasAndPost(Canvas canvas) {}
        public Rect getSurfaceFrame() { return new Rect(mFrame); }
        public Surface getSurface() { return mSurface; }
    };

    public SurfaceView(Context context) { super(context); }
    public SurfaceView(Context context, AttributeSet attrs) { super(context); }
    public SurfaceView(Context context, AttributeSet attrs, int defStyleAttr) { super(context); }
    public SurfaceView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context); }

    public SurfaceHolder getHolder() { return mHolder; }
    public void setZOrderOnTop(boolean onTop) {}
    public void setZOrderMediaOverlay(boolean isMediaOverlay) {}
    public void setSecure(boolean isSecure) {}
    public boolean gatherTransparentRegion(android.graphics.Region region) { return false; }

    private ArrayList<SurfaceHolder.Callback> callbacks() {
        synchronized (mCallbacks) {
            return new ArrayList<>(mCallbacks);
        }
    }

    private void notifyChanged() {
        int w = mFixedWidth > 0 ? mFixedWidth : getWidth();
        int h = mFixedHeight > 0 ? mFixedHeight : getHeight();
        mFrame.set(0, 0, w, h);
        for (SurfaceHolder.Callback c : callbacks()) c.surfaceChanged(mHolder, mFormat, w, h);
    }

    private void createIfReady() {
        if (mCreated || !mAttached || getWidth() <= 0 || getHeight() <= 0) return;
        mSurface.mNativeObject = refract.view.NativeWindows.window();
        if (mSurface.mNativeObject == 0) return;
        mCreated = true;
        for (SurfaceHolder.Callback c : callbacks()) c.surfaceCreated(mHolder);
        notifyChanged();
        for (SurfaceHolder.Callback c : callbacks())
            if (c instanceof SurfaceHolder.Callback2 c2) c2.surfaceRedrawNeeded(mHolder);
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        post(this::createIfReady);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        if (mCreated) notifyChanged();
        else post(this::createIfReady);
    }

    @Override
    protected void onDetachedFromWindow() {
        if (mCreated) {
            for (SurfaceHolder.Callback c : callbacks()) c.surfaceDestroyed(mHolder);
            mCreated = false;
            mSurface.mNativeObject = 0;
        }
        super.onDetachedFromWindow();
    }
}
