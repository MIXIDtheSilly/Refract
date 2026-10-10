package android.os;

/** Reports the Quest 2 (Horizon OS, Android 14) build the firmware sysroot came from. */
public class Build {
    public static final boolean IS_EMULATOR = false;
    public static final boolean IS_DEBUGGABLE = false;
    public static final boolean IS_ENG = false;
    public static final boolean IS_USERDEBUG = false;
    public static final boolean IS_USER = true;
    public static final boolean IS_CONTAINER = false;
    public static final boolean IS_TREBLE_ENABLED = true;
    public static final String UNKNOWN = "unknown";
    public static final String ID = "UP1A.231005.007.A1";
    public static final String DISPLAY = "UP1A.231005.007.A1";
    public static final String PRODUCT = "hollywood";
    public static final String DEVICE = "hollywood";
    public static final String BOARD = "hollywood";
    public static final String CPU_ABI = "arm64-v8a";
    public static final String CPU_ABI2 = "";
    public static final String MANUFACTURER = "Oculus";
    public static final String BRAND = "oculus";
    public static final String MODEL = "Quest 2";
    public static final String SOC_MANUFACTURER = "Qualcomm";
    public static final String SOC_MODEL = "SM8250";
    public static final String BOOTLOADER = "unknown";
    public static final String RADIO = "unknown";
    public static final String HARDWARE = "hollywood";
    public static final String SKU = "unknown";
    public static final String ODM_SKU = "unknown";
    public static final String SERIAL = "unknown";
    public static final String[] SUPPORTED_ABIS = {"arm64-v8a"};
    public static final String[] SUPPORTED_32_BIT_ABIS = {};
    public static final String[] SUPPORTED_64_BIT_ABIS = {"arm64-v8a"};
    public static final String TYPE = "user";
    public static final String TAGS = "release-keys";
    public static final String FINGERPRINT =
            "oculus/hollywood/hollywood:14/UP1A.231005.007.A1/51978090107800150:user/release-keys";
    public static final long TIME = 1700000000000L;
    public static final String USER = "chronos_secgrp_releng_foundry";
    public static final String HOST = "refract";

    public Build() {}

    public static String getSerial() { return "1WMHHA63M61447"; }
    public static String getRadioVersion() { return null; }
    public static java.util.List<Partition> getFingerprintedPartitions() { return new java.util.ArrayList<>(); }

    public static class VERSION {
        public static final String INCREMENTAL = "51978090107800150";
        public static final String RELEASE = "14";
        public static final String RELEASE_OR_CODENAME = "14";
        public static final String RELEASE_OR_PREVIEW_DISPLAY = "14";
        public static final String BASE_OS = "";
        public static final String SECURITY_PATCH = "2025-07-05";
        public static final int MEDIA_PERFORMANCE_CLASS = 0;
        public static final String SDK = "34";
        public static final int SDK_INT = 34;
        public static final int PREVIEW_SDK_INT = 0;
        public static final String CODENAME = "REL";

        public VERSION() {}
    }

    public static class VERSION_CODES {
        public static final int CUR_DEVELOPMENT = 10000;
        public static final int BASE = 1;
        public static final int LOLLIPOP = 21;
        public static final int M = 23;
        public static final int N = 24;
        public static final int O = 26;
        public static final int P = 28;
        public static final int Q = 29;
        public static final int R = 30;
        public static final int S = 31;
        public static final int TIRAMISU = 33;
        public static final int UPSIDE_DOWN_CAKE = 34;

        public VERSION_CODES() {}
    }

    public static class Partition {
        public static final String PARTITION_NAME_SYSTEM = "system";

        Partition() {}

        public String getName() { return "system"; }
        public String getFingerprint() { return FINGERPRINT; }
        public long getBuildTimeMillis() { return TIME; }
    }
}
