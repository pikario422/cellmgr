#include "db.h"
#include "json_util.h"

static int exec_sql(app_db *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db->conn, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        log_error("sqlite exec failed: %s", err ? err : sqlite3_errmsg(db->conn));
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int db_open(app_db *db, const char *path)
{
    memset(db, 0, sizeof(*db));
    int rc = sqlite3_open(path, &db->conn);
    if (rc != SQLITE_OK) {
        log_error("sqlite open failed: %s", sqlite3_errmsg(db->conn));
        return -1;
    }
    sqlite3_busy_timeout(db->conn, 3000);
    return 0;
}

void db_close(app_db *db)
{
    if (db->conn) {
        sqlite3_close(db->conn);
    }
    memset(db, 0, sizeof(*db));
}

int db_init_schema(app_db *db)
{
    const char *sql =
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE IF NOT EXISTS settings ("
        " key TEXT PRIMARY KEY,"
        " value TEXT NOT NULL,"
        " updated_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS device_profiles ("
        " id TEXT PRIMARY KEY,"
        " name TEXT NOT NULL,"
        " json TEXT NOT NULL,"
        " updated_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS capability_cache ("
        " profile_id TEXT NOT NULL,"
        " capability TEXT NOT NULL,"
        " modem_path TEXT NOT NULL,"
        " raw_response TEXT NOT NULL,"
        " parsed_json TEXT NOT NULL,"
        " updated_at INTEGER NOT NULL,"
        " ttl_sec INTEGER NOT NULL,"
        " PRIMARY KEY (profile_id, capability, modem_path)"
        ");"
        "CREATE TABLE IF NOT EXISTS at_history ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " command TEXT NOT NULL,"
        " response TEXT NOT NULL,"
        " exit_code INTEGER NOT NULL,"
        " created_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS sms_messages ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " modem_index INTEGER,"
        " sender TEXT,"
        " receiver TEXT,"
        " body TEXT,"
        " status TEXT,"
        " raw TEXT,"
        " created_at INTEGER NOT NULL,"
        " forwarded_at INTEGER"
        ");"
        "CREATE TABLE IF NOT EXISTS tasks ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " type TEXT NOT NULL,"
        " payload TEXT NOT NULL,"
        " status TEXT NOT NULL,"
        " run_at INTEGER,"
        " created_at INTEGER NOT NULL,"
        " updated_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS dbus_events ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " source TEXT NOT NULL,"
        " line TEXT NOT NULL,"
        " created_at INTEGER NOT NULL"
        ");";
    return exec_sql(db, sql);
}

int db_set_setting(app_db *db, const char *key, const char *value)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO settings(key,value,updated_at) VALUES(?,?,?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value,updated_at=excluded.updated_at";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, now_unix());
    int ok = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    return ok;
}

char *db_get_setting(app_db *db, const char *key)
{
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT value FROM settings WHERE key=?";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) != SQLITE_OK) {
        return NULL;
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        out = xstrdup((const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    return out;
}

int db_upsert_profile(app_db *db, const char *profile_id, const char *name, const char *json)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO device_profiles(id,name,json,updated_at) VALUES(?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET name=excluded.name,json=excluded.json,updated_at=excluded.updated_at";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, profile_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, now_unix());
    int ok = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    return ok;
}

char *db_get_profile_json(app_db *db, const char *profile_id)
{
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT json FROM device_profiles WHERE id=?";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) != SQLITE_OK) {
        return NULL;
    }
    sqlite3_bind_text(st, 1, profile_id, -1, SQLITE_TRANSIENT);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        out = xstrdup((const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    return out;
}

char *db_list_profiles_json(app_db *db, const char *active_profile)
{
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT id,name,updated_at FROM device_profiles ORDER BY name,id";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) != SQLITE_OK) {
        return NULL;
    }
    cellmgr_buf b;
    if (buf_init(&b, 512) != 0) {
        sqlite3_finalize(st);
        return NULL;
    }
    buf_append(&b, "{\"profiles\":[");
    int first = 1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(st, 0);
        const char *name = (const char *)sqlite3_column_text(st, 1);
        sqlite3_int64 updated_at = sqlite3_column_int64(st, 2);
        if (!first) {
            buf_append(&b, ",");
        }
        first = 0;
        buf_append(&b, "{");
        json_prop_string(&b, "id", id ? id : "", 0);
        json_prop_string(&b, "name", name ? name : "", 1);
        json_prop_int(&b, "updated_at", (long)updated_at, 1);
        buf_append(&b, ",\"active\":");
        buf_append(&b, (active_profile && id && strcmp(active_profile, id) == 0) ? "true" : "false");
        buf_append(&b, "}");
    }
    buf_append(&b, "]}");
    sqlite3_finalize(st);
    return b.data;
}

int db_profile_exists(app_db *db, const char *profile_id)
{
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT 1 FROM device_profiles WHERE id=?";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, profile_id, -1, SQLITE_TRANSIENT);
    int exists = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return exists;
}

int db_delete_profile(app_db *db, const char *profile_id)
{
    sqlite3_stmt *st = NULL;
    const char *sql = "DELETE FROM device_profiles WHERE id=?";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, profile_id, -1, SQLITE_TRANSIENT);
    int ok = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    if (ok != 0) {
        return -1;
    }
    st = NULL;
    sql = "DELETE FROM capability_cache WHERE profile_id=?";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, profile_id, -1, SQLITE_TRANSIENT);
    ok = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    return ok;
}

int db_insert_at_history(app_db *db, const char *command, const char *response, int exit_code)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO at_history(command,response,exit_code,created_at) VALUES(?,?,?,?)";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, command, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, response, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, exit_code);
    sqlite3_bind_int64(st, 4, now_unix());
    int ok = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    return ok;
}

char *db_list_at_history_json(app_db *db, int limit)
{
    if (limit <= 0 || limit > 100) {
        limit = 30;
    }
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT id,command,response,exit_code,created_at FROM at_history "
        "ORDER BY id DESC LIMIT ?";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) != SQLITE_OK) {
        return NULL;
    }
    sqlite3_bind_int(st, 1, limit);
    cellmgr_buf b;
    if (buf_init(&b, 1024) != 0) {
        sqlite3_finalize(st);
        return NULL;
    }
    buf_append(&b, "{\"items\":[");
    int first = 1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (!first) {
            buf_append(&b, ",");
        }
        first = 0;
        buf_append(&b, "{");
        json_prop_int(&b, "id", sqlite3_column_int64(st, 0), 0);
        json_prop_string(&b, "command", (const char *)sqlite3_column_text(st, 1), 1);
        json_prop_string(&b, "response", (const char *)sqlite3_column_text(st, 2), 1);
        json_prop_int(&b, "exit_code", sqlite3_column_int(st, 3), 1);
        json_prop_int(&b, "created_at", sqlite3_column_int64(st, 4), 1);
        buf_append(&b, "}");
    }
    buf_append(&b, "]}");
    sqlite3_finalize(st);
    return b.data;
}

int db_insert_dbus_event(app_db *db, const char *source, const char *line)
{
    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO dbus_events(source,line,created_at) VALUES(?,?,?)";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, source ? source : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, line ? line : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, now_unix());
    int ok = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    return ok;
}

char *db_list_dbus_events_json(app_db *db, int limit)
{
    if (limit <= 0 || limit > 200) limit = 50;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT id,source,line,created_at FROM dbus_events ORDER BY id DESC LIMIT ?";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) != SQLITE_OK) {
        return NULL;
    }
    sqlite3_bind_int(st, 1, limit);
    cellmgr_buf b;
    if (buf_init(&b, 1024) != 0) {
        sqlite3_finalize(st);
        return NULL;
    }
    buf_append(&b, "{\"items\":[");
    int first = 1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (!first) buf_append(&b, ",");
        first = 0;
        buf_append(&b, "{");
        json_prop_int(&b, "id", sqlite3_column_int64(st, 0), 0);
        json_prop_string(&b, "source", (const char *)sqlite3_column_text(st, 1), 1);
        json_prop_string(&b, "line", (const char *)sqlite3_column_text(st, 2), 1);
        json_prop_int(&b, "created_at", sqlite3_column_int64(st, 3), 1);
        buf_append(&b, "}");
    }
    buf_append(&b, "]}");
    sqlite3_finalize(st);
    return b.data;
}

int db_put_capability_cache(app_db *db, const char *profile_id, const char *capability,
                            const char *modem_path, const char *raw_response,
                            const char *parsed_json, int ttl_sec)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO capability_cache(profile_id,capability,modem_path,raw_response,parsed_json,updated_at,ttl_sec) "
        "VALUES(?,?,?,?,?,?,?) "
        "ON CONFLICT(profile_id,capability,modem_path) DO UPDATE SET "
        "raw_response=excluded.raw_response,parsed_json=excluded.parsed_json,"
        "updated_at=excluded.updated_at,ttl_sec=excluded.ttl_sec";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, profile_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, capability, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, modem_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, raw_response, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, parsed_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, now_unix());
    sqlite3_bind_int(st, 7, ttl_sec);
    int ok = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    return ok;
}

char *db_get_capability_cache(app_db *db, const char *profile_id, const char *capability,
                              const char *modem_path)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT parsed_json FROM capability_cache "
        "WHERE profile_id=? AND capability=? AND modem_path=? "
        "AND updated_at + ttl_sec >= ?";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &st, NULL) != SQLITE_OK) {
        return NULL;
    }
    sqlite3_bind_text(st, 1, profile_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, capability, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, modem_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, now_unix());
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        out = xstrdup((const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    return out;
}
