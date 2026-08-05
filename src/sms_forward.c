#include "sms_forward.h"

#include "dbus_cmd.h"
#include "json_util.h"
#include "smtp.h"

static int webhook_send(const app_config *cfg, const char *from, const char *text, char **out_log)
{
    cellmgr_buf payload;
    buf_init(&payload, 256);
    buf_append(&payload, "{\"from\":");
    json_escape(&payload, from ? from : "");
    buf_append(&payload, ",\"text\":");
    json_escape(&payload, text ? text : "");
    buf_append(&payload, "}");

    char *const argv[] = {
        "curl",
        "-sS",
        "-X", "POST",
        "-H", "Content-Type: application/json",
        "--data-binary", payload.data,
        (char *)cfg->sms_webhook_url,
        NULL
    };
    command_result res;
    int ok = run_capture(argv, 10000, 8192, &res);
    cellmgr_buf log;
    buf_init(&log, 512);
    buf_append(&log, "stdout:\n");
    buf_append(&log, res.out ? res.out : "");
    buf_append(&log, "\nstderr:\n");
    buf_append(&log, res.err ? res.err : "");
    if (out_log) *out_log = log.data; else buf_free(&log);
    int exit_code = res.exit_code;
    command_result_free(&res);
    buf_free(&payload);
    return ok == 0 && exit_code == 0 ? 0 : -1;
}

int sms_forward_send(const app_config *cfg, const char *from, const char *text,
                     char **out_log)
{
    if (out_log) *out_log = NULL;
    if (!cfg->sms_forward_enabled) {
        if (out_log) *out_log = xstrdup("sms forwarding disabled");
        return -1;
    }
    if (strcmp(cfg->sms_forward_mode, "webhook") == 0) {
        if (cfg->sms_webhook_url[0] == '\0') {
            if (out_log) *out_log = xstrdup("sms_webhook_url is empty");
            return -1;
        }
        return webhook_send(cfg, from, text, out_log);
    }
    smtp_config smtp;
    memset(&smtp, 0, sizeof(smtp));
    snprintf(smtp.host, sizeof(smtp.host), "%s", cfg->smtp_host);
    smtp.port = cfg->smtp_port;
    snprintf(smtp.user, sizeof(smtp.user), "%s", cfg->smtp_user);
    snprintf(smtp.pass, sizeof(smtp.pass), "%s", cfg->smtp_pass);
    snprintf(smtp.from, sizeof(smtp.from), "%s", cfg->smtp_from);
    snprintf(smtp.to, sizeof(smtp.to), "%s", cfg->smtp_to);
    if (smtp.host[0] == '\0' || smtp.from[0] == '\0' || smtp.to[0] == '\0') {
        if (out_log) *out_log = xstrdup("smtp_host/from/to are required");
        return -1;
    }
    cellmgr_buf body;
    buf_init(&body, 256);
    buf_append(&body, "From: ");
    buf_append(&body, from ? from : "");
    buf_append(&body, "\n\n");
    buf_append(&body, text ? text : "");
    int ok = smtp_send_plain(&smtp, "CellMgr SMS Forward", body.data, out_log);
    buf_free(&body);
    return ok;
}
