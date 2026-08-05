#ifndef CELLMGR_DBUS_CMD_H
#define CELLMGR_DBUS_CMD_H

#include "common.h"

typedef struct command_result {
    int exit_code;
    char *out;
    char *err;
} command_result;

void command_result_free(command_result *res);
int run_capture(char *const argv[], int timeout_ms, size_t max_output, command_result *res);
int dbus_send_call(const char *destination, const char *path, const char *interface_method,
                   const char *const *args, int arg_count, int timeout_ms,
                   command_result *res);

#endif
