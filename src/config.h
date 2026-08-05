#ifndef CELLMGR_CONFIG_H
#define CELLMGR_CONFIG_H

#include "common.h"

typedef struct app_config {
    char listen_host[64];
    int listen_port;
    char db_path[256];
    char profile_path[256];
    char active_profile[128];
    char ofono_backend[64];
    char ofono_destination[128];
    char ofono_modem_path[128];
    char at_backend[64];
    int at_timeout_ms;
    int auth_enabled;
    char auth_user[64];
    char auth_pass[128];
    int allow_dangerous_at;
    char wan_iface[64];
    int sample_interval_ms;
    int scheduled_reboot_enabled;
    int scheduled_reboot_hour;
    int scheduled_reboot_minute;
    int sms_forward_enabled;
    char sms_forward_mode[32];
    char smtp_host[128];
    int smtp_port;
    char smtp_user[128];
    char smtp_pass[128];
    char smtp_from[128];
    char smtp_to[128];
    char sms_webhook_url[256];
} app_config;

void config_defaults(app_config *cfg);
int config_load(app_config *cfg, const char *path);
void config_dump(const app_config *cfg);

#endif
