#ifndef CELLMGR_AT_H
#define CELLMGR_AT_H

#include "config.h"
#include "db.h"
#include "dbus_cmd.h"

int at_is_safe_command(const char *command);
int at_send(app_db *db, const app_config *cfg, const char *command, int allow_dangerous,
            command_result *res);

#endif
