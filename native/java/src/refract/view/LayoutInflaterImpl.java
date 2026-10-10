package refract.view;

import android.content.Context;
import android.view.LayoutInflater;

/** PhoneLayoutInflater stand-in. */
public final class LayoutInflaterImpl extends LayoutInflater {
    public LayoutInflaterImpl(Context context) { super(context); }
    private LayoutInflaterImpl(LayoutInflater original, Context context) { super(original, context); }

    @Override public LayoutInflater cloneInContext(Context newContext) { return new LayoutInflaterImpl(this, newContext); }
}
