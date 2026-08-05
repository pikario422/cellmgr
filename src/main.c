#include "http.h"
#include "dbus_monitor.h"
#include "profile.h"
#include "scheduler.h"

#include <unistd.h>

static const char *arg_value(int argc, char **argv, const char *name)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *config_path = arg_value(argc, argv, "-c");
    if (!config_path) {
        config_path = CELLMGR_DEFAULT_CONFIG;
    }

    app_state state;
    memset(&state, 0, sizeof(state));
    config_load(&state.cfg, config_path);
    snprintf(state.session_token, sizeof(state.session_token), "%ld%ld", now_unix(), (long)getpid());
    config_dump(&state.cfg);

    if (db_open(&state.db, state.cfg.db_path) != 0) {
        return 1;
    }
    if (db_init_schema(&state.db) != 0) {
        db_close(&state.db);
        return 1;
    }
    profile_import_file(&state.db, state.cfg.profile_path, state.cfg.active_profile);

    char *active = db_get_setting(&state.db, "active_profile");
    if (active && active[0] != '\0') {
        snprintf(state.cfg.active_profile, sizeof(state.cfg.active_profile), "%s", active);
    } else {
        db_set_setting(&state.db, "active_profile", state.cfg.active_profile);
    }
    free(active);

    char *enabled = db_get_setting(&state.db, "scheduled_reboot_enabled");
    char *hour = db_get_setting(&state.db, "scheduled_reboot_hour");
    char *minute = db_get_setting(&state.db, "scheduled_reboot_minute");
    char *sms_forward_enabled = db_get_setting(&state.db, "sms_forward_enabled");
    char *sms_forward_mode = db_get_setting(&state.db, "sms_forward_mode");
    char *smtp_host = db_get_setting(&state.db, "smtp_host");
    char *smtp_port = db_get_setting(&state.db, "smtp_port");
    char *smtp_user = db_get_setting(&state.db, "smtp_user");
    char *smtp_pass = db_get_setting(&state.db, "smtp_pass");
    char *smtp_from = db_get_setting(&state.db, "smtp_from");
    char *smtp_to = db_get_setting(&state.db, "smtp_to");
    char *sms_webhook_url = db_get_setting(&state.db, "sms_webhook_url");
    char *ofono_modem_path = db_get_setting(&state.db, "ofono_modem_path");
    char *wan_iface = db_get_setting(&state.db, "wan_iface");
    char *at_timeout_ms = db_get_setting(&state.db, "at_timeout_ms");
    char *allow_dangerous_at = db_get_setting(&state.db, "allow_dangerous_at");
    char *auth_user = db_get_setting(&state.db, "auth_user");
    char *auth_pass = db_get_setting(&state.db, "auth_pass");
    if (enabled) {
        state.cfg.scheduled_reboot_enabled = atoi(enabled);
    }
    if (hour) {
        state.cfg.scheduled_reboot_hour = atoi(hour);
    }
    if (minute) {
        state.cfg.scheduled_reboot_minute = atoi(minute);
    }
    if (sms_forward_enabled) state.cfg.sms_forward_enabled = atoi(sms_forward_enabled);
    if (sms_forward_mode) snprintf(state.cfg.sms_forward_mode, sizeof(state.cfg.sms_forward_mode), "%s", sms_forward_mode);
    if (smtp_host) snprintf(state.cfg.smtp_host, sizeof(state.cfg.smtp_host), "%s", smtp_host);
    if (smtp_port) state.cfg.smtp_port = atoi(smtp_port);
    if (smtp_user) snprintf(state.cfg.smtp_user, sizeof(state.cfg.smtp_user), "%s", smtp_user);
    if (smtp_pass) snprintf(state.cfg.smtp_pass, sizeof(state.cfg.smtp_pass), "%s", smtp_pass);
    if (smtp_from) snprintf(state.cfg.smtp_from, sizeof(state.cfg.smtp_from), "%s", smtp_from);
    if (smtp_to) snprintf(state.cfg.smtp_to, sizeof(state.cfg.smtp_to), "%s", smtp_to);
    if (sms_webhook_url) snprintf(state.cfg.sms_webhook_url, sizeof(state.cfg.sms_webhook_url), "%s", sms_webhook_url);
    if (ofono_modem_path) snprintf(state.cfg.ofono_modem_path, sizeof(state.cfg.ofono_modem_path), "%s", ofono_modem_path);
    if (wan_iface) snprintf(state.cfg.wan_iface, sizeof(state.cfg.wan_iface), "%s", wan_iface);
    if (at_timeout_ms) state.cfg.at_timeout_ms = atoi(at_timeout_ms);
    if (allow_dangerous_at) state.cfg.allow_dangerous_at = atoi(allow_dangerous_at);
    if (auth_user) snprintf(state.cfg.auth_user, sizeof(state.cfg.auth_user), "%s", auth_user);
    if (auth_pass) snprintf(state.cfg.auth_pass, sizeof(state.cfg.auth_pass), "%s", auth_pass);
    free(enabled);
    free(hour);
    free(minute);
    free(sms_forward_enabled);
    free(sms_forward_mode);
    free(smtp_host);
    free(smtp_port);
    free(smtp_user);
    free(smtp_pass);
    free(smtp_from);
    free(smtp_to);
    free(sms_webhook_url);
    free(ofono_modem_path);
    free(wan_iface);
    free(at_timeout_ms);
    free(allow_dangerous_at);
    free(auth_user);
    free(auth_pass);

    scheduler_start(&state);
    dbus_monitor_start(&state);

    int rc = http_serve(&state);
    db_close(&state.db);
    return rc == 0 ? 0 : 1;
}
