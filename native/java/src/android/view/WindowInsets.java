package android.view;

import android.graphics.Insets;
import android.graphics.Rect;

/** A headset window has no bars, cutout or IME: every inset is zero and every type hidden. */
public final class WindowInsets {
    public static final WindowInsets CONSUMED = new WindowInsets(Insets.NONE, Insets.NONE, true);

    private final Insets mSystem;
    private final Insets mStable;
    private final boolean mConsumed;

    WindowInsets(Insets system, Insets stable, boolean consumed) {
        mSystem = system;
        mStable = stable;
        mConsumed = consumed;
    }

    public WindowInsets(WindowInsets src) { this(src.mSystem, src.mStable, src.mConsumed); }

    /** The insets View.getRootWindowInsets() reports. */
    public static WindowInsets refractNone() { return new WindowInsets(Insets.NONE, Insets.NONE, false); }

    public Insets getSystemWindowInsets() { return mSystem; }
    public Insets getInsets(int typeMask) { return Insets.NONE; }
    public Insets getInsetsIgnoringVisibility(int typeMask) { return Insets.NONE; }
    public boolean isVisible(int typeMask) { return false; }
    public int getSystemWindowInsetLeft() { return mSystem.left; }
    public int getSystemWindowInsetTop() { return mSystem.top; }
    public int getSystemWindowInsetRight() { return mSystem.right; }
    public int getSystemWindowInsetBottom() { return mSystem.bottom; }
    public boolean hasSystemWindowInsets() { return !mSystem.equals(Insets.NONE); }
    public boolean hasInsets() { return hasSystemWindowInsets() || hasStableInsets(); }
    public DisplayCutout getDisplayCutout() { return null; }
    public RoundedCorner getRoundedCorner(int position) { return null; }
    public Rect getPrivacyIndicatorBounds() { return null; }
    public DisplayShape getDisplayShape() { return null; }
    public WindowInsets consumeDisplayCutout() { return this; }
    public boolean isConsumed() { return mConsumed; }
    public boolean isRound() { return false; }
    public WindowInsets consumeSystemWindowInsets() { return new WindowInsets(Insets.NONE, mStable, true); }
    public WindowInsets replaceSystemWindowInsets(int left, int top, int right, int bottom) {
        return new WindowInsets(Insets.of(left, top, right, bottom), mStable, false);
    }
    public WindowInsets replaceSystemWindowInsets(Rect r) { return replaceSystemWindowInsets(r.left, r.top, r.right, r.bottom); }
    public Insets getStableInsets() { return mStable; }
    public int getStableInsetTop() { return mStable.top; }
    public int getStableInsetLeft() { return mStable.left; }
    public int getStableInsetRight() { return mStable.right; }
    public int getStableInsetBottom() { return mStable.bottom; }
    public boolean hasStableInsets() { return !mStable.equals(Insets.NONE); }
    public Insets getSystemGestureInsets() { return Insets.NONE; }
    public Insets getMandatorySystemGestureInsets() { return Insets.NONE; }
    public Insets getTappableElementInsets() { return Insets.NONE; }
    public WindowInsets consumeStableInsets() { return new WindowInsets(mSystem, Insets.NONE, mConsumed); }
    public WindowInsets inset(Insets insets) { return inset(insets.left, insets.top, insets.right, insets.bottom); }
    public WindowInsets inset(int left, int top, int right, int bottom) {
        return new WindowInsets(Insets.of(Math.max(0, mSystem.left - left), Math.max(0, mSystem.top - top),
                Math.max(0, mSystem.right - right), Math.max(0, mSystem.bottom - bottom)), mStable, mConsumed);
    }
    @Override public String toString() { return "WindowInsets{systemWindowInsets=" + mSystem + " stable=" + mStable + "}"; }
    @Override public boolean equals(Object o) {
        return o instanceof WindowInsets w && w.mSystem.equals(mSystem) && w.mStable.equals(mStable) && w.mConsumed == mConsumed;
    }
    @Override public int hashCode() { return mSystem.hashCode() * 31 + mStable.hashCode() + (mConsumed ? 1 : 0); }

    public static final class Builder {
        private Insets mSystem = Insets.NONE, mStable = Insets.NONE;

        public Builder() {}
        public Builder(WindowInsets insets) {
            mSystem = insets.mSystem;
            mStable = insets.mStable;
        }
        public Builder setSystemWindowInsets(Insets insets) { mSystem = insets; return this; }
        public Builder setSystemGestureInsets(Insets insets) { return this; }
        public Builder setMandatorySystemGestureInsets(Insets insets) { return this; }
        public Builder setTappableElementInsets(Insets insets) { return this; }
        public Builder setInsets(int typeMask, Insets insets) { return this; }
        public Builder setInsetsIgnoringVisibility(int typeMask, Insets insets) { return this; }
        public Builder setVisible(int typeMask, boolean visible) { return this; }
        public Builder setStableInsets(Insets insets) { mStable = insets; return this; }
        public Builder setDisplayCutout(DisplayCutout cutout) { return this; }
        public Builder setRoundedCorner(int position, RoundedCorner roundedCorner) { return this; }
        public Builder setPrivacyIndicatorBounds(Rect bounds) { return this; }
        public Builder setDisplayShape(DisplayShape displayShape) { return this; }
        public WindowInsets build() { return new WindowInsets(mSystem, mStable, false); }
    }
}
