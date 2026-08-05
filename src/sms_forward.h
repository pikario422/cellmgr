#ifndef CELLMGR_SMS_FORWARD_H
#define CELLMGR_SMS_FORWARD_H

#include "config.h"

int sms_forward_send(const app_config *cfg, const char *from, const char *text,
                     char **out_log);

#endif
