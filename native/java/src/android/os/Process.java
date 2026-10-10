package android.os;

public class Process {
    public static final int THREAD_PRIORITY_DEFAULT = 0;
    public static final int THREAD_PRIORITY_DISPLAY = -4;
    public static final int THREAD_PRIORITY_URGENT_DISPLAY = -8;
    public static final int THREAD_PRIORITY_AUDIO = -16;
    public static final int THREAD_PRIORITY_URGENT_AUDIO = -19;
    public static final int THREAD_PRIORITY_BACKGROUND = 10;
    public static final int THREAD_PRIORITY_FOREGROUND = -2;
    public static final int THREAD_PRIORITY_LOWEST = 19;
    public static final int THREAD_PRIORITY_MORE_FAVORABLE = -1;
    public static final int THREAD_PRIORITY_LESS_FAVORABLE = 1;
    public static final int SYSTEM_UID = 1000;
    public static final int PHONE_UID = 1001;
    public static final int FIRST_APPLICATION_UID = 10000;
    public static final int LAST_APPLICATION_UID = 19999;
    public static final int INVALID_UID = -1;
    public static final int SIGNAL_KILL = 9;
    public static final int SIGNAL_QUIT = 3;
    public static final int SIGNAL_USR1 = 10;

    private static final int PID = (int) ProcessHandle.current().pid();

    public Process() {}

    public static long getStartElapsedRealtime() { return 0; }
    public static long getStartUptimeMillis() { return 0; }
    public static long getStartRequestedElapsedRealtime() { return 0; }
    public static long getStartRequestedUptimeMillis() { return 0; }
    public static boolean is64Bit() { return true; }
    public static int myPid() { return PID; }
    public static int myTid() { return (int) Thread.currentThread().threadId(); }
    public static int myUid() { return 10100; }
    public static UserHandle myUserHandle() { return null; }
    public static boolean isApplicationUid(int uid) { return uid >= FIRST_APPLICATION_UID && uid <= LAST_APPLICATION_UID; }
    public static boolean isIsolated() { return false; }
    public static boolean isSdkSandbox() { return false; }
    public static int getUidForName(String name) { return -1; }
    public static int getGidForName(String name) { return -1; }
    public static void setThreadPriority(int tid, int priority) {}
    public static void setThreadPriority(int priority) {}
    public static int getThreadPriority(int tid) { return 0; }
    public static int getExclusiveCores()[] { return new int[0]; }
    public static boolean supportsProcesses() { return true; }
    public static void killProcess(int pid) {
        if (pid == PID) {
            refract.Runtime.log(4, "Process", "killProcess(self)");
            java.lang.Runtime.getRuntime().halt(0);
        }
    }
    public static void sendSignal(int pid, int signal) {
        if (pid == PID && signal == SIGNAL_KILL) killProcess(pid);
    }
    public static long getElapsedCpuTime() { return SystemClock.currentThreadTimeMillis(); }
    public static String myProcessName() { return refract.Runtime.PACKAGE; }
}
