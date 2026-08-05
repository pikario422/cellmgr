#ifndef CELLMGR_PARSERS_H
#define CELLMGR_PARSERS_H

#include "common.h"

char *parse_at_response_json(const char *parser, const char *raw);
char *parse_dbus_properties_json(const char *raw);
char *parse_dbus_messages_json(const char *raw);

#endif
