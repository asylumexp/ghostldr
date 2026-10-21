#ifndef TITLE_ID
#define TITLE_ID "CUSA03492"
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define EXIT_POLL_LIMIT 100U
#define EXIT_POLL_DELAY_US 100000U

extern int sceUserServiceInitialize(void *params);
extern int sceUserServiceGetForegroundUser(int *user_id);
extern int sceLncUtilInitialize(void);
extern int sceLncUtilGetAppId(const char *title_id);
extern uint32_t sceLncUtilKillApp(uint32_t app_id);
extern int sceLncUtilLaunchApp(const char *title_id, char *argv, void *param);
extern int sceSystemServiceLaunchApp(const char *title_id, const char **argv,
                                     void *opts);

typedef struct {
    char unknown[45];
    char message[3075];
} NotifyRequest;

extern int sceKernelSendNotificationRequest(int, NotifyRequest *, size_t, int);

static int app_id_is_valid(int app_id) {
    return app_id >= 0;
}

int main(void) {
    static const char title_id[] = TITLE_ID;
    struct app_launch_ctx {
        uint32_t size;
        uint32_t user_id;
        uint32_t app_opt;
        uint64_t crash_report;
        uint32_t check_flag;
    } launch_param;
    const char *argv[] = {NULL};
    NotifyRequest note;
    int app_id;
    uint32_t kill_rc = 0;
    unsigned int exit_polls = 0;
    int foreground_user = 0;
    int restarted = 0;
    int rc;

    memset(&launch_param, 0, sizeof(launch_param));
    memset(&note, 0, sizeof(note));

    (void)sceUserServiceInitialize(NULL);
    (void)sceUserServiceGetForegroundUser(&foreground_user);
    (void)sceLncUtilInitialize();

    app_id = sceLncUtilGetAppId(title_id);
    if (app_id_is_valid(app_id)) {
        restarted = 1;
        kill_rc = sceLncUtilKillApp((uint32_t)app_id);
        do {
            (void)usleep(EXIT_POLL_DELAY_US);
            ++exit_polls;
            app_id = sceLncUtilGetAppId(title_id);
        } while (app_id_is_valid(app_id) && exit_polls < EXIT_POLL_LIMIT);
        if (app_id_is_valid(app_id)) {
            snprintf(note.message, sizeof(note.message),
                     "launch " TITLE_ID " close timeout app=0x%x kill=0x%x polls=%u",
                     (uint32_t)app_id, kill_rc, exit_polls);
            (void)sceKernelSendNotificationRequest(0, &note, sizeof(note), 0);
            printf("[launch] close timeout app=0x%x kill=0x%x "
                   "polls=%u\n",
                   (uint32_t)app_id, kill_rc, exit_polls);
            return 1;
        }
    }

#ifdef KILL_ONLY
    printf("[launch] kill-only %s restarted=%d kill=0x%x polls=%u\n", title_id, restarted, kill_rc, exit_polls);
    return 0;
#endif
    launch_param.size = sizeof(launch_param);
    launch_param.user_id = (uint32_t)foreground_user;
    rc = sceSystemServiceLaunchApp(title_id, argv, &launch_param);
    if (rc < 0) {
        struct lnc_app_param {
            uint32_t size;
            int32_t user_id;
            uint32_t app_opt;
            uint64_t crash_report;
            uint64_t check_flag;
        } __attribute__((packed)) lnc_param;
        memset(&lnc_param, 0, sizeof(lnc_param));
        lnc_param.size = sizeof(lnc_param);
        lnc_param.user_id = foreground_user;
        rc = sceLncUtilLaunchApp(title_id, NULL, &lnc_param);
    }

    snprintf(note.message, sizeof(note.message),
             "launch " TITLE_ID " launch rc=0x%x user=0x%x restart=%d kill=0x%x polls=%u",
             rc, foreground_user, restarted, kill_rc, exit_polls);
    (void)sceKernelSendNotificationRequest(0, &note, sizeof(note), 0);
    printf("[launch] rc=0x%x user=0x%x restart=%d kill=0x%x "
           "polls=%u\n",
           rc, foreground_user, restarted, kill_rc, exit_polls);
    return rc < 0 ? 1 : 0;
}
