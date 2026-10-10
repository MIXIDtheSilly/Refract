package android.os;

public final class Message implements Parcelable {
    public int what;
    public int arg1;
    public int arg2;
    public Object obj;
    public Messenger replyTo;
    public int sendingUid = -1;

    long when;
    Handler target;
    Runnable callback;
    Bundle data;
    Message next;
    boolean async;

    public Message() {}

    public static Message obtain() { return new Message(); }

    public static Message obtain(Message orig) {
        Message m = new Message();
        m.copyFrom(orig);
        m.target = orig.target;
        m.callback = orig.callback;
        return m;
    }

    public static Message obtain(Handler h) {
        Message m = new Message();
        m.target = h;
        return m;
    }

    public static Message obtain(Handler h, Runnable callback) {
        Message m = obtain(h);
        m.callback = callback;
        return m;
    }

    public static Message obtain(Handler h, int what) {
        Message m = obtain(h);
        m.what = what;
        return m;
    }

    public static Message obtain(Handler h, int what, Object obj) {
        Message m = obtain(h, what);
        m.obj = obj;
        return m;
    }

    public static Message obtain(Handler h, int what, int arg1, int arg2) {
        Message m = obtain(h, what);
        m.arg1 = arg1;
        m.arg2 = arg2;
        return m;
    }

    public static Message obtain(Handler h, int what, int arg1, int arg2, Object obj) {
        Message m = obtain(h, what, arg1, arg2);
        m.obj = obj;
        return m;
    }

    public void recycle() {}

    public void copyFrom(Message o) {
        what = o.what;
        arg1 = o.arg1;
        arg2 = o.arg2;
        obj = o.obj;
        replyTo = o.replyTo;
        data = o.data == null ? null : new Bundle(o.data);
    }

    public long getWhen() { return when; }
    public void setTarget(Handler target) { this.target = target; }
    public Handler getTarget() { return target; }
    public Runnable getCallback() { return callback; }

    public Bundle getData() {
        if (data == null) data = new Bundle();
        return data;
    }

    public Bundle peekData() { return data; }
    public void setData(Bundle data) { this.data = data; }
    public void sendToTarget() { target.sendMessage(this); }
    public boolean isAsynchronous() { return async; }
    public void setAsynchronous(boolean async) { this.async = async; }

    @Override
    public String toString() {
        return "Message{what=" + what + " when=" + when + "}";
    }

    public int describeContents() { return 0; }
    public void writeToParcel(Parcel dest, int flags) {}
}
