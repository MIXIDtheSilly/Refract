package android.util;

import java.io.PrintWriter;
import java.io.StringWriter;

public final class Log {
    public static final int VERBOSE = 2;
    public static final int DEBUG = 3;
    public static final int INFO = 4;
    public static final int WARN = 5;
    public static final int ERROR = 6;
    public static final int ASSERT = 7;

    private Log() {}

    private static int write(int prio, String tag, String msg, Throwable tr) {
        String text = msg == null ? "" : msg;
        if (tr != null) text = text + "\n" + getStackTraceString(tr);
        refract.Runtime.log(prio, tag == null ? "" : tag, text);
        return text.length();
    }

    public static int v(String tag, String msg) { return write(VERBOSE, tag, msg, null); }
    public static int v(String tag, String msg, Throwable tr) { return write(VERBOSE, tag, msg, tr); }
    public static int d(String tag, String msg) { return write(DEBUG, tag, msg, null); }
    public static int d(String tag, String msg, Throwable tr) { return write(DEBUG, tag, msg, tr); }
    public static int i(String tag, String msg) { return write(INFO, tag, msg, null); }
    public static int i(String tag, String msg, Throwable tr) { return write(INFO, tag, msg, tr); }
    public static int w(String tag, String msg) { return write(WARN, tag, msg, null); }
    public static int w(String tag, String msg, Throwable tr) { return write(WARN, tag, msg, tr); }
    public static int w(String tag, Throwable tr) { return write(WARN, tag, null, tr); }
    public static int e(String tag, String msg) { return write(ERROR, tag, msg, null); }
    public static int e(String tag, String msg, Throwable tr) { return write(ERROR, tag, msg, tr); }
    public static int wtf(String tag, String msg) { return write(ASSERT, tag, msg, null); }
    public static int wtf(String tag, Throwable tr) { return write(ASSERT, tag, null, tr); }
    public static int wtf(String tag, String msg, Throwable tr) { return write(ASSERT, tag, msg, tr); }
    public static boolean isLoggable(String tag, int level) { return level >= DEBUG; }
    public static int println(int priority, String tag, String msg) { return write(priority, tag, msg, null); }

    public static String getStackTraceString(Throwable tr) {
        if (tr == null) return "";
        StringWriter sw = new StringWriter();
        tr.printStackTrace(new PrintWriter(sw));
        return sw.toString();
    }
}
