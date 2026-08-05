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

static char *json_raw_with_parser(const char *parser, const char *raw)
{
    cellmgr_buf b;
    buf_init(&b, 512);
    buf_append(&b, "{");
    json_prop_string(&b, "parser", parser ? parser : "raw", 0);
    json_prop_string(&b, "raw", raw ? raw : "", 1);
    buf_append(&b, "}");
    return b.data;
}

static char *parse_csq(const char *raw)
{
    int rssi = -1;
    int ber = -1;
    const char *p = find_line_prefix(raw, "+CSQ:");
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
    json_prop_string(&b, "raw", raw ? raw : "", 1);
    buf_append(&b, "}");
    return b.data;
}

static char *parse_cops(const char *raw)
{
    int mode = -1, format = -1, act = -1;
    char oper[128] = "";
    const char *p = find_line_prefix(raw, "+COPS:");
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
    json_prop_string(&b, "raw", raw ? raw : "", 1);
    buf_append(&b, "}");
    return b.data;
}

static char *parse_registration(const char *raw, const char *prefix)
{
    int n = -1, stat = -1, act = -1;
    char area[64] = "";
    char cell[64] = "";
    const char *p = find_line_prefix(raw, prefix);
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
    json_prop_string(&b, "raw", raw ? raw : "", 1);
    buf_append(&b, "}");
    return b.data;
}

static void append_fibocom_band_items(cellmgr_buf *b, const char *raw)
{
    buf_append(b, "[");
    int first = 1;
    const char *p = raw;
    while ((p = strstr(p, "BAND_")) != NULL) {
        char rat[16] = "";
        int band = 0;
        if (sscanf(p, "BAND_%15[^_]_%d", rat, &band) == 2) {
            if (!first) buf_append(b, ",");
            first = 0;
            buf_append(b, "{");
            json_prop_string(b, "rat", rat, 0);
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
    buf_append(b, "]");
}

static char *parse_gtact(const char *raw)
{
    cellmgr_buf b;
    buf_init(&b, 1024);
    buf_append(&b, "{\"bands\":");
    append_fibocom_band_items(&b, raw ? raw : "");
    buf_append(&b, ",\"raw\":");
    json_escape(&b, raw ? raw : "");
    buf_append(&b, "}");
    return b.data;
}

static char *parse_gtcelllock(const char *raw)
{
    const char *p = find_line_prefix(raw, "+GTCELLLOCK:");
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
    json_prop_string(&b, "raw", raw ? raw : "", 1);
    buf_append(&b, "}");
    return b.data;
}

static char *parse_cgeqos(const char *raw)
{
    cellmgr_buf b;
    buf_init(&b, 768);
    buf_append(&b, "{\"items\":[");
    const char *p = raw;
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
    json_escape(&b, raw ? raw : "");
    buf_append(&b, "}");
    return b.data;
}

static char *parse_sms_list_or_read(const char *parser, const char *raw)
{
    cellmgr_buf b;
    buf_init(&b, 1024);
    buf_append(&b, "{");
    json_prop_string(&b, "parser", parser, 0);
    json_prop_string(&b, "raw", raw ? raw : "", 1);
    buf_append(&b, "}");
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
    if (strcmp(parser, "cgeqos") == 0) return parse_cgeqos(raw);
    if (strcmp(parser, "cmgl") == 0 || strcmp(parser, "cmgr") == 0) {
        return parse_sms_list_or_read(parser, raw);
    }
    return json_raw_with_parser(parser, raw);
}

char *parse_dbus_properties_json(const char *raw)
{
    cellmgr_buf b;
    buf_init(&b, 512);
    buf_append(&b, "{");
    json_prop_string(&b, "raw", raw ? raw : "", 0);
    buf_append(&b, "}");
    return b.data;
}
