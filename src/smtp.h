#ifndef CELLMGR_SMTP_H
#define CELLMGR_SMTP_H

#include "common.h"

typedef struct smtp_config {
    char host[128];
    int port;
    char user[128];
    char pass[128];
    char from[128];
    char to[128];
} smtp_config;

int smtp_send_plain(const smtp_config *cfg, const char *subject, const char *body,
                    char **out_log);

#endif
