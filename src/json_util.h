#ifndef CELLMGR_JSON_UTIL_H
#define CELLMGR_JSON_UTIL_H

#include "common.h"

int json_escape(cellmgr_buf *buf, const char *s);
int json_prop_string(cellmgr_buf *buf, const char *name, const char *value, int comma);
int json_prop_int(cellmgr_buf *buf, const char *name, long value, int comma);
char *json_get_string(const char *json, const char *key);
int json_get_int(const char *json, const char *key, int fallback);

#endif
