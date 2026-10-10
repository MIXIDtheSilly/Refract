package android.os;

import android.util.Printer;

public final class Looper {
    private static final ThreadLocal<Looper> current = new ThreadLocal<>();
    private static volatile Looper main;

    final MessageQueue mQueue = new MessageQueue();
    final Thread mThread = Thread.currentThread();

    private Looper() {}

    public static void prepare() {
        if (current.get() != null) throw new RuntimeException("Only one Looper may be created per thread");
        current.set(new Looper());
    }

    public static void prepareMainLooper() {
        prepare();
        main = current.get();
    }

    public static Looper getMainLooper() { return main; }
    public static Looper myLooper() { return current.get(); }

    public static MessageQueue myQueue() {
        Looper l = current.get();
        return l == null ? null : l.mQueue;
    }

    public static void loop() {
        Looper me = current.get();
        if (me == null) throw new RuntimeException("No Looper; Looper.prepare() wasn't called on this thread.");
        while (true) {
            Message msg = me.mQueue.next();
            if (msg == null) return;
            try {
                msg.target.dispatchMessage(msg);
            } catch (RuntimeException | Error e) {
                refract.Runtime.log(6, "Looper", "uncaught exception on " + Thread.currentThread().getName() + ": " + e);
                e.printStackTrace();
                throw e;
            }
        }
    }

    public boolean isCurrentThread() { return Thread.currentThread() == mThread; }
    public void setMessageLogging(Printer printer) {}
    public void quit() { mQueue.quit(false); }
    public void quitSafely() { mQueue.quit(true); }
    public Thread getThread() { return mThread; }
    public MessageQueue getQueue() { return mQueue; }
    public void dump(Printer pw, String prefix) {}

    @Override
    public String toString() {
        return "Looper (" + mThread.getName() + ")";
    }
}
