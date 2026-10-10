package dalvik.system;

import java.lang.reflect.Array;

/** The parts of ART's VMRuntime that framework utility classes call. */
public final class VMRuntime {
    public static final int SDK_VERSION_CUR_DEVELOPMENT = 10000;
    private static final VMRuntime INSTANCE = new VMRuntime();

    private VMRuntime() {}

    public static VMRuntime getRuntime() { return INSTANCE; }
    public static String getCurrentInstructionSet() { return "arm64"; }
    public static boolean is64BitAbi(String abi) { return abi.contains("64"); }
    public static boolean is64BitInstructionSet(String isa) { return isa.contains("64"); }
    public static int getSdkVersion() { return 34; }
    public static boolean isJavaDebuggable() { return false; }
    public static void registerAppInfo(String a, String b, String c, String[] d, int e, boolean f) {}

    public Object newUnpaddedArray(Class<?> componentType, int minLength) { return Array.newInstance(componentType, minLength); }
    public Object newNonMovableArray(Class<?> componentType, int length) { return Array.newInstance(componentType, length); }
    public long addressOf(Object array) { return 0; }
    public int getTargetSdkVersion() { return 34; }
    public void setTargetSdkVersion(int v) {}
    public boolean is64Bit() { return true; }
    public String vmInstructionSet() { return "arm64"; }
    public String vmVersion() { return "2.1.0"; }
    public String vmLibrary() { return "libart.so"; }
    public boolean isCheckJniEnabled() { return false; }
    public boolean isNativeDebuggable() { return false; }
    public String[] properties() { return new String[0]; }
    public String bootClassPath() { return ""; }
    public String classPath() { return System.getProperty("java.class.path", ""); }
    public float getTargetHeapUtilization() { return 0.75f; }
    public float setTargetHeapUtilization(float f) { return 0.75f; }
    public void registerNativeAllocation(long bytes) {}
    public void registerNativeAllocation(int bytes) {}
    public void registerNativeFree(long bytes) {}
    public void registerNativeFree(int bytes) {}
    public void notifyNativeAllocation() {}
    public void requestConcurrentGC() {}
    public void requestHeapTrim() {}
    public void trimHeap() {}
    public void clearGrowthLimit() {}
    public void clampGrowthLimit() {}
    public void gcSoftReferences() {}
    public void runFinalization(long timeout) {}
    public void setHiddenApiExemptions(String[] signaturePrefixes) {}
    public void updateProcessState(int state) {}
    public void setProcessPackageName(String name) {}
    public void setProcessDataDirectory(String dir) {}
    public void setDedupeHiddenApiWarnings(boolean b) {}
    public boolean hasBootImageSpaces() { return false; }
}
