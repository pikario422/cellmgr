#include "smtp.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <unistd.h>

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const char *s)
{
    size_t len = strlen(s ? s : "");
    size_t out_len = ((len + 2) / 3) * 4;
    char *out = malloc(out_len + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned int v = (unsigned char)s[i] << 16;
        if (i + 1 < len) v |= (unsigned char)s[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned char)s[i + 2];
        out[j++] = b64_table[(v >> 18) & 63];
        out[j++] = b64_table[(v >> 12) & 63];
        out[j++] = (i + 1 < len) ? b64_table[(v >> 6) & 63] : '=';
        out[j++] = (i + 2 < len) ? b64_table[v & 63] : '=';
    }
    out[j] = '\0';
    return out;
}

static int smtp_read_line(int fd, cellmgr_buf *log)
{
    char c;
    int code = 0;
    char line[512];
    size_t n = 0;
    while (read(fd, &c, 1) == 1) {
        if (n + 1 < sizeof(line)) line[n++] = c;
        if (c == '\n') break;
    }
    line[n] = '\0';
    buf_append(log, line);
    sscanf(line, "%d", &code);
    return code;
}

static int smtp_writef(int fd, cellmgr_buf *log, const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    buf_append(log, "C: ");
    buf_append(log, line);
    size_t len = strlen(line);
    return write(fd, line, len) == (ssize_t)len ? 0 : -1;
}

static int connect_tcp(const char *host, int port)
{
    char port_s[16];
    snprintf(port_s, sizeof(port_s), "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_s, &hints, &res) != 0) {
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int smtp_send_plain(const smtp_config *cfg, const char *subject, const char *body,
                    char **out_log)
{
    if (out_log) *out_log = NULL;
    cellmgr_buf log;
    buf_init(&log, 1024);
    int fd = connect_tcp(cfg->host, cfg->port > 0 ? cfg->port : 25);
    if (fd < 0) {
        buf_append(&log, "connect failed\n");
        if (out_log) *out_log = log.data; else buf_free(&log);
        return -1;
    }
    int code = smtp_read_line(fd, &log);
    if (code < 200 || code >= 400) goto fail;
    smtp_writef(fd, &log, "EHLO cellmgr\r\n");
    smtp_read_line(fd, &log);
    if (cfg->user[0]) {
        char *u = base64_encode(cfg->user);
        char *p = base64_encode(cfg->pass);
        smtp_writef(fd, &log, "AUTH LOGIN\r\n");
        smtp_read_line(fd, &log);
        smtp_writef(fd, &log, "%s\r\n", u ? u : "");
        smtp_read_line(fd, &log);
        smtp_writef(fd, &log, "%s\r\n", p ? p : "");
        code = smtp_read_line(fd, &log);
        free(u);
        free(p);
        if (code < 200 || code >= 400) goto fail;
    }
    smtp_writef(fd, &log, "MAIL FROM:<%s>\r\n", cfg->from);
    if (smtp_read_line(fd, &log) >= 400) goto fail;
    smtp_writef(fd, &log, "RCPT TO:<%s>\r\n", cfg->to);
    if (smtp_read_line(fd, &log) >= 400) goto fail;
    smtp_writef(fd, &log, "DATA\r\n");
    if (smtp_read_line(fd, &log) >= 400) goto fail;
    smtp_writef(fd, &log,
                "From: <%s>\r\nTo: <%s>\r\nSubject: %s\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n\r\n%s\r\n.\r\n",
                cfg->from, cfg->to, subject ? subject : "CellMgr SMS", body ? body : "");
    if (smtp_read_line(fd, &log) >= 400) goto fail;
    smtp_writef(fd, &log, "QUIT\r\n");
    smtp_read_line(fd, &log);
    close(fd);
    if (out_log) *out_log = log.data; else buf_free(&log);
    return 0;
fail:
    close(fd);
    if (out_log) *out_log = log.data; else buf_free(&log);
    return -1;
}
