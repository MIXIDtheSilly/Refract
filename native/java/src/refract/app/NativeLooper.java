package refract.app;

/** The calling thread's ALooper (host libandroid), backing android.os.Looper on the main thread. */
public final class NativeLooper {
    private NativeLooper() {}

    /** ALooper_prepare on this thread; the looper's address. */
    public static native long prepare();
    /** ALooper_pollOnce(timeoutMs) on this thread's looper (-1 = until woken). */
    public static native int pollOnce(long looper, int timeoutMs);
    public static native void wake(long looper);
}
