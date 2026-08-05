#ifndef CELLMGR_COMMON_H
#define CELLMGR_COMMON_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CELLMGR_VERSION "0.1.0"
#define CELLMGR_DEFAULT_HOST "0.0.0.0"
#define CELLMGR_DEFAULT_PORT 4242
#define CELLMGR_DEFAULT_DB "./data/cellmgr.db"
#define CELLMGR_DEFAULT_CONFIG "./config/cellmgrd.conf"
#define CELLMGR_DEFAULT_PROFILE "./profiles/fm650.json"
#define CELLMGR_MAX_BODY (64 * 1024)
#define CELLMGR_MAX_AT_RESPONSE 8192

typedef struct cellmgr_buf {
    char *data;
    size_t len;
    size_t cap;
} cellmgr_buf;

void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_error(const char *fmt, ...);

int buf_init(cellmgr_buf *buf, size_t initial_cap);
void buf_free(cellmgr_buf *buf);
int buf_append(cellmgr_buf *buf, const char *s);
int buf_append_n(cellmgr_buf *buf, const char *s, size_t n);
int buf_appendf(cellmgr_buf *buf, const char *fmt, ...);

char *read_text_file(const char *path, size_t max_bytes, size_t *out_len);
int write_text_file(const char *path, const char *data);
char *xstrdup(const char *s);
void trim_in_place(char *s);
long now_unix(void);

#endif
