package android.animation;

import java.util.ArrayList;

/** Listener bookkeeping for the animators implemented here (ValueAnimator). */
public abstract class Animator implements Cloneable {
    public static final long DURATION_INFINITE = -1;

    ArrayList<AnimatorListener> mListeners;
    ArrayList<AnimatorPauseListener> mPauseListeners;
    boolean mPaused;

    public Animator() {}

    public void start() {}
    public void cancel() {}
    public void end() {}
    public void pause() {
        if (isStarted() && !mPaused) {
            mPaused = true;
            if (mPauseListeners != null)
                for (AnimatorPauseListener l : new ArrayList<>(mPauseListeners)) l.onAnimationPause(this);
        }
    }
    public void resume() {
        if (mPaused) {
            mPaused = false;
            if (mPauseListeners != null)
                for (AnimatorPauseListener l : new ArrayList<>(mPauseListeners)) l.onAnimationResume(this);
        }
    }
    public boolean isPaused() { return mPaused; }
    public long getStartDelay() { return 0; }
    public void setStartDelay(long startDelay) {}
    public Animator setDuration(long duration) { return this; }
    public long getDuration() { return 0; }
    public long getTotalDuration() {
        long d = getDuration();
        return d == DURATION_INFINITE ? DURATION_INFINITE : getStartDelay() + d;
    }
    public void setInterpolator(TimeInterpolator value) {}
    public TimeInterpolator getInterpolator() { return null; }
    public boolean isRunning() { return false; }
    public boolean isStarted() { return isRunning(); }

    public void addListener(AnimatorListener listener) {
        if (mListeners == null) mListeners = new ArrayList<>();
        mListeners.add(listener);
    }
    public void removeListener(AnimatorListener listener) {
        if (mListeners != null) mListeners.remove(listener);
    }
    public ArrayList<AnimatorListener> getListeners() { return mListeners; }
    public void addPauseListener(AnimatorPauseListener listener) {
        if (mPauseListeners == null) mPauseListeners = new ArrayList<>();
        mPauseListeners.add(listener);
    }
    public void removePauseListener(AnimatorPauseListener listener) {
        if (mPauseListeners != null) mPauseListeners.remove(listener);
    }
    public void removeAllListeners() {
        mListeners = null;
        mPauseListeners = null;
    }

    ArrayList<AnimatorListener> listenersCopy() {
        return mListeners == null ? new ArrayList<>() : new ArrayList<>(mListeners);
    }

    @Override public Animator clone() {
        try {
            Animator a = (Animator) super.clone();
            if (mListeners != null) a.mListeners = new ArrayList<>(mListeners);
            if (mPauseListeners != null) a.mPauseListeners = new ArrayList<>(mPauseListeners);
            return a;
        } catch (CloneNotSupportedException e) {
            throw new AssertionError(e);
        }
    }
    public void setupStartValues() {}
    public void setupEndValues() {}
    public void setTarget(Object target) {}

    public interface AnimatorListener {
        default void onAnimationStart(Animator animation, boolean isReverse) { onAnimationStart(animation); }
        default void onAnimationEnd(Animator animation, boolean isReverse) { onAnimationEnd(animation); }
        void onAnimationStart(Animator animation);
        void onAnimationEnd(Animator animation);
        void onAnimationCancel(Animator animation);
        void onAnimationRepeat(Animator animation);
    }

    public interface AnimatorPauseListener {
        void onAnimationPause(Animator animation);
        void onAnimationResume(Animator animation);
    }
}
