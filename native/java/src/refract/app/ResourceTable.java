package refract.app;

import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * The APK's resources.arsc: resource ids by name and simple values (strings,
 * integers, booleans, colors). Entries from the default configuration win; for
 * others the first configuration seen is used.
 */
public final class ResourceTable {
    /** A value as stored in the table (Res_value). */
    public static final class Value {
        public final int type;
        public final int data;
        Value(int type, int data) {
            this.type = type;
            this.data = data;
        }
    }

    public static final int TYPE_REFERENCE = 0x01, TYPE_STRING = 0x03, TYPE_FLOAT = 0x04, TYPE_DIMENSION = 0x05,
            TYPE_INT_DEC = 0x10, TYPE_INT_HEX = 0x11, TYPE_BOOLEAN = 0x12, TYPE_FIRST_COLOR = 0x1c, TYPE_LAST_COLOR = 0x1f;

    private static ResourceTable instance;

    private final Map<String, Integer> ids = new HashMap<>();      // "type/name" -> id
    private final Map<Integer, String> names = new HashMap<>();    // id -> "pkg:type/name"
    private final Map<Integer, Value> values = new HashMap<>();
    private final Map<Integer, Boolean> fromDefault = new HashMap<>();
    private String[] globalStrings = new String[0];

    public static synchronized ResourceTable get() {
        if (instance == null) {
            instance = new ResourceTable();
            try (ZipFile z = new ZipFile(System.getProperty("refract.apkHost"))) {
                ZipEntry e = z.getEntry("resources.arsc");
                if (e != null) {
                    try (InputStream in = z.getInputStream(e)) {
                        instance.parse(ByteBuffer.wrap(in.readAllBytes()).order(ByteOrder.LITTLE_ENDIAN));
                    }
                }
            } catch (Exception ex) {
                refract.Runtime.log(5, "refract", "resources.arsc: " + ex);
            }
        }
        return instance;
    }

    public int identifier(String name, String defType, String defPackage) {
        if (name == null)
            return 0;
        int colon = name.indexOf(':');
        if (colon >= 0)
            name = name.substring(colon + 1);
        if (name.startsWith("@"))
            name = name.substring(1);
        String key = name.indexOf('/') >= 0 ? name : defType + "/" + name;
        Integer id = ids.get(key);
        return id == null ? 0 : id;
    }

    public String name(int id) { return names.get(id); }
    public Value value(int id) {
        Value v = values.get(id);
        for (int depth = 0; v != null && v.type == TYPE_REFERENCE && v.data != 0 && depth < 8; ++depth)
            v = values.get(v.data);
        return v;
    }
    public String string(int index) { return index >= 0 && index < globalStrings.length ? globalStrings[index] : null; }

    // --- parsing --------------------------------------------------------------------

    private void parse(ByteBuffer b) {
        int headerSize = b.getShort(2) & 0xffff;
        int size = b.getInt(4);
        int pos = headerSize;
        while (pos + 8 <= size) {
            int type = b.getShort(pos) & 0xffff;
            int chunkSize = b.getInt(pos + 4);
            if (chunkSize <= 0)
                break;
            if (type == 0x0001)
                globalStrings = strings(b, pos);
            else if (type == 0x0200)
                parsePackage(b, pos);
            pos += chunkSize;
        }
    }

    private void parsePackage(ByteBuffer b, int start) {
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
        String[] typeNames = strings(b, start + b.getInt(start + 12 + 256));
        String[] keyNames = strings(b, start + b.getInt(start + 12 + 256 + 8));
        int pos = start + headerSize;
        while (pos + 8 <= start + size) {
            int type = b.getShort(pos) & 0xffff;
            int chunkSize = b.getInt(pos + 4);
            if (chunkSize <= 0)
                break;
            if (type == 0x0201)
                parseType(b, pos, pkgId, pkg, typeNames, keyNames);
            pos += chunkSize;
        }
    }

    private void parseType(ByteBuffer b, int start, int pkgId, String pkg, String[] typeNames, String[] keyNames) {
        int headerSize = b.getShort(start + 2) & 0xffff;
        int typeId = b.get(start + 8) & 0xff;
        int flags = b.get(start + 9) & 0xff;
        int entryCount = b.getInt(start + 12);
        int entriesStart = b.getInt(start + 16);
        int configStart = start + 20;
        int configSize = b.getInt(configStart);
        boolean isDefault = true;
        for (int i = 4; i < configSize && isDefault; ++i)
            isDefault = b.get(configStart + i) == 0;
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
            int entryFlags = b.getShort(e + 2) & 0xffff;
            int key;
            Value value = null;
            if ((entryFlags & 0x0008) != 0) {  // compact: key, flags (type in high byte), data
                key = b.getShort(e) & 0xffff;
                value = new Value(entryFlags >>> 8, b.getInt(e + 4));
            } else {
                key = b.getInt(e + 4);
                if ((entryFlags & 0x0001) == 0) {  // simple entry: Res_value follows
                    int v = e + (b.getShort(e) & 0xffff);
                    value = new Value(b.get(v + 3) & 0xff, b.getInt(v + 4));
                }
            }
            int id = (pkgId << 24) | (typeId << 16) | index;
            String name = key >= 0 && key < keyNames.length ? keyNames[key] : "res" + Integer.toHexString(id);
            ids.putIfAbsent(typeName + "/" + name, id);
            names.putIfAbsent(id, pkg + ":" + typeName + "/" + name);
            if (value != null && (!values.containsKey(id) || (isDefault && !fromDefault.getOrDefault(id, false)))) {
                values.put(id, value);
                fromDefault.put(id, isDefault);
            }
        }
    }

    private static String[] strings(ByteBuffer b, int start) {
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
