package android.content;

import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;

/** Queries go to refract.app.ContentProviders (the providers this process stands in for). */
public abstract class ContentResolver {
    private final Context mContext;

    public ContentResolver(Context context) { mContext = context; }

    public final Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        return refract.app.ContentProviders.query(uri, projection);
    }
    public final Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder,
                              CancellationSignal cancellationSignal) {
        return refract.app.ContentProviders.query(uri, projection);
    }
    public final Cursor query(Uri uri, String[] projection, Bundle queryArgs, CancellationSignal cancellationSignal) {
        return refract.app.ContentProviders.query(uri, projection);
    }
    public final String getType(Uri url) { return null; }

    // Nothing changes behind a cursor here, so observers are never notified.
    public final void registerContentObserver(Uri uri, boolean notifyForDescendants, android.database.ContentObserver observer) {}
    public final void registerContentObserver(Uri uri, boolean notifyForDescendants, android.database.ContentObserver observer,
                                              int userHandle) {}
    public final void unregisterContentObserver(android.database.ContentObserver observer) {}
    public void notifyChange(Uri uri, android.database.ContentObserver observer) {}
    public void notifyChange(Uri uri, android.database.ContentObserver observer, boolean syncToNetwork) {}
    public int getUserId() { return 0; }
    public void onDbCorruption(String tag, String message, Throwable stacktrace) {
        android.util.Log.e(tag, message, stacktrace);
    }
}
