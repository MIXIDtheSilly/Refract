package android.os;

import java.util.ArrayList;

public final class MessageQueue {
    public interface IdleHandler {
        boolean queueIdle();
    }

    public interface OnFileDescriptorEventListener {
        int EVENT_INPUT = 1;
        int EVENT_OUTPUT = 2;
        int EVENT_ERROR = 4;

        int onFileDescriptorEvents(java.io.FileDescriptor fd, int events);
    }

    long mPtr;  // the thread's ALooper (main thread), polled instead of wait(); 0 = none
    private Message head;
    private boolean quitting;
    private final ArrayList<IdleHandler> idle = new ArrayList<>();

    MessageQueue() {}

    synchronized boolean enqueueMessage(Message msg, long when) {
        if (quitting) return false;
        msg.when = when;
        if (head == null || when == 0 || when < head.when) {
            msg.next = head;
            head = msg;
        } else {
            Message p = head;
            while (p.next != null && p.next.when <= when) p = p.next;
            msg.next = p.next;
            p.next = msg;
        }
        notifyAll();
        if (mPtr != 0) refract.app.NativeLooper.wake(mPtr);
        return true;
    }

    /** Blocks until a message is due; null once the queue quits. */
    Message next() {
        while (true) {
            ArrayList<IdleHandler> idlers = null;
            synchronized (this) {
                long now = SystemClock.uptimeMillis();
                if (head != null && head.when <= now) {
                    Message m = head;
                    head = m.next;
                    m.next = null;
                    return m;
                }
                if (quitting) return null;
                if (!idle.isEmpty()) idlers = new ArrayList<>(idle);
            }
            if (idlers != null) {
                for (IdleHandler h : idlers) {
                    boolean keep;
                    try {
                        keep = h.queueIdle();
                    } catch (Throwable t) {
                        keep = false;
                    }
                    if (!keep) removeIdleHandler(h);
                }
            }
            long timeout;
            synchronized (this) {
                long now = SystemClock.uptimeMillis();
                if (head != null && head.when <= now) continue;
                if (quitting) return null;
                timeout = head == null ? -1 : Math.max(1, head.when - now);
                if (mPtr == 0) {
                    try {
                        if (timeout < 0) wait();
                        else wait(timeout);
                    } catch (InterruptedException e) {
                        // keep looping
                    }
                    continue;
                }
            }
            // Native looper: runs its fd/frame callbacks; enqueueMessage wakes it.
            refract.app.NativeLooper.pollOnce(mPtr, (int) Math.min(timeout, Integer.MAX_VALUE));
        }
    }

    synchronized void quit(boolean safe) {
        quitting = true;
        if (!safe) head = null;
        notifyAll();
        if (mPtr != 0) refract.app.NativeLooper.wake(mPtr);
    }

    synchronized boolean hasMessages(Handler h, int what, Object obj) {
        for (Message p = head; p != null; p = p.next)
            if (p.target == h && p.what == what && (obj == null || p.obj == obj)) return true;
        return false;
    }

    synchronized boolean hasCallbacks(Handler h, Runnable r, Object obj) {
        for (Message p = head; p != null; p = p.next)
            if (p.target == h && p.callback == r && (obj == null || p.obj == obj)) return true;
        return false;
    }

    synchronized boolean hasAny(Handler h) {
        for (Message p = head; p != null; p = p.next)
            if (p.target == h) return true;
        return false;
    }

    interface Matcher {
        boolean matches(Message m);
    }

    synchronized void remove(Matcher m) {
        Message prev = null;
        for (Message p = head; p != null; ) {
            Message next = p.next;
            if (m.matches(p)) {
                if (prev == null) head = next;
                else prev.next = next;
            } else {
                prev = p;
            }
            p = next;
        }
    }

    public synchronized boolean isIdle() {
        return head == null || SystemClock.uptimeMillis() < head.when;
    }

    public void addIdleHandler(IdleHandler handler) {
        synchronized (this) {
            idle.add(handler);
        }
    }

    public void removeIdleHandler(IdleHandler handler) {
        synchronized (this) {
            idle.remove(handler);
        }
    }

    public void addOnFileDescriptorEventListener(java.io.FileDescriptor fd, int events,
            OnFileDescriptorEventListener listener) {}

    public void removeOnFileDescriptorEventListener(java.io.FileDescriptor fd) {}
}
