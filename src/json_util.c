#include "json_util.h"

#include <ctype.h>

int json_escape(cellmgr_buf *buf, const char *s)
{
    if (buf_append(buf, "\"") != 0) {
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        switch (*p) {
        case '\"':
            if (buf_append(buf, "\\\"") != 0) return -1;
            break;
        case '\\':
            if (buf_append(buf, "\\\\") != 0) return -1;
            break;
        case '\b':
            if (buf_append(buf, "\\b") != 0) return -1;
            break;
        case '\f':
            if (buf_append(buf, "\\f") != 0) return -1;
            break;
        case '\n':
            if (buf_append(buf, "\\n") != 0) return -1;
            break;
        case '\r':
            if (buf_append(buf, "\\r") != 0) return -1;
            break;
        case '\t':
            if (buf_append(buf, "\\t") != 0) return -1;
            break;
        default:
            if (*p < 0x20) {
                if (buf_appendf(buf, "\\u%04x", *p) != 0) return -1;
            } else {
                if (buf_append_n(buf, (const char *)p, 1) != 0) return -1;
            }
            break;
        }
    }
    return buf_append(buf, "\"");
}

int json_prop_string(cellmgr_buf *buf, const char *name, const char *value, int comma)
{
    if (comma && buf_append(buf, ",") != 0) return -1;
    if (json_escape(buf, name) != 0) return -1;
    if (buf_append(buf, ":") != 0) return -1;
    return json_escape(buf, value ? value : "");
}

int json_prop_int(cellmgr_buf *buf, const char *name, long value, int comma)
{
    if (comma && buf_append(buf, ",") != 0) return -1;
    if (json_escape(buf, name) != 0) return -1;
    return buf_appendf(buf, ":%ld", value);
}

static const char *find_key_value(const char *json, const char *key)
{
    if (!json || !key) {
        return NULL;
    }
    char needle[160];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) {
        return NULL;
    }
    p += strlen(needle);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') {
        return NULL;
    }
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

char *json_get_string(const char *json, const char *key)
{
    const char *p = find_key_value(json, key);
    if (!p || *p != '"') {
        return NULL;
    }
    p++;
    cellmgr_buf out;
    if (buf_init(&out, 64) != 0) {
        return NULL;
    }
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': buf_append_n(&out, "\n", 1); break;
            case 'r': buf_append_n(&out, "\r", 1); break;
            case 't': buf_append_n(&out, "\t", 1); break;
            default: buf_append_n(&out, p, 1); break;
            }
        } else {
            buf_append_n(&out, p, 1);
        }
        p++;
    }
    return out.data;
}

int json_get_int(const char *json, const char *key, int fallback)
{
    const char *p = find_key_value(json, key);
    if (!p) {
        return fallback;
    }
    return atoi(p);
}
