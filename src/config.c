#include "config.h"

static void copy_value(char *dst, size_t dst_sz, const char *src)
{
    if (dst_sz == 0) {
        return;
    }
    snprintf(dst, dst_sz, "%s", src ? src : "");
}

void config_defaults(app_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    copy_value(cfg->listen_host, sizeof(cfg->listen_host), CELLMGR_DEFAULT_HOST);
    cfg->listen_port = CELLMGR_DEFAULT_PORT;
    copy_value(cfg->db_path, sizeof(cfg->db_path), CELLMGR_DEFAULT_DB);
    copy_value(cfg->profile_path, sizeof(cfg->profile_path), CELLMGR_DEFAULT_PROFILE);
    copy_value(cfg->active_profile, sizeof(cfg->active_profile), "fibocom-fm650");
    copy_value(cfg->ofono_backend, sizeof(cfg->ofono_backend), "dbus-send");
    copy_value(cfg->ofono_destination, sizeof(cfg->ofono_destination), "org.ofono");
    copy_value(cfg->ofono_modem_path, sizeof(cfg->ofono_modem_path), "/ril_0");
    copy_value(cfg->at_backend, sizeof(cfg->at_backend), "ofono-sendatcmd");
    cfg->at_timeout_ms = 3000;
    cfg->auth_enabled = 0;
    copy_value(cfg->auth_user, sizeof(cfg->auth_user), "admin");
    copy_value(cfg->auth_pass, sizeof(cfg->auth_pass), "admin");
    cfg->allow_dangerous_at = 0;
    copy_value(cfg->wan_iface, sizeof(cfg->wan_iface), "rmnet_data0");
    cfg->sample_interval_ms = 1000;
    cfg->scheduled_reboot_enabled = 0;
    cfg->scheduled_reboot_hour = 3;
    cfg->scheduled_reboot_minute = 30;
    cfg->sms_forward_enabled = 0;
    copy_value(cfg->sms_forward_mode, sizeof(cfg->sms_forward_mode), "smtp");
    cfg->smtp_port = 25;
}

static void set_key(app_config *cfg, const char *key, const char *value)
{
    if (strcmp(key, "listen_host") == 0) {
        copy_value(cfg->listen_host, sizeof(cfg->listen_host), value);
    } else if (strcmp(key, "listen_port") == 0) {
        cfg->listen_port = atoi(value);
    } else if (strcmp(key, "db_path") == 0) {
        copy_value(cfg->db_path, sizeof(cfg->db_path), value);
    } else if (strcmp(key, "profile_path") == 0) {
        copy_value(cfg->profile_path, sizeof(cfg->profile_path), value);
    } else if (strcmp(key, "active_profile") == 0) {
        copy_value(cfg->active_profile, sizeof(cfg->active_profile), value);
    } else if (strcmp(key, "ofono_backend") == 0) {
        copy_value(cfg->ofono_backend, sizeof(cfg->ofono_backend), value);
    } else if (strcmp(key, "ofono_destination") == 0) {
        copy_value(cfg->ofono_destination, sizeof(cfg->ofono_destination), value);
    } else if (strcmp(key, "ofono_modem_path") == 0) {
        copy_value(cfg->ofono_modem_path, sizeof(cfg->ofono_modem_path), value);
    } else if (strcmp(key, "at_backend") == 0) {
        copy_value(cfg->at_backend, sizeof(cfg->at_backend), value);
    } else if (strcmp(key, "at_timeout_ms") == 0) {
        cfg->at_timeout_ms = atoi(value);
    } else if (strcmp(key, "auth_enabled") == 0) {
        cfg->auth_enabled = atoi(value);
    } else if (strcmp(key, "auth_user") == 0) {
        copy_value(cfg->auth_user, sizeof(cfg->auth_user), value);
    } else if (strcmp(key, "auth_pass") == 0) {
        copy_value(cfg->auth_pass, sizeof(cfg->auth_pass), value);
    } else if (strcmp(key, "allow_dangerous_at") == 0) {
        cfg->allow_dangerous_at = atoi(value);
    } else if (strcmp(key, "wan_iface") == 0) {
        copy_value(cfg->wan_iface, sizeof(cfg->wan_iface), value);
    } else if (strcmp(key, "sample_interval_ms") == 0) {
        cfg->sample_interval_ms = atoi(value);
    } else if (strcmp(key, "scheduled_reboot_enabled") == 0) {
        cfg->scheduled_reboot_enabled = atoi(value);
    } else if (strcmp(key, "scheduled_reboot_hour") == 0) {
        cfg->scheduled_reboot_hour = atoi(value);
    } else if (strcmp(key, "scheduled_reboot_minute") == 0) {
        cfg->scheduled_reboot_minute = atoi(value);
    } else if (strcmp(key, "sms_forward_enabled") == 0) {
        cfg->sms_forward_enabled = atoi(value);
    } else if (strcmp(key, "sms_forward_mode") == 0) {
        copy_value(cfg->sms_forward_mode, sizeof(cfg->sms_forward_mode), value);
    } else if (strcmp(key, "smtp_host") == 0) {
        copy_value(cfg->smtp_host, sizeof(cfg->smtp_host), value);
    } else if (strcmp(key, "smtp_port") == 0) {
        cfg->smtp_port = atoi(value);
    } else if (strcmp(key, "smtp_user") == 0) {
        copy_value(cfg->smtp_user, sizeof(cfg->smtp_user), value);
    } else if (strcmp(key, "smtp_pass") == 0) {
        copy_value(cfg->smtp_pass, sizeof(cfg->smtp_pass), value);
    } else if (strcmp(key, "smtp_from") == 0) {
        copy_value(cfg->smtp_from, sizeof(cfg->smtp_from), value);
    } else if (strcmp(key, "smtp_to") == 0) {
        copy_value(cfg->smtp_to, sizeof(cfg->smtp_to), value);
    } else if (strcmp(key, "sms_webhook_url") == 0) {
        copy_value(cfg->sms_webhook_url, sizeof(cfg->sms_webhook_url), value);
    }
}

int config_load(app_config *cfg, const char *path)
{
    config_defaults(cfg);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        log_warn("config not found, using defaults: %s", path);
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        trim_in_place(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        trim_in_place(key);
        trim_in_place(value);
        set_key(cfg, key, value);
    }
    fclose(fp);

    if (cfg->listen_port <= 0 || cfg->listen_port > 65535) {
        cfg->listen_port = CELLMGR_DEFAULT_PORT;
    }
    if (cfg->at_timeout_ms <= 0) {
        cfg->at_timeout_ms = 3000;
    }
    return 0;
}

void config_dump(const app_config *cfg)
{
    log_info("listen %s:%d", cfg->listen_host, cfg->listen_port);
    log_info("db_path=%s profile_path=%s active_profile=%s",
             cfg->db_path, cfg->profile_path, cfg->active_profile);
    log_info("ofono backend=%s destination=%s modem_path=%s",
             cfg->ofono_backend, cfg->ofono_destination, cfg->ofono_modem_path);
}
