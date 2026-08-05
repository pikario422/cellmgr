#include "dbus_monitor.h"

#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void *monitor_main(void *arg)
{
    app_state *state = (app_state *)arg;
    for (;;) {
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            sleep(5);
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
            char *const argv[] = {
                "dbus-monitor",
                "--system",
                "sender='org.ofono'",
                NULL
            };
            execvp(argv[0], argv);
            _exit(127);
        }
        close(pipefd[1]);
        if (pid < 0) {
            close(pipefd[0]);
            sleep(5);
            continue;
        }

        FILE *fp = fdopen(pipefd[0], "r");
        if (fp) {
            char line[1024];
            while (fgets(line, sizeof(line), fp)) {
                trim_in_place(line);
                if (line[0] != '\0') {
                    db_insert_dbus_event(&state->db, "org.ofono", line);
                }
            }
            fclose(fp);
        } else {
            close(pipefd[0]);
        }
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        sleep(3);
    }
    return NULL;
}

int dbus_monitor_start(app_state *state)
{
    pthread_t tid;
    int rc = pthread_create(&tid, NULL, monitor_main, state);
    if (rc != 0) {
        log_warn("dbus monitor thread start failed");
        return -1;
    }
    pthread_detach(tid);
    return 0;
}
