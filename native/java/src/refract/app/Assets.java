package refract.app;

import android.content.res.AssetManager;
import java.io.InputStream;

/** APK asset access for native code (AAssetManager in libandroid, see android_hle.cpp). */
public final class Assets {
    private Assets() {}

    public static byte[] read(String name) {
        try (InputStream in = AssetManager.refractInstance().open(name)) {
            return in.readAllBytes();
        } catch (Exception e) {
            return null;
        }
    }

    public static String[] list(String dir) {
        try {
            return AssetManager.refractInstance().list(dir);
        } catch (Exception e) {
            return new String[0];
        }
    }
}
