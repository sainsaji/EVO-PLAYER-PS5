/*
 * EVO Player — one-shot Media tile remover
 *
 * Use when Options → Delete on the home menu does nothing (common for
 * Media BigApp hosts), or when the resident launcher is not running.
 *
 * Removes EVOP10001 from app.db, /user/app and /system_ex/app, then exits.
 * Re-inject the media launcher to reinstall.
 *
 * This removes EVO Player's tile and nothing else. ProsperoPlayer's PRSP10001
 * is deliberately absent from PP_TITLES: the two tiles coexist, and an
 * uninstaller that reaches into another project's registration would be a
 * destructive surprise. Do not add ids this project did not register.
 */

#include <errno.h>
#include <fcntl.h>
#include <ps5/kernel.h>
#include <ps5/payload.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#define PP_TITLE_ID "EVOP10001"
#define PP_APPINST_AUTHID UINT64_C(0x4801000000000013)
#define PP_LOG_PATH "/data/evoplayer/uninstall_tile.log"
#define PP_RUNTIME_DIR "/data/evoplayer/app"

static const char *const PP_TITLES[] = {
    PP_TITLE_ID,
    NULL,
};

#define IOVEC_ENTRY(x)                                                         \
  { x ? x : 0, x ? strlen(x) + 1 : 0 }

int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppUnInstall(const char *, void *, void *);
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);
int sceKernelSendNotificationRequest(int, void *, size_t, int);

typedef struct {
  char reserved[45];
  char message[3075];
} pp_notify_t;

static void pp_log(const char *msg) {
  int fd;
  if (!msg)
    return;
  fputs(msg, stdout);
  fputc('\n', stdout);
  fflush(stdout);
  (void)mkdir("/data", 0755);
  (void)mkdir("/data/evoplayer", 0755);
  fd = open(PP_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd >= 0) {
    (void)write(fd, msg, strlen(msg));
    (void)write(fd, "\n", 1);
    close(fd);
  }
}

static void pp_notify(const char *msg) {
  pp_notify_t req;
  memset(&req, 0, sizeof(req));
  snprintf(req.message, sizeof(req.message), "%s", msg ? msg : "");
  (void)sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static void pp_try_unlink(const char *path) {
  if (unlink(path) == 0)
    pp_log(path);
}

static void pp_try_rmdir(const char *path) {
  if (rmdir(path) == 0)
    pp_log(path);
}

static void pp_wipe_user(const char *title) {
  char p[320];
  snprintf(p, sizeof(p), "/user/app/%s/sce_sys/param.json", title);
  pp_try_unlink(p);
  snprintf(p, sizeof(p), "/user/app/%s/sce_sys/icon0.png", title);
  pp_try_unlink(p);
  snprintf(p, sizeof(p), "/user/app/%s/sce_sys", title);
  pp_try_rmdir(p);
  snprintf(p, sizeof(p), "/user/app/%s/eboot.bin", title);
  pp_try_unlink(p);
  snprintf(p, sizeof(p), "/user/app/%s", title);
  pp_try_rmdir(p);
}

static int pp_remount_system_ex(void) {
  struct iovec iov[] = {
      IOVEC_ENTRY("from"),      IOVEC_ENTRY("/dev/ssd0.system_ex"),
      IOVEC_ENTRY("fspath"),    IOVEC_ENTRY("/system_ex"),
      IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
      IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
      IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
      IOVEC_ENTRY("async"),     {NULL, 0},
      IOVEC_ENTRY("ignoreacl"), {NULL, 0},
  };
  return nmount(iov, (int)(sizeof(iov) / sizeof(iov[0])), MNT_UPDATE);
}

static void pp_wipe_host(const char *title) {
  char p[320];
  (void)pp_remount_system_ex();
  snprintf(p, sizeof(p), "/system_ex/app/%s/sce_sys/param.json", title);
  pp_try_unlink(p);
  snprintf(p, sizeof(p), "/system_ex/app/%s/sce_sys/icon0.png", title);
  pp_try_unlink(p);
  snprintf(p, sizeof(p), "/system_ex/app/%s/sce_sys", title);
  pp_try_rmdir(p);
  snprintf(p, sizeof(p), "/system_ex/app/%s/eboot.bin", title);
  pp_try_unlink(p);
  snprintf(p, sizeof(p), "/system_ex/app/%s", title);
  pp_try_rmdir(p);
}

/*
 * Only the tile's own copy of the player, under /data/evoplayer/app.
 * /data/homebrew/EVOPlayer belongs to the websrv install path
 * (scripts/install-homebrew.sh) and must survive a tile uninstall - removing
 * a tile should never break the development loop.
 */
static void pp_wipe_runtime(void) {
  pp_try_unlink(PP_RUNTIME_DIR "/eboot.elf");
  pp_try_unlink(PP_RUNTIME_DIR "/sce_sys/param.json");
  pp_try_unlink(PP_RUNTIME_DIR "/sce_sys/icon0.png");
  pp_try_rmdir(PP_RUNTIME_DIR "/sce_sys");
  pp_try_rmdir(PP_RUNTIME_DIR);
}

static int pp_do_uninstall(void) {
  int i, rc, app_id;

  app_id = sceSystemServiceGetAppIdOfRunningBigApp();
  if (app_id > 0) {
    pp_log("killing running BigApp");
    (void)sceSystemServiceKillApp(app_id, -1, 0, 0);
    usleep(300000);
  }

  if (sceAppInstUtilInitialize() != 0) {
    pp_log("AppInstUtilInitialize failed");
    return -1;
  }
  for (i = 0; PP_TITLES[i]; i++) {
    rc = sceAppInstUtilAppUnInstall(PP_TITLES[i], NULL, NULL);
    char line[96];
    snprintf(line, sizeof(line), "UnInstall %s 0x%08x", PP_TITLES[i],
             (unsigned)rc);
    pp_log(line);
    usleep(150000);
  }
  (void)sceAppInstUtilTerminate();

  for (i = 0; PP_TITLES[i]; i++) {
    pp_wipe_user(PP_TITLES[i]);
    pp_wipe_host(PP_TITLES[i]);
  }
  pp_wipe_runtime();
  return 0;
}

int main(void) {
  const pid_t pid = getpid();
  const uint64_t orig = kernel_get_ucred_authid(pid);
  int rc;

  pp_log("EVO Player uninstall tile start");
  if (kernel_set_ucred_authid(pid, PP_APPINST_AUTHID) != 0) {
    pp_log("authid raise failed");
    pp_notify("EVO Player uninstall failed (authid)");
    return 1;
  }
  rc = pp_do_uninstall();
  if (orig != 0)
    (void)kernel_set_ucred_authid(pid, orig);

  if (rc == 0) {
    pp_notify("EVO Player removed from Media");
    pp_log("done");
  } else {
    pp_notify("EVO Player uninstall incomplete");
  }
  payload_exit(rc == 0 ? 0 : 1);
  return rc == 0 ? 0 : 1;
}
