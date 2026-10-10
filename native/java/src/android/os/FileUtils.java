package android.os;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public final class FileUtils {
    public FileUtils() {}

    public static long copy(InputStream in, OutputStream out) throws IOException { return in.transferTo(out); }
    public static void closeQuietly(AutoCloseable closeable) {
        if (closeable == null) return;
        try {
            closeable.close();
        } catch (RuntimeException rethrown) {
            throw rethrown;
        } catch (Exception ignored) {
        }
    }
    /** Hidden; guest files keep the permissions they were created with. */
    public static int setPermissions(String path, int mode, int uid, int gid) { return 0; }
}
