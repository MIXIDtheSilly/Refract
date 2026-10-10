package android.app;

import android.content.pm.ConfigurationInfo;
import java.util.ArrayList;
import java.util.List;

public class ActivityManager {
    public ActivityManager() {}

    public static boolean isUserAMonkey() { return false; }
    public static boolean isRunningInTestHarness() { return false; }
    public static boolean isLowRamDeviceStatic() { return false; }

    public static void getMyMemoryState(RunningAppProcessInfo outState) {
        outState.processName = refract.Runtime.PACKAGE;
        outState.pid = android.os.Process.myPid();
        outState.importance = RunningAppProcessInfo.IMPORTANCE_FOREGROUND;
    }

    public void getMemoryInfo(MemoryInfo outInfo) {
        long total = Runtime.getRuntime().maxMemory();
        try {
            var os = (com.sun.management.OperatingSystemMXBean) java.lang.management.ManagementFactory.getOperatingSystemMXBean();
            outInfo.totalMem = os.getTotalMemorySize();
            outInfo.availMem = os.getFreeMemorySize();
        } catch (Throwable t) {
            outInfo.totalMem = 6L << 30;
            outInfo.availMem = 3L << 30;
        }
        outInfo.threshold = 256L << 20;
        outInfo.lowMemory = false;
        if (total <= 0) total = 0;
    }

    public int getMemoryClass() { return 512; }
    public int getLargeMemoryClass() { return 1024; }
    public boolean isLowRamDevice() { return false; }
    public boolean isBackgroundRestricted() { return false; }

    public List<RunningAppProcessInfo> getRunningAppProcesses() {
        RunningAppProcessInfo info = new RunningAppProcessInfo(refract.Runtime.PACKAGE, android.os.Process.myPid(),
                new String[] {refract.Runtime.PACKAGE});
        info.importance = RunningAppProcessInfo.IMPORTANCE_FOREGROUND;
        info.uid = 10100;
        List<RunningAppProcessInfo> l = new ArrayList<>();
        l.add(info);
        return l;
    }

    public ConfigurationInfo getDeviceConfigurationInfo() {
        ConfigurationInfo c = new ConfigurationInfo();
        c.reqGlEsVersion = 0x00030002;
        return c;
    }

    public android.os.Debug.MemoryInfo[] getProcessMemoryInfo(int[] pids) {
        android.os.Debug.MemoryInfo[] r = new android.os.Debug.MemoryInfo[pids.length];
        for (int i = 0; i < r.length; i++) r[i] = new android.os.Debug.MemoryInfo();
        return r;
    }

    public static class MemoryInfo {
        public long availMem;
        public long totalMem;
        public long threshold;
        public boolean lowMemory;

        public MemoryInfo() {}
    }

    public static class RunningAppProcessInfo {
        public static final int IMPORTANCE_FOREGROUND = 100;
        public String processName;
        public int pid;
        public int uid;
        public String[] pkgList;
        public int importance;

        public RunningAppProcessInfo() {}

        public RunningAppProcessInfo(String processName, int pid, String[] pkgList) {
            this.processName = processName;
            this.pid = pid;
            this.pkgList = pkgList;
        }
    }
}
