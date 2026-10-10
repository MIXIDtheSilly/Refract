package android.os;

/** Policies stay off; the hidden hooks framework code calls are no-ops. */
public final class StrictMode {
    public StrictMode() {}

    public static boolean vmSqliteObjectLeaksEnabled() { return false; }
    public static boolean vmClosableObjectLeaksEnabled() { return false; }
    public static void onSqliteObjectLeaked(String message, Throwable originStack) {}
    public static void onIntentReceiverLeaked(Throwable originStack) {}
    public static void onServiceConnectionLeaked(Throwable originStack) {}
    public static void noteSlowCall(String name) {}
}
