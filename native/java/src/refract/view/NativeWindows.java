package refract.view;

/** The host window behind the app's surfaces (one per process). */
public final class NativeWindows {
    private NativeWindows() {}

    public static final int WIDTH = Integer.getInteger("refract.windowWidth", 1280);
    public static final int HEIGHT = Integer.getInteger("refract.windowHeight", 720);

    private static long window;

    static native long createNativeWindow(int width, int height, String title);

    /** Host ANativeWindow*, created on first use. */
    public static synchronized long window() {
        if (window == 0) window = createNativeWindow(WIDTH, HEIGHT, refract.Runtime.LABEL);
        return window;
    }
}
