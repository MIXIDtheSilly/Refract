package android.os;

public final class SystemClock {
    private SystemClock() {}

    // Same clock as the guest's CLOCK_MONOTONIC (QueryPerformanceCounter on the host).
    public static long uptimeMillis() { return System.nanoTime() / 1000000L; }
    public static long uptimeNanos() { return System.nanoTime(); }
    public static long elapsedRealtime() { return System.nanoTime() / 1000000L; }
    public static long elapsedRealtimeNanos() { return System.nanoTime(); }
    public static long currentThreadTimeMillis() { return currentThreadTimeNanos() / 1000000L; }

    static long currentThreadTimeNanos() {
        java.lang.management.ThreadMXBean b = java.lang.management.ManagementFactory.getThreadMXBean();
        return b.isCurrentThreadCpuTimeSupported() ? b.getCurrentThreadCpuTime() : System.nanoTime();
    }

    public static boolean setCurrentTimeMillis(long millis) { return false; }

    public static void sleep(long ms) {
        long end = uptimeMillis() + ms;
        while (true) {
            long left = end - uptimeMillis();
            if (left <= 0) return;
            try {
                Thread.sleep(left);
            } catch (InterruptedException e) {
                // keep sleeping, like Android
            }
        }
    }

    public static java.time.Clock currentNetworkTimeClock() { return java.time.Clock.systemUTC(); }
    public static java.time.Clock currentGnssTimeClock() { return java.time.Clock.systemUTC(); }
}
