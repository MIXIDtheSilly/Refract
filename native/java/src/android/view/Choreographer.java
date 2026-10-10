package android.view;

import android.os.Handler;
import android.os.Looper;
import java.util.ArrayList;

/** Frame callbacks at the display rate on the calling thread's Looper. */
public final class Choreographer {
    public interface FrameCallback {
        void doFrame(long frameTimeNanos);
    }

    public interface VsyncCallback {
        void onVsync(FrameData data);
    }

    public static class FrameData {
        public long getFrameTimeNanos() { return System.nanoTime(); }
    }

    private static final ThreadLocal<Choreographer> sThread = ThreadLocal.withInitial(() -> {
        Looper l = Looper.myLooper();
        return l == null ? null : new Choreographer(l);
    });

    private final Handler mHandler;
    private final ArrayList<FrameCallback> mCallbacks = new ArrayList<>();
    private boolean mScheduled;

    private Choreographer(Looper looper) {
        mHandler = new Handler(looper);
    }

    public static Choreographer getInstance() {
        Choreographer c = sThread.get();
        if (c == null) throw new IllegalStateException("The current thread must have a looper!");
        return c;
    }

    public static Choreographer getMainThreadChoreographer() { return getInstance(); }

    private void schedule(long delayMillis) {
        if (mScheduled) return;
        mScheduled = true;
        long periodNanos = (long) (1e9 / Display.sCurrentRate);
        long now = System.nanoTime();
        long untilVsync = (periodNanos - now % periodNanos) / 1_000_000L;
        mHandler.postDelayed(this::doFrame, Math.max(delayMillis, untilVsync));
    }

    private void doFrame() {
        ArrayList<FrameCallback> run;
        synchronized (mCallbacks) {
            mScheduled = false;
            run = new ArrayList<>(mCallbacks);
            mCallbacks.clear();
        }
        long t = System.nanoTime();
        for (FrameCallback cb : run) cb.doFrame(t);
    }

    public void postFrameCallback(FrameCallback callback) { postFrameCallbackDelayed(callback, 0); }

    public void postFrameCallbackDelayed(FrameCallback callback, long delayMillis) {
        synchronized (mCallbacks) {
            mCallbacks.add(callback);
            schedule(delayMillis);
        }
    }

    public void removeFrameCallback(FrameCallback callback) {
        synchronized (mCallbacks) {
            mCallbacks.remove(callback);
        }
    }

    public void postVsyncCallback(VsyncCallback callback) {
        postFrameCallback(t -> callback.onVsync(new FrameData()));
    }

    public void removeVsyncCallback(VsyncCallback callback) {}
}
