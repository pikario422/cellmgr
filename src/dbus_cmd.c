#include "dbus_cmd.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

void command_result_free(command_result *res)
{
    if (!res) {
        return;
    }
    free(res->out);
    free(res->err);
    memset(res, 0, sizeof(*res));
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int run_capture(char *const argv[], int timeout_ms, size_t max_output, command_result *res)
{
    memset(res, 0, sizeof(*res));
    res->exit_code = -1;
    int out_pipe[2];
    int err_pipe[2];
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);
    set_nonblock(out_pipe[0]);
    set_nonblock(err_pipe[0]);

    cellmgr_buf out;
    cellmgr_buf err;
    if (buf_init(&out, 1024) != 0 || buf_init(&err, 512) != 0) {
        kill(pid, SIGKILL);
        close(out_pipe[0]); close(err_pipe[0]);
        buf_free(&out); buf_free(&err);
        return -1;
    }

    long start = now_unix();
    int out_open = 1;
    int err_open = 1;
    int status = 0;
    while (out_open || err_open) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (out_open) {
            FD_SET(out_pipe[0], &rfds);
            maxfd = out_pipe[0] > maxfd ? out_pipe[0] : maxfd;
        }
        if (err_open) {
            FD_SET(err_pipe[0], &rfds);
            maxfd = err_pipe[0] > maxfd ? err_pipe[0] : maxfd;
        }
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        int rv = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rv > 0) {
            char tmp[512];
            if (out_open && FD_ISSET(out_pipe[0], &rfds)) {
                ssize_t n = read(out_pipe[0], tmp, sizeof(tmp));
                if (n > 0 && out.len < max_output) {
                    size_t keep = (out.len + (size_t)n > max_output) ? max_output - out.len : (size_t)n;
                    buf_append_n(&out, tmp, keep);
                } else if (n == 0) {
                    out_open = 0;
                }
            }
            if (err_open && FD_ISSET(err_pipe[0], &rfds)) {
                ssize_t n = read(err_pipe[0], tmp, sizeof(tmp));
                if (n > 0 && err.len < max_output) {
                    size_t keep = (err.len + (size_t)n > max_output) ? max_output - err.len : (size_t)n;
                    buf_append_n(&err, tmp, keep);
                } else if (n == 0) {
                    err_open = 0;
                }
            }
        }
        if (timeout_ms > 0 && (now_unix() - start) * 1000L > timeout_ms) {
            kill(pid, SIGKILL);
            break;
        }
        pid_t wr = waitpid(pid, &status, WNOHANG);
        if (wr == pid && !out_open && !err_open) {
            break;
        }
    }
    close(out_pipe[0]);
    close(err_pipe[0]);
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        res->exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        res->exit_code = 128 + WTERMSIG(status);
    }
    res->out = out.data;
    res->err = err.data;
    return 0;
}

int dbus_send_call(const char *destination, const char *path, const char *interface_method,
                   const char *const *args, int arg_count, int timeout_ms,
                   command_result *res)
{
    char dest_arg[256];
    snprintf(dest_arg, sizeof(dest_arg), "--dest=%s", destination);

    int base = 6;
    int argc = base + arg_count;
    char **argv = calloc((size_t)argc + 1, sizeof(char *));
    if (!argv) {
        return -1;
    }
    argv[0] = "dbus-send";
    argv[1] = "--system";
    argv[2] = "--print-reply";
    argv[3] = dest_arg;
    argv[4] = (char *)path;
    argv[5] = (char *)interface_method;
    for (int i = 0; i < arg_count; i++) {
        argv[base + i] = (char *)args[i];
    }
    int ok = run_capture(argv, timeout_ms, CELLMGR_MAX_AT_RESPONSE, res);
    free(argv);
    return ok;
}
