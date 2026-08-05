#ifndef CELLMGR_DB_H
#define CELLMGR_DB_H

#include "common.h"
#include <sqlite3.h>

typedef struct app_db {
    sqlite3 *conn;
} app_db;

int db_open(app_db *db, const char *path);
void db_close(app_db *db);
int db_init_schema(app_db *db);
int db_set_setting(app_db *db, const char *key, const char *value);
char *db_get_setting(app_db *db, const char *key);
int db_upsert_profile(app_db *db, const char *profile_id, const char *name, const char *json);
char *db_get_profile_json(app_db *db, const char *profile_id);
char *db_list_profiles_json(app_db *db, const char *active_profile);
int db_profile_exists(app_db *db, const char *profile_id);
int db_delete_profile(app_db *db, const char *profile_id);
int db_insert_at_history(app_db *db, const char *command, const char *response, int exit_code);
char *db_list_at_history_json(app_db *db, int limit);
int db_insert_dbus_event(app_db *db, const char *source, const char *line);
char *db_list_dbus_events_json(app_db *db, int limit);
int db_put_capability_cache(app_db *db, const char *profile_id, const char *capability,
                            const char *modem_path, const char *raw_response,
                            const char *parsed_json, int ttl_sec);
char *db_get_capability_cache(app_db *db, const char *profile_id, const char *capability,
                              const char *modem_path);

#endif
