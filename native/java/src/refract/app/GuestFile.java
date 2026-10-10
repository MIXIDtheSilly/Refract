package refract.app;

import java.io.File;

/** A File naming a guest path: Windows' File would turn the separators into backslashes. */
public final class GuestFile extends File {
    private final String path;

    public GuestFile(String guestPath) {
        super(guestPath);
        path = guestPath;
    }

    @Override public String getPath() { return path; }
    @Override public String getAbsolutePath() { return path; }
    @Override public String getCanonicalPath() { return path; }
    @Override public boolean isAbsolute() { return true; }
    @Override public String toString() { return path; }
    @Override public String getName() { return path.substring(path.lastIndexOf('/') + 1); }
    @Override public String getParent() {
        int i = path.lastIndexOf('/');
        return i <= 0 ? null : path.substring(0, i);
    }
    @Override public File getParentFile() {
        String p = getParent();
        return p == null ? null : new GuestFile(p);
    }
    @Override public File getAbsoluteFile() { return this; }
}
