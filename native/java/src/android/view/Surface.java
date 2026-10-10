package android.view;

import android.graphics.Canvas;
import android.graphics.Rect;
import android.graphics.SurfaceTexture;
import android.os.Parcel;
import android.os.Parcelable;

/** A Surface is a host ANativeWindow (see refract_native's android_hle.cpp). */
public class Surface implements Parcelable {
    public static final int ROTATION_0 = 0;
    public static final int ROTATION_90 = 1;
    public static final int ROTATION_180 = 2;
    public static final int ROTATION_270 = 3;
    public static final Parcelable.Creator<Surface> CREATOR = null;

    /** Host ANativeWindow*; read by ANativeWindow_fromSurface. */
    long mNativeObject;
    private String mName = "";

    public Surface() {}

    public Surface(SurfaceTexture surfaceTexture) {
        refract.Runtime.log(5, "Surface", "Surface(SurfaceTexture) is not supported");
    }

    /** For refract: wraps a host native window. */
    public Surface(long nativeWindow, String name) {
        mNativeObject = nativeWindow;
        mName = name;
    }

    public long refractNativeWindow() { return mNativeObject; }
    public boolean isValid() { return mNativeObject != 0; }
    public void release() {}
    public Canvas lockCanvas(Rect inOutDirty) { return null; }
    public Canvas lockHardwareCanvas() { return null; }
    public void unlockCanvasAndPost(Canvas canvas) {}
    public void setFrameRate(float frameRate, int compatibility) {}
    public void setFrameRate(float frameRate, int compatibility, int changeFrameRateStrategy) {}
    public void clearFrameRate() {}
    public int describeContents() { return 0; }
    public void writeToParcel(Parcel dest, int flags) {}
    public void readFromParcel(Parcel source) {}

    @Override
    public String toString() {
        return "Surface(name=" + mName + ")/@0x" + Long.toHexString(mNativeObject);
    }
}
