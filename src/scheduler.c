#include "scheduler.h"

#include "dbus_cmd.h"

#include <pthread.h>
#include <unistd.h>

static void do_reboot(void)
{
    command_result res;
    char *const sync_argv[] = {"sync", NULL};
    run_capture(sync_argv, 3000, 1024, &res);
    command_result_free(&res);
    char *const reboot_argv[] = {"reboot", NULL};
    run_capture(reboot_argv, 3000, 1024, &res);
    command_result_free(&res);
}

static void *scheduler_main(void *arg)
{
    app_state *state = (app_state *)arg;
    int last_yday = -1;
    for (;;) {
        sleep(30);
        if (!state->cfg.scheduled_reboot_enabled) {
            continue;
        }
        time_t t = time(NULL);
        struct tm tmv;
        localtime_r(&t, &tmv);
        if (tmv.tm_hour == state->cfg.scheduled_reboot_hour &&
            tmv.tm_min == state->cfg.scheduled_reboot_minute &&
            tmv.tm_yday != last_yday) {
            last_yday = tmv.tm_yday;
            log_warn("scheduled reboot triggered at %02d:%02d",
                     tmv.tm_hour, tmv.tm_min);
            do_reboot();
        }
    }
    return NULL;
}

int scheduler_start(app_state *state)
{
    pthread_t tid;
    int rc = pthread_create(&tid, NULL, scheduler_main, state);
    if (rc != 0) {
        log_warn("scheduler thread start failed");
        return -1;
    }
    pthread_detach(tid);
    return 0;
}
