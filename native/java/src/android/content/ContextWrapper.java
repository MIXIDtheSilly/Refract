package android.content;

/**
 * Every other method forwards to mBase; those forwarders are generated into the
 * base jar by ShimBuilder (one per android.content.Context method).
 */
public class ContextWrapper extends Context {
    Context mBase;

    public ContextWrapper(Context base) {
        mBase = base;
    }

    protected void attachBaseContext(Context base) {
        if (mBase != null) throw new IllegalStateException("Base context already set");
        mBase = base;
    }

    public Context getBaseContext() {
        return mBase;
    }

    /** For refract.app: lets the launcher attach contexts created without one. */
    public void refractAttach(Context base) {
        mBase = base;
    }
}
