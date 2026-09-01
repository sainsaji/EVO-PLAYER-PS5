/*
 * EVO Player - media_watcher
 *
 * Scans and logs all live processes, detecting any active foreground application
 * (Media Gallery, YouTube, Player, etc.) and sending notifications.
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

#define LOG_PATH      "/mnt/usb0/media_watcher_log.txt"
#define PROCLIST_PATH "/mnt/usb0/live_processes.txt"

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

extern uint64_t kernel_get_ucred_authid(pid_t pid);
extern int32_t  kernel_set_ucred_authid(pid_t pid, uint64_t authid);
extern int32_t  kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]);
extern int32_t  kernel_set_ucred_caps(pid_t pid, const uint8_t caps[16]);

#define MAX_PID 0x1000

static void dump_live_processes(pid_t self)
{
    FILE *fp = fopen(PROCLIST_PATH, "w");
    if (!fp) return;

    fprintf(fp, "=== LIVE PS5 PROCESS LIST ===\n");
    fprintf(fp, "%-6s  %-24s  %-18s  %-18s\n", "PID", "NAME (p_comm)", "TITLE ID", "AUTHID");
    fprintf(fp, "----------------------------------------------------------------------\n");

    for (pid_t pid = 1; pid < MAX_PID; pid++) {
        intptr_t proc = kernel_get_proc(pid);
        if (!proc) continue;

        uint8_t buf[0x800];
        if (kernel_copyout(proc, buf, sizeof(buf)) != 0) continue;

        char pcomm[32] = {0};
        memcpy(pcomm, buf + 0x5e4, 16);
        for (int i = 0; i < 16; i++) {
            if (pcomm[i] < 0x20 || pcomm[i] >= 0x7f) pcomm[i] = '\0';
        }

        char title[32] = "-";
        for (size_t off = 0; off + 10 < sizeof(buf); off++) {
            if (memcmp(buf + off, "NPXS", 4) == 0 ||
                memcmp(buf + off, "PPSA", 4) == 0 ||
                memcmp(buf + off, "CUSA", 4) == 0) {
                strncpy(title, (char*)(buf + off), 16);
                title[16] = '\0';
                for (int k = 0; k < 16; k++) {
                    if (title[k] < 0x20 || title[k] >= 0x7f) { title[k] = '\0'; break; }
                }
                break;
            }
        }

        uint64_t authid = kernel_get_ucred_authid(pid);
        fprintf(fp, "%-6d  %-24s  %-18s  0x%016llx\n",
                pid, pcomm[0] ? pcomm : "(no-comm)", title, (unsigned long long)authid);
    }

    fclose(fp);
}

int main(int argc, char **argv)
{
    g_log_file = fopen(LOG_PATH, "w");

    LOG("\n==========================================================\n");
    LOG("  EVO Player — Background Official Media Player Watcher\n");
    LOG("==========================================================\n");
    LOG("Firmware : 0x%08x | Watcher PID: %d\n\n", kernel_get_fw_version(), getpid());

    pid_t self = getpid();
    uint64_t orig_authid = kernel_get_ucred_authid(self);
    uint8_t orig_caps[16] = {0};
    uint8_t privcaps[16];
    memset(privcaps, 0xFF, sizeof(privcaps));
    kernel_get_ucred_caps(self, orig_caps);

    kernel_set_ucred_authid(self, 0x4900000000000002ULL);
    kernel_set_ucred_caps(self, privcaps);

    sceUserServiceInitialize(NULL);

    evo_notify("EVO Watcher ACTIVE: Open Media Gallery & Play!");
    LOG("[Watcher] Scanning all live processes...\n");

    pid_t last_app_pid = 0;
    int detections = 0;

    /* Monitor continuously for 10 minutes */
    for (int iter = 0; iter < 1200; iter++) {
        if (iter % 10 == 0) {
            dump_live_processes(self);
        }

        for (pid_t pid = 1; pid < MAX_PID; pid++) {
            if (pid == self) continue;

            intptr_t proc = kernel_get_proc(pid);
            if (!proc) continue;

            uint8_t buf[0x800];
            if (kernel_copyout(proc, buf, sizeof(buf)) != 0) continue;

            char title[32] = {0};
            bool is_target = false;

            /* Check for any application Title ID */
            for (size_t off = 0; off + 9 < sizeof(buf); off++) {
                if (memcmp(buf + off, "NPXS40001", 9) == 0 ||
                    memcmp(buf + off, "NPXS40000", 9) == 0 ||
                    memcmp(buf + off, "PPSA01650", 9) == 0 ||
                    memcmp(buf + off, "CUSA00126", 9) == 0 ||
                    memcmp(buf + off, "NPXS40172", 9) == 0) {
                    strncpy(title, (char*)(buf + off), 16);
                    title[16] = '\0';
                    is_target = true;
                    break;
                }
            }

            char pcomm[32] = {0};
            memcpy(pcomm, buf + 0x5e4, 16);

            if (is_target || strstr(pcomm, "Media") || strstr(pcomm, "Gallery") || strstr(pcomm, "Player")) {
                uint64_t authid = kernel_get_ucred_authid(pid);

                LOG("\n>>> [OFFICIAL MEDIA APP DETECTED] <<<\n");
                LOG("  PID       : %d\n", pid);
                LOG("  Comm      : %s\n", pcomm[0] ? pcomm : "unknown");
                LOG("  Title ID  : %s\n", title[0] ? title : "unknown");
                LOG("  AuthID    : 0x%016llx\n", (unsigned long long)authid);

                if (pid != last_app_pid || (iter % 10 == 0)) {
                    evo_notify("Sony Media Player Active! PID: %d (%s)",
                               pid, title[0] ? title : pcomm);
                    last_app_pid = pid;
                    detections++;
                }
            }
        }

        usleep(500000); /* 500 ms */
    }

    kernel_set_ucred_authid(self, orig_authid);
    kernel_set_ucred_caps(self, orig_caps);

    LOG("\n[Watcher] Finished. Total detections: %d\n", detections);
    if (g_log_file) fclose(g_log_file);
    return EXIT_SUCCESS;
}
