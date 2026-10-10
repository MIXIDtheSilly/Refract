// android.database.sqlite.SQLiteConnection and android.database.CursorWindow natives (the
// framework's own Java classes run unchanged, see java/real-classes.txt). SQL runs in the
// firmware's libsqlite.so on the guest JIT, called the way libandroid_runtime calls it on a
// device, so databases are ordinary guest files and behave like on Android (collators and
// functions from register_android_functions included). Cursor windows live on the host.
#include "sqlite_jni.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "host_runtime.h"
#include "thunks.h"

namespace rn {

namespace {

// --- libsqlite.so ----------------------------------------------------------------------------------

enum : int {
    SQLITE_OK = 0, SQLITE_PERM = 3, SQLITE_ABORT = 4, SQLITE_BUSY = 5, SQLITE_LOCKED = 6, SQLITE_NOMEM = 7,
    SQLITE_READONLY = 8, SQLITE_INTERRUPT = 9, SQLITE_IOERR = 10, SQLITE_CORRUPT = 11, SQLITE_FULL = 13,
    SQLITE_CANTOPEN = 14, SQLITE_TOOBIG = 18, SQLITE_CONSTRAINT = 19, SQLITE_MISMATCH = 20, SQLITE_MISUSE = 21,
    SQLITE_RANGE = 25, SQLITE_NOTADB = 26, SQLITE_ROW = 100, SQLITE_DONE = 101,
};
enum : int { SQLITE_INTEGER = 1, SQLITE_FLOAT = 2, SQLITE_TEXT = 3, SQLITE_BLOB = 4, SQLITE_NULL = 5 };
constexpr int SQLITE_OPEN_READONLY = 1, SQLITE_OPEN_READWRITE = 2, SQLITE_OPEN_CREATE = 4;
constexpr u64 SQLITE_TRANSIENT = ~0ull;
constexpr int SQLITE_DBCONFIG_LOOKASIDE = 1001, SQLITE_DBSTATUS_LOOKASIDE_USED = 0;
// SQLiteDatabase open flags.
constexpr int OPEN_READONLY = 0x1, CREATE_IF_NECESSARY = 0x10000000;
constexpr int kBusyTimeoutMs = 2500;
constexpr int kUtf16Storage = 0;  // as in android_database_SQLiteConnection.cpp

struct SqliteApi {
    u64 open_v2, close_v2, prepare16_v2, finalize, bind_parameter_count, stmt_readonly, column_count,
        column_name16, bind_null, bind_int64, bind_double, bind_text16, bind_blob, reset, clear_bindings, step,
        column_int64, column_double, column_text16, column_bytes16, column_blob, column_bytes, column_type, changes,
        last_insert_rowid, errcode, extended_errcode, errmsg, busy_timeout, db_readonly, interrupt, db_status,
        db_config, register_android_functions, register_localized_collators;
    bool ok = false;
};

SqliteApi& Api() {
    static SqliteApi api;
    static std::once_flag once;
    std::call_once(once, [] {
        const u64 lib = GuestDlopen("libsqlite.so");
        if (!lib) {
            Log("sqlite: cannot load the firmware's libsqlite.so");
            return;
        }
        bool ok = true;
        auto sym = [&](const char* name) {
            const u64 p = GuestDlsym(lib, name);
            if (!p) {
                Log("sqlite: libsqlite.so lacks %s", name);
                ok = false;
            }
            return p;
        };
#define RN_SQ(f) api.f = sym("sqlite3_" #f);
        RN_SQ(open_v2) RN_SQ(close_v2) RN_SQ(prepare16_v2) RN_SQ(finalize) RN_SQ(bind_parameter_count)
        RN_SQ(stmt_readonly) RN_SQ(column_count) RN_SQ(column_name16) RN_SQ(bind_null) RN_SQ(bind_int64)
        RN_SQ(bind_double) RN_SQ(bind_text16) RN_SQ(bind_blob) RN_SQ(reset) RN_SQ(clear_bindings) RN_SQ(step)
        RN_SQ(column_int64) RN_SQ(column_double) RN_SQ(column_text16) RN_SQ(column_bytes16) RN_SQ(column_blob)
        RN_SQ(column_bytes) RN_SQ(column_type) RN_SQ(changes) RN_SQ(last_insert_rowid) RN_SQ(errcode)
        RN_SQ(extended_errcode) RN_SQ(errmsg) RN_SQ(busy_timeout) RN_SQ(db_readonly) RN_SQ(interrupt)
        RN_SQ(db_status) RN_SQ(db_config)
#undef RN_SQ
        api.register_android_functions = sym("register_android_functions");
        api.register_localized_collators = sym("register_localized_collators");
        api.ok = ok;
    });
    return api;
}

template <typename... A>
u64 Sq(u64 fn, A... args) {
    return CallGuest(EnsureGuestThread(), fn, {static_cast<u64>(args)...});
}
int SqInt(u64 fn, auto... args) { return static_cast<int>(Sq(fn, args...)); }

// --- exceptions (throw_sqlite3_exception in AOSP) ---------------------------------------------------

void ThrowNew(JNIEnv* env, const char* cls, const std::string& msg) {
    jclass c = env->FindClass(cls);
    if (!c) {
        env->ExceptionClear();
        c = env->FindClass("android/database/sqlite/SQLiteException");
    }
    env->ThrowNew(c, msg.c_str());
}

void ThrowSqlite(JNIEnv* env, int errcode, const char* sqlite_message, const char* message) {
    const char* cls;
    switch (errcode & 0xff) {
    case SQLITE_IOERR:
        cls = "android/database/sqlite/SQLiteDiskIOException";
        break;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
        cls = "android/database/sqlite/SQLiteDatabaseCorruptException";
        break;
    case SQLITE_CONSTRAINT:
        cls = "android/database/sqlite/SQLiteConstraintException";
        break;
    case SQLITE_ABORT:
        cls = "android/database/sqlite/SQLiteAbortException";
        break;
    case SQLITE_DONE:
        cls = "android/database/sqlite/SQLiteDoneException";
        sqlite_message = nullptr;
        break;
    case SQLITE_FULL:
        cls = "android/database/sqlite/SQLiteFullException";
        break;
    case SQLITE_MISUSE:
        cls = "android/database/sqlite/SQLiteMisuseException";
        break;
    case SQLITE_PERM:
        cls = "android/database/sqlite/SQLiteAccessPermException";
        break;
    case SQLITE_BUSY:
        cls = "android/database/sqlite/SQLiteDatabaseLockedException";
        break;
    case SQLITE_LOCKED:
        cls = "android/database/sqlite/SQLiteTableLockedException";
        break;
    case SQLITE_READONLY:
        cls = "android/database/sqlite/SQLiteReadOnlyDatabaseException";
        break;
    case SQLITE_CANTOPEN:
        cls = "android/database/sqlite/SQLiteCantOpenDatabaseException";
        break;
    case SQLITE_TOOBIG:
        cls = "android/database/sqlite/SQLiteBlobTooBigException";
        break;
    case SQLITE_RANGE:
        cls = "android/database/sqlite/SQLiteBindOrColumnIndexOutOfRangeException";
        break;
    case SQLITE_NOMEM:
        cls = "android/database/sqlite/SQLiteOutOfMemoryException";
        break;
    case SQLITE_MISMATCH:
        cls = "android/database/sqlite/SQLiteDatatypeMismatchException";
        break;
    case SQLITE_INTERRUPT:
        cls = "android/os/OperationCanceledException";
        break;
    default:
        cls = "android/database/sqlite/SQLiteException";
        break;
    }
    std::string msg;
    if (sqlite_message) {
        msg = Format("%s (code %d)", sqlite_message, errcode);
        if (message)
            msg += std::string(": ") + message;
    } else if (message) {
        msg = message;
    }
    ThrowNew(env, cls, msg);
}

// The connection's last error.
void ThrowFromDb(JNIEnv* env, u64 db, const char* message = nullptr) {
    SqliteApi& a = Api();
    if (!db) {
        ThrowSqlite(env, SQLITE_OK, nullptr, message ? message : "unknown error");
        return;
    }
    const int code = SqInt(a.extended_errcode, db);
    std::string text;
    SafeReadString(Sq(a.errmsg, db), text);
    ThrowSqlite(env, code, text.c_str(), message);
}

// --- SQLiteConnection natives ----------------------------------------------------------------------

struct Connection {
    u64 db;
    std::string path, label;
    std::atomic<bool> canceled{false};
};

Connection* Conn(jlong p) { return reinterpret_cast<Connection*>(p); }

std::string Utf8(JNIEnv* env, jstring s) {
    if (!s)
        return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    env->ReleaseStringUTFChars(s, c);
    return r;
}

jlong JNICALL C_open(JNIEnv* env, jclass, jstring jpath, jint open_flags, jstring jlabel, jboolean, jboolean,
                     jint lookaside_size, jint lookaside_count) {
    SqliteApi& a = Api();
    if (!a.ok) {
        ThrowNew(env, "android/database/sqlite/SQLiteCantOpenDatabaseException", "libsqlite.so is unavailable");
        return 0;
    }
    int flags;
    if (open_flags & CREATE_IF_NECESSARY)
        flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    else if (open_flags & OPEN_READONLY)
        flags = SQLITE_OPEN_READONLY;
    else
        flags = SQLITE_OPEN_READWRITE;
    const std::string path = Utf8(env, jpath);
    u64 db = 0;
    int err = SqInt(a.open_v2, reinterpret_cast<u64>(path.c_str()), reinterpret_cast<u64>(&db), flags, 0);
    if (err != SQLITE_OK) {
        ThrowSqlite(env, err, nullptr, "Could not open database");
        if (db)
            Sq(a.close_v2, db);
        return 0;
    }
    if (lookaside_size >= 0 && lookaside_count >= 0)
        Sq(a.db_config, db, SQLITE_DBCONFIG_LOOKASIDE, 0, lookaside_size, lookaside_count);
    if ((flags & SQLITE_OPEN_READWRITE) && SqInt(a.db_readonly, db, 0)) {
        ThrowSqlite(env, SQLITE_CANTOPEN, nullptr, "Could not open the database in read/write mode.");
        Sq(a.close_v2, db);
        return 0;
    }
    err = SqInt(a.busy_timeout, db, kBusyTimeoutMs);
    if (err == SQLITE_OK)
        err = SqInt(a.register_android_functions, db, kUtf16Storage);
    if (err != SQLITE_OK) {
        ThrowFromDb(env, db, "Could not set up the database");
        Sq(a.close_v2, db);
        return 0;
    }
    auto* c = new Connection;
    c->db = db;
    c->path = path;
    c->label = Utf8(env, jlabel);
    RN_INFO("sqlite: opened %s", path.c_str());
    return reinterpret_cast<jlong>(c);
}

void JNICALL C_close(JNIEnv* env, jclass, jlong p) {
    Connection* c = Conn(p);
    if (!c)
        return;
    const int err = SqInt(Api().close_v2, c->db);
    if (err != SQLITE_OK) {
        ThrowFromDb(env, c->db, "Count not close db.");
        return;
    }
    delete c;
}

void JNICALL C_registerScalar(JNIEnv*, jclass, jlong, jstring, jobject) {
    static std::once_flag once;
    std::call_once(once, [] { Log("sqlite: custom SQL functions are not supported"); });
}
void JNICALL C_registerAggregate(JNIEnv* env, jclass cls, jlong p, jstring name, jobject fn) {
    C_registerScalar(env, cls, p, name, fn);
}

void JNICALL C_registerLocalizedCollators(JNIEnv* env, jclass, jlong p, jstring locale) {
    Connection* c = Conn(p);
    const std::string l = Utf8(env, locale);
    if (SqInt(Api().register_localized_collators, c->db, reinterpret_cast<u64>(l.c_str()), kUtf16Storage) !=
        SQLITE_OK)
        ThrowFromDb(env, c->db);
}

jlong JNICALL C_prepare(JNIEnv* env, jclass, jlong p, jstring sql) {
    Connection* c = Conn(p);
    const jsize len = env->GetStringLength(sql);
    std::vector<jchar> chars(static_cast<size_t>(len) + 1);
    env->GetStringRegion(sql, 0, len, chars.data());
    u64 stmt = 0;
    const int err = SqInt(Api().prepare16_v2, c->db, reinterpret_cast<u64>(chars.data()), len * 2,
                          reinterpret_cast<u64>(&stmt), 0);
    if (err != SQLITE_OK) {
        const std::string message = ", while compiling: " + Utf8(env, sql);
        ThrowFromDb(env, c->db, message.c_str() + 2);
        return 0;
    }
    return static_cast<jlong>(stmt);
}

void JNICALL C_finalize(JNIEnv*, jclass, jlong, jlong stmt) { Sq(Api().finalize, stmt); }
jint JNICALL C_parameterCount(JNIEnv*, jclass, jlong, jlong stmt) { return SqInt(Api().bind_parameter_count, stmt); }
jboolean JNICALL C_isReadOnly(JNIEnv*, jclass, jlong, jlong stmt) { return SqInt(Api().stmt_readonly, stmt) != 0; }
jint JNICALL C_columnCount(JNIEnv*, jclass, jlong, jlong stmt) { return SqInt(Api().column_count, stmt); }

jstring NewString16(JNIEnv* env, u64 p, size_t chars) {
    return env->NewString(reinterpret_cast<const jchar*>(p), static_cast<jsize>(chars));
}

jstring JNICALL C_columnName(JNIEnv* env, jclass, jlong, jlong stmt, jint index) {
    const u64 p = Sq(Api().column_name16, stmt, index);
    if (!p)
        return nullptr;
    size_t n = 0;
    while (reinterpret_cast<const jchar*>(p)[n])
        ++n;
    return NewString16(env, p, n);
}

void CheckBind(JNIEnv* env, Connection* c, int err) {
    if (err != SQLITE_OK)
        ThrowFromDb(env, c->db);
}
void JNICALL C_bindNull(JNIEnv* env, jclass, jlong p, jlong stmt, jint i) {
    CheckBind(env, Conn(p), SqInt(Api().bind_null, stmt, i));
}
void JNICALL C_bindLong(JNIEnv* env, jclass, jlong p, jlong stmt, jint i, jlong v) {
    CheckBind(env, Conn(p), SqInt(Api().bind_int64, stmt, i, v));
}
void JNICALL C_bindDouble(JNIEnv* env, jclass, jlong p, jlong stmt, jint i, jdouble v) {
    GuestCallArgs args;
    args.x[0] = static_cast<u64>(stmt);
    args.x[1] = static_cast<u64>(i);
    u64 bits;
    memcpy(&bits, &v, 8);
    args.v[0] = {bits, 0};
    CpuContext r;
    CallGuestRegs(EnsureGuestThread(), Api().bind_double, args, &r);
    CheckBind(env, Conn(p), static_cast<int>(r.x[0]));
}
void JNICALL C_bindString(JNIEnv* env, jclass, jlong p, jlong stmt, jint i, jstring s) {
    const jsize len = env->GetStringLength(s);
    std::vector<jchar> chars(static_cast<size_t>(len) + 1);
    env->GetStringRegion(s, 0, len, chars.data());
    CheckBind(env, Conn(p),
              SqInt(Api().bind_text16, stmt, i, reinterpret_cast<u64>(chars.data()), len * 2, SQLITE_TRANSIENT));
}
void JNICALL C_bindBlob(JNIEnv* env, jclass, jlong p, jlong stmt, jint i, jbyteArray b) {
    const jsize len = env->GetArrayLength(b);
    std::vector<jbyte> bytes(static_cast<size_t>(len) + 1);
    env->GetByteArrayRegion(b, 0, len, bytes.data());
    CheckBind(env, Conn(p),
              SqInt(Api().bind_blob, stmt, i, reinterpret_cast<u64>(bytes.data()), len, SQLITE_TRANSIENT));
}

void JNICALL C_resetAndClear(JNIEnv* env, jclass, jlong p, jlong stmt) {
    if (SqInt(Api().reset, stmt) != SQLITE_OK) {
        ThrowFromDb(env, Conn(p)->db);
        return;
    }
    Sq(Api().clear_bindings, stmt);
}

int ExecuteNonQuery(JNIEnv* env, Connection* c, u64 stmt, bool pragma) {
    int err = SqInt(Api().step, stmt);
    if (pragma)
        while (err == SQLITE_ROW)
            err = SqInt(Api().step, stmt);
    if (err == SQLITE_ROW)
        ThrowSqlite(env, SQLITE_OK, nullptr,
                    "Queries can be performed using SQLiteDatabase query or rawQuery methods only.");
    else if (err != SQLITE_DONE)
        ThrowFromDb(env, c->db);
    return err;
}
int ExecuteOneRowQuery(JNIEnv* env, Connection* c, u64 stmt) {
    const int err = SqInt(Api().step, stmt);
    if (err != SQLITE_ROW)
        ThrowFromDb(env, c->db);
    return err;
}

void JNICALL C_execute(JNIEnv* env, jclass, jlong p, jlong stmt, jboolean pragma) {
    ExecuteNonQuery(env, Conn(p), stmt, pragma);
}
jint JNICALL C_executeForChangedRowCount(JNIEnv* env, jclass, jlong p, jlong stmt) {
    Connection* c = Conn(p);
    return ExecuteNonQuery(env, c, stmt, false) == SQLITE_DONE ? SqInt(Api().changes, c->db) : -1;
}
jlong JNICALL C_executeForLastInsertedRowId(JNIEnv* env, jclass, jlong p, jlong stmt) {
    Connection* c = Conn(p);
    return ExecuteNonQuery(env, c, stmt, false) == SQLITE_DONE && SqInt(Api().changes, c->db) > 0
               ? static_cast<jlong>(Sq(Api().last_insert_rowid, c->db))
               : -1;
}
jlong JNICALL C_executeForLong(JNIEnv* env, jclass, jlong p, jlong stmt) {
    if (ExecuteOneRowQuery(env, Conn(p), stmt) == SQLITE_ROW && SqInt(Api().column_count, stmt) >= 1)
        return static_cast<jlong>(Sq(Api().column_int64, stmt, 0));
    return -1;
}
jstring JNICALL C_executeForString(JNIEnv* env, jclass, jlong p, jlong stmt) {
    if (ExecuteOneRowQuery(env, Conn(p), stmt) == SQLITE_ROW && SqInt(Api().column_count, stmt) >= 1) {
        const u64 text = Sq(Api().column_text16, stmt, 0);
        if (text)
            return NewString16(env, text, static_cast<size_t>(SqInt(Api().column_bytes16, stmt, 0)) / 2);
    }
    return nullptr;
}
jint JNICALL C_executeForBlobFileDescriptor(JNIEnv* env, jclass, jlong p, jlong stmt) {
    if (ExecuteOneRowQuery(env, Conn(p), stmt) == SQLITE_ROW)
        ThrowNew(env, "java/lang/UnsupportedOperationException", "blob file descriptors are not supported");
    return -1;
}
jint JNICALL C_getDbLookaside(JNIEnv*, jclass, jlong p) {
    int cur = -1, high = -1;
    Sq(Api().db_status, Conn(p)->db, SQLITE_DBSTATUS_LOOKASIDE_USED, reinterpret_cast<u64>(&cur),
       reinterpret_cast<u64>(&high), 0);
    return cur;
}
void JNICALL C_cancel(JNIEnv*, jclass, jlong p) {
    Conn(p)->canceled = true;
    Sq(Api().interrupt, Conn(p)->db);
}
void JNICALL C_resetCancel(JNIEnv*, jclass, jlong p, jboolean) { Conn(p)->canceled = false; }

// --- CursorWindow ----------------------------------------------------------------------------------

enum : jint { FIELD_TYPE_NULL = 0, FIELD_TYPE_INTEGER = 1, FIELD_TYPE_FLOAT = 2, FIELD_TYPE_STRING = 3,
              FIELD_TYPE_BLOB = 4 };

struct Field {
    jint type = FIELD_TYPE_NULL;
    s64 l = 0;
    double d = 0;
    std::u16string s;
    std::vector<u8> b;
};

struct Window {
    std::string name;
    size_t capacity;
    size_t used = 0;
    u32 columns = 0;
    std::vector<std::vector<Field>> rows;
};

constexpr size_t kFieldOverhead = 16;

Window* Win(jlong p) { return reinterpret_cast<Window*>(p); }

Field* At(JNIEnv* env, Window* w, jint row, jint col) {
    if (row < 0 || col < 0 || static_cast<size_t>(row) >= w->rows.size() || static_cast<u32>(col) >= w->columns) {
        ThrowNew(env, "java/lang/IllegalStateException",
                 Format("Couldn't read row %d, col %d from CursorWindow.  Make sure the Cursor is initialized "
                        "correctly before accessing data from it.",
                        row, col));
        return nullptr;
    }
    return &w->rows[row][col];
}

size_t FieldBytes(const Field& f) { return f.s.size() * 2 + f.b.size(); }

// Replaces a field if the window has room for it.
bool Put(Window* w, jint row, jint col, Field&& f) {
    if (row < 0 || col < 0 || static_cast<size_t>(row) >= w->rows.size() || static_cast<u32>(col) >= w->columns)
        return false;
    Field& old = w->rows[row][col];
    const size_t grow = FieldBytes(f), shrink = FieldBytes(old);
    if (w->used + grow - shrink > w->capacity)
        return false;
    w->used = w->used + grow - shrink;
    old = std::move(f);
    return true;
}

std::string ToUtf8(const std::u16string& s) {
    if (s.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(s.data()), static_cast<int>(s.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(s.data()), static_cast<int>(s.size()), out.data(),
                        n, nullptr, nullptr);
    return out;
}

jlong JNICALL W_create(JNIEnv* env, jclass, jstring name, jint size) {
    auto* w = new Window;
    w->name = Utf8(env, name);
    w->capacity = size > 0 ? static_cast<size_t>(size) : 2 * 1024 * 1024;
    return reinterpret_cast<jlong>(w);
}
jlong JNICALL W_createFromParcel(JNIEnv* env, jclass, jobject) {
    ThrowNew(env, "java/lang/UnsupportedOperationException", "CursorWindow parcels are not supported");
    return 0;
}
void JNICALL W_dispose(JNIEnv*, jclass, jlong p) { delete Win(p); }
void JNICALL W_writeToParcel(JNIEnv* env, jclass, jlong, jobject) {
    ThrowNew(env, "java/lang/UnsupportedOperationException", "CursorWindow parcels are not supported");
}
jstring JNICALL W_getName(JNIEnv* env, jclass, jlong p) { return env->NewStringUTF(Win(p)->name.c_str()); }
void JNICALL W_clear(JNIEnv*, jclass, jlong p) {
    Window* w = Win(p);
    w->rows.clear();
    w->columns = 0;
    w->used = 0;
}
jint JNICALL W_getNumRows(JNIEnv*, jclass, jlong p) { return static_cast<jint>(Win(p)->rows.size()); }
jboolean JNICALL W_setNumColumns(JNIEnv*, jclass, jlong p, jint n) {
    Window* w = Win(p);
    if ((w->columns > 0 || !w->rows.empty()) && w->columns != static_cast<u32>(n))
        return JNI_FALSE;
    w->columns = static_cast<u32>(n);
    return JNI_TRUE;
}
jboolean JNICALL W_allocRow(JNIEnv*, jclass, jlong p) {
    Window* w = Win(p);
    const size_t need = w->columns * kFieldOverhead;
    if (w->used + need > w->capacity)
        return JNI_FALSE;
    w->used += need;
    w->rows.emplace_back(w->columns);
    return JNI_TRUE;
}
void JNICALL W_freeLastRow(JNIEnv*, jclass, jlong p) {
    Window* w = Win(p);
    if (w->rows.empty())
        return;
    for (const Field& f : w->rows.back())
        w->used -= FieldBytes(f);
    w->used -= w->columns * kFieldOverhead;
    w->rows.pop_back();
}
jint JNICALL W_getType(JNIEnv* env, jclass, jlong p, jint row, jint col) {
    Field* f = At(env, Win(p), row, col);
    return f ? f->type : FIELD_TYPE_NULL;
}
jbyteArray JNICALL W_getBlob(JNIEnv* env, jclass, jlong p, jint row, jint col) {
    Field* f = At(env, Win(p), row, col);
    if (!f || f->type == FIELD_TYPE_NULL)
        return nullptr;
    std::vector<u8> bytes;
    if (f->type == FIELD_TYPE_BLOB) {
        bytes = f->b;
    } else if (f->type == FIELD_TYPE_STRING) {
        const std::string s = ToUtf8(f->s);
        bytes.assign(s.begin(), s.end());
        bytes.push_back(0);  // the window stores strings NUL-terminated
    } else {
        ThrowNew(env, "android/database/sqlite/SQLiteException",
                 f->type == FIELD_TYPE_INTEGER ? "INTEGER data in nativeGetBlob " : "FLOAT data in nativeGetBlob ");
        return nullptr;
    }
    jbyteArray a = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (a)
        env->SetByteArrayRegion(a, 0, static_cast<jsize>(bytes.size()), reinterpret_cast<const jbyte*>(bytes.data()));
    return a;
}
std::u16string FieldString(JNIEnv* env, const Field& f, bool* is_null) {
    *is_null = false;
    switch (f.type) {
    case FIELD_TYPE_STRING:
        return f.s;
    case FIELD_TYPE_INTEGER: {
        const std::string s = std::to_string(f.l);
        return std::u16string(s.begin(), s.end());
    }
    case FIELD_TYPE_FLOAT: {
        const std::string s = Format("%g", f.d);
        return std::u16string(s.begin(), s.end());
    }
    case FIELD_TYPE_NULL:
        *is_null = true;
        return {};
    default:
        ThrowNew(env, "android/database/sqlite/SQLiteException", "Unable to convert BLOB to string");
        *is_null = true;
        return {};
    }
}
jstring JNICALL W_getString(JNIEnv* env, jclass, jlong p, jint row, jint col) {
    Field* f = At(env, Win(p), row, col);
    if (!f)
        return nullptr;
    bool is_null;
    const std::u16string s = FieldString(env, *f, &is_null);
    return is_null ? nullptr : env->NewString(reinterpret_cast<const jchar*>(s.data()), static_cast<jsize>(s.size()));
}
void JNICALL W_copyStringToBuffer(JNIEnv* env, jclass, jlong p, jint row, jint col, jobject buffer) {
    Field* f = At(env, Win(p), row, col);
    if (!f)
        return;
    bool is_null;
    const std::u16string s = FieldString(env, *f, &is_null);
    if (env->ExceptionCheck())
        return;
    jclass c = env->GetObjectClass(buffer);
    jfieldID data_f = env->GetFieldID(c, "data", "[C"), size_f = env->GetFieldID(c, "sizeCopied", "I");
    auto data = static_cast<jcharArray>(env->GetObjectField(buffer, data_f));
    const jsize n = static_cast<jsize>(s.size());
    if (!data || env->GetArrayLength(data) < n) {
        data = env->NewCharArray(std::max<jsize>(n, 1));
        env->SetObjectField(buffer, data_f, data);
    }
    env->SetCharArrayRegion(data, 0, n, reinterpret_cast<const jchar*>(s.data()));
    env->SetIntField(buffer, size_f, n);
}
jlong JNICALL W_getLong(JNIEnv* env, jclass, jlong p, jint row, jint col) {
    Field* f = At(env, Win(p), row, col);
    if (!f)
        return 0;
    switch (f->type) {
    case FIELD_TYPE_INTEGER:
        return f->l;
    case FIELD_TYPE_FLOAT:
        return static_cast<jlong>(f->d);
    case FIELD_TYPE_STRING:
        return f->s.empty() ? 0 : strtoll(ToUtf8(f->s).c_str(), nullptr, 0);
    case FIELD_TYPE_NULL:
        return 0;
    default:
        ThrowNew(env, "android/database/sqlite/SQLiteException", "Unable to convert BLOB to long");
        return 0;
    }
}
jdouble JNICALL W_getDouble(JNIEnv* env, jclass, jlong p, jint row, jint col) {
    Field* f = At(env, Win(p), row, col);
    if (!f)
        return 0;
    switch (f->type) {
    case FIELD_TYPE_FLOAT:
        return f->d;
    case FIELD_TYPE_INTEGER:
        return static_cast<jdouble>(f->l);
    case FIELD_TYPE_STRING:
        return f->s.empty() ? 0.0 : strtod(ToUtf8(f->s).c_str(), nullptr);
    case FIELD_TYPE_NULL:
        return 0;
    default:
        ThrowNew(env, "android/database/sqlite/SQLiteException", "Unable to convert BLOB to double");
        return 0;
    }
}
jboolean JNICALL W_putBlob(JNIEnv* env, jclass, jlong p, jbyteArray value, jint row, jint col) {
    Field f;
    f.type = FIELD_TYPE_BLOB;
    f.b.resize(static_cast<size_t>(env->GetArrayLength(value)));
    env->GetByteArrayRegion(value, 0, static_cast<jsize>(f.b.size()), reinterpret_cast<jbyte*>(f.b.data()));
    return Put(Win(p), row, col, std::move(f));
}
jboolean JNICALL W_putString(JNIEnv* env, jclass, jlong p, jstring value, jint row, jint col) {
    Field f;
    f.type = FIELD_TYPE_STRING;
    f.s.resize(static_cast<size_t>(env->GetStringLength(value)));
    env->GetStringRegion(value, 0, static_cast<jsize>(f.s.size()), reinterpret_cast<jchar*>(f.s.data()));
    return Put(Win(p), row, col, std::move(f));
}
jboolean JNICALL W_putLong(JNIEnv*, jclass, jlong p, jlong value, jint row, jint col) {
    Field f;
    f.type = FIELD_TYPE_INTEGER;
    f.l = value;
    return Put(Win(p), row, col, std::move(f));
}
jboolean JNICALL W_putDouble(JNIEnv*, jclass, jlong p, jdouble value, jint row, jint col) {
    Field f;
    f.type = FIELD_TYPE_FLOAT;
    f.d = value;
    return Put(Win(p), row, col, std::move(f));
}
jboolean JNICALL W_putNull(JNIEnv*, jclass, jlong p, jint row, jint col) {
    return Put(Win(p), row, col, Field{});
}

// One result row into the window; false if it does not fit.
bool CopyRow(Window* w, u64 stmt, int columns, int row) {
    SqliteApi& a = Api();
    for (int i = 0; i < columns; ++i) {
        Field f;
        switch (SqInt(a.column_type, stmt, i)) {
        case SQLITE_INTEGER:
            f.type = FIELD_TYPE_INTEGER;
            f.l = static_cast<s64>(Sq(a.column_int64, stmt, i));
            break;
        case SQLITE_FLOAT: {
            f.type = FIELD_TYPE_FLOAT;
            CpuContext r;
            CallGuestFull(EnsureGuestThread(), a.column_double, {stmt, static_cast<u64>(i)}, &r);
            memcpy(&f.d, &r.v[0][0], 8);
            break;
        }
        case SQLITE_TEXT: {
            f.type = FIELD_TYPE_STRING;
            const u64 text = Sq(a.column_text16, stmt, i);
            const size_t n = static_cast<size_t>(SqInt(a.column_bytes16, stmt, i)) / 2;
            if (text)
                f.s.assign(reinterpret_cast<const char16_t*>(text), n);
            break;
        }
        case SQLITE_BLOB: {
            f.type = FIELD_TYPE_BLOB;
            const u64 blob = Sq(a.column_blob, stmt, i);
            const size_t n = static_cast<size_t>(SqInt(a.column_bytes, stmt, i));
            if (blob)
                f.b.assign(reinterpret_cast<const u8*>(blob), reinterpret_cast<const u8*>(blob) + n);
            break;
        }
        default:
            break;
        }
        if (!Put(w, row, i, std::move(f)))
            return false;
    }
    return true;
}

jlong JNICALL C_executeForCursorWindow(JNIEnv* env, jclass, jlong p, jlong stmt, jlong window, jint start_pos,
                                       jint required_pos, jboolean count_all_rows) {
    Connection* c = Conn(p);
    Window* w = Win(window);
    SqliteApi& a = Api();
    W_clear(env, nullptr, window);
    const int columns = SqInt(a.column_count, stmt);
    if (!W_setNumColumns(env, nullptr, window, columns)) {
        ThrowNew(env, "java/lang/IllegalStateException", "Couldn't set the number of columns in the cursor window");
        return 0;
    }
    int total = 0, added = 0, retries = 0;
    bool full = false, failed = false;
    while (!failed) {
        const int err = SqInt(a.step, stmt);
        if (err == SQLITE_ROW) {
            retries = 0;
            ++total;
            if (start_pos >= total || full)
                continue;
            bool ok = W_allocRow(env, nullptr, window) && CopyRow(w, stmt, columns, added);
            if (!ok) {
                if (static_cast<int>(w->rows.size()) > added)
                    W_freeLastRow(env, nullptr, window);
                // The window filled up before the required row: start over at this row.
                if (added && start_pos + added <= required_pos) {
                    W_clear(env, nullptr, window);
                    W_setNumColumns(env, nullptr, window, columns);
                    start_pos += added;
                    added = 0;
                    ok = W_allocRow(env, nullptr, window) && CopyRow(w, stmt, columns, 0);
                    if (!ok && !w->rows.empty())
                        W_freeLastRow(env, nullptr, window);
                }
            }
            if (ok) {
                ++added;
            } else {
                full = true;
                if (!count_all_rows)
                    break;
            }
        } else if (err == SQLITE_DONE) {
            break;
        } else if ((err & 0xff) == SQLITE_LOCKED || (err & 0xff) == SQLITE_BUSY) {
            if (++retries > 50) {
                ThrowSqlite(env, err, nullptr, "retrycount exceeded");
                failed = true;
            } else {
                Sleep(1);
            }
        } else {
            ThrowFromDb(env, c->db, "nativeExecuteForCursorWindow");
            failed = true;
        }
    }
    Sq(a.reset, stmt);
    if (failed)
        return 0;
    return (static_cast<jlong>(start_pos) << 32) | static_cast<jlong>(static_cast<u32>(total));
}

JNINativeMethod M(const char* name, const char* sig, void* fn) {
    return JNINativeMethod{const_cast<char*>(name), const_cast<char*>(sig), fn};
}
#define RN_FN(f) reinterpret_cast<void*>(&f)

}  // namespace

bool RegisterSqliteNatives(JNIEnv* env) {
    jclass conn = env->FindClass("android/database/sqlite/SQLiteConnection");
    jclass window = conn ? env->FindClass("android/database/CursorWindow") : nullptr;
    if (!conn || !window) {
        env->ExceptionClear();
        Log("android.database classes missing: no SQLite");
        return false;
    }
    const JNINativeMethod cm[] = {
        M("nativeOpen", "(Ljava/lang/String;ILjava/lang/String;ZZII)J", RN_FN(C_open)),
        M("nativeClose", "(J)V", RN_FN(C_close)),
        M("nativeRegisterCustomScalarFunction", "(JLjava/lang/String;Ljava/util/function/UnaryOperator;)V",
          RN_FN(C_registerScalar)),
        M("nativeRegisterCustomAggregateFunction", "(JLjava/lang/String;Ljava/util/function/BinaryOperator;)V",
          RN_FN(C_registerAggregate)),
        M("nativeRegisterLocalizedCollators", "(JLjava/lang/String;)V", RN_FN(C_registerLocalizedCollators)),
        M("nativePrepareStatement", "(JLjava/lang/String;)J", RN_FN(C_prepare)),
        M("nativeFinalizeStatement", "(JJ)V", RN_FN(C_finalize)),
        M("nativeGetParameterCount", "(JJ)I", RN_FN(C_parameterCount)),
        M("nativeIsReadOnly", "(JJ)Z", RN_FN(C_isReadOnly)),
        M("nativeGetColumnCount", "(JJ)I", RN_FN(C_columnCount)),
        M("nativeGetColumnName", "(JJI)Ljava/lang/String;", RN_FN(C_columnName)),
        M("nativeBindNull", "(JJI)V", RN_FN(C_bindNull)),
        M("nativeBindLong", "(JJIJ)V", RN_FN(C_bindLong)),
        M("nativeBindDouble", "(JJID)V", RN_FN(C_bindDouble)),
        M("nativeBindString", "(JJILjava/lang/String;)V", RN_FN(C_bindString)),
        M("nativeBindBlob", "(JJI[B)V", RN_FN(C_bindBlob)),
        M("nativeResetStatementAndClearBindings", "(JJ)V", RN_FN(C_resetAndClear)),
        M("nativeExecute", "(JJZ)V", RN_FN(C_execute)),
        M("nativeExecuteForLong", "(JJ)J", RN_FN(C_executeForLong)),
        M("nativeExecuteForString", "(JJ)Ljava/lang/String;", RN_FN(C_executeForString)),
        M("nativeExecuteForBlobFileDescriptor", "(JJ)I", RN_FN(C_executeForBlobFileDescriptor)),
        M("nativeExecuteForChangedRowCount", "(JJ)I", RN_FN(C_executeForChangedRowCount)),
        M("nativeExecuteForLastInsertedRowId", "(JJ)J", RN_FN(C_executeForLastInsertedRowId)),
        M("nativeExecuteForCursorWindow", "(JJJIIZ)J", RN_FN(C_executeForCursorWindow)),
        M("nativeGetDbLookaside", "(J)I", RN_FN(C_getDbLookaside)),
        M("nativeCancel", "(J)V", RN_FN(C_cancel)),
        M("nativeResetCancel", "(JZ)V", RN_FN(C_resetCancel)),
    };
    const JNINativeMethod wm[] = {
        M("nativeCreate", "(Ljava/lang/String;I)J", RN_FN(W_create)),
        M("nativeCreateFromParcel", "(Landroid/os/Parcel;)J", RN_FN(W_createFromParcel)),
        M("nativeDispose", "(J)V", RN_FN(W_dispose)),
        M("nativeWriteToParcel", "(JLandroid/os/Parcel;)V", RN_FN(W_writeToParcel)),
        M("nativeGetName", "(J)Ljava/lang/String;", RN_FN(W_getName)),
        M("nativeGetBlob", "(JII)[B", RN_FN(W_getBlob)),
        M("nativeGetString", "(JII)Ljava/lang/String;", RN_FN(W_getString)),
        M("nativeCopyStringToBuffer", "(JIILandroid/database/CharArrayBuffer;)V", RN_FN(W_copyStringToBuffer)),
        M("nativePutBlob", "(J[BII)Z", RN_FN(W_putBlob)),
        M("nativePutString", "(JLjava/lang/String;II)Z", RN_FN(W_putString)),
        M("nativeClear", "(J)V", RN_FN(W_clear)),
        M("nativeGetNumRows", "(J)I", RN_FN(W_getNumRows)),
        M("nativeSetNumColumns", "(JI)Z", RN_FN(W_setNumColumns)),
        M("nativeAllocRow", "(J)Z", RN_FN(W_allocRow)),
        M("nativeFreeLastRow", "(J)V", RN_FN(W_freeLastRow)),
        M("nativeGetType", "(JII)I", RN_FN(W_getType)),
        M("nativeGetLong", "(JII)J", RN_FN(W_getLong)),
        M("nativeGetDouble", "(JII)D", RN_FN(W_getDouble)),
        M("nativePutLong", "(JJII)Z", RN_FN(W_putLong)),
        M("nativePutDouble", "(JDII)Z", RN_FN(W_putDouble)),
        M("nativePutNull", "(JII)Z", RN_FN(W_putNull)),
    };
    const bool ok = env->RegisterNatives(conn, cm, sizeof(cm) / sizeof(cm[0])) == JNI_OK &&
                    env->RegisterNatives(window, wm, sizeof(wm) / sizeof(wm[0])) == JNI_OK;
    if (!ok) {
        env->ExceptionClear();
        Log("SQLite natives: registration failed");
    }
    return ok;
}

}  // namespace rn
