#include "at.h"
#include "ofono.h"

#include <ctype.h>

static int starts_with_ci(const char *s, const char *prefix)
{
    while (*prefix) {
        if (toupper((unsigned char)*s) != toupper((unsigned char)*prefix)) {
            return 0;
        }
        s++;
        prefix++;
    }
    return 1;
}

int at_is_safe_command(const char *command)
{
    if (!command) {
        return 0;
    }
    while (*command == ' ' || *command == '\t') {
        command++;
    }
    if (!starts_with_ci(command, "AT")) {
        return 0;
    }
    const char *danger[] = {
        "AT+CFUN=1,1",
        "AT+CPWROFF",
        "AT+GTACT=",
        "AT+GTCELLLOCK=",
        "AT+CMGD=",
        "AT+CGDCONT=",
        "AT+CGACT=",
        "AT+CGEQOS=",
        NULL
    };
    for (int i = 0; danger[i]; i++) {
        if (starts_with_ci(command, danger[i])) {
            return 0;
        }
    }
    return 1;
}

int at_send(app_db *db, const app_config *cfg, const char *command, int allow_dangerous,
            command_result *res)
{
    if (!allow_dangerous && !at_is_safe_command(command)) {
        memset(res, 0, sizeof(*res));
        res->exit_code = 403;
        res->out = xstrdup("");
        res->err = xstrdup("dangerous or invalid AT command blocked");
        return -1;
    }
    int ok = ofono_send_at(cfg, command, res);
    if (db && res->out) {
        db_insert_at_history(db, command, res->out, res->exit_code);
    }
    return ok;
}
