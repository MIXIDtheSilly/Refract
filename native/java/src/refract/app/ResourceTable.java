package refract.app;

import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * The resources.arsc of the APK and of the firmware's framework-res.apk: resource
 * ids by name, simple values (strings, integers, booleans, colors, dimensions,
 * file paths) and styles (bags). For each resource the configuration closest to
 * the device is used: the default one or an API-level-only qualifier up to
 * {@link #SDK}; anything else (night, land, locales...) only when nothing better exists.
 */
public final class ResourceTable {
    /** A value as stored in the table (Res_value); strings are resolved against their own table. */
    public static final class Value {
        public final int type;
        public final int data;
        public final String string;
        public Value(int type, int data, String string) {
            this.type = type;
            this.data = data;
            this.string = string;
        }
    }

    /** A style or other bag entry: parent and (attribute id, value) pairs. */
    public static final class Bag {
        public final int parent;
        public final int[] keys;
        public final Value[] values;
        Bag(int parent, int[] keys, Value[] values) {
            this.parent = parent;
            this.keys = keys;
            this.values = values;
        }
    }

    public static final int TYPE_NULL = 0x00, TYPE_REFERENCE = 0x01, TYPE_ATTRIBUTE = 0x02, TYPE_STRING = 0x03,
            TYPE_FLOAT = 0x04, TYPE_DIMENSION = 0x05, TYPE_FRACTION = 0x06, TYPE_INT_DEC = 0x10, TYPE_INT_HEX = 0x11,
            TYPE_BOOLEAN = 0x12, TYPE_FIRST_COLOR = 0x1c, TYPE_LAST_COLOR = 0x1f;
    static final int SDK = 32;

    private static ResourceTable instance;

    private final Map<String, Integer> ids = new HashMap<>();      // "pkg:type/name" -> id
    private final Map<Integer, String> names = new HashMap<>();    // id -> "pkg:type/name"
    private final Map<Integer, Value> values = new HashMap<>();
    private final Map<Integer, Bag> bags = new HashMap<>();
    private final Map<Integer, Integer> scores = new HashMap<>();  // id -> score of the stored config
    private String appPackage = "";
    private String[] appStrings = new String[0];

    public static synchronized ResourceTable get() {
        if (instance == null) {
            instance = new ResourceTable();
            instance.load(System.getProperty("refract.apkHost"), true);
            try {
                instance.load(refract.Runtime.hostPath("/system/framework/framework-res.apk"), false);
            } catch (Throwable t) {
                refract.Runtime.log(5, "refract", "framework-res.apk: " + t);
            }
        }
        return instance;
    }

    private void load(String path, boolean app) {
        try (ZipFile z = new ZipFile(path)) {
            ZipEntry e = z.getEntry("resources.arsc");
            if (e != null) {
                try (InputStream in = z.getInputStream(e)) {
                    parse(ByteBuffer.wrap(in.readAllBytes()).order(ByteOrder.LITTLE_ENDIAN), app);
                }
            }
        } catch (Exception ex) {
            refract.Runtime.log(5, "refract", "resources.arsc of " + path + ": " + ex);
        }
    }

    public int identifier(String name, String defType, String defPackage) {
        if (name == null)
            return 0;
        if (name.startsWith("@") || name.startsWith("?"))
            name = name.substring(1);
        String pkg = defPackage;
        int colon = name.indexOf(':');
        if (colon >= 0) {
            pkg = name.substring(0, colon);
            name = name.substring(colon + 1);
        }
        if (pkg == null || pkg.isEmpty() || !pkg.equals("android"))
            pkg = appPackage;
        String key = pkg + ":" + (name.indexOf('/') >= 0 ? name : defType + "/" + name);
        Integer id = ids.get(key);
        return id == null ? 0 : id;
    }

    public String name(int id) { return names.get(id); }

    /** The value of a resource, following references to other resources. */
    public Value value(int id) {
        Value v = values.get(id);
        for (int depth = 0; v != null && v.type == TYPE_REFERENCE && v.data != 0 && depth < 8; ++depth)
            v = values.get(v.data);
        return v;
    }
    /** The value as stored (references not followed). */
    public Value rawValue(int id) { return values.get(id); }
    public Bag bag(int id) { return bags.get(id); }
    /** A string of the APK's global string pool. */
    public String string(int index) { return index >= 0 && index < appStrings.length ? appStrings[index] : null; }

    private final Map<String, ZipFile> zips = new HashMap<>();

    /** A file of the APK (or of framework-res.apk), e.g. "res/layout/main.xml"; null if absent. */
    public synchronized byte[] file(String path, boolean framework) {
        try {
            String apk = framework ? refract.Runtime.hostPath("/system/framework/framework-res.apk")
                                   : System.getProperty("refract.apkHost");
            ZipFile z = zips.get(apk);
            if (z == null)
                zips.put(apk, z = new ZipFile(apk));
            ZipEntry e = z.getEntry(path);
            if (e == null)
                return null;
            try (InputStream in = z.getInputStream(e)) {
                return in.readAllBytes();
            }
        } catch (Exception ex) {
            refract.Runtime.log(5, "refract", "resource file " + path + ": " + ex);
            return null;
        }
    }

    /** The attributes a style sets, its parents' included (the style's own values win). */
    public Map<Integer, Value> styleAttributes(int styleId) {
        List<Bag> chain = new ArrayList<>();
        for (int id = styleId, depth = 0; id != 0 && depth < 64; ++depth) {
            Bag b = bags.get(id);
            if (b == null)
                break;
            chain.add(b);
            id = b.parent;
        }
        Map<Integer, Value> out = new HashMap<>();
        for (int i = chain.size() - 1; i >= 0; --i) {
            Bag b = chain.get(i);
            for (int k = 0; k < b.keys.length; ++k)
                out.put(b.keys[k], b.values[k]);
        }
        return out;
    }

    // --- parsing --------------------------------------------------------------------

    private void parse(ByteBuffer b, boolean app) {
        int headerSize = b.getShort(2) & 0xffff;
        int size = b.getInt(4);
        int pos = headerSize;
        String[] strings = new String[0];
        while (pos + 8 <= size) {
            int type = b.getShort(pos) & 0xffff;
            int chunkSize = b.getInt(pos + 4);
            if (chunkSize <= 0)
                break;
            if (type == 0x0001) {
                strings = strings(b, pos);
                if (app)
                    appStrings = strings;
            } else if (type == 0x0200) {
                parsePackage(b, pos, strings, app);
            }
            pos += chunkSize;
        }
    }

    private void parsePackage(ByteBuffer b, int start, String[] strings, boolean app) {
        int headerSize = b.getShort(start + 2) & 0xffff;
        int size = b.getInt(start + 4);
        int pkgId = b.getInt(start + 8);
        StringBuilder pn = new StringBuilder();
        for (int i = 0; i < 128; ++i) {
            char c = b.getChar(start + 12 + i * 2);
            if (c == 0)
                break;
            pn.append(c);
        }
        String pkg = pn.toString();
        if (app && appPackage.isEmpty())
            appPackage = pkg;
        String[] typeNames = strings(b, start + b.getInt(start + 12 + 256));
        String[] keyNames = strings(b, start + b.getInt(start + 12 + 256 + 8));
        int pos = start + headerSize;
        while (pos + 8 <= start + size) {
            int type = b.getShort(pos) & 0xffff;
            int chunkSize = b.getInt(pos + 4);
            if (chunkSize <= 0)
                break;
            if (type == 0x0201)
                parseType(b, pos, pkgId, pkg, typeNames, keyNames, strings);
            pos += chunkSize;
        }
    }

    /** -1: a configuration that does not describe this device; else higher = more specific match. */
    private static int configScore(ByteBuffer b, int config) {
        int size = b.getInt(config);
        for (int i = 4; i < size; ++i) {
            if (i == 14 || i == 15 || (i >= 24 && i < 28))
                continue;  // density, sdkVersion/minorVersion
            if (b.get(config + i) != 0)
                return -1;
        }
        int sdk = size >= 26 ? b.getShort(config + 24) & 0xffff : 0;
        return sdk <= SDK ? sdk : -1;
    }

    private Value readValue(ByteBuffer b, int v, String[] strings) {
        int type = b.get(v + 3) & 0xff;
        int data = b.getInt(v + 4);
        return new Value(type, data, type == TYPE_STRING && data >= 0 && data < strings.length ? strings[data] : null);
    }

    private void parseType(ByteBuffer b, int start, int pkgId, String pkg, String[] typeNames, String[] keyNames,
                           String[] strings) {
        int headerSize = b.getShort(start + 2) & 0xffff;
        int typeId = b.get(start + 8) & 0xff;
        int flags = b.get(start + 9) & 0xff;
        int entryCount = b.getInt(start + 12);
        int entriesStart = b.getInt(start + 16);
        int score = configScore(b, start + 20);
        String typeName = typeId - 1 < typeNames.length ? typeNames[typeId - 1] : "type" + typeId;
        int offsets = start + headerSize;
        boolean sparse = (flags & 0x01) != 0, offset16 = (flags & 0x02) != 0;
        for (int i = 0; i < entryCount; ++i) {
            int index, offset;
            if (sparse) {
                index = b.getShort(offsets + i * 4) & 0xffff;
                offset = (b.getShort(offsets + i * 4 + 2) & 0xffff) * 4;
            } else if (offset16) {
                index = i;
                int o = b.getShort(offsets + i * 2) & 0xffff;
                if (o == 0xffff)
                    continue;
                offset = o * 4;
            } else {
                index = i;
                offset = b.getInt(offsets + i * 4);
                if (offset == -1)
                    continue;
            }
            int e = start + entriesStart + offset;
            int entrySize = b.getShort(e) & 0xffff;
            int entryFlags = b.getShort(e + 2) & 0xffff;
            int id = (pkgId << 24) | (typeId << 16) | index;
            int key;
            boolean compact = (entryFlags & 0x0008) != 0;
            key = compact ? b.getShort(e) & 0xffff : b.getInt(e + 4);
            String name = key >= 0 && key < keyNames.length ? keyNames[key] : "res" + Integer.toHexString(id);
            ids.putIfAbsent(pkg + ":" + typeName + "/" + name, id);
            names.putIfAbsent(id, pkg + ":" + typeName + "/" + name);
            Integer old = scores.get(id);
            if (old != null && old >= score)
                continue;
            scores.put(id, score);
            if (compact) {  // key, flags (type in high byte), data
                int type = entryFlags >>> 8, data = b.getInt(e + 4);
                values.put(id, new Value(type, data,
                        type == TYPE_STRING && data >= 0 && data < strings.length ? strings[data] : null));
            } else if ((entryFlags & 0x0001) == 0) {  // simple entry: Res_value follows
                values.put(id, readValue(b, e + entrySize, strings));
            } else {  // map entry: parent, count, then (name, Res_value) pairs
                int parent = b.getInt(e + 8);
                int count = b.getInt(e + 12);
                int[] keys = new int[count];
                Value[] vals = new Value[count];
                int m = e + entrySize;
                for (int k = 0; k < count; ++k, m += 12) {
                    keys[k] = b.getInt(m);
                    vals[k] = readValue(b, m + 4, strings);
                }
                bags.put(id, new Bag(parent, keys, vals));
            }
        }
    }

    static String[] strings(ByteBuffer b, int start) {
        int count = b.getInt(start + 8);
        int flags = b.getInt(start + 16);
        int stringsStart = start + b.getInt(start + 20);
        boolean utf8 = (flags & 0x100) != 0;
        int offsets = start + (b.getShort(start + 2) & 0xffff);
        String[] out = new String[count];
        for (int i = 0; i < count; ++i) {
            int p = stringsStart + b.getInt(offsets + i * 4);
            if (utf8) {
                int n = b.get(p) & 0xff;  // utf-16 length, skipped
                p += (n & 0x80) != 0 ? 2 : 1;
                int len = b.get(p) & 0xff;
                if ((len & 0x80) != 0) {
                    len = ((len & 0x7f) << 8) | (b.get(p + 1) & 0xff);
                    p += 2;
                } else {
                    p += 1;
                }
                byte[] bytes = new byte[len];
                for (int j = 0; j < len; ++j)
                    bytes[j] = b.get(p + j);
                out[i] = new String(bytes, StandardCharsets.UTF_8);
            } else {
                int len = b.getShort(p) & 0xffff;
                if ((len & 0x8000) != 0) {
                    len = ((len & 0x7fff) << 16) | (b.getShort(p + 2) & 0xffff);
                    p += 4;
                } else {
                    p += 2;
                }
                char[] chars = new char[len];
                for (int j = 0; j < len; ++j)
                    chars[j] = b.getChar(p + j * 2);
                out[i] = new String(chars);
            }
        }
        return out;
    }
}
