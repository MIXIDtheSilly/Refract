package android.content.res;

import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.util.Enumeration;
import java.util.TreeSet;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** Assets read straight from the installed APK. */
public final class AssetManager implements AutoCloseable {
    public static final int ACCESS_UNKNOWN = 0;
    public static final int ACCESS_RANDOM = 1;
    public static final int ACCESS_STREAMING = 2;
    public static final int ACCESS_BUFFER = 3;

    private static AssetManager instance;
    private ZipFile mZip;

    public AssetManager() {}

    public static synchronized AssetManager refractInstance() {
        if (instance == null) {
            instance = new AssetManager();
            try {
                instance.mZip = new ZipFile(System.getProperty("refract.apkHost"));
            } catch (IOException e) {
                refract.Runtime.log(6, "AssetManager", "cannot open the APK: " + e);
            }
        }
        return instance;
    }

    public InputStream open(String fileName) throws IOException { return open(fileName, ACCESS_STREAMING); }

    public InputStream open(String fileName, int accessMode) throws IOException {
        ZipEntry e = mZip == null ? null : mZip.getEntry("assets/" + fileName);
        if (e == null || e.isDirectory()) throw new FileNotFoundException(fileName);
        return mZip.getInputStream(e);
    }

    public String[] list(String path) throws IOException {
        if (mZip == null) return new String[0];
        String prefix = "assets/" + (path.isEmpty() || path.endsWith("/") ? path : path + "/");
        TreeSet<String> names = new TreeSet<>();
        for (Enumeration<? extends ZipEntry> it = mZip.entries(); it.hasMoreElements(); ) {
            String n = it.nextElement().getName();
            if (!n.startsWith(prefix) || n.length() == prefix.length()) continue;
            String rest = n.substring(prefix.length());
            int slash = rest.indexOf('/');
            names.add(slash < 0 ? rest : rest.substring(0, slash));
        }
        return names.toArray(new String[0]);
    }

    public AssetFileDescriptor openFd(String fileName) throws IOException {
        throw new FileNotFoundException("This file can not be opened as a file descriptor; it is probably compressed");
    }

    public AssetFileDescriptor openNonAssetFd(String fileName) throws IOException { return openFd(fileName); }
    public AssetFileDescriptor openNonAssetFd(int cookie, String fileName) throws IOException { return openFd(fileName); }
    public XmlResourceParser openXmlResourceParser(String fileName) throws IOException { throw new FileNotFoundException(fileName); }
    public XmlResourceParser openXmlResourceParser(int cookie, String fileName) throws IOException {
        throw new FileNotFoundException(fileName);
    }
    public String[] getLocales() { return new String[] {"en-US"}; }
    public void close() {}
}
