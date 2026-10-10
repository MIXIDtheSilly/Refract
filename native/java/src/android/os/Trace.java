package android.os;

/** No systrace here: sections are no-ops (the hidden tag API is what framework code calls). */
public final class Trace {
    public Trace() {}

    public static boolean isEnabled() { return false; }
    public static void beginSection(String sectionName) {}
    public static void endSection() {}
    public static void beginAsyncSection(String methodName, int cookie) {}
    public static void endAsyncSection(String methodName, int cookie) {}
    public static void setCounter(String counterName, long counterValue) {}

    public static boolean isTagEnabled(long traceTag) { return false; }
    public static void traceBegin(long traceTag, String methodName) {}
    public static void traceEnd(long traceTag) {}
    public static void asyncTraceBegin(long traceTag, String methodName, int cookie) {}
    public static void asyncTraceEnd(long traceTag, String methodName, int cookie) {}
    public static void traceCounter(long traceTag, String counterName, int counterValue) {}
}
