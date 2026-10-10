package refract.app;

import android.content.ContentResolver;
import android.database.CharArrayBuffer;
import android.database.ContentObserver;
import android.database.Cursor;
import android.database.DataSetObserver;
import android.net.Uri;
import android.os.Bundle;

/** An in-memory Cursor over rows of objects (MatrixCursor's role). */
public final class ListCursor implements Cursor {
    private final String[] columns;
    private final Object[][] rows;
    private int pos = -1;
    private boolean closed;
    private Bundle extras = Bundle.EMPTY;

    public ListCursor(String[] columns, Object[][] rows) {
        this.columns = columns;
        this.rows = rows;
    }

    private Object get(int column) {
        if (pos < 0 || pos >= rows.length) throw new IndexOutOfBoundsException("cursor position " + pos);
        return rows[pos][column];
    }

    public int getCount() { return rows.length; }
    public int getPosition() { return pos; }
    public boolean move(int offset) { return moveToPosition(pos + offset); }
    public boolean moveToPosition(int position) {
        pos = Math.max(-1, Math.min(rows.length, position));
        return pos >= 0 && pos < rows.length;
    }
    public boolean moveToFirst() { return moveToPosition(0); }
    public boolean moveToLast() { return moveToPosition(rows.length - 1); }
    public boolean moveToNext() { return moveToPosition(pos + 1); }
    public boolean moveToPrevious() { return moveToPosition(pos - 1); }
    public boolean isFirst() { return pos == 0 && rows.length > 0; }
    public boolean isLast() { return pos == rows.length - 1 && rows.length > 0; }
    public boolean isBeforeFirst() { return rows.length == 0 || pos == -1; }
    public boolean isAfterLast() { return rows.length == 0 || pos == rows.length; }
    public int getColumnIndex(String name) {
        for (int i = 0; i < columns.length; ++i)
            if (columns[i].equals(name)) return i;
        return -1;
    }
    public int getColumnIndexOrThrow(String name) {
        int i = getColumnIndex(name);
        if (i < 0) throw new IllegalArgumentException("column '" + name + "' does not exist");
        return i;
    }
    public String getColumnName(int column) { return columns[column]; }
    public String[] getColumnNames() { return columns.clone(); }
    public int getColumnCount() { return columns.length; }
    public byte[] getBlob(int column) { return (byte[]) get(column); }
    public String getString(int column) {
        Object o = get(column);
        return o == null ? null : o.toString();
    }
    public void copyStringToBuffer(int column, CharArrayBuffer buffer) {
        String s = getString(column);
        buffer.data = s == null ? new char[0] : s.toCharArray();
        buffer.sizeCopied = buffer.data.length;
    }
    private Number number(int column) {
        Object o = get(column);
        if (o instanceof Number n) return n;
        if (o instanceof Boolean b) return b ? 1 : 0;
        return o == null ? 0 : Double.parseDouble(o.toString());
    }
    public short getShort(int column) { return number(column).shortValue(); }
    public int getInt(int column) { return number(column).intValue(); }
    public long getLong(int column) { return number(column).longValue(); }
    public float getFloat(int column) { return number(column).floatValue(); }
    public double getDouble(int column) { return number(column).doubleValue(); }
    public int getType(int column) {
        Object o = get(column);
        if (o == null) return FIELD_TYPE_NULL;
        if (o instanceof byte[]) return FIELD_TYPE_BLOB;
        if (o instanceof Float || o instanceof Double) return FIELD_TYPE_FLOAT;
        if (o instanceof Number || o instanceof Boolean) return FIELD_TYPE_INTEGER;
        return FIELD_TYPE_STRING;
    }
    public boolean isNull(int column) { return get(column) == null; }
    public void deactivate() {}
    public boolean requery() { return !closed; }
    public void close() { closed = true; }
    public boolean isClosed() { return closed; }
    public void registerContentObserver(ContentObserver observer) {}
    public void unregisterContentObserver(ContentObserver observer) {}
    public void registerDataSetObserver(DataSetObserver observer) {}
    public void unregisterDataSetObserver(DataSetObserver observer) {}
    public void setNotificationUri(ContentResolver cr, Uri uri) {}
    public Uri getNotificationUri() { return null; }
    public boolean getWantsAllOnMoveCalls() { return false; }
    public void setExtras(Bundle extras) { this.extras = extras == null ? Bundle.EMPTY : extras; }
    public Bundle getExtras() { return extras; }
    public Bundle respond(Bundle extras) { return Bundle.EMPTY; }
}
