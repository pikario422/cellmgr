#include "parsers.h"
#include "json_util.h"

#include <ctype.h>

static const char *find_line_prefix(const char *raw, const char *prefix)
{
    const char *p = raw;
    size_t n = strlen(prefix);
    while (p && *p) {
        while (*p == '\r' || *p == '\n') p++;
        if (strncmp(p, prefix, n) == 0) {
            return p + n;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return NULL;
}

static char *extract_quoted_after(const char *p)
{
    p = strchr(p, '"');
    if (!p) {
        return NULL;
    }
    p++;
    cellmgr_buf out;
    if (buf_init(&out, 64) != 0) {
        return NULL;
    }
    int escaped = 0;
    for (; *p; p++) {
        if (escaped) {
            switch (*p) {
            case 'n': buf_append_n(&out, "\n", 1); break;
            case 'r': buf_append_n(&out, "\r", 1); break;
            case 't': buf_append_n(&out, "\t", 1); break;
            case '"': buf_append_n(&out, "\"", 1); break;
            case '\\': buf_append_n(&out, "\\", 1); break;
            default: buf_append_n(&out, p, 1); break;
            }
            escaped = 0;
            continue;
        }
        if (*p == '\\') {
            escaped = 1;
            continue;
        }
        if (*p == '"') {
            break;
        }
        buf_append_n(&out, p, 1);
    }
    return out.data;
}

static char *normalized_at_payload(const char *raw)
{
    if (!raw) {
        return xstrdup("");
    }
    const char *p = strstr(raw, "string ");
    if (!p) {
        return xstrdup(raw);
    }
    char *s = extract_quoted_after(p);
    return s ? s : xstrdup(raw);
}

static char *json_raw_with_parser(const char *parser, const char *raw)
{
    char *payload = normalized_at_payload(raw);
    cellmgr_buf b;
    buf_init(&b, 512);
    buf_append(&b, "{");
    json_prop_string(&b, "parser", parser ? parser : "raw", 0);
    json_prop_string(&b, "raw", payload ? payload : "", 1);
    buf_append(&b, "}");
    free(payload);
    return b.data;
}

static char *parse_csq(const char *raw)
{
    char *payload = normalized_at_payload(raw);
    int rssi = -1;
    int ber = -1;
    const char *p = find_line_prefix(payload, "+CSQ:");
    if (p) {
        sscanf(p, " %d,%d", &rssi, &ber);
    }
    int dbm = (rssi >= 0 && rssi <= 31) ? -113 + 2 * rssi : 0;
    cellmgr_buf b;
    buf_init(&b, 256);
    buf_append(&b, "{");
    json_prop_int(&b, "rssi", rssi, 0);
    json_prop_int(&b, "ber", ber, 1);
    if (dbm != 0) {
        json_prop_int(&b, "rssi_dbm", dbm, 1);
    } else {
        buf_append(&b, ",\"rssi_dbm\":null");
    }
    json_prop_string(&b, "raw", payload ? payload : "", 1);
    buf_append(&b, "}");
    free(payload);
    return b.data;
}

static char *parse_cops(const char *raw)
{
    char *payload = normalized_at_payload(raw);
    int mode = -1, format = -1, act = -1;
    char oper[128] = "";
    const char *p = find_line_prefix(payload, "+COPS:");
    if (p) {
        sscanf(p, " %d,%d,\"%127[^\"]\",%d", &mode, &format, oper, &act);
    }
    cellmgr_buf b;
    buf_init(&b, 256);
    buf_append(&b, "{");
    json_prop_int(&b, "mode", mode, 0);
    json_prop_int(&b, "format", format, 1);
    json_prop_string(&b, "operator", oper, 1);
    json_prop_int(&b, "act", act, 1);
    json_prop_string(&b, "raw", payload ? payload : "", 1);
    buf_append(&b, "}");
    free(payload);
    return b.data;
}

static char *parse_registration(const char *raw, const char *prefix)
{
    char *payload = normalized_at_payload(raw);
    int n = -1, stat = -1, act = -1;
    char area[64] = "";
    char cell[64] = "";
    const char *p = find_line_prefix(payload, prefix);
    if (p) {
        sscanf(p, " %d,%d,\"%63[^\"]\",\"%63[^\"]\",%d", &n, &stat, area, cell, &act);
        if (stat < 0) {
            sscanf(p, " %d,%d", &n, &stat);
        }
    }
    cellmgr_buf b;
    buf_init(&b, 320);
    buf_append(&b, "{");
    json_prop_int(&b, "n", n, 0);
    json_prop_int(&b, "stat", stat, 1);
    json_prop_string(&b, "area", area, 1);
    json_prop_string(&b, "cell", cell, 1);
    json_prop_int(&b, "act", act, 1);
    json_prop_string(&b, "raw", payload ? payload : "", 1);
    buf_append(&b, "}");
    free(payload);
    return b.data;
}

static void append_fibocom_band_items(cellmgr_buf *b, const char *raw)
{
    buf_append(b, "[");
    int first = 1;
    int seen[256];
    int seen_count = 0;
    const char *p = raw;
    while ((p = strstr(p, "BAND_")) != NULL) {
        char rat[16] = "";
        int band = 0;
        if (sscanf(p, "BAND_%15[^_]_%d", rat, &band) == 2) {
            int code = strcmp(rat, "NR") == 0 ? 5000 + band : 100 + band;
            if (seen_count < (int)(sizeof(seen) / sizeof(seen[0]))) seen[seen_count++] = code;
            if (!first) buf_append(b, ",");
            first = 0;
            buf_append(b, "{");
            json_prop_string(b, "rat", rat, 0);
            json_prop_int(b, "code", code, 1);
            json_prop_int(b, "band", band, 1);
            buf_append(b, ",\"label\":");
            if (strcmp(rat, "LTE") == 0) {
                buf_appendf(b, "\"B%d\"", band);
            } else if (strcmp(rat, "NR") == 0) {
                buf_appendf(b, "\"n%d\"", band);
            } else {
                json_escape(b, p);
            }
            buf_append(b, "}");
        }
        p += 5;
    }
    p = raw;
    while (*p) {
        if (!isdigit((unsigned char)*p)) {
            p++;
            continue;
        }
        int code = atoi(p);
        while (isdigit((unsigned char)*p)) p++;
        if (code < 100) {
            continue;
        }
        int exists = 0;
        for (int i = 0; i < seen_count; i++) {
            if (seen[i] == code) {
                exists = 1;
                break;
            }
        }
        if (exists) {
            continue;
        }
        if (seen_count < (int)(sizeof(seen) / sizeof(seen[0]))) seen[seen_count++] = code;
        const char *rat = code >= 5000 ? "NR" : "LTE";
        int band = code >= 5000 ? code - 5000 : code - 100;
        if (!first) buf_append(b, ",");
        first = 0;
        buf_append(b, "{");
        json_prop_string(b, "rat", rat, 0);
        json_prop_int(b, "code", code, 1);
        json_prop_int(b, "band", band, 1);
        buf_append(b, ",\"label\":");
        if (code >= 5000) {
            buf_appendf(b, "\"n%d\"", band);
        } else {
            buf_appendf(b, "\"B%d\"", band);
        }
        buf_append(b, "}");
    }
    buf_append(b, "]");
}

static char *parse_gtact(const char *raw)
{
    char *payload = normalized_at_payload(raw);
    cellmgr_buf b;
    buf_init(&b, 1024);
    buf_append(&b, "{\"bands\":");
    append_fibocom_band_items(&b, payload ? payload : "");
    buf_append(&b, ",\"raw\":");
    json_escape(&b, payload ? payload : "");
    buf_append(&b, "}");
    free(payload);
    return b.data;
}

static char *parse_gtcelllock(const char *raw)
{
    char *payload = normalized_at_payload(raw);
    const char *p = find_line_prefix(payload, "+GTCELLLOCK:");
    int mode = -1, rat = -1, type = -1, earfcn = -1, pci = -1;
    if (p) {
        sscanf(p, " %d,%d,%d,%d,%d", &mode, &rat, &type, &earfcn, &pci);
    }
    cellmgr_buf b;
    buf_init(&b, 320);
    buf_append(&b, "{");
    json_prop_int(&b, "mode", mode, 0);
    json_prop_int(&b, "rat", rat, 1);
    json_prop_int(&b, "type", type, 1);
    json_prop_int(&b, "earfcn", earfcn, 1);
    json_prop_int(&b, "pci", pci, 1);
    json_prop_string(&b, "raw", payload ? payload : "", 1);
    buf_append(&b, "}");
    free(payload);
    return b.data;
}

static char *parse_cgeqos(const char *raw)
{
    char *payload = normalized_at_payload(raw);
    cellmgr_buf b;
    buf_init(&b, 768);
    buf_append(&b, "{\"items\":[");
    const char *p = payload;
    int first = 1;
    while ((p = strstr(p, "+CGEQOS:")) != NULL) {
        int cid = -1, qci = -1;
        sscanf(p + 8, " %d,%d", &cid, &qci);
        if (!first) buf_append(&b, ",");
        first = 0;
        buf_append(&b, "{");
        json_prop_int(&b, "cid", cid, 0);
        json_prop_int(&b, "qci", qci, 1);
        buf_append(&b, "}");
        p += 8;
    }
    buf_append(&b, "],\"raw\":");
    json_escape(&b, payload ? payload : "");
    buf_append(&b, "}");
    free(payload);
    return b.data;
}

static char *parse_gtccinfo(const char *raw)
{
    char *payload = normalized_at_payload(raw);
    char *copy = xstrdup(payload ? payload : "");
    cellmgr_buf b;
    buf_init(&b, 1536);
    buf_append(&b, "{\"items\":[");
    int first = 1;
    char *save = NULL;
    char *line = strtok_r(copy, "\r\n", &save);
    while (line) {
        trim_in_place(line);
        if (line[0] == '\0' || strcmp(line, "OK") == 0 || strstr(line, "+GTCCINFO:") == line) {
            line = strtok_r(NULL, "\r\n", &save);
            continue;
        }
        if (!strchr(line, ',')) {
            line = strtok_r(NULL, "\r\n", &save);
            continue;
        }
        char row[512];
        snprintf(row, sizeof(row), "%s", line);
        char *fields[24] = {0};
        int n = 0;
        char *fsave = NULL;
        char *f = strtok_r(row, ",", &fsave);
        while (f && n < 24) {
            trim_in_place(f);
            fields[n++] = f;
            f = strtok_r(NULL, ",", &fsave);
        }
        if (n < 8) {
            line = strtok_r(NULL, "\r\n", &save);
            continue;
        }
        if (!first) buf_append(&b, ",");
        first = 0;
        buf_append(&b, "{");
        json_prop_int(&b, "service", atoi(fields[0]), 0);
        json_prop_string(&b, "rat", fields[1], 1);
        json_prop_string(&b, "mcc", n > 2 ? fields[2] : "", 1);
        json_prop_string(&b, "mnc", n > 3 ? fields[3] : "", 1);
        json_prop_string(&b, "area", n > 4 ? fields[4] : "", 1);
        json_prop_string(&b, "cell", n > 5 ? fields[5] : "", 1);
        json_prop_int(&b, "earfcn", atoi(fields[6]), 1);
        json_prop_int(&b, "pci", atoi(fields[7]), 1);
        json_prop_string(&b, "band", n > 8 ? fields[8] : "", 1);
        json_prop_string(&b, "sinr", n > 10 ? fields[10] : "", 1);
        json_prop_string(&b, "rsrp", n > 12 ? fields[12] : "", 1);
        json_prop_string(&b, "rsrq", n > 13 ? fields[13] : "", 1);
        json_prop_string(&b, "raw", line, 1);
        buf_append(&b, "}");
        line = strtok_r(NULL, "\r\n", &save);
    }
    buf_append(&b, "],\"raw\":");
    json_escape(&b, payload ? payload : "");
    buf_append(&b, "}");
    free(copy);
    free(payload);
    return b.data;
}

static char *parse_sms_list_or_read(const char *parser, const char *raw)
{
    char *payload = normalized_at_payload(raw);
    char *copy = xstrdup(payload ? payload : "");
    cellmgr_buf b;
    buf_init(&b, 1024);
    buf_append(&b, "{");
    json_prop_string(&b, "parser", parser, 0);
    if (strcmp(parser, "cmgl") == 0) {
        buf_append(&b, ",\"items\":[");
        int first = 1;
        char *save = NULL;
        char *line = strtok_r(copy, "\r\n", &save);
        while (line) {
            if (strncmp(line, "+CMGL:", 6) == 0) {
                int idx = 0;
                char status[64] = "", sender[96] = "", alpha[64] = "", ts[64] = "";
                sscanf(line + 6, " %d,\"%63[^\"]\",\"%95[^\"]\",\"%63[^\"]\",\"%63[^\"]\"",
                       &idx, status, sender, alpha, ts);
                char body[512] = "";
                char *next = strtok_r(NULL, "\r\n", &save);
                while (next && strncmp(next, "+CMGL:", 6) != 0 && strcmp(next, "OK") != 0) {
                    if (body[0]) strncat(body, "\n", sizeof(body) - strlen(body) - 1);
                    strncat(body, next, sizeof(body) - strlen(body) - 1);
                    next = strtok_r(NULL, "\r\n", &save);
                }
                if (!first) buf_append(&b, ",");
                first = 0;
                buf_append(&b, "{");
                json_prop_int(&b, "index", idx, 0);
                json_prop_string(&b, "status", status, 1);
                json_prop_string(&b, "sender", sender, 1);
                json_prop_string(&b, "timestamp", ts, 1);
                json_prop_string(&b, "body", body, 1);
                buf_append(&b, "}");
                line = next;
                continue;
            }
            line = strtok_r(NULL, "\r\n", &save);
        }
        buf_append(&b, "]");
    } else {
        char *line = copy;
        while (line && *line && strncmp(line, "+CMGR:", 6) != 0) {
            line = strpbrk(line, "\r\n");
            while (line && (*line == '\r' || *line == '\n')) line++;
        }
        char status[64] = "", sender[96] = "", alpha[64] = "", ts[64] = "";
        char body[512] = "";
        if (line && strncmp(line, "+CMGR:", 6) == 0) {
            sscanf(line + 6, " \"%63[^\"]\",\"%95[^\"]\",\"%63[^\"]\",\"%63[^\"]\"",
                   status, sender, alpha, ts);
            char *body_start = strpbrk(line, "\r\n");
            while (body_start && (*body_start == '\r' || *body_start == '\n')) body_start++;
            if (body_start) {
                char *ok = strstr(body_start, "\nOK");
                size_t len = ok ? (size_t)(ok - body_start) : strlen(body_start);
                if (len >= sizeof(body)) len = sizeof(body) - 1;
                memcpy(body, body_start, len);
                body[len] = '\0';
                trim_in_place(body);
            }
        }
        buf_append(&b, ",\"message\":{");
        json_prop_string(&b, "status", status, 0);
        json_prop_string(&b, "sender", sender, 1);
        json_prop_string(&b, "timestamp", ts, 1);
        json_prop_string(&b, "body", body, 1);
        buf_append(&b, "}");
    }
    json_prop_string(&b, "raw", payload ? payload : "", 1);
    buf_append(&b, "}");
    free(copy);
    free(payload);
    return b.data;
}

char *parse_at_response_json(const char *parser, const char *raw)
{
    if (!parser || strcmp(parser, "raw") == 0 || strcmp(parser, "ok_error") == 0) {
        return json_raw_with_parser(parser, raw);
    }
    if (strcmp(parser, "csq") == 0) return parse_csq(raw);
    if (strcmp(parser, "cops") == 0) return parse_cops(raw);
    if (strcmp(parser, "creg") == 0) return parse_registration(raw, "+CREG:");
    if (strcmp(parser, "cgreg") == 0) return parse_registration(raw, "+CGREG:");
    if (strcmp(parser, "cereg") == 0) return parse_registration(raw, "+CEREG:");
    if (strcmp(parser, "c5greg") == 0) return parse_registration(raw, "+C5GREG:");
    if (strcmp(parser, "gtact") == 0) return parse_gtact(raw);
    if (strcmp(parser, "gtcelllock") == 0) return parse_gtcelllock(raw);
    if (strcmp(parser, "gtccinfo") == 0) return parse_gtccinfo(raw);
    if (strcmp(parser, "cgeqos") == 0) return parse_cgeqos(raw);
    if (strcmp(parser, "cmgl") == 0 || strcmp(parser, "cmgr") == 0) {
        return parse_sms_list_or_read(parser, raw);
    }
    return json_raw_with_parser(parser, raw);
}

char *parse_dbus_properties_json(const char *raw)
{
    cellmgr_buf b;
    buf_init(&b, 1024);
    buf_append(&b, "{\"properties\":{");
    int first = 1;
    const char *p = raw ? raw : "";
    while ((p = strstr(p, "dict entry(")) != NULL) {
        const char *entry_end = strstr(p + 11, "dict entry(");
        const char *key_mark = strstr(p, "string ");
        if (!key_mark || (entry_end && key_mark > entry_end)) {
            p += 11;
            continue;
        }
        char *key = extract_quoted_after(key_mark);
        const char *variant = strstr(key_mark, "variant");
        if (!key || !variant || (entry_end && variant > entry_end)) {
            free(key);
            p += 11;
            continue;
        }
        char *value = NULL;
        const char *v = variant + 7;
        while (*v && isspace((unsigned char)*v)) v++;
        if (strncmp(v, "string ", 7) == 0 || strncmp(v, "object path ", 12) == 0) {
            value = extract_quoted_after(v);
        } else if (strncmp(v, "boolean ", 8) == 0) {
            v += 8;
            value = xstrdup(strncmp(v, "true", 4) == 0 ? "true" : "false");
        } else {
            const char *line = v;
            while (*line && isspace((unsigned char)*line)) line++;
            while (*line && !isspace((unsigned char)*line)) line++;
            while (*line && isspace((unsigned char)*line)) line++;
            char tmp[96] = "";
            size_t n = 0;
            while (line[n] && !isspace((unsigned char)line[n]) && line[n] != ')' && n + 1 < sizeof(tmp)) {
                tmp[n] = line[n];
                n++;
            }
            tmp[n] = '\0';
            value = xstrdup(tmp[0] ? tmp : "");
        }
        if (key && key[0] && value) {
            json_prop_string(&b, key, value, !first);
            first = 0;
        }
        free(key);
        free(value);
        p += 11;
    }
    buf_append(&b, "},\"raw\":");
    json_escape(&b, raw ? raw : "");
    buf_append(&b, "}");
    return b.data;
}

static void append_dbus_variant_json(cellmgr_buf *b, const char *line)
{
    const char *p = strstr(line, "variant");
    if (!p) {
        buf_append(b, "null");
        return;
    }
    p += 7;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "string", 6) == 0 || strncmp(p, "object path", 11) == 0) {
        p = strchr(p, '"');
        if (!p) {
            buf_append(b, "\"\"");
            return;
        }
        char *s = extract_quoted_after(p);
        if (!s) {
            buf_append(b, "\"\"");
            return;
        }
        json_escape(b, s);
        free(s);
        return;
    }
    if (strncmp(p, "boolean", 7) == 0) {
        p += 7;
        while (*p && isspace((unsigned char)*p)) p++;
        buf_append(b, (strncmp(p, "true", 4) == 0) ? "true" : "false");
        return;
    }
    while (*p && !isspace((unsigned char)*p)) p++;
    while (*p && isspace((unsigned char)*p)) p++;
    char token[128];
    size_t n = 0;
    while (p[n] && !isspace((unsigned char)p[n]) && p[n] != ')' && n + 1 < sizeof(token)) {
        token[n] = p[n];
        n++;
    }
    token[n] = '\0';
    if (token[0] == '\0') {
        buf_append(b, "null");
        return;
    }
    if (strcmp(token, "true") == 0 || strcmp(token, "false") == 0) {
        buf_append(b, token);
        return;
    }
    buf_append(b, token);
}

char *parse_dbus_messages_json(const char *raw)
{
    char *copy = xstrdup(raw ? raw : "");
    cellmgr_buf out;
    cellmgr_buf item;
    buf_init(&out, 2048);
    buf_init(&item, 1024);
    buf_append(&out, "{\"items\":[");
    int have_item = 0;
    int first_item = 1;
    int first_prop = 1;
    int index = 0;
    char *current_key = NULL;
    char *save = NULL;
    char *line = strtok_r(copy, "\r\n", &save);
    while (line) {
        trim_in_place(line);
        if (strncmp(line, "object path ", 12) == 0) {
            if (have_item) {
                buf_append(&item, "}}");
                if (!first_item) buf_append(&out, ",");
                first_item = 0;
                buf_append(&out, item.data);
                buf_free(&item);
                buf_init(&item, 1024);
                free(current_key);
                current_key = NULL;
                have_item = 0;
            }
            char *path = extract_quoted_after(line);
            if (!path) {
                line = strtok_r(NULL, "\r\n", &save);
                continue;
            }
            index++;
            buf_append(&item, "{\"index\":");
            buf_appendf(&item, "%d", index);
            buf_append(&item, ",\"path\":");
            json_escape(&item, path);
            buf_append(&item, ",\"properties\":{");
            have_item = 1;
            first_prop = 1;
            free(path);
            line = strtok_r(NULL, "\r\n", &save);
            continue;
        }
        if (!have_item) {
            line = strtok_r(NULL, "\r\n", &save);
            continue;
        }
        if (strncmp(line, "string ", 7) == 0 && !current_key) {
            current_key = extract_quoted_after(line);
            line = strtok_r(NULL, "\r\n", &save);
            continue;
        }
        if (strncmp(line, "variant", 7) == 0 && current_key) {
            if (!first_prop) buf_append(&item, ",");
            first_prop = 0;
            json_escape(&item, current_key);
            buf_append(&item, ":");
            append_dbus_variant_json(&item, line);
            free(current_key);
            current_key = NULL;
            line = strtok_r(NULL, "\r\n", &save);
            continue;
        }
        if (strcmp(line, ")") == 0 || strcmp(line, "]") == 0) {
            free(current_key);
            current_key = NULL;
        }
        line = strtok_r(NULL, "\r\n", &save);
    }
    if (have_item) {
        buf_append(&item, "}}");
        if (!first_item) buf_append(&out, ",");
        buf_append(&out, item.data);
    }
    buf_append(&out, "],\"raw\":");
    json_escape(&out, raw ? raw : "");
    buf_append(&out, "}");
    free(current_key);
    buf_free(&item);
    free(copy);
    return out.data;
}
