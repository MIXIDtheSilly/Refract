package refract.app;

import android.database.Cursor;
import android.net.Uri;
import java.util.List;

/**
 * Content providers of other packages that this process answers itself. The OpenXR
 * runtime broker (org.khronos.openxr.runtime_broker) names Refract's runtime, which the
 * installer puts in the app's library directory, as RuntimeBrokerProvider does on the emulator.
 */
public final class ContentProviders {
    private ContentProviders() {}

    private static final String[] ACTIVE_RUNTIME_COLUMNS = {"package_name", "native_lib_dir", "so_filename", "has_functions"};
    private static final String[] FUNCTION_COLUMNS = {"function_name", "symbol_name"};

    public static Cursor query(Uri uri, String[] projection) {
        String authority = uri != null ? uri.getAuthority() : null;
        if ("org.khronos.openxr.runtime_broker".equals(authority)) {
            // openxr/1/abi/<abi>/runtimes/active[/<n>/functions]
            List<String> s = uri.getPathSegments();
            if (s.size() >= 6 && "openxr".equals(s.get(0)) && "1".equals(s.get(1)) && "abi".equals(s.get(2))
                    && "arm64-v8a".equals(s.get(3)) && "runtimes".equals(s.get(4))) {
                if (s.size() == 6 && "active".equals(s.get(5))) {
                    refract.Runtime.log(4, "refract", "OpenXR runtime broker: Refract runtime in " + refract.Runtime.NATIVE_LIBRARY_DIR);
                    return new ListCursor(ACTIVE_RUNTIME_COLUMNS, new Object[][] {
                        {refract.Runtime.PACKAGE, refract.Runtime.NATIVE_LIBRARY_DIR, "libopenxr_runtime.so", 0}});
                }
                if (s.size() >= 7 && "functions".equals(s.get(6)))
                    return new ListCursor(FUNCTION_COLUMNS, new Object[0][]);
            }
            return null;
        }
        refract.Runtime.log(5, "refract", "ContentResolver.query(" + uri + "): no provider");
        return null;
    }
}
