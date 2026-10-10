package refract.app;

import android.content.res.XmlResourceParser;
import android.util.TypedValue;
import java.io.InputStream;
import java.io.Reader;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

/** A compiled (binary) XML file of the APK, e.g. a layout, as XmlResourceParser + AttributeSet. */
public final class BinaryXml implements XmlResourceParser {
    private static final String ANDROID_NS = "http://schemas.android.com/apk/res/android";
    private static final int ATTR_ID = 0x010100d0;

    private static final class Attr {
        String ns, name, raw;
        int resource;  // attribute id from the resource map, 0 if none
        ResourceTable.Value value;
    }

    private static final class Event {
        int type, line, depth;
        String ns, name, text;
        Attr[] attrs = new Attr[0];
        int idIndex = -1, classIndex = -1, styleIndex = -1;
    }

    private final List<Event> events = new ArrayList<>();
    private int pos = -1;  // index of the current event; -1 = START_DOCUMENT

    public BinaryXml(byte[] data) {
        ByteBuffer b = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        String[] strings = new String[0];
        int[] resMap = new int[0];
        int pos = b.getShort(2) & 0xffff, size = Math.min(b.getInt(4), data.length), depth = 0;
        while (pos + 8 <= size) {
            int type = b.getShort(pos) & 0xffff, header = b.getShort(pos + 2) & 0xffff, chunk = b.getInt(pos + 4);
            if (chunk <= 0)
                break;
            if (type == 0x0001) {
                strings = ResourceTable.strings(b, pos);
            } else if (type == 0x0180) {
                resMap = new int[(chunk - header) / 4];
                for (int i = 0; i < resMap.length; ++i)
                    resMap[i] = b.getInt(pos + header + i * 4);
            } else if (type == 0x0102 || type == 0x0103 || type == 0x0104) {
                Event ev = new Event();
                ev.line = b.getInt(pos + 8);
                int x = pos + header;
                if (type == 0x0104) {
                    ev.type = TEXT;
                    ev.text = str(strings, b.getInt(x));
                    ev.depth = depth;
                } else {
                    ev.ns = str(strings, b.getInt(x));
                    ev.name = str(strings, b.getInt(x + 4));
                }
                if (type == 0x0102) {
                    ev.type = START_TAG;
                    ev.depth = ++depth;
                    int attrStart = b.getShort(x + 8) & 0xffff, attrSize = b.getShort(x + 10) & 0xffff;
                    int count = b.getShort(x + 12) & 0xffff;
                    ev.idIndex = (b.getShort(x + 14) & 0xffff) - 1;
                    ev.classIndex = (b.getShort(x + 16) & 0xffff) - 1;
                    ev.styleIndex = (b.getShort(x + 18) & 0xffff) - 1;
                    ev.attrs = new Attr[count];
                    for (int i = 0; i < count; ++i) {
                        int a = x + attrStart + i * attrSize;
                        Attr at = new Attr();
                        at.ns = str(strings, b.getInt(a));
                        int nameIdx = b.getInt(a + 4);
                        at.name = str(strings, nameIdx);
                        at.resource = nameIdx >= 0 && nameIdx < resMap.length ? resMap[nameIdx] : 0;
                        at.raw = str(strings, b.getInt(a + 8));
                        int vt = b.get(a + 15) & 0xff, vd = b.getInt(a + 16);
                        at.value = new ResourceTable.Value(vt, vd, vt == ResourceTable.TYPE_STRING ? str(strings, vd) : at.raw);
                        ev.attrs[i] = at;
                    }
                } else if (type == 0x0103) {
                    ev.type = END_TAG;
                    ev.depth = depth--;
                }
                events.add(ev);
            }
            pos += chunk;
        }
    }

    private static String str(String[] strings, int i) { return i >= 0 && i < strings.length ? strings[i] : null; }

    private Event cur() { return pos >= 0 && pos < events.size() ? events.get(pos) : null; }
    private Attr attr(int i) {
        Event e = cur();
        if (e == null || e.type != START_TAG || i < 0 || i >= e.attrs.length)
            throw new IndexOutOfBoundsException("attribute " + i);
        return e.attrs[i];
    }
    private int find(String ns, String name) {
        Event e = cur();
        if (e == null || e.type != START_TAG)
            return -1;
        for (int i = 0; i < e.attrs.length; ++i) {
            Attr a = e.attrs[i];
            if (a.name != null && a.name.equals(name) && (ns == null || ns.equals(a.ns) || (a.ns == null && ns.isEmpty())))
                return i;
        }
        return -1;
    }

    /** The current element's attributes by attribute resource id, for Resources.obtain. */
    public Map<Integer, ResourceTable.Value> refractValues() {
        Map<Integer, ResourceTable.Value> out = new HashMap<>();
        Event e = cur();
        if (e != null && e.type == START_TAG)
            for (Attr a : e.attrs)
                if (a.resource != 0) out.put(a.resource, a.value);
        return out;
    }
    public ResourceTable.Value refractStyleValue() {
        Event e = cur();
        return e != null && e.styleIndex >= 0 && e.styleIndex < e.attrs.length ? e.attrs[e.styleIndex].value : null;
    }

    // --- XmlPullParser ----------------------------------------------------------------

    public int getEventType() {
        if (pos < 0) return START_DOCUMENT;
        Event e = cur();
        return e == null ? END_DOCUMENT : e.type;
    }
    public int next() {
        if (pos < events.size()) ++pos;
        return getEventType();
    }
    public int nextToken() { return next(); }
    public int nextTag() throws XmlPullParserException {
        int t = next();
        while (t == TEXT && isWhitespace()) t = next();
        if (t != START_TAG && t != END_TAG)
            throw new XmlPullParserException("expected start or end tag", this, null);
        return t;
    }
    public String nextText() throws XmlPullParserException {
        if (getEventType() != START_TAG)
            throw new XmlPullParserException("parser must be on START_TAG to read next text", this, null);
        int t = next();
        if (t == TEXT) {
            String s = getText();
            next();
            return s;
        }
        return "";
    }
    public void require(int type, String namespace, String name) throws XmlPullParserException {
        if (type != getEventType() || (namespace != null && !namespace.equals(getNamespace()))
                || (name != null && !name.equals(getName())))
            throw new XmlPullParserException("expected " + TYPES[type] + " " + name, this, null);
    }
    public int getDepth() {
        Event e = cur();
        return e == null ? 0 : e.depth;  // an END_TAG has its START_TAG's depth
    }
    public String getName() { Event e = cur(); return e != null && e.type != TEXT ? e.name : null; }
    public String getNamespace() { Event e = cur(); return e != null && e.ns != null ? e.ns : ""; }
    public String getPrefix() { return null; }
    public String getText() { Event e = cur(); return e != null ? e.text : null; }
    public char[] getTextCharacters(int[] holderForStartAndLength) {
        String t = getText();
        if (t == null) return null;
        holderForStartAndLength[0] = 0;
        holderForStartAndLength[1] = t.length();
        return t.toCharArray();
    }
    public boolean isWhitespace() { String t = getText(); return t == null || t.isBlank(); }
    public boolean isEmptyElementTag() { return false; }
    public int getLineNumber() { Event e = cur(); return e != null ? e.line : -1; }
    public int getColumnNumber() { return -1; }
    public String getPositionDescription() { return "Binary XML file line #" + getLineNumber(); }
    public void setFeature(String name, boolean state) {}
    public boolean getFeature(String name) { return false; }
    public void setProperty(String name, Object value) {}
    public Object getProperty(String name) { return null; }
    public void setInput(Reader in) {}
    public void setInput(InputStream in, String encoding) {}
    public String getInputEncoding() { return "UTF-8"; }
    public void defineEntityReplacementText(String entityName, String replacementText) {}
    public int getNamespaceCount(int depth) { return 0; }
    public String getNamespacePrefix(int pos) { return null; }
    public String getNamespaceUri(int pos) { return null; }
    public String getNamespace(String prefix) { return "android".equals(prefix) ? ANDROID_NS : null; }
    public String getAttributePrefix(int index) { return null; }
    public String getAttributeType(int index) { return "CDATA"; }
    public boolean isAttributeDefault(int index) { return false; }
    public void close() {}

    // --- AttributeSet ------------------------------------------------------------------

    public int getAttributeCount() {
        Event e = cur();
        return e != null && e.type == START_TAG ? e.attrs.length : -1;
    }
    public String getAttributeNamespace(int index) { String ns = attr(index).ns; return ns != null ? ns : ""; }
    public String getAttributeName(int index) { return attr(index).name; }
    public int getAttributeNameResource(int index) { return attr(index).resource; }
    public String getAttributeValue(int index) {
        Attr a = attr(index);
        if (a.raw != null) return a.raw;
        if (a.value.string != null) return a.value.string;
        return TypedValue.coerceToString(a.value.type, a.value.data);
    }
    public String getAttributeValue(String namespace, String name) {
        int i = find(namespace, name);
        return i < 0 ? null : getAttributeValue(i);
    }
    public int getAttributeListValue(String namespace, String attribute, String[] options, int defaultValue) {
        return getAttributeListValue(find(namespace, attribute), options, defaultValue);
    }
    public boolean getAttributeBooleanValue(String namespace, String attribute, boolean defaultValue) {
        return getAttributeBooleanValue(find(namespace, attribute), defaultValue);
    }
    public int getAttributeResourceValue(String namespace, String attribute, int defaultValue) {
        return getAttributeResourceValue(find(namespace, attribute), defaultValue);
    }
    public int getAttributeIntValue(String namespace, String attribute, int defaultValue) {
        return getAttributeIntValue(find(namespace, attribute), defaultValue);
    }
    public int getAttributeUnsignedIntValue(String namespace, String attribute, int defaultValue) {
        return getAttributeIntValue(find(namespace, attribute), defaultValue);
    }
    public float getAttributeFloatValue(String namespace, String attribute, float defaultValue) {
        return getAttributeFloatValue(find(namespace, attribute), defaultValue);
    }
    public int getAttributeListValue(int index, String[] options, int defaultValue) {
        if (index < 0) return defaultValue;
        Attr a = attr(index);
        if (a.value.type >= ResourceTable.TYPE_INT_DEC && a.value.type <= ResourceTable.TYPE_LAST_COLOR) return a.value.data;
        String s = getAttributeValue(index);
        for (int i = 0; options != null && i < options.length; ++i)
            if (options[i].equals(s)) return i;
        return defaultValue;
    }
    public boolean getAttributeBooleanValue(int index, boolean defaultValue) {
        if (index < 0) return defaultValue;
        Attr a = attr(index);
        if (a.value.type >= ResourceTable.TYPE_INT_DEC && a.value.type <= ResourceTable.TYPE_LAST_COLOR) return a.value.data != 0;
        return a.raw != null ? Boolean.parseBoolean(a.raw) : defaultValue;
    }
    public int getAttributeResourceValue(int index, int defaultValue) {
        if (index < 0) return defaultValue;
        Attr a = attr(index);
        return a.value.type == ResourceTable.TYPE_REFERENCE || a.value.type == 0x07 ? a.value.data : defaultValue;
    }
    public int getAttributeIntValue(int index, int defaultValue) {
        if (index < 0) return defaultValue;
        Attr a = attr(index);
        if (a.value.type >= ResourceTable.TYPE_INT_DEC && a.value.type <= ResourceTable.TYPE_LAST_COLOR) return a.value.data;
        try { return a.raw != null ? Integer.decode(a.raw) : defaultValue; } catch (NumberFormatException e) { return defaultValue; }
    }
    public int getAttributeUnsignedIntValue(int index, int defaultValue) { return getAttributeIntValue(index, defaultValue); }
    public float getAttributeFloatValue(int index, float defaultValue) {
        if (index < 0) return defaultValue;
        Attr a = attr(index);
        if (a.value.type == ResourceTable.TYPE_FLOAT) return Float.intBitsToFloat(a.value.data);
        try { return a.raw != null ? Float.parseFloat(a.raw) : defaultValue; } catch (NumberFormatException e) { return defaultValue; }
    }
    public String getIdAttribute() {
        Event e = cur();
        return e != null && e.idIndex >= 0 && e.idIndex < e.attrs.length ? getAttributeValue(e.idIndex) : null;
    }
    public String getClassAttribute() {
        Event e = cur();
        return e != null && e.classIndex >= 0 && e.classIndex < e.attrs.length ? getAttributeValue(e.classIndex) : null;
    }
    public int getIdAttributeResourceValue(int defaultValue) {
        Event e = cur();
        if (e != null && e.idIndex >= 0 && e.idIndex < e.attrs.length) return getAttributeResourceValue(e.idIndex, defaultValue);
        for (int i = 0; e != null && i < e.attrs.length; ++i)
            if (e.attrs[i].resource == ATTR_ID) return getAttributeResourceValue(i, defaultValue);
        return defaultValue;
    }
    public int getStyleAttribute() {
        Event e = cur();
        if (e == null || e.styleIndex < 0 || e.styleIndex >= e.attrs.length) return 0;
        ResourceTable.Value v = e.attrs[e.styleIndex].value;
        return v.type == ResourceTable.TYPE_REFERENCE || v.type == ResourceTable.TYPE_ATTRIBUTE ? v.data : 0;
    }
}
