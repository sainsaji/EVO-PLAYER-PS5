/*
 * EVO Player - sony_media_launcher
 *
 * Uses Sony's SceSystemService to launch the official PS5 Media Gallery
 * (NPXS40001) targeting /mnt/usb0/Big_Buck_Bunny_1080_10s_30MB.mp4.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdarg.h>

#include <ps5/kernel.h>
#include <ps5/nid.h>

#include "evo_ps5.h"

#define DEFAULT_TARGET_FILE "/mnt/usb0/Big_Buck_Bunny_1080_10s_30MB.mp4"
#define LOG_PATH            "/mnt/usb0/sony_launcher_log.txt"

static FILE *g_log_file = NULL;

static void LOG(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);

    if (g_log_file) {
        va_start(ap, fmt);
        vfprintf(g_log_file, fmt, ap);
        va_end(ap);
        fflush(g_log_file);
    }
}

typedef struct {
    uint32_t structsize;
    uint32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint32_t check_flag;
} SceAppLaunchCtx;

int sceSystemServiceLaunchApp(const char* title_id, char** argv, SceAppLaunchCtx* ctx);
int sceSystemServiceGetAppIdOfRunningBigApp(void);

extern uint64_t kernel_get_ucred_authid(pid_t pid);
extern int32_t  kernel_set_ucred_authid(pid_t pid, uint64_t authid);
extern int32_t  kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]);
extern int32_t  kernel_set_ucred_caps(pid_t pid, const uint8_t caps[16]);

int main(int argc, char **argv)
{
    const char *target_file = DEFAULT_TARGET_FILE;
    if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        target_file = argv[1];
    }

    g_log_file = fopen(LOG_PATH, "w");

    LOG("\n==========================================================\n");
    LOG("  EVO Player — Official Sony Player Direct Launcher\n");
    LOG("==========================================================\n");
    LOG("Target Video : %s\n", target_file);
    LOG("Firmware     : 0x%08x | PID: %d\n\n", kernel_get_fw_version(), getpid());

    /* 1. Elevate credentials to system level */
    pid_t mypid = getpid();
    uint64_t orig_authid = kernel_get_ucred_authid(mypid);
    uint8_t orig_caps[16] = {0};
    uint8_t privcaps[16];
    memset(privcaps, 0xFF, sizeof(privcaps));
    kernel_get_ucred_caps(mypid, orig_caps);

    kernel_set_ucred_authid(mypid, 0x4800000000010003ULL); /* System Shell & Package AuthID */
    kernel_set_ucred_caps(mypid, privcaps);

    /* 2. Initialize User Service & Query Real Logged-in User */
    sceUserServiceInitialize(NULL);

    SceAppLaunchCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.structsize = sizeof(ctx);

    typedef struct {
        int32_t userId[4];
    } SceLoginList;
    SceLoginList list;
    memset(&list, 0, sizeof(list));

    int uret = sceUserServiceGetLoginUserIdList(&list);
    LOG("[User] sceUserServiceGetLoginUserIdList -> ret=0x%08x (user0=0x%x, user1=0x%x)\n",
        uret, list.userId[0], list.userId[1]);

    if (list.userId[0] > 0) {
        ctx.user_id = (uint32_t)list.userId[0];
    } else {
        ctx.user_id = 0x10000000;
    }
    LOG("[User] Using launch user_id: 0x%08x\n", ctx.user_id);

    evo_notify("EVO: Launching Official Sony Media Player...");

    /* 3. Prepare launch arguments */
    char *launch_argv[] = {
        (char*)target_file,
        NULL
    };

    /* Candidate Title IDs for official Media Players */
    static const char *const kCandidates[] = {
        "NPXS40140", /* PS5 Official Disc Player */
        "NPXS40038", /* SceVideoCore4K Video Decoder */
        "NPXS40039", /* MediaCoreServer */
        "NPXS40146"  /* Share Video Transcoder */
    };
    size_t num_candidates = sizeof(kCandidates) / sizeof(kCandidates[0]);

    bool launched = false;
    for (size_t i = 0; i < num_candidates; i++) {
        const char *title = kCandidates[i];
        LOG("\n[Launch] Attempting to launch %s with \"%s\"...\n", title, target_file);

        int lrc = sceSystemServiceLaunchApp(title, launch_argv, &ctx);
        LOG("  sceSystemServiceLaunchApp(\"%s\") -> 0x%08x\n", title, lrc);

        if (lrc == 0) {
            LOG(">>> SUCCESS: %s launched successfully! <<<\n", title);
            evo_notify("SUCCESS! Official Player (%s) Launched!", title);
            launched = true;
            break;
        } else {
            LOG("  Failed to launch %s (0x%08x), trying next candidate...\n", title, lrc);
        }
    }

    if (!launched) {
        LOG("\n[!] All candidate official media apps returned launch errors.\n");
        evo_notify("Launch Failed. See sony_launcher_log.txt");
    }

    /* Restore credentials */
    kernel_set_ucred_authid(mypid, orig_authid);
    kernel_set_ucred_caps(mypid, orig_caps);

    LOG("\n=== Final Status ===\n");
    LOG("Launched: %s\n", launched ? "YES" : "NO");
    LOG("Log written to: %s\n", LOG_PATH);

    if (g_log_file) fclose(g_log_file);
    return launched ? EXIT_SUCCESS : EXIT_FAILURE;
}
