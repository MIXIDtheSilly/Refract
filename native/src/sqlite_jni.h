// android.database.sqlite.SQLiteConnection / android.database.CursorWindow natives on the
// firmware's libsqlite.so.
#pragma once

#include <jni.h>

namespace rn {

// Registers the natives (after the JVM starts).
bool RegisterSqliteNatives(JNIEnv* env);

}  // namespace rn
