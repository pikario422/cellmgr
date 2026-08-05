#include "common.h"

#include <stdarg.h>

static void vlog_line(const char *level, const char *fmt, va_list ap)
{
    time_t t = time(NULL);
    struct tm tmv;
    char ts[32];
    localtime_r(&t, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(stderr, "[%s] %s ", ts, level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_line("INFO", fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_line("WARN", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_line("ERROR", fmt, ap);
    va_end(ap);
}

static int buf_reserve(cellmgr_buf *buf, size_t need)
{
    if (need <= buf->cap) {
        return 0;
    }
    size_t next = buf->cap ? buf->cap : 256;
    while (next < need) {
        next *= 2;
    }
    char *p = realloc(buf->data, next);
    if (!p) {
        return -1;
    }
    buf->data = p;
    buf->cap = next;
    return 0;
}

int buf_init(cellmgr_buf *buf, size_t initial_cap)
{
    memset(buf, 0, sizeof(*buf));
    if (initial_cap == 0) {
        initial_cap = 256;
    }
    buf->data = malloc(initial_cap);
    if (!buf->data) {
        return -1;
    }
    buf->data[0] = '\0';
    buf->cap = initial_cap;
    return 0;
}

void buf_free(cellmgr_buf *buf)
{
    free(buf->data);
    memset(buf, 0, sizeof(*buf));
}

int buf_append_n(cellmgr_buf *buf, const char *s, size_t n)
{
    if (buf_reserve(buf, buf->len + n + 1) != 0) {
        return -1;
    }
    memcpy(buf->data + buf->len, s, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return 0;
}

int buf_append(cellmgr_buf *buf, const char *s)
{
    return buf_append_n(buf, s, strlen(s));
}

int buf_appendf(cellmgr_buf *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return -1;
    }
    if (buf_reserve(buf, buf->len + (size_t)n + 1) != 0) {
        va_end(ap2);
        return -1;
    }
    vsnprintf(buf->data + buf->len, buf->cap - buf->len, fmt, ap2);
    va_end(ap2);
    buf->len += (size_t)n;
    return 0;
}

char *read_text_file(const char *path, size_t max_bytes, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz < 0 || (max_bytes > 0 && (size_t)sz > max_bytes)) {
        fclose(fp);
        errno = EFBIG;
        return NULL;
    }
    rewind(fp);
    char *data = malloc((size_t)sz + 1);
    if (!data) {
        fclose(fp);
        return NULL;
    }
    size_t got = fread(data, 1, (size_t)sz, fp);
    fclose(fp);
    data[got] = '\0';
    if (out_len) {
        *out_len = got;
    }
    return data;
}

int write_text_file(const char *path, const char *data)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return -1;
    }
    size_t len = strlen(data);
    int ok = fwrite(data, 1, len, fp) == len ? 0 : -1;
    fclose(fp);
    return ok;
}

char *xstrdup(const char *s)
{
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *p = malloc(len + 1);
    if (!p) {
        return NULL;
    }
    memcpy(p, s, len + 1);
    return p;
}

void trim_in_place(char *s)
{
    if (!s) {
        return;
    }
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t len = strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        s[--len] = '\0';
    }
}

long now_unix(void)
{
    return (long)time(NULL);
}
