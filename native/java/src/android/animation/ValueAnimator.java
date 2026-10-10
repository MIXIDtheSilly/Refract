package android.animation;

import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import java.util.ArrayList;

/** Frames come from a Handler on the starting thread's Looper (Choreographer's job on Android). */
public class ValueAnimator extends Animator {
    public static final int RESTART = 1, REVERSE = 2, INFINITE = -1;

    private static long sFrameDelay = 16;
    private static final TimeInterpolator ACCELERATE_DECELERATE =
            t -> (float) (Math.cos((t + 1) * Math.PI) / 2.0) + 0.5f;
    private static final int INTS = 0, FLOATS = 1, OBJECTS = 2;

    private int mKind = FLOATS;
    private int[] mInts;
    private float[] mFloats;
    private Object[] mObjects;
    private TypeEvaluator mEvaluator;
    private TimeInterpolator mInterpolator = ACCELERATE_DECELERATE;
    private long mDuration = 300, mStartDelay, mStartTime, mPauseTime;
    private int mRepeatCount, mRepeatMode = RESTART, mIteration;
    private float mFraction;
    private Object mAnimatedValue;
    private boolean mStarted, mRunning;
    private ArrayList<AnimatorUpdateListener> mUpdateListeners = new ArrayList<>();
    private Handler mHandler;
    private Runnable mTick = this::tick;

    public ValueAnimator() {}

    public static ValueAnimator ofInt(int... values) {
        ValueAnimator a = new ValueAnimator();
        a.setIntValues(values);
        return a;
    }
    public static ValueAnimator ofArgb(int... values) {
        ValueAnimator a = ofInt(values);
        a.setEvaluator((TypeEvaluator<Integer>) (t, from, to) -> {
            int r = 0;
            for (int shift = 0; shift < 32; shift += 8) {
                int x = (from >>> shift) & 0xff, y = (to >>> shift) & 0xff;
                r |= (Math.round(x + t * (y - x)) & 0xff) << shift;
            }
            return r;
        });
        return a;
    }
    public static ValueAnimator ofFloat(float... values) {
        ValueAnimator a = new ValueAnimator();
        a.setFloatValues(values);
        return a;
    }
    public static ValueAnimator ofObject(TypeEvaluator evaluator, Object... values) {
        ValueAnimator a = new ValueAnimator();
        a.setObjectValues(values);
        a.setEvaluator(evaluator);
        return a;
    }

    public static float getDurationScale() { return 1f; }
    public static boolean areAnimatorsEnabled() { return true; }
    public static long getFrameDelay() { return sFrameDelay; }
    public static void setFrameDelay(long frameDelay) { sFrameDelay = Math.max(1, frameDelay); }

    public void setIntValues(int... values) {
        mKind = INTS;
        mInts = values.length == 1 ? new int[] {values[0], values[0]} : values.clone();
    }
    public void setFloatValues(float... values) {
        mKind = FLOATS;
        mFloats = values.length == 1 ? new float[] {values[0], values[0]} : values.clone();
    }
    public void setObjectValues(Object... values) {
        mKind = OBJECTS;
        mObjects = values.length == 1 ? new Object[] {values[0], values[0]} : values.clone();
    }
    public void setEvaluator(TypeEvaluator value) { mEvaluator = value; }

    @Override public ValueAnimator setDuration(long duration) {
        if (duration < 0) throw new IllegalArgumentException("Animators cannot have negative duration: " + duration);
        mDuration = duration;
        return this;
    }
    @Override public long getDuration() { return mDuration; }
    @Override public long getStartDelay() { return mStartDelay; }
    @Override public void setStartDelay(long startDelay) { mStartDelay = Math.max(0, startDelay); }
    @Override public long getTotalDuration() {
        return mRepeatCount == INFINITE ? DURATION_INFINITE : mStartDelay + mDuration * (mRepeatCount + 1);
    }
    @Override public void setInterpolator(TimeInterpolator value) {
        mInterpolator = value != null ? value : t -> t;
    }
    @Override public TimeInterpolator getInterpolator() { return mInterpolator; }
    public void setRepeatCount(int value) { mRepeatCount = value; }
    public int getRepeatCount() { return mRepeatCount; }
    public void setRepeatMode(int value) { mRepeatMode = value; }
    public int getRepeatMode() { return mRepeatMode; }

    public void addUpdateListener(AnimatorUpdateListener listener) { mUpdateListeners.add(listener); }
    public void removeUpdateListener(AnimatorUpdateListener listener) { mUpdateListeners.remove(listener); }
    public void removeAllUpdateListeners() { mUpdateListeners.clear(); }

    public Object getAnimatedValue() { return mAnimatedValue; }
    public Object getAnimatedValue(String propertyName) { return mAnimatedValue; }
    public float getAnimatedFraction() { return mFraction; }
    public long getCurrentPlayTime() {
        return mStarted ? Math.max(0, SystemClock.uptimeMillis() - mStartTime - mStartDelay) : 0;
    }
    public void setCurrentPlayTime(long playTime) {
        mStartTime = SystemClock.uptimeMillis() - mStartDelay - playTime;
        animateAt(playTime);
    }
    public void setCurrentFraction(float fraction) {
        setCurrentPlayTime((long) (fraction * mDuration));
    }

    @Override public void start() {
        Looper looper = Looper.myLooper();
        mHandler = new Handler(looper != null ? looper : Looper.getMainLooper());
        mStarted = true;
        mRunning = mStartDelay == 0;
        mPaused = false;
        mIteration = 0;
        mStartTime = SystemClock.uptimeMillis();
        for (AnimatorListener l : listenersCopy()) l.onAnimationStart(this, false);
        if (mStartDelay == 0) animateValue(0f);
        mHandler.removeCallbacks(mTick);
        mHandler.postDelayed(mTick, sFrameDelay);
    }

    @Override public void cancel() {
        if (!mStarted) return;
        stopTicking();
        for (AnimatorListener l : listenersCopy()) l.onAnimationCancel(this);
        for (AnimatorListener l : listenersCopy()) l.onAnimationEnd(this, false);
    }

    @Override public void end() {
        if (!mStarted) start();
        boolean backwards = mRepeatMode == REVERSE && mRepeatCount > 0 && (mRepeatCount & 1) == 1;
        animateValue(backwards ? 0f : 1f);
        stopTicking();
        for (AnimatorListener l : listenersCopy()) l.onAnimationEnd(this, false);
    }

    @Override public void pause() {
        if (mStarted && !mPaused) mPauseTime = SystemClock.uptimeMillis();
        super.pause();
    }
    @Override public void resume() {
        if (mPaused) {
            mStartTime += SystemClock.uptimeMillis() - mPauseTime;
            if (mHandler != null) mHandler.postDelayed(mTick, sFrameDelay);
        }
        super.resume();
    }
    public void reverse() { start(); }
    @Override public boolean isRunning() { return mRunning; }
    @Override public boolean isStarted() { return mStarted; }

    private void stopTicking() {
        if (mHandler != null) mHandler.removeCallbacks(mTick);
        mStarted = false;
        mRunning = false;
        mPaused = false;
    }

    private void tick() {
        if (!mStarted || mPaused) return;
        long playTime = SystemClock.uptimeMillis() - mStartTime - mStartDelay;
        if (playTime >= 0) {
            mRunning = true;
            if (animateAt(playTime)) {
                stopTicking();
                for (AnimatorListener l : listenersCopy()) l.onAnimationEnd(this, false);
                return;
            }
        }
        mHandler.postDelayed(mTick, sFrameDelay);
    }

    /** Animates to a play time; true once the animation is over. */
    private boolean animateAt(long playTime) {
        float raw = mDuration > 0 ? (float) playTime / mDuration : 1f;
        int iteration = (int) raw;
        boolean done = mRepeatCount != INFINITE && iteration > mRepeatCount;
        if (done) iteration = mRepeatCount;
        if (iteration != mIteration) {
            mIteration = iteration;
            for (AnimatorListener l : listenersCopy()) l.onAnimationRepeat(this);
        }
        float f = done ? 1f : raw - iteration;
        if (mRepeatMode == REVERSE && (iteration & 1) == 1) f = 1f - f;
        animateValue(f);
        return done;
    }

    @SuppressWarnings("unchecked")
    private void animateValue(float fraction) {
        mFraction = fraction;
        float f = mInterpolator.getInterpolation(fraction);
        int n = mKind == INTS ? (mInts != null ? mInts.length : 0)
                : mKind == FLOATS ? (mFloats != null ? mFloats.length : 0) : (mObjects != null ? mObjects.length : 0);
        if (n >= 2) {
            float pos = Math.max(0f, Math.min(1f, f)) * (n - 1);
            int i = Math.min((int) pos, n - 2);
            float local = f * (n - 1) - i;
            if (mEvaluator != null) {
                Object a = mKind == INTS ? (Object) mInts[i] : mKind == FLOATS ? (Object) mFloats[i] : mObjects[i];
                Object b = mKind == INTS ? (Object) mInts[i + 1] : mKind == FLOATS ? (Object) mFloats[i + 1] : mObjects[i + 1];
                mAnimatedValue = mEvaluator.evaluate(local, a, b);
            } else if (mKind == INTS) {
                mAnimatedValue = (int) (mInts[i] + local * (mInts[i + 1] - mInts[i]));
            } else if (mKind == FLOATS) {
                mAnimatedValue = mFloats[i] + local * (mFloats[i + 1] - mFloats[i]);
            } else {
                mAnimatedValue = local < 1f ? mObjects[i] : mObjects[i + 1];
            }
        }
        for (AnimatorUpdateListener l : new ArrayList<>(mUpdateListeners)) l.onAnimationUpdate(this);
    }

    @Override public ValueAnimator clone() {
        ValueAnimator a = (ValueAnimator) super.clone();
        a.mUpdateListeners = new ArrayList<>(mUpdateListeners);
        a.mStarted = false;
        a.mRunning = false;
        a.mHandler = null;
        a.mTick = a::tick;
        return a;
    }

    public interface AnimatorUpdateListener {
        void onAnimationUpdate(ValueAnimator animation);
    }
}
