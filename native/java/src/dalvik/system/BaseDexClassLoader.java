package dalvik.system;

import java.io.File;

/**
 * The app's class loader as Android code sees it. Classes come from the parent
 * (the JVM loader holding the app's converted dex); findLibrary searches the
 * guest library path the way ART's does.
 */
public class BaseDexClassLoader extends ClassLoader {
    private final String mDexPath;
    private final String[] mLibraryDirs;

    public BaseDexClassLoader(String dexPath, File optimizedDirectory, String librarySearchPath, ClassLoader parent) {
        super(parent != null ? parent : BaseDexClassLoader.class.getClassLoader());
        mDexPath = dexPath != null ? dexPath : refract.Runtime.APK;
        String path = librarySearchPath != null ? librarySearchPath : refract.Runtime.NATIVE_LIBRARY_DIR;
        mLibraryDirs = path.isEmpty() ? new String[0] : path.split(":");
    }

    public BaseDexClassLoader() { this(null, null, null, null); }

    @Override
    public String findLibrary(String name) {
        String file = System.mapLibraryName(name);
        if (!file.startsWith("lib")) file = "lib" + name + ".so";  // mapLibraryName is the host's (.dll)
        for (String dir : mLibraryDirs) {
            String guest = dir + "/" + file;
            if (new File(refract.Runtime.hostPath(guest)).isFile()) return guest;
        }
        return null;
    }

    public String getLdLibraryPath() { return String.join(":", mLibraryDirs); }

    @Override
    public String toString() {
        return getClass().getName() + "[DexPathList[[zip file \"" + mDexPath + "\"],nativeLibraryDirectories=["
                + String.join(", ", mLibraryDirs) + "]]]";
    }
}
