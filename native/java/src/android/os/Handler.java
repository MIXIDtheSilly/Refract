package android.os;

import android.util.Printer;

public class Handler {
    public interface Callback {
        boolean handleMessage(Message msg);
    }

    final Looper mLooper;
    final MessageQueue mQueue;
    final Callback mCallback;
    final boolean mAsynchronous;

    public Handler() { this((Callback) null); }

    public Handler(Callback callback) {
        this(Looper.myLooper(), callback, false);
    }

    public Handler(Looper looper) { this(looper, null, false); }

    public Handler(Looper looper, Callback callback) { this(looper, callback, false); }

    Handler(Looper looper, Callback callback, boolean async) {
        if (looper == null)
            throw new RuntimeException("Can't create handler inside thread " + Thread.currentThread().getName()
                    + " that has not called Looper.prepare()");
        mLooper = looper;
        mQueue = looper.mQueue;
        mCallback = callback;
        mAsynchronous = async;
    }

    public static Handler createAsync(Looper looper) { return new Handler(looper, null, true); }
    public static Handler createAsync(Looper looper, Callback callback) { return new Handler(looper, callback, true); }

    public void handleMessage(Message msg) {}

    public void dispatchMessage(Message msg) {
        if (msg.callback != null) {
            msg.callback.run();
            return;
        }
        if (mCallback != null && mCallback.handleMessage(msg)) return;
        handleMessage(msg);
    }

    public String getMessageName(Message message) {
        return message.callback != null ? message.callback.getClass().getName() : "0x" + Integer.toHexString(message.what);
    }

    public final Message obtainMessage() { return Message.obtain(this); }
    public final Message obtainMessage(int what) { return Message.obtain(this, what); }
    public final Message obtainMessage(int what, Object obj) { return Message.obtain(this, what, obj); }
    public final Message obtainMessage(int what, int arg1, int arg2) { return Message.obtain(this, what, arg1, arg2); }
    public final Message obtainMessage(int what, int arg1, int arg2, Object obj) {
        return Message.obtain(this, what, arg1, arg2, obj);
    }

    private Message wrap(Runnable r, Object token) {
        Message m = Message.obtain(this, r);
        m.obj = token;
        return m;
    }

    public final boolean post(Runnable r) { return sendMessageDelayed(wrap(r, null), 0); }
    public final boolean postAtTime(Runnable r, long uptimeMillis) { return sendMessageAtTime(wrap(r, null), uptimeMillis); }
    public final boolean postAtTime(Runnable r, Object token, long uptimeMillis) {
        return sendMessageAtTime(wrap(r, token), uptimeMillis);
    }
    public final boolean postDelayed(Runnable r, long delayMillis) { return sendMessageDelayed(wrap(r, null), delayMillis); }
    public final boolean postDelayed(Runnable r, Object token, long delayMillis) {
        return sendMessageDelayed(wrap(r, token), delayMillis);
    }
    public final boolean postAtFrontOfQueue(Runnable r) { return sendMessageAtFrontOfQueue(wrap(r, null)); }

    public final void removeCallbacks(Runnable r) { mQueue.remove(m -> m.target == this && m.callback == r); }
    public final void removeCallbacks(Runnable r, Object token) {
        mQueue.remove(m -> m.target == this && m.callback == r && (token == null || m.obj == token));
    }

    public final boolean sendMessage(Message msg) { return sendMessageDelayed(msg, 0); }
    public final boolean sendEmptyMessage(int what) { return sendEmptyMessageDelayed(what, 0); }
    public final boolean sendEmptyMessageDelayed(int what, long delayMillis) {
        return sendMessageDelayed(Message.obtain(this, what), delayMillis);
    }
    public final boolean sendEmptyMessageAtTime(int what, long uptimeMillis) {
        return sendMessageAtTime(Message.obtain(this, what), uptimeMillis);
    }
    public final boolean sendMessageDelayed(Message msg, long delayMillis) {
        if (delayMillis < 0) delayMillis = 0;
        return sendMessageAtTime(msg, SystemClock.uptimeMillis() + delayMillis);
    }
    public boolean sendMessageAtTime(Message msg, long uptimeMillis) {
        msg.target = this;
        if (mAsynchronous) msg.async = true;
        return mQueue.enqueueMessage(msg, uptimeMillis);
    }
    public final boolean sendMessageAtFrontOfQueue(Message msg) {
        msg.target = this;
        return mQueue.enqueueMessage(msg, 0);
    }

    public final void removeMessages(int what) {
        mQueue.remove(m -> m.target == this && m.callback == null && m.what == what);
    }
    public final void removeMessages(int what, Object object) {
        mQueue.remove(m -> m.target == this && m.callback == null && m.what == what && (object == null || m.obj == object));
    }
    public final void removeCallbacksAndMessages(Object token) {
        mQueue.remove(m -> m.target == this && (token == null || m.obj == token));
    }

    public final boolean hasMessages(int what) { return mQueue.hasMessages(this, what, null); }
    public final boolean hasMessages(int what, Object object) { return mQueue.hasMessages(this, what, object); }
    public final boolean hasCallbacks(Runnable r) { return mQueue.hasCallbacks(this, r, null); }

    public final Looper getLooper() { return mLooper; }
    public final void dump(Printer pw, String prefix) {}

    @Override
    public String toString() {
        return "Handler (" + getClass().getName() + ") {" + Integer.toHexString(System.identityHashCode(this)) + "}";
    }
}
