#include "ofono.h"

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
