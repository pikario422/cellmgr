#ifndef CELLMGR_PROFILE_H
#define CELLMGR_PROFILE_H

#include "config.h"
#include "db.h"

int profile_import_file(app_db *db, const char *path, const char *fallback_id);
int profile_import_json(app_db *db, const char *json, char **out_id, char **out_name);
char *profile_get_active_json(app_db *db, const app_config *cfg);
char *profile_get_capability_command(const char *profile_json, const char *capability,
                                     const char *field);
int profile_get_capability_ttl(const char *profile_json, const char *capability, int fallback);

#endif
