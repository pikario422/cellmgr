#ifndef CELLMGR_OFONO_H
#define CELLMGR_OFONO_H

#include "config.h"
#include "dbus_cmd.h"

int ofono_get_modem_properties(const app_config *cfg, command_result *res);
int ofono_get_network_properties(const app_config *cfg, command_result *res);
int ofono_get_sim_properties(const app_config *cfg, command_result *res);
int ofono_set_powered(const app_config *cfg, int powered, command_result *res);
int ofono_set_online(const app_config *cfg, int online, command_result *res);
int ofono_send_at(const app_config *cfg, const char *command, command_result *res);
int ofono_send_sms(const app_config *cfg, const char *number, const char *text, command_result *res);

#endif
