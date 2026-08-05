#ifndef CELLMGR_HTTP_H
#define CELLMGR_HTTP_H

#include "config.h"
#include "db.h"

typedef struct app_state {
    app_config cfg;
    app_db db;
    char session_token[64];
} app_state;

int http_serve(app_state *state);

#endif
