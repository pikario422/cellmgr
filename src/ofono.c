#include "ofono.h"

static int path_supported_score(const char *block)
{
    int score = 0;
    if (strstr(block, "org.ofono.NetworkRegistration")) score += 4;
    if (strstr(block, "org.ofono.MessageManager")) score += 4;
    if (strstr(block, "org.ofono.SimManager")) score += 2;
    if (strstr(block, "org.ofono.Modem")) score += 2;
    if (strstr(block, "org.ofono.RadioSettings")) score += 1;
    return score;
}

static int extract_path(char *out, size_t out_sz, const char *line)
{
    const char *p = strstr(line, "object path ");
    if (!p) {
        return -1;
    }
    p = strchr(p, '"');
    if (!p) {
        return -1;
    }
    p++;
    size_t n = 0;
    while (p[n] && p[n] != '"' && n + 1 < out_sz) {
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';
    return out[0] ? 0 : -1;
}

int ofono_get_modem_properties(const app_config *cfg, command_result *res)
{
    return dbus_send_call(cfg->ofono_destination, cfg->ofono_modem_path,
                          "org.ofono.Modem.GetProperties", NULL, 0,
                          cfg->at_timeout_ms, res);
}

int ofono_get_network_properties(const app_config *cfg, command_result *res)
{
    return dbus_send_call(cfg->ofono_destination, cfg->ofono_modem_path,
                          "org.ofono.NetworkRegistration.GetProperties", NULL, 0,
                          cfg->at_timeout_ms, res);
}

int ofono_get_sim_properties(const app_config *cfg, command_result *res)
{
    return dbus_send_call(cfg->ofono_destination, cfg->ofono_modem_path,
                          "org.ofono.SimManager.GetProperties", NULL, 0,
                          cfg->at_timeout_ms, res);
}

int ofono_get_modems(const app_config *cfg, command_result *res)
{
    return dbus_send_call(cfg->ofono_destination, "/", "org.ofono.Manager.GetModems",
                          NULL, 0, cfg->at_timeout_ms, res);
}

int ofono_resolve_modem_path(const app_config *cfg, char *out_path, size_t out_sz, command_result *res)
{
    if (!out_path || out_sz == 0) {
        return -1;
    }
    out_path[0] = '\0';

    command_result probe;
    memset(&probe, 0, sizeof(probe));
    if (ofono_get_modem_properties(cfg, &probe) == 0 && probe.exit_code == 0) {
        snprintf(out_path, out_sz, "%s", cfg->ofono_modem_path);
        command_result_free(&probe);
        return 0;
    }
    command_result_free(&probe);

    command_result modems;
    memset(&modems, 0, sizeof(modems));
    if (ofono_get_modems(cfg, &modems) != 0 || modems.exit_code != 0 || !modems.out) {
        if (res) {
            *res = modems;
        } else {
            command_result_free(&modems);
        }
        return -1;
    }

    const char *p = modems.out;
    int best_score = -1;
    char best[128] = "";
    while ((p = strstr(p, "object path ")) != NULL) {
        char path[128];
        if (extract_path(path, sizeof(path), p) != 0) {
            p += 11;
            continue;
        }
        const char *block_end = strstr(p + 11, "object path ");
        size_t block_len = block_end ? (size_t)(block_end - p) : strlen(p);
        if (block_len >= 2048) block_len = 2047;
        char block[2048];
        memcpy(block, p, block_len);
        block[block_len] = '\0';
        int score = path_supported_score(block);
        if (score > best_score) {
            best_score = score;
            snprintf(best, sizeof(best), "%s", path);
        }
        p += 11;
    }

    if (best[0] == '\0') {
        if (res) {
            *res = modems;
        } else {
            command_result_free(&modems);
        }
        return -1;
    }

    snprintf(out_path, out_sz, "%s", best);
    if (res) {
        *res = modems;
    } else {
        command_result_free(&modems);
    }
    return 0;
}

int ofono_set_powered(const app_config *cfg, int powered, command_result *res)
{
    const char *args[2];
    args[0] = "string:Powered";
    args[1] = powered ? "variant:boolean:true" : "variant:boolean:false";
    return dbus_send_call(cfg->ofono_destination, cfg->ofono_modem_path,
                          "org.ofono.Modem.SetProperty", args, 2,
                          cfg->at_timeout_ms, res);
}

int ofono_set_online(const app_config *cfg, int online, command_result *res)
{
    const char *args[2];
    args[0] = "string:Online";
    args[1] = online ? "variant:boolean:true" : "variant:boolean:false";
    return dbus_send_call(cfg->ofono_destination, cfg->ofono_modem_path,
                          "org.ofono.Modem.SetProperty", args, 2,
                          cfg->at_timeout_ms, res);
}

int ofono_send_at(const app_config *cfg, const char *command, command_result *res)
{
    char at_arg[1024];
    snprintf(at_arg, sizeof(at_arg), "string:%s", command);
    const char *args[1];
    args[0] = at_arg;
    return dbus_send_call(cfg->ofono_destination, cfg->ofono_modem_path,
                          "org.ofono.Modem.SendAtcmd", args, 1,
                          cfg->at_timeout_ms, res);
}

int ofono_send_sms(const app_config *cfg, const char *number, const char *text, command_result *res)
{
    char num_arg[256];
    char text_arg[2048];
    snprintf(num_arg, sizeof(num_arg), "string:%s", number ? number : "");
    snprintf(text_arg, sizeof(text_arg), "string:%s", text ? text : "");
    const char *args[2];
    args[0] = num_arg;
    args[1] = text_arg;
    return dbus_send_call(cfg->ofono_destination, cfg->ofono_modem_path,
                          "org.ofono.MessageManager.SendMessage", args, 2,
                          cfg->at_timeout_ms, res);
}
