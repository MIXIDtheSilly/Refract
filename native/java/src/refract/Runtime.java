package refract;

import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.HashSet;
import java.util.Set;

/** Bridge to refract_native: guest dynamic linker, native method binding, logging, paths. */
public final class Runtime {
    private Runtime() {}

    public static final String PACKAGE = System.getProperty("refract.package", "");
    public static final String ACTIVITY = System.getProperty("refract.activity", "");
    public static final String LABEL = System.getProperty("refract.label", PACKAGE);
    public static final String APK = System.getProperty("refract.apk", "");
    public static final String NATIVE_LIBRARY_DIR = System.getProperty("refract.nativeLibraryDir", "");
    public static final String DATA_DIR = System.getProperty("refract.dataDir", "");
    public static final String EXTERNAL_DIR = System.getProperty("refract.externalDir", "");
    public static final int VERSION_CODE = Integer.getInteger("refract.versionCode", 1);
    public static final String VERSION_NAME = System.getProperty("refract.versionName", "1.0");

    static native long dlopen(String path);
    static native long dlsym(long handle, String name);
    static native String dlerror();
    static native int callJniOnLoad(long fn);
    static native String[] javaExports(String path);
    static native boolean bind(Class<?> c, String name, String sig, long fn);
    public static native void log(int priority, String tag, String msg);
    /** Host path for a guest path (for java.io); returns the input if unmapped. */
    public static native String hostPath(String guestPath);
    public static native String guestPath(String hostPath);

    private static final Set<String> loaded = new HashSet<>();

    public static void loadLibrary(String name) {
        load(NATIVE_LIBRARY_DIR + "/lib" + name + ".so");
    }

    public static void runtimeLoadLibrary(java.lang.Runtime r, String name) {
        loadLibrary(name);
    }

    public static void runtimeLoad(java.lang.Runtime r, String path) {
        load(path);
    }

    public static synchronized void load(String path) {
        if (path.length() > 1 && path.charAt(1) == ':') path = guestPath(path);
        if (loaded.contains(path)) return;
        long handle = dlopen(path);
        if (handle == 0) {
            String err = dlerror();
            log(6, "refract", "dlopen failed: " + err);
            throw new UnsatisfiedLinkError(err);
        }
        loaded.add(path);
        bindExports(path, handle);
        long onLoad = dlsym(handle, "JNI_OnLoad");
        if (onLoad != 0) {
            int version = callJniOnLoad(onLoad);
            if (version < 0) throw new UnsatisfiedLinkError("JNI_OnLoad failed in " + path);
        }
    }

    /** Binds exported Java_* functions (natives not registered through RegisterNatives). */
    static void bindExports(String path, long handle) {
        ClassLoader loader = Runtime.class.getClassLoader();
        for (String sym : javaExports(path)) {
            String[] decoded = demangle(sym.substring(5));
            if (decoded == null) continue;
            Class<?> c;
            try {
                c = Class.forName(decoded[0], false, loader);
            } catch (Throwable t) {
                continue;
            }
            long fn = dlsym(handle, sym);
            if (fn == 0) continue;
            for (Method m : c.getDeclaredMethods()) {
                if (!Modifier.isNative(m.getModifiers()) || !m.getName().equals(decoded[1])) continue;
                String desc = descriptor(m);
                if (decoded[2] != null && !desc.startsWith("(" + decoded[2] + ")")) continue;
                bind(c, m.getName(), desc, fn);
            }
        }
    }

    /** "com_foo_Bar_baz__I" -> {"com.foo.Bar", "baz", "I"} (JNI name mangling). */
    static String[] demangle(String s) {
        StringBuilder cur = new StringBuilder();
        java.util.List<String> parts = new java.util.ArrayList<>();
        String sig = null;
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            if (ch != '_') {
                cur.append(ch);
                continue;
            }
            if (i + 1 < s.length()) {
                char n = s.charAt(i + 1);
                if (n == '1') { cur.append('_'); i++; continue; }
                if (n == '2') { cur.append(';'); i++; continue; }
                if (n == '3') { cur.append('['); i++; continue; }
                if (n == '0' && i + 5 < s.length()) {
                    cur.append((char) Integer.parseInt(s.substring(i + 2, i + 6), 16));
                    i += 5;
                    continue;
                }
                if (n == '_') {
                    parts.add(cur.toString());
                    String rest = s.substring(i + 2);
                    sig = rest.replace("_2", ";").replace("_3", "[").replace("_1", "\u0001").replace('_', '/')
                            .replace('\u0001', '_');
                    cur = null;
                    break;
                }
            }
            parts.add(cur.toString());
            cur = new StringBuilder();
        }
        if (cur != null) parts.add(cur.toString());
        if (parts.size() < 2) return null;
        String method = parts.remove(parts.size() - 1);
        return new String[] {String.join(".", parts), method, sig};
    }

    public static String descriptor(Method m) {
        StringBuilder sb = new StringBuilder("(");
        for (Class<?> p : m.getParameterTypes()) sb.append(typeDescriptor(p));
        return sb.append(')').append(typeDescriptor(m.getReturnType())).toString();
    }

    static String typeDescriptor(Class<?> c) {
        if (c == void.class) return "V";
        if (c == boolean.class) return "Z";
        if (c == byte.class) return "B";
        if (c == char.class) return "C";
        if (c == short.class) return "S";
        if (c == int.class) return "I";
        if (c == long.class) return "J";
        if (c == float.class) return "F";
        if (c == double.class) return "D";
        if (c.isArray()) return c.getName().replace('.', '/');
        return "L" + c.getName().replace('.', '/') + ";";
    }
}
