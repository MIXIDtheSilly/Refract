package android.view;

import android.graphics.Point;
import android.graphics.Rect;
import android.util.DisplayMetrics;

/** The headset's display as Android apps on Quest see it (72/90/120 Hz modes). */
public final class Display {
    public static final int DEFAULT_DISPLAY = 0;
    public static final int STATE_ON = 2;
    public static final int INVALID_DISPLAY = -1;

    static final float[] RATES = {72f, 90f, 120f};
    static float sCurrentRate = 90f;

    public Display() {}

    static int width() { return refract.view.NativeWindows.WIDTH; }
    static int height() { return refract.view.NativeWindows.HEIGHT; }

    public int getDisplayId() { return DEFAULT_DISPLAY; }
    public String getName() { return "Built-in Screen"; }
    public boolean isValid() { return true; }
    public int getState() { return STATE_ON; }
    public int getFlags() { return 0; }
    @Deprecated public int getWidth() { return width(); }
    @Deprecated public int getHeight() { return height(); }
    public void getSize(Point outSize) { outSize.set(width(), height()); }
    public void getRealSize(Point outSize) { outSize.set(width(), height()); }
    public void getRectSize(Rect outSize) { outSize.set(0, 0, width(), height()); }
    public void getCurrentSizeRange(Point smallest, Point largest) {
        smallest.set(Math.min(width(), height()), Math.min(width(), height()));
        largest.set(Math.max(width(), height()), Math.max(width(), height()));
    }
    public void getMetrics(DisplayMetrics m) { fill(m); }
    public void getRealMetrics(DisplayMetrics m) { fill(m); }

    static void fill(DisplayMetrics m) {
        m.setToDefaults();
        m.widthPixels = width();
        m.heightPixels = height();
        m.density = 1.0f;
        m.scaledDensity = 1.0f;
        m.densityDpi = DisplayMetrics.DENSITY_DEFAULT;
        m.xdpi = 160f;
        m.ydpi = 160f;
    }

    @Deprecated public int getOrientation() { return 0; }
    public int getRotation() { return Surface.ROTATION_0; }
    @Deprecated public int getPixelFormat() { return 1; }
    public float getRefreshRate() { return sCurrentRate; }
    @Deprecated public float[] getSupportedRefreshRates() { return RATES.clone(); }
    public long getAppVsyncOffsetNanos() { return 0; }
    public long getPresentationDeadlineNanos() { return (long) (1e9 / sCurrentRate); }
    public Mode getMode() { return new Mode(indexOf(sCurrentRate) + 1, width(), height(), sCurrentRate); }
    public Mode[] getSupportedModes() {
        Mode[] modes = new Mode[RATES.length];
        for (int i = 0; i < RATES.length; i++) modes[i] = new Mode(i + 1, width(), height(), RATES[i]);
        return modes;
    }
    public boolean isHdr() { return false; }
    public boolean isWideColorGamut() { return false; }
    public HdrCapabilities getHdrCapabilities() { return null; }
    public DisplayCutout getCutout() { return null; }
    public RoundedCorner getRoundedCorner(int position) { return null; }

    static int indexOf(float rate) {
        for (int i = 0; i < RATES.length; i++)
            if (Math.abs(RATES[i] - rate) < 0.5f) return i;
        return 1;
    }

    @Override
    public String toString() {
        return "Display id 0: " + width() + "x" + height() + " " + sCurrentRate + "fps";
    }

    public static final class Mode {
        private final int mModeId;
        private final int mWidth;
        private final int mHeight;
        private final float mRefreshRate;

        public Mode(int width, int height, float refreshRate) { this(0, width, height, refreshRate); }

        Mode(int modeId, int width, int height, float refreshRate) {
            mModeId = modeId;
            mWidth = width;
            mHeight = height;
            mRefreshRate = refreshRate;
        }

        public int getModeId() { return mModeId; }
        public int getPhysicalWidth() { return mWidth; }
        public int getPhysicalHeight() { return mHeight; }
        public float getRefreshRate() { return mRefreshRate; }
        public float[] getAlternativeRefreshRates() { return new float[0]; }
        public int[] getSupportedHdrTypes() { return new int[0]; }

        @Override
        public String toString() {
            return "{id=" + mModeId + ", width=" + mWidth + ", height=" + mHeight + ", fps=" + mRefreshRate + "}";
        }
    }

    public static final class HdrCapabilities {
        public int[] getSupportedHdrTypes() { return new int[0]; }
    }
}
