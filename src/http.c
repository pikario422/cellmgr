#include "http.h"

#include "at.h"
#include "json_util.h"
#include "ofono.h"
#include "parsers.h"
#include "profile.h"
#include "sms_forward.h"
#include "sysinfo.h"
#include "web_assets.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct http_request {
    char method[8];
    char path[256];
    char query[256];
    char session_token[128];
    char *body;
} http_request;

static char *profile_cmd_or_default(app_state *state, const char *capability,
                                    const char *field, const char *fallback);
static int sms_messages_via_ofono(app_state *state, command_result *res, char **parsed);

static void response_raw(int fd, int status, const char *ctype, const char *body)
{
    const char *reason = status == 200 ? "OK" : status == 404 ? "Not Found" : "Error";
    size_t len = body ? strlen(body) : 0;
    dprintf(fd,
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s; charset=utf-8\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "Cache-Control: no-store\r\n\r\n",
            status, reason, ctype, len);
    if (body) {
        write(fd, body, len);
    }
}

static void response_json_body(int fd, int ok, const char *data_json, const char *err)
{
    cellmgr_buf b;
    buf_init(&b, 512);
    if (ok) {
        buf_append(&b, "{\"ok\":true,\"data\":");
        buf_append(&b, data_json ? data_json : "{}");
        buf_append(&b, ",\"error\":null}");
    } else {
        buf_append(&b, "{\"ok\":false,\"data\":null,\"error\":");
        json_escape(&b, err ? err : "error");
        buf_append(&b, "}");
    }
    response_raw(fd, ok ? 200 : 500, "application/json", b.data);
    buf_free(&b);
}

static int parse_request(int fd, http_request *req)
{
    memset(req, 0, sizeof(*req));
    char buf[CELLMGR_MAX_BODY + 4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';
    char *header_end = strstr(buf, "\r\n\r\n");
    if (!header_end) {
        return -1;
    }
    *header_end = '\0';
    req->body = header_end + 4;

    char *line_end = strstr(buf, "\r\n");
    if (!line_end) {
        return -1;
    }
    *line_end = '\0';
    char url[512];
    if (sscanf(buf, "%7s %511s", req->method, url) != 2) {
        return -1;
    }
    char *q = strchr(url, '?');
    if (q) {
        *q = '\0';
        snprintf(req->query, sizeof(req->query), "%s", q + 1);
    }
    snprintf(req->path, sizeof(req->path), "%s", url);
    char *headers = line_end + 2;
    char *save = NULL;
    char *line = strtok_r(headers, "\r\n", &save);
    while (line) {
        if (strncmp(line, "X-Cellmgr-Session:", 18) == 0) {
            snprintf(req->session_token, sizeof(req->session_token), "%s", line + 18);
            trim_in_place(req->session_token);
        }
        line = strtok_r(NULL, "\r\n", &save);
    }
    return 0;
}

static int request_authorized(app_state *state, http_request *req)
{
    if (!state->cfg.auth_enabled) return 1;
    if (strcmp(req->path, "/") == 0 || strcmp(req->path, "/style.css") == 0 ||
        strcmp(req->path, "/app.js") == 0 || strcmp(req->path, "/favicon.ico") == 0 ||
        strcmp(req->path, "/api/auth/login") == 0) {
        return 1;
    }
    return req->session_token[0] && strcmp(req->session_token, state->session_token) == 0;
}

static void api_auth_login(int fd, app_state *state, const char *body)
{
    char *user = json_get_string(body, "username");
    char *pass = json_get_string(body, "password");
    int ok = user && pass && strcmp(user, state->cfg.auth_user) == 0 &&
             strcmp(pass, state->cfg.auth_pass) == 0;
    cellmgr_buf b;
    buf_init(&b, 256);
    if (ok) {
        buf_append(&b, "{\"token\":");
        json_escape(&b, state->session_token);
        buf_append(&b, "}");
        response_json_body(fd, 1, b.data, NULL);
    } else {
        response_json_body(fd, 0, NULL, "invalid username or password");
    }
    buf_free(&b);
    free(user);
    free(pass);
}

static void api_device_status(int fd, app_state *state)
{
    cellmgr_buf b;
    buf_init(&b, 1024);
    sysinfo_json(&b, state->cfg.wan_iface);
    response_json_body(fd, 1, b.data, NULL);
    buf_free(&b);
}

static void api_overview(int fd, app_state *state)
{
    cellmgr_buf dev;
    cellmgr_buf out;
    buf_init(&dev, 1024);
    buf_init(&out, 2048);
    sysinfo_json(&dev, state->cfg.wan_iface);
    buf_append(&out, "{\"device\":");
    buf_append(&out, dev.data);
    buf_append(&out, ",\"profile\":");
    json_escape(&out, state->cfg.active_profile);
    buf_append(&out, "}");
    response_json_body(fd, 1, out.data, NULL);
    buf_free(&dev);
    buf_free(&out);
}

static void api_modem_status(int fd, app_state *state)
{
    command_result modem, net, sim;
    ofono_get_modem_properties(&state->cfg, &modem);
    ofono_get_network_properties(&state->cfg, &net);
    ofono_get_sim_properties(&state->cfg, &sim);
    cellmgr_buf b;
    char *modem_parsed = parse_dbus_properties_json(modem.out ? modem.out : "");
    char *network_parsed = parse_dbus_properties_json(net.out ? net.out : "");
    char *sim_parsed = parse_dbus_properties_json(sim.out ? sim.out : "");
    buf_init(&b, 4096);
    buf_append(&b, "{");
    json_prop_int(&b, "modem_exit", modem.exit_code, 0);
    json_prop_string(&b, "modem_raw", modem.out ? modem.out : "", 1);
    buf_append(&b, ",\"modem_parsed\":");
    buf_append(&b, modem_parsed ? modem_parsed : "{}");
    json_prop_int(&b, "network_exit", net.exit_code, 1);
    json_prop_string(&b, "network_raw", net.out ? net.out : "", 1);
    buf_append(&b, ",\"network_parsed\":");
    buf_append(&b, network_parsed ? network_parsed : "{}");
    json_prop_int(&b, "sim_exit", sim.exit_code, 1);
    json_prop_string(&b, "sim_raw", sim.out ? sim.out : "", 1);
    buf_append(&b, ",\"sim_parsed\":");
    buf_append(&b, sim_parsed ? sim_parsed : "{}");
    buf_append(&b, "}");
    response_json_body(fd, 1, b.data, NULL);
    buf_free(&b);
    free(modem_parsed);
    free(network_parsed);
    free(sim_parsed);
    command_result_free(&modem);
    command_result_free(&net);
    command_result_free(&sim);
}

static void append_at_cap_result(cellmgr_buf *b, app_state *state, const char *name,
                                 const char *capability, const char *field,
                                 const char *parser, const char *fallback, int comma)
{
    char *cmd = profile_cmd_or_default(state, capability, field, fallback);
    command_result res;
    at_send(&state->db, &state->cfg, cmd, 1, &res);
    char *parsed = parse_at_response_json(parser, res.out ? res.out : "");
    if (comma) buf_append(b, ",");
    json_escape(b, name);
    buf_append(b, ":{\"command\":");
    json_escape(b, cmd ? cmd : "");
    buf_append(b, ",\"exit_code\":");
    buf_appendf(b, "%d", res.exit_code);
    buf_append(b, ",\"parsed\":");
    buf_append(b, parsed);
    buf_append(b, "}");
    free(parsed);
    free(cmd);
    command_result_free(&res);
}

static void api_modem_radio(int fd, app_state *state)
{
    cellmgr_buf b;
    buf_init(&b, 4096);
    buf_append(&b, "{");
    append_at_cap_result(&b, state, "signal", "signal.basic", "command", "csq", "AT+CSQ", 0);
    append_at_cap_result(&b, state, "operator", "operator.current", "read", "cops", "AT+COPS?", 1);
    append_at_cap_result(&b, state, "eps_registration", "registration.eps", "read", "cereg", "AT+CEREG?", 1);
    append_at_cap_result(&b, state, "nr_registration", "registration.nr", "read", "c5greg", "AT+C5GREG?", 1);
    append_at_cap_result(&b, state, "qci", "qos.qci", "read", "cgeqos", "AT+CGEQOS?", 1);
    append_at_cap_result(&b, state, "cell_lock", "network.cell_lock", "read", "gtcelllock", "AT+GTCELLLOCK?", 1);
    buf_append(&b, "}");
    response_json_body(fd, 1, b.data, NULL);
    buf_free(&b);
}

static void api_modem_set_bool(int fd, app_state *state, const char *body, int is_power)
{
    int value = json_get_int(body, is_power ? "powered" : "online", 1);
    int confirm = json_get_int(body, "confirm", 0);
    if (!confirm) {
        response_json_body(fd, 0, NULL, "confirm is required");
        return;
    }
    command_result res;
    if (is_power) {
        ofono_set_powered(&state->cfg, value ? 1 : 0, &res);
    } else {
        ofono_set_online(&state->cfg, value ? 1 : 0, &res);
    }
    cellmgr_buf b;
    buf_init(&b, 512);
    buf_append(&b, "{");
    json_prop_int(&b, is_power ? "powered" : "online", value ? 1 : 0, 0);
    json_prop_int(&b, "exit_code", res.exit_code, 1);
    json_prop_string(&b, "stdout", res.out ? res.out : "", 1);
    json_prop_string(&b, "stderr", res.err ? res.err : "", 1);
    buf_append(&b, "}");
    response_json_body(fd, res.exit_code == 0, b.data, res.err);
    buf_free(&b);
    command_result_free(&res);
}

static void api_at_send(int fd, app_state *state, const char *body)
{
    char *cmd = json_get_string(body, "command");
    if (!cmd) {
        response_json_body(fd, 0, NULL, "missing command");
        return;
    }
    command_result res;
    int allow = state->cfg.allow_dangerous_at;
    at_send(&state->db, &state->cfg, cmd, allow, &res);
    cellmgr_buf b;
    buf_init(&b, 1024);
    buf_append(&b, "{");
    json_prop_string(&b, "command", cmd, 0);
    json_prop_int(&b, "exit_code", res.exit_code, 1);
    json_prop_string(&b, "stdout", res.out ? res.out : "", 1);
    json_prop_string(&b, "stderr", res.err ? res.err : "", 1);
    buf_append(&b, "}");
    response_json_body(fd, res.exit_code == 0, b.data, res.err);
    buf_free(&b);
    command_result_free(&res);
    free(cmd);
}

static void api_at_history(int fd, app_state *state, const char *query)
{
    int limit = 30;
    const char *p = strstr(query ? query : "", "limit=");
    if (p) {
        limit = atoi(p + 6);
    }
    char *json = db_list_at_history_json(&state->db, limit);
    if (!json) {
        response_json_body(fd, 0, NULL, "list at history failed");
        return;
    }
    response_json_body(fd, 1, json, NULL);
    free(json);
}

static char *profile_cmd_or_default(app_state *state, const char *capability,
                                    const char *field, const char *fallback)
{
    char *profile = profile_get_active_json(&state->db, &state->cfg);
    if (!profile) {
        return xstrdup(fallback);
    }
    char *cmd = profile_get_capability_command(profile, capability, field);
    free(profile);
    return cmd ? cmd : xstrdup(fallback);
}

static int sms_messages_via_ofono(app_state *state, command_result *res, char **parsed)
{
    memset(res, 0, sizeof(*res));
    dbus_send_call(state->cfg.ofono_destination, state->cfg.ofono_modem_path,
                   "org.ofono.MessageManager.GetMessages", NULL, 0,
                   state->cfg.at_timeout_ms, res);
    *parsed = parse_dbus_messages_json(res->out ? res->out : "");
    return res->exit_code;
}

static void api_sms_list(int fd, app_state *state, const char *query)
{
    (void)query;
    command_result res;
    char *parsed = NULL;
    int rc = sms_messages_via_ofono(state, &res, &parsed);
    if (rc == 0) {
        cellmgr_buf b;
        buf_init(&b, 1024);
        buf_append(&b, "{");
        json_prop_string(&b, "command", "org.ofono.MessageManager.GetMessages", 0);
        json_prop_int(&b, "exit_code", res.exit_code, 1);
        json_prop_string(&b, "raw", res.out ? res.out : "", 1);
        buf_append(&b, ",\"parsed\":");
        buf_append(&b, parsed ? parsed : "{\"items\":[]}");
        buf_append(&b, "}");
        response_json_body(fd, 1, b.data, NULL);
        buf_free(&b);
    } else {
        response_json_body(fd, 0, NULL, res.err ? res.err : "ofono MessageManager.GetMessages failed");
    }
    free(parsed);
    command_result_free(&res);
}

static void api_sms_read(int fd, app_state *state, const char *query)
{
    int index = 0;
    const char *p = strstr(query ? query : "", "index=");
    if (p) {
        index = atoi(p + 6);
    }
    if (index <= 0) {
        response_json_body(fd, 0, NULL, "missing sms index");
        return;
    }
    command_result res;
    char *parsed = NULL;
    if (sms_messages_via_ofono(state, &res, &parsed) != 0) {
        response_json_body(fd, 0, NULL, res.err ? res.err : "ofono MessageManager.GetMessages failed");
        free(parsed);
        command_result_free(&res);
        return;
    }
    cellmgr_buf b;
    buf_init(&b, 2048);
    buf_append(&b, "{");
    json_prop_int(&b, "index", index, 0);
    json_prop_string(&b, "raw", res.out ? res.out : "", 1);
    json_prop_int(&b, "exit_code", res.exit_code, 1);
    buf_append(&b, ",\"parsed\":");
    buf_append(&b, parsed);
    buf_append(&b, "}");
    response_json_body(fd, res.exit_code == 0, b.data, res.err);
    buf_free(&b);
    free(parsed);
    command_result_free(&res);
}

static void api_sms_send(int fd, app_state *state, const char *body)
{
    char *number = json_get_string(body, "number");
    char *text = json_get_string(body, "text");
    if (!number || !text || number[0] == '\0') {
        free(number);
        free(text);
        response_json_body(fd, 0, NULL, "number and text are required");
        return;
    }
    command_result res;
    ofono_send_sms(&state->cfg, number, text, &res);
    cellmgr_buf b;
    buf_init(&b, 1024);
    buf_append(&b, "{");
    json_prop_string(&b, "number", number, 0);
    json_prop_int(&b, "exit_code", res.exit_code, 1);
    json_prop_string(&b, "stdout", res.out ? res.out : "", 1);
    json_prop_string(&b, "stderr", res.err ? res.err : "", 1);
    buf_append(&b, "}");
    response_json_body(fd, res.exit_code == 0, b.data, res.err);
    buf_free(&b);
    command_result_free(&res);
    free(number);
    free(text);
}

static void api_sms_delete(int fd, app_state *state, const char *body)
{
    int index = json_get_int(body, "index", 0);
    int confirm = json_get_int(body, "confirm", 0);
    if (index <= 0 || !confirm) {
        response_json_body(fd, 0, NULL, "index and confirm are required");
        return;
    }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", index);
    command_result res;
    at_send(&state->db, &state->cfg, cmd, 1, &res);
    cellmgr_buf b;
    buf_init(&b, 256);
    buf_append(&b, "{");
    json_prop_int(&b, "index", index, 0);
    json_prop_int(&b, "exit_code", res.exit_code, 1);
    json_prop_string(&b, "raw", res.out ? res.out : "", 1);
    buf_append(&b, "}");
    response_json_body(fd, res.exit_code == 0, b.data, res.err);
    buf_free(&b);
    command_result_free(&res);
}

static void api_sms_forward_test(int fd, app_state *state, const char *body)
{
    char *from = json_get_string(body, "from");
    char *text = json_get_string(body, "text");
    if (!text) {
        text = xstrdup("CellMgr SMS forwarding test");
    }
    char *log = NULL;
    int ok = sms_forward_send(&state->cfg, from ? from : "cellmgr", text, &log);
    cellmgr_buf b;
    buf_init(&b, 512);
    buf_append(&b, "{");
    json_prop_int(&b, "sent", ok == 0 ? 1 : 0, 0);
    json_prop_string(&b, "log", log ? log : "", 1);
    buf_append(&b, "}");
    response_json_body(fd, ok == 0, b.data, log);
    buf_free(&b);
    free(log);
    free(from);
    free(text);
}

static void api_profiles_current(int fd, app_state *state)
{
    char *json = profile_get_active_json(&state->db, &state->cfg);
    if (!json) {
        response_json_body(fd, 0, NULL, "active profile not found");
        return;
    }
    response_json_body(fd, 1, json, NULL);
    free(json);
}

static void api_profiles_get(int fd, app_state *state, const char *query)
{
    const char *idp = strstr(query ? query : "", "id=");
    if (!idp) {
        response_json_body(fd, 0, NULL, "missing profile id");
        return;
    }
    idp += 3;
    char id[128];
    size_t i = 0;
    while (idp[i] && idp[i] != '&' && i + 1 < sizeof(id)) {
        id[i] = idp[i];
        i++;
    }
    id[i] = '\0';
    char *json = db_get_profile_json(&state->db, id);
    if (!json) {
        response_json_body(fd, 0, NULL, "profile not found");
        return;
    }
    response_json_body(fd, 1, json, NULL);
    free(json);
}

static void api_profiles_list(int fd, app_state *state)
{
    char *json = db_list_profiles_json(&state->db, state->cfg.active_profile);
    if (!json) {
        response_json_body(fd, 0, NULL, "list profiles failed");
        return;
    }
    response_json_body(fd, 1, json, NULL);
    free(json);
}

static void api_profiles_save(int fd, app_state *state, const char *body)
{
    char *id = NULL;
    char *name = NULL;
    if (profile_import_json(&state->db, body, &id, &name) != 0) {
        response_json_body(fd, 0, NULL, "invalid profile json; id and name are required");
        return;
    }
    cellmgr_buf b;
    buf_init(&b, 256);
    buf_append(&b, "{");
    json_prop_string(&b, "id", id, 0);
    json_prop_string(&b, "name", name, 1);
    buf_append(&b, ",\"saved\":true}");
    response_json_body(fd, 1, b.data, NULL);
    buf_free(&b);
    free(id);
    free(name);
}

static void api_profiles_activate(int fd, app_state *state, const char *body)
{
    char *id = json_get_string(body, "id");
    if (!id || id[0] == '\0') {
        free(id);
        response_json_body(fd, 0, NULL, "missing profile id");
        return;
    }
    if (!db_profile_exists(&state->db, id)) {
        free(id);
        response_json_body(fd, 0, NULL, "profile not found");
        return;
    }
    snprintf(state->cfg.active_profile, sizeof(state->cfg.active_profile), "%s", id);
    db_set_setting(&state->db, "active_profile", id);
    cellmgr_buf b;
    buf_init(&b, 128);
    buf_append(&b, "{");
    json_prop_string(&b, "active_profile", id, 0);
    buf_append(&b, "}");
    response_json_body(fd, 1, b.data, NULL);
    buf_free(&b);
    free(id);
}

static void api_profiles_delete(int fd, app_state *state, const char *body)
{
    char *id = json_get_string(body, "id");
    if (!id || id[0] == '\0') {
        free(id);
        response_json_body(fd, 0, NULL, "missing profile id");
        return;
    }
    if (strcmp(id, state->cfg.active_profile) == 0) {
        free(id);
        response_json_body(fd, 0, NULL, "cannot delete active profile");
        return;
    }
    if (!db_profile_exists(&state->db, id)) {
        free(id);
        response_json_body(fd, 0, NULL, "profile not found");
        return;
    }
    int ok = db_delete_profile(&state->db, id) == 0;
    cellmgr_buf b;
    buf_init(&b, 128);
    buf_append(&b, "{");
    json_prop_string(&b, "id", id, 0);
    buf_append(&b, ",\"deleted\":true}");
    response_json_body(fd, ok, b.data, ok ? NULL : "delete failed");
    buf_free(&b);
    free(id);
}

static void api_profiles_export(int fd, app_state *state, const char *body)
{
    char *id = json_get_string(body, "id");
    const char *profile_id = (id && id[0] != '\0') ? id : state->cfg.active_profile;
    char *json = db_get_profile_json(&state->db, profile_id);
    if (!json) {
        free(id);
        response_json_body(fd, 0, NULL, "profile not found");
        return;
    }
    response_json_body(fd, 1, json, NULL);
    free(json);
    free(id);
}

static void api_profiles_validate(int fd, const char *body)
{
    char *id = json_get_string(body, "id");
    char *name = json_get_string(body, "name");
    int ok = id && id[0] != '\0' && name && name[0] != '\0';
    cellmgr_buf b;
    buf_init(&b, 256);
    buf_append(&b, "{");
    buf_append(&b, "\"valid\":");
    buf_append(&b, ok ? "true" : "false");
    if (id) {
        json_prop_string(&b, "id", id, 1);
    }
    if (name) {
        json_prop_string(&b, "name", name, 1);
    }
    buf_append(&b, "}");
    response_json_body(fd, 1, b.data, NULL);
    buf_free(&b);
    free(id);
    free(name);
}

static void api_at_capabilities(int fd, app_state *state)
{
    char *json = profile_get_active_json(&state->db, &state->cfg);
    if (!json) {
        response_json_body(fd, 0, NULL, "active profile not found");
        return;
    }
    cellmgr_buf b;
    buf_init(&b, 1024);
    buf_append(&b, "{\"profile_id\":");
    json_escape(&b, state->cfg.active_profile);
    buf_append(&b, ",\"profile\":");
    buf_append(&b, json);
    buf_append(&b, "}");
    response_json_body(fd, 1, b.data, NULL);
    buf_free(&b);
    free(json);
}

static void api_at_test_capability(int fd, app_state *state, const char *body)
{
    char *cap = json_get_string(body, "capability");
    char *field = json_get_string(body, "field");
    if (!field) field = xstrdup("read");
    if (!cap) {
        free(field);
        response_json_body(fd, 0, NULL, "capability is required");
        return;
    }
    char *cmd = profile_cmd_or_default(state, cap, field, "");
    if (!cmd || cmd[0] == '\0') {
        free(cap); free(field); free(cmd);
        response_json_body(fd, 0, NULL, "capability command not found");
        return;
    }
    command_result res;
    at_send(&state->db, &state->cfg, cmd, 1, &res);
    cellmgr_buf b;
    buf_init(&b, 1024);
    buf_append(&b, "{");
    json_prop_string(&b, "capability", cap, 0);
    json_prop_string(&b, "field", field, 1);
    json_prop_string(&b, "command", cmd, 1);
    json_prop_int(&b, "exit_code", res.exit_code, 1);
    json_prop_string(&b, "raw", res.out ? res.out : "", 1);
    buf_append(&b, "}");
    response_json_body(fd, res.exit_code == 0, b.data, res.err);
    buf_free(&b);
    command_result_free(&res);
    free(cap); free(field); free(cmd);
}

static void api_settings(int fd, app_state *state)
{
    cellmgr_buf b;
    buf_init(&b, 1024);
    buf_append(&b, "{");
    json_prop_string(&b, "listen_host", state->cfg.listen_host, 0);
    json_prop_int(&b, "listen_port", state->cfg.listen_port, 1);
    json_prop_string(&b, "db_path", state->cfg.db_path, 1);
    json_prop_string(&b, "active_profile", state->cfg.active_profile, 1);
    json_prop_string(&b, "ofono_modem_path", state->cfg.ofono_modem_path, 1);
    json_prop_string(&b, "wan_iface", state->cfg.wan_iface, 1);
    json_prop_int(&b, "at_timeout_ms", state->cfg.at_timeout_ms, 1);
    json_prop_int(&b, "allow_dangerous_at", state->cfg.allow_dangerous_at, 1);
    json_prop_string(&b, "at_backend", state->cfg.at_backend, 1);
    json_prop_int(&b, "auth_enabled", state->cfg.auth_enabled, 1);
    json_prop_int(&b, "scheduled_reboot_enabled", state->cfg.scheduled_reboot_enabled, 1);
    json_prop_int(&b, "scheduled_reboot_hour", state->cfg.scheduled_reboot_hour, 1);
    json_prop_int(&b, "scheduled_reboot_minute", state->cfg.scheduled_reboot_minute, 1);
    json_prop_int(&b, "sms_forward_enabled", state->cfg.sms_forward_enabled, 1);
    json_prop_string(&b, "sms_forward_mode", state->cfg.sms_forward_mode, 1);
    json_prop_string(&b, "smtp_host", state->cfg.smtp_host, 1);
    json_prop_int(&b, "smtp_port", state->cfg.smtp_port, 1);
    json_prop_string(&b, "smtp_user", state->cfg.smtp_user, 1);
    json_prop_string(&b, "smtp_from", state->cfg.smtp_from, 1);
    json_prop_string(&b, "smtp_to", state->cfg.smtp_to, 1);
    json_prop_string(&b, "sms_webhook_url", state->cfg.sms_webhook_url, 1);
    buf_append(&b, "}");
    response_json_body(fd, 1, b.data, NULL);
    buf_free(&b);
}

static void api_system_reboot(int fd, const char *body)
{
    if (!json_get_int(body, "confirm", 0)) {
        response_json_body(fd, 0, NULL, "confirm is required");
        return;
    }
    char *const sync_argv[] = {"sync", NULL};
    command_result sync_res;
    run_capture(sync_argv, 3000, 1024, &sync_res);
    command_result_free(&sync_res);
    char *const reboot_argv[] = {"reboot", NULL};
    command_result reboot_res;
    run_capture(reboot_argv, 3000, 1024, &reboot_res);
    cellmgr_buf b;
    buf_init(&b, 256);
    buf_append(&b, "{");
    json_prop_int(&b, "exit_code", reboot_res.exit_code, 0);
    json_prop_string(&b, "stdout", reboot_res.out ? reboot_res.out : "", 1);
    json_prop_string(&b, "stderr", reboot_res.err ? reboot_res.err : "", 1);
    buf_append(&b, "}");
    response_json_body(fd, reboot_res.exit_code == 0, b.data, reboot_res.err);
    buf_free(&b);
    command_result_free(&reboot_res);
}

static void api_system_scheduled_reboot(int fd, app_state *state, const char *body)
{
    int enabled = json_get_int(body, "enabled", 0);
    int hour = json_get_int(body, "hour", 3);
    int minute = json_get_int(body, "minute", 30);
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        response_json_body(fd, 0, NULL, "invalid hour or minute");
        return;
    }
    state->cfg.scheduled_reboot_enabled = enabled ? 1 : 0;
    state->cfg.scheduled_reboot_hour = hour;
    state->cfg.scheduled_reboot_minute = minute;
    char v[32];
    snprintf(v, sizeof(v), "%d", enabled ? 1 : 0);
    db_set_setting(&state->db, "scheduled_reboot_enabled", v);
    snprintf(v, sizeof(v), "%d", hour);
    db_set_setting(&state->db, "scheduled_reboot_hour", v);
    snprintf(v, sizeof(v), "%d", minute);
    db_set_setting(&state->db, "scheduled_reboot_minute", v);
    cellmgr_buf b;
    buf_init(&b, 128);
    buf_append(&b, "{");
    json_prop_int(&b, "enabled", enabled ? 1 : 0, 0);
    json_prop_int(&b, "hour", hour, 1);
    json_prop_int(&b, "minute", minute, 1);
    buf_append(&b, "}");
    response_json_body(fd, 1, b.data, NULL);
    buf_free(&b);
}

static void cfg_set_string(char *dst, size_t dst_sz, char *value)
{
    if (value) {
        snprintf(dst, dst_sz, "%s", value);
    }
}

static void api_system_settings_save(int fd, app_state *state, const char *body)
{
    char *mode = json_get_string(body, "sms_forward_mode");
    char *host = json_get_string(body, "smtp_host");
    char *user = json_get_string(body, "smtp_user");
    char *pass = json_get_string(body, "smtp_pass");
    char *from = json_get_string(body, "smtp_from");
    char *to = json_get_string(body, "smtp_to");
    char *webhook = json_get_string(body, "sms_webhook_url");
    char *modem_path = json_get_string(body, "ofono_modem_path");
    char *wan_iface = json_get_string(body, "wan_iface");
    state->cfg.sms_forward_enabled = json_get_int(body, "sms_forward_enabled", state->cfg.sms_forward_enabled);
    state->cfg.smtp_port = json_get_int(body, "smtp_port", state->cfg.smtp_port);
    state->cfg.at_timeout_ms = json_get_int(body, "at_timeout_ms", state->cfg.at_timeout_ms);
    state->cfg.allow_dangerous_at = json_get_int(body, "allow_dangerous_at", state->cfg.allow_dangerous_at);
    cfg_set_string(state->cfg.sms_forward_mode, sizeof(state->cfg.sms_forward_mode), mode);
    cfg_set_string(state->cfg.smtp_host, sizeof(state->cfg.smtp_host), host);
    cfg_set_string(state->cfg.smtp_user, sizeof(state->cfg.smtp_user), user);
    cfg_set_string(state->cfg.smtp_pass, sizeof(state->cfg.smtp_pass), pass);
    cfg_set_string(state->cfg.smtp_from, sizeof(state->cfg.smtp_from), from);
    cfg_set_string(state->cfg.smtp_to, sizeof(state->cfg.smtp_to), to);
    cfg_set_string(state->cfg.sms_webhook_url, sizeof(state->cfg.sms_webhook_url), webhook);
    cfg_set_string(state->cfg.ofono_modem_path, sizeof(state->cfg.ofono_modem_path), modem_path);
    cfg_set_string(state->cfg.wan_iface, sizeof(state->cfg.wan_iface), wan_iface);

    char v[32];
    snprintf(v, sizeof(v), "%d", state->cfg.sms_forward_enabled);
    db_set_setting(&state->db, "sms_forward_enabled", v);
    db_set_setting(&state->db, "sms_forward_mode", state->cfg.sms_forward_mode);
    db_set_setting(&state->db, "smtp_host", state->cfg.smtp_host);
    snprintf(v, sizeof(v), "%d", state->cfg.smtp_port);
    db_set_setting(&state->db, "smtp_port", v);
    db_set_setting(&state->db, "smtp_user", state->cfg.smtp_user);
    db_set_setting(&state->db, "smtp_pass", state->cfg.smtp_pass);
    db_set_setting(&state->db, "smtp_from", state->cfg.smtp_from);
    db_set_setting(&state->db, "smtp_to", state->cfg.smtp_to);
    db_set_setting(&state->db, "sms_webhook_url", state->cfg.sms_webhook_url);
    db_set_setting(&state->db, "ofono_modem_path", state->cfg.ofono_modem_path);
    db_set_setting(&state->db, "wan_iface", state->cfg.wan_iface);
    snprintf(v, sizeof(v), "%d", state->cfg.at_timeout_ms);
    db_set_setting(&state->db, "at_timeout_ms", v);
    snprintf(v, sizeof(v), "%d", state->cfg.allow_dangerous_at);
    db_set_setting(&state->db, "allow_dangerous_at", v);

    free(mode); free(host); free(user); free(pass); free(from); free(to); free(webhook);
    free(modem_path); free(wan_iface);
    api_settings(fd, state);
}

static void api_system_password(int fd, app_state *state, const char *body)
{
    char *user = json_get_string(body, "username");
    char *pass = json_get_string(body, "password");
    if (!user || !pass || user[0] == '\0' || pass[0] == '\0') {
        free(user); free(pass);
        response_json_body(fd, 0, NULL, "username and password are required");
        return;
    }
    snprintf(state->cfg.auth_user, sizeof(state->cfg.auth_user), "%s", user);
    snprintf(state->cfg.auth_pass, sizeof(state->cfg.auth_pass), "%s", pass);
    db_set_setting(&state->db, "auth_user", state->cfg.auth_user);
    db_set_setting(&state->db, "auth_pass", state->cfg.auth_pass);
    response_json_body(fd, 1, "{\"saved\":true}", NULL);
    free(user); free(pass);
}

static void api_dbus_commands(int fd)
{
    const char *json =
        "{\"commands\":["
        "{\"name\":\"modem_properties\",\"method\":\"org.ofono.Modem.GetProperties\"},"
        "{\"name\":\"network_properties\",\"method\":\"org.ofono.NetworkRegistration.GetProperties\"},"
        "{\"name\":\"sim_properties\",\"method\":\"org.ofono.SimManager.GetProperties\"},"
        "{\"name\":\"set_online_false\",\"method\":\"org.ofono.Modem.SetProperty\",\"args\":[\"string:Online\",\"variant:boolean:false\"]},"
        "{\"name\":\"send_at\",\"method\":\"org.ofono.Modem.SendAtcmd\",\"args\":[\"string:AT+CGSN\"]}"
        "],\"monitors\":[\"sender='org.ofono'\",\"destination='org.ofono'\",\"interface='org.ofono.MessageManager'\"]}";
    response_json_body(fd, 1, json, NULL);
}

static void api_dbus_events(int fd, app_state *state, const char *query)
{
    int limit = 50;
    const char *p = strstr(query ? query : "", "limit=");
    if (p) limit = atoi(p + 6);
    char *json = db_list_dbus_events_json(&state->db, limit);
    if (!json) {
        response_json_body(fd, 0, NULL, "list dbus events failed");
        return;
    }
    response_json_body(fd, 1, json, NULL);
    free(json);
}

static void api_dbus_test(int fd, app_state *state, const char *body)
{
    char *method = json_get_string(body, "method");
    if (!method) method = xstrdup("org.ofono.Modem.GetProperties");
    command_result res;
    dbus_send_call(state->cfg.ofono_destination, state->cfg.ofono_modem_path,
                   method, NULL, 0, state->cfg.at_timeout_ms, &res);
    cellmgr_buf b;
    buf_init(&b, 1024);
    buf_append(&b, "{");
    json_prop_string(&b, "method", method, 0);
    json_prop_int(&b, "exit_code", res.exit_code, 1);
    json_prop_string(&b, "stdout", res.out ? res.out : "", 1);
    json_prop_string(&b, "stderr", res.err ? res.err : "", 1);
    buf_append(&b, "}");
    response_json_body(fd, res.exit_code == 0, b.data, res.err);
    buf_free(&b);
    command_result_free(&res);
    free(method);
}

static void api_network_bands(int fd, app_state *state, const char *query)
{
    int refresh = strstr(query ? query : "", "refresh=1") != NULL;
    char *cached = NULL;
    if (!refresh) {
        cached = db_get_capability_cache(&state->db, state->cfg.active_profile,
                                         "network.band", state->cfg.ofono_modem_path);
    }
    if (cached) {
        response_json_body(fd, 1, cached, NULL);
        free(cached);
        return;
    }

    char *profile = profile_get_active_json(&state->db, &state->cfg);
    if (!profile) {
        response_json_body(fd, 0, NULL, "active profile not found");
        return;
    }
    char *cmd = profile_get_capability_command(profile, "network.band", "discovery.command");
    int ttl = profile_get_capability_ttl(profile, "network.band", 86400);
    free(profile);
    if (!cmd) {
        response_json_body(fd, 0, NULL, "network.band discovery command not configured");
        return;
    }
    command_result res;
    at_send(&state->db, &state->cfg, cmd, 1, &res);
    char *parsed = parse_at_response_json("gtact", res.out ? res.out : "");
    db_put_capability_cache(&state->db, state->cfg.active_profile, "network.band",
                            state->cfg.ofono_modem_path, res.out ? res.out : "",
                            parsed, ttl);
    response_json_body(fd, res.exit_code == 0, parsed, res.err);
    free(parsed);
    free(cmd);
    command_result_free(&res);
}

static void api_network_cells(int fd, app_state *state)
{
    char *cmd = profile_cmd_or_default(state, "network.cell_info", "read", "AT+GTCCINFO?");
    command_result res;
    at_send(&state->db, &state->cfg, cmd, 1, &res);
    char *parsed = parse_at_response_json("gtccinfo", res.out ? res.out : "");
    cellmgr_buf b;
    buf_init(&b, 2048);
    buf_append(&b, "{");
    json_prop_string(&b, "command", cmd ? cmd : "", 0);
    json_prop_int(&b, "exit_code", res.exit_code, 1);
    buf_append(&b, ",\"parsed\":");
    buf_append(&b, parsed ? parsed : "{}");
    buf_append(&b, "}");
    response_json_body(fd, res.exit_code == 0, b.data, res.err);
    buf_free(&b);
    free(parsed);
    free(cmd);
    command_result_free(&res);
}

static void replace_token(char *dst, size_t dst_sz, const char *src,
                          const char *token, const char *value)
{
    dst[0] = '\0';
    const char *p = src;
    size_t token_len = strlen(token);
    while (*p && strlen(dst) + 1 < dst_sz) {
        const char *m = strstr(p, token);
        if (!m) {
            strncat(dst, p, dst_sz - strlen(dst) - 1);
            break;
        }
        strncat(dst, p, (size_t)(m - p) < dst_sz - strlen(dst) - 1 ? (size_t)(m - p) : dst_sz - strlen(dst) - 1);
        strncat(dst, value ? value : "", dst_sz - strlen(dst) - 1);
        p = m + token_len;
    }
}

static char *network_template_command(app_state *state, const char *capability, const char *field)
{
    return profile_cmd_or_default(state, capability, field, "");
}

static void api_network_lock_band(int fd, app_state *state, const char *body)
{
    if (!json_get_int(body, "confirm", 0)) {
        response_json_body(fd, 0, NULL, "confirm is required");
        return;
    }
    char *mode = json_get_string(body, "mode");
    char *rat = json_get_string(body, "rat");
    char *pref = json_get_string(body, "preferred_rat");
    char *bands = json_get_string(body, "bands");
    char *tmpl = network_template_command(state, "network.band", "set_template");
    if (!mode || !rat || !pref || !bands || !tmpl || tmpl[0] == '\0') {
        free(mode); free(rat); free(pref); free(bands); free(tmpl);
        response_json_body(fd, 0, NULL, "mode, rat, preferred_rat and bands are required");
        return;
    }
    char a[512], b[512], c[512], cmd[512];
    replace_token(a, sizeof(a), tmpl, "{mode}", mode);
    replace_token(b, sizeof(b), a, "{rat}", rat);
    replace_token(c, sizeof(c), b, "{preferred_rat}", pref);
    replace_token(cmd, sizeof(cmd), c, "{bands}", bands);
    command_result res;
    at_send(&state->db, &state->cfg, cmd, 1, &res);
    cellmgr_buf out;
    buf_init(&out, 512);
    buf_append(&out, "{");
    json_prop_string(&out, "command", cmd, 0);
    json_prop_int(&out, "exit_code", res.exit_code, 1);
    json_prop_string(&out, "raw", res.out ? res.out : "", 1);
    buf_append(&out, "}");
    response_json_body(fd, res.exit_code == 0, out.data, res.err);
    buf_free(&out);
    command_result_free(&res);
    free(mode); free(rat); free(pref); free(bands); free(tmpl);
}

static void api_network_lock_cell(int fd, app_state *state, const char *body)
{
    if (!json_get_int(body, "confirm", 0)) {
        response_json_body(fd, 0, NULL, "confirm is required");
        return;
    }
    char mode[32], rat[32], type[32], earfcn[32], pci[32];
    snprintf(mode, sizeof(mode), "%d", json_get_int(body, "mode", 1));
    snprintf(rat, sizeof(rat), "%d", json_get_int(body, "rat", 0));
    snprintf(type, sizeof(type), "%d", json_get_int(body, "type", 1));
    snprintf(earfcn, sizeof(earfcn), "%d", json_get_int(body, "earfcn", -1));
    snprintf(pci, sizeof(pci), "%d", json_get_int(body, "pci", -1));
    if (atoi(earfcn) < 0 || atoi(pci) < 0) {
        response_json_body(fd, 0, NULL, "earfcn and pci are required");
        return;
    }
    char *tmpl = network_template_command(state, "network.cell_lock", "set_template");
    if (!tmpl || tmpl[0] == '\0') {
        free(tmpl);
        response_json_body(fd, 0, NULL, "cell lock template not configured");
        return;
    }
    char a[512], b[512], c[512], d[512], e[512], cmd[512];
    replace_token(a, sizeof(a), tmpl, "{mode}", mode);
    replace_token(b, sizeof(b), a, "{rat}", rat);
    replace_token(c, sizeof(c), b, "{type}", type);
    replace_token(d, sizeof(d), c, "{earfcn}", earfcn);
    replace_token(e, sizeof(e), d, "{pci}", pci);
    snprintf(cmd, sizeof(cmd), "%s", e);
    command_result res;
    at_send(&state->db, &state->cfg, cmd, 1, &res);
    cellmgr_buf out;
    buf_init(&out, 512);
    buf_append(&out, "{");
    json_prop_string(&out, "command", cmd, 0);
    json_prop_int(&out, "exit_code", res.exit_code, 1);
    json_prop_string(&out, "raw", res.out ? res.out : "", 1);
    buf_append(&out, "}");
    response_json_body(fd, res.exit_code == 0, out.data, res.err);
    buf_free(&out);
    command_result_free(&res);
    free(tmpl);
}

static void api_network_lock_frequency(int fd, app_state *state, const char *body)
{
    if (!json_get_int(body, "confirm", 0)) {
        response_json_body(fd, 0, NULL, "confirm is required");
        return;
    }
    char mode[32], rat[32], earfcn[32];
    snprintf(mode, sizeof(mode), "%d", json_get_int(body, "mode", 1));
    snprintf(rat, sizeof(rat), "%d", json_get_int(body, "rat", 0));
    snprintf(earfcn, sizeof(earfcn), "%d", json_get_int(body, "earfcn", -1));
    if (atoi(earfcn) < 0) {
        response_json_body(fd, 0, NULL, "earfcn is required");
        return;
    }
    char *tmpl = network_template_command(state, "network.cell_lock", "frequency_template");
    if (!tmpl || tmpl[0] == '\0') {
        free(tmpl);
        tmpl = xstrdup("AT+GTCELLLOCK={mode},{rat},1,{earfcn}");
    }
    char a[512], b[512], cmd[512];
    replace_token(a, sizeof(a), tmpl, "{mode}", mode);
    replace_token(b, sizeof(b), a, "{rat}", rat);
    replace_token(cmd, sizeof(cmd), b, "{earfcn}", earfcn);
    command_result res;
    at_send(&state->db, &state->cfg, cmd, 1, &res);
    cellmgr_buf out;
    buf_init(&out, 512);
    buf_append(&out, "{");
    json_prop_string(&out, "command", cmd, 0);
    json_prop_int(&out, "exit_code", res.exit_code, 1);
    json_prop_string(&out, "raw", res.out ? res.out : "", 1);
    buf_append(&out, "}");
    response_json_body(fd, res.exit_code == 0, out.data, res.err);
    buf_free(&out);
    command_result_free(&res);
    free(tmpl);
}

static void api_network_unlock(int fd, app_state *state, const char *body)
{
    if (!json_get_int(body, "confirm", 0)) {
        response_json_body(fd, 0, NULL, "confirm is required");
        return;
    }
    char *cmd = network_template_command(state, "network.cell_lock", "unlock");
    if (!cmd || cmd[0] == '\0') {
        free(cmd);
        cmd = xstrdup("AT+GTCELLLOCK=0");
    }
    command_result res;
    at_send(&state->db, &state->cfg, cmd, 1, &res);
    cellmgr_buf out;
    buf_init(&out, 512);
    buf_append(&out, "{");
    json_prop_string(&out, "command", cmd, 0);
    json_prop_int(&out, "exit_code", res.exit_code, 1);
    json_prop_string(&out, "raw", res.out ? res.out : "", 1);
    buf_append(&out, "}");
    response_json_body(fd, res.exit_code == 0, out.data, res.err);
    buf_free(&out);
    command_result_free(&res);
    free(cmd);
}

static void route_request(int fd, app_state *state, http_request *req)
{
    if (!request_authorized(state, req)) {
        response_json_body(fd, 0, NULL, "unauthorized");
        return;
    }
    if (strcmp(req->path, "/") == 0) {
        response_raw(fd, 200, "text/html", web_index_html());
    } else if (strcmp(req->path, "/style.css") == 0) {
        response_raw(fd, 200, "text/css", web_style_css());
    } else if (strcmp(req->path, "/app.js") == 0) {
        response_raw(fd, 200, "application/javascript", web_app_js());
    } else if (strcmp(req->path, "/favicon.ico") == 0) {
        response_raw(fd, 200, "image/x-icon", "");
    } else if (strcmp(req->path, "/api/overview") == 0) {
        api_overview(fd, state);
    } else if (strcmp(req->path, "/api/auth/login") == 0) {
        api_auth_login(fd, state, req->body);
    } else if (strcmp(req->path, "/api/device/status") == 0) {
        api_device_status(fd, state);
    } else if (strcmp(req->path, "/api/device/drop-caches") == 0) {
        int ok = sysinfo_drop_caches() == 0;
        response_json_body(fd, ok, "{\"dropped\":true}", ok ? NULL : "drop caches failed");
    } else if (strcmp(req->path, "/api/modem/status") == 0) {
        api_modem_status(fd, state);
    } else if (strcmp(req->path, "/api/modem/radio") == 0) {
        api_modem_radio(fd, state);
    } else if (strcmp(req->path, "/api/modem/power") == 0) {
        api_modem_set_bool(fd, state, req->body, 1);
    } else if (strcmp(req->path, "/api/modem/online") == 0) {
        api_modem_set_bool(fd, state, req->body, 0);
    } else if (strcmp(req->path, "/api/at/send") == 0) {
        api_at_send(fd, state, req->body);
    } else if (strcmp(req->path, "/api/at/history") == 0) {
        api_at_history(fd, state, req->query);
    } else if (strcmp(req->path, "/api/sms/list") == 0) {
        api_sms_list(fd, state, req->query);
    } else if (strcmp(req->path, "/api/sms/read") == 0) {
        api_sms_read(fd, state, req->query);
    } else if (strcmp(req->path, "/api/sms/send") == 0) {
        api_sms_send(fd, state, req->body);
    } else if (strcmp(req->path, "/api/sms/delete") == 0) {
        api_sms_delete(fd, state, req->body);
    } else if (strcmp(req->path, "/api/sms/forward-test") == 0) {
        api_sms_forward_test(fd, state, req->body);
    } else if (strcmp(req->path, "/api/at/capabilities") == 0) {
        api_at_capabilities(fd, state);
    } else if (strcmp(req->path, "/api/at/test-capability") == 0) {
        api_at_test_capability(fd, state, req->body);
    } else if (strcmp(req->path, "/api/profiles") == 0) {
        api_profiles_list(fd, state);
    } else if (strcmp(req->path, "/api/profiles/get") == 0) {
        api_profiles_get(fd, state, req->query);
    } else if (strcmp(req->path, "/api/profiles/current") == 0) {
        api_profiles_current(fd, state);
    } else if (strcmp(req->path, "/api/profiles/save") == 0 || strcmp(req->path, "/api/profiles/import") == 0) {
        api_profiles_save(fd, state, req->body);
    } else if (strcmp(req->path, "/api/profiles/activate") == 0) {
        api_profiles_activate(fd, state, req->body);
    } else if (strcmp(req->path, "/api/profiles/delete") == 0) {
        api_profiles_delete(fd, state, req->body);
    } else if (strcmp(req->path, "/api/profiles/export") == 0) {
        api_profiles_export(fd, state, req->body);
    } else if (strcmp(req->path, "/api/profiles/validate") == 0) {
        api_profiles_validate(fd, req->body);
    } else if (strcmp(req->path, "/api/system/settings") == 0) {
        api_settings(fd, state);
    } else if (strcmp(req->path, "/api/system/settings-save") == 0) {
        api_system_settings_save(fd, state, req->body);
    } else if (strcmp(req->path, "/api/system/password") == 0) {
        api_system_password(fd, state, req->body);
    } else if (strcmp(req->path, "/api/system/reboot") == 0) {
        api_system_reboot(fd, req->body);
    } else if (strcmp(req->path, "/api/system/scheduled-reboot") == 0) {
        api_system_scheduled_reboot(fd, state, req->body);
    } else if (strcmp(req->path, "/api/system/dbus/commands") == 0) {
        api_dbus_commands(fd);
    } else if (strcmp(req->path, "/api/system/dbus/events") == 0) {
        api_dbus_events(fd, state, req->query);
    } else if (strcmp(req->path, "/api/system/dbus/test") == 0) {
        api_dbus_test(fd, state, req->body);
    } else if (strcmp(req->path, "/api/network/bands") == 0) {
        api_network_bands(fd, state, req->query);
    } else if (strcmp(req->path, "/api/network/cells") == 0) {
        api_network_cells(fd, state);
    } else if (strcmp(req->path, "/api/network/lock-band") == 0) {
        api_network_lock_band(fd, state, req->body);
    } else if (strcmp(req->path, "/api/network/lock-cell") == 0) {
        api_network_lock_cell(fd, state, req->body);
    } else if (strcmp(req->path, "/api/network/lock-frequency") == 0) {
        api_network_lock_frequency(fd, state, req->body);
    } else if (strcmp(req->path, "/api/network/unlock") == 0) {
        api_network_unlock(fd, state, req->body);
    } else {
        response_json_body(fd, 0, NULL, "not found");
    }
}

int http_serve(app_state *state)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        log_error("socket failed: %s", strerror(errno));
        return -1;
    }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)state->cfg.listen_port);
    if (inet_pton(AF_INET, state->cfg.listen_host, &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        log_error("bind %s:%d failed: %s", state->cfg.listen_host,
                  state->cfg.listen_port, strerror(errno));
        close(srv);
        return -1;
    }
    if (listen(srv, 16) != 0) {
        log_error("listen failed: %s", strerror(errno));
        close(srv);
        return -1;
    }
    log_info("cellmgrd listening on http://%s:%d", state->cfg.listen_host, state->cfg.listen_port);
    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_warn("accept failed: %s", strerror(errno));
            continue;
        }
        http_request req;
        if (parse_request(fd, &req) == 0) {
            route_request(fd, state, &req);
        } else {
            response_raw(fd, 400, "text/plain", "bad request");
        }
        close(fd);
    }
}
