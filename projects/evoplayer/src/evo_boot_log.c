/* evo_boot_log.c — see evo_boot_log.h. The one diagnostic log. */
#include "evo_boot_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/*
 * Credentials out of anything logged.
 *
 * Stream URLs are logged at every playback stage, and a provider's URL carries
 * its login: Emby/Jellyfin "api_key=<session token>", Xtream "username=" and
 * "password=", or the account in the path (/live/USER/PASS/123.ts). evo.log is
 * readable by anyone with FTP and is the file users send when reporting a bug.
 * Hardware, 2026-10-01: a Jellyfin session token in full in four lines.
 *
 * Masks the value of known secret query parameters, and the two path segments
 * after /live/, /movie/, /series/ and /timeshift/. Works in place on `msg`.
 * Applied to every line evo_boot_log() writes, so no caller can leak one.
 */
static int bc_param_is_secret(const char *name, size_t n)
{
    static const char *const keys[] = {
        "api_key", "apikey", "token", "access_token", "auth", "password",
        "pass", "username", "user", "X-Emby-Token", "X-Plex-Token", "key",
        "hdnts", "sig", "signature", NULL };
    for (int i = 0; keys[i]; ++i)
        if (strlen(keys[i]) == n && strncasecmp(name, keys[i], n) == 0)
            return 1;
    return 0;
}

void evo_log_redact(char *msg, size_t cap)
{
    char out[640];
    size_t o = 0;
    const char *p = msg;
    const int is_url = strstr(msg, "://") != NULL;   /* path masking: web links only */
    while (*p && o + 1 < sizeof out) {
        /* ?name=value / &name=value */
        if ((*p == '?' || *p == '&') && p[1]) {
            const char *name = p + 1;
            const char *eq = name;
            while (*eq && *eq != '=' && *eq != '&' && *eq != ' ') eq++;
            if (*eq == '=' && bc_param_is_secret(name, (size_t)(eq - name))) {
                size_t len = (size_t)(eq - p) + 1;              /* "?name=" */
                if (o + len + 3 >= sizeof out) break;
                memcpy(out + o, p, len);
                o += len;
                memcpy(out + o, "***", 3);
                o += 3;
                p = eq + 1;
                while (*p && *p != '&' && *p != ' ') p++;       /* skip the value */
                continue;
            }
        }
        /* /live/USER/PASS/ - Xtream's account in the path */
        static const char *const segs[] = { "/live/", "/movie/", "/series/", "/timeshift/", NULL };
        int hit = 0;
        for (int i = 0; is_url && segs[i] && !hit; ++i) {
            size_t sl = strlen(segs[i]);
            if (strncmp(p, segs[i], sl) == 0) {
                const char *u_end = strchr(p + sl, '/');
                const char *pw_end = u_end ? strchr(u_end + 1, '/') : NULL;
                if (u_end && pw_end) {
                    if (o + sl + 8 >= sizeof out) break;
                    memcpy(out + o, segs[i], sl);
                    o += sl;
                    memcpy(out + o, "***/***", 7);
                    o += 7;
                    p = pw_end;                                /* keep the "/123.ts" */
                    hit = 1;
                }
            }
        }
        if (hit) continue;
        out[o++] = *p++;
    }
    out[o] = 0;
    if (o + 1 > cap) { o = cap - 1; out[o] = 0; }
    memcpy(msg, out, o + 1);
}

#ifdef EVO_APP_MODULE

#include <pthread.h>
#include <time.h>
#include <unistd.h>

#ifdef EVO_BOOT_TRACE_POPUP
struct bl_note { char pad[45]; char msg[3075]; };
extern int sceKernelSendNotificationRequest(int, void *, unsigned long, int);
#endif

#define EVO_LOG_PATH "/mnt/usb0/evo.log"

/*
 * Two phases:
 *   1. before /mnt/usb0 is reachable (boot, pre-self-unjail) - lines are held
 *      in g_boot. A hard cap; overflow bumps g_dropped and is reported once
 *      the file opens.
 *   2. after evo_boot_log_flush() first opens the file - lines go into a
 *      memory queue and a writer thread puts them on the USB stick.
 *
 * Why a writer thread: the log lives on the same USB stick a movie streams
 * from. Writing it from the caller - fflush per line, plus the render loop's
 * once-a-second fsync - blocked the main loop for 33-83 ms whenever the stick
 * was busy, which is a visible hitch in 4K playback (hardware 2026-09-28,
 * `pace:` trace, LG "Art" demo). Now evo_boot_log() is a memcpy, the
 * periodic flush from the loop only wakes the writer (evo_boot_log_kick),
 * and only an explicit evo_boot_log_flush() - a breadcrumb before something
 * risky - still waits for the stick. The crash handler writes out whatever
 * is still queued (evo_boot_log_crash_drain), so the last lines survive.
 */
#define BL_BOOT_CAP  16384
#define BL_QUEUE_CAP (256 * 1024)
#define BL_SYNC_MS   2000

static char   g_boot[BL_BOOT_CAP];
static size_t g_boot_len;
static int    g_dropped;
static FILE  *g_fp;
static long   g_written;   /* bytes in evo.log since open/rotation, incl. what was there */

#define BL_ROTATE_BYTES (16L * 1024 * 1024)
static FILE *bl_rotate(FILE *fp);
static void  bl_session_header(FILE *fp, const char *why);
static void  bl_wall_iso(char *out, size_t cap);

/*
 * Live-tail ring (evo_log_server.c). Every line lands here from the first
 * call, even before /mnt/usb0 opens, so a client that connects late still sees
 * the boot. `g_ring_total` counts every byte ever logged; a reader keeps its
 * own absolute position, so a slow client loses old lines instead of stalling
 * the logger (evo_boot_log is only ever a memcpy under g_ring_lock).
 */
#define BL_RING_CAP (128 * 1024)
static char            g_ring[BL_RING_CAP];
static unsigned long long g_ring_total;
static pthread_mutex_t g_ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ring_cond = PTHREAD_COND_INITIALIZER;

static void bl_ring_put(const char *s, size_t n)
{
    pthread_mutex_lock(&g_ring_lock);
    for (size_t i = 0; i < n;) {
        size_t at = (size_t)(g_ring_total % BL_RING_CAP);
        size_t c = BL_RING_CAP - at;
        if (c > n - i) c = n - i;
        memcpy(g_ring + at, s + i, c);
        g_ring_total += c;
        i += c;
    }
    pthread_cond_broadcast(&g_ring_cond);
    pthread_mutex_unlock(&g_ring_lock);
}

unsigned long long evo_log_ring_total(void)
{
    pthread_mutex_lock(&g_ring_lock);
    unsigned long long t = g_ring_total;
    pthread_mutex_unlock(&g_ring_lock);
    return t;
}

size_t evo_log_ring_read(unsigned long long *pos, char *buf, size_t cap,
                         int wait_ms, unsigned long long *missed)
{
    size_t n = 0;
    pthread_mutex_lock(&g_ring_lock);
    if (*pos > g_ring_total)
        *pos = g_ring_total;
    if (*pos == g_ring_total && wait_ms > 0) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += wait_ms / 1000;
        until.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&g_ring_cond, &g_ring_lock, &until);
    }
    if (g_ring_total - *pos > BL_RING_CAP) {
        /* Overwritten while the reader was away: jump to the oldest byte still
         * held, then on to the next line start. */
        unsigned long long from = g_ring_total - BL_RING_CAP;
        while (from < g_ring_total && g_ring[from % BL_RING_CAP] != '\n')
            from++;
        if (from < g_ring_total)
            from++;
        if (missed)
            *missed += from - *pos;
        *pos = from;
    }
    unsigned long long avail = g_ring_total - *pos;
    n = avail < cap ? (size_t)avail : cap;
    for (size_t i = 0; i < n; ++i)
        buf[i] = g_ring[(*pos + i) % BL_RING_CAP];
    if (n == cap && n > 0) {
        /* Hand out whole lines only; the tail is picked up on the next call. */
        size_t k = n;
        while (k > 0 && buf[k - 1] != '\n')
            k--;
        if (k > 0)
            n = k;
    }
    *pos += n;
    pthread_mutex_unlock(&g_ring_lock);
    return n;
}

static pthread_mutex_t g_q_lock  = PTHREAD_MUTEX_INITIALIZER;  /* the queue */
static pthread_mutex_t g_io_lock = PTHREAD_MUTEX_INITIALIZER;  /* g_fp writes */
static pthread_cond_t  g_q_cond  = PTHREAD_COND_INITIALIZER;
static char   g_q[BL_QUEUE_CAP];
static size_t g_q_len;
static int    g_q_dropped;
static int    g_want_sync;
static int    g_writer_up;

/* Take the queue and write it. Caller holds nothing; returns bytes written. */
static size_t bl_drain(int do_sync)
{
    static char out[BL_QUEUE_CAP];
    size_t n;
    int dropped;

    pthread_mutex_lock(&g_io_lock);
    pthread_mutex_lock(&g_q_lock);
    n = g_q_len;
    memcpy(out, g_q, n);
    g_q_len = 0;
    dropped = g_q_dropped;
    g_q_dropped = 0;
    pthread_mutex_unlock(&g_q_lock);

    if (g_fp) {
        if (dropped)
            fprintf(g_fp, "[log] %d line(s) dropped - log queue full\n", dropped);
        if (n)
            fwrite(out, 1, n, g_fp);
        fflush(g_fp);
        g_written += (long)n;
        if (g_written > BL_ROTATE_BYTES) {
            FILE *fresh = bl_rotate(g_fp);
            if (fresh) {
                g_fp = fresh;
                g_written = 0;
                bl_session_header(g_fp, "rotated (previous file is evo.log.1)");
                fflush(g_fp);
            }
        }
        if (do_sync)
            fsync(fileno(g_fp));
    }
    pthread_mutex_unlock(&g_io_lock);
    return n;
}

static void *bl_writer(void *arg)
{
    (void)arg;
    struct timespec last_sync;
    clock_gettime(CLOCK_MONOTONIC, &last_sync);
    struct timespec last_clock = last_sync;
    for (;;) {
        pthread_mutex_lock(&g_q_lock);
        if (g_q_len == 0 && !g_want_sync) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 250 * 1000000L;
            if (until.tv_nsec >= 1000000000L) {
                until.tv_sec++;
                until.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&g_q_cond, &g_q_lock, &until);
        }
        const int want_sync = g_want_sync;
        g_want_sync = 0;
        pthread_mutex_unlock(&g_q_lock);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const long since_ms = (long)(now.tv_sec - last_sync.tv_sec) * 1000L +
                              (now.tv_nsec - last_sync.tv_nsec) / 1000000L;
        const int do_sync = want_sync || since_ms >= BL_SYNC_MS;
        /* Re-state the wall clock every 10 minutes so any stretch of the log
         * can be dated, and a rotated file carries its own anchor. */
        if ((now.tv_sec - last_clock.tv_sec) >= 600) {
            char iso[32];
            bl_wall_iso(iso, sizeof iso);
            evo_boot_log("log clock: wall clock %s at this line", iso);
            last_clock = now;
        }
        bl_drain(do_sync);
        if (do_sync)
            last_sync = now;
    }
    return NULL;
}

static void bl_vlog(const char *level, const char *fmt, va_list ap)
{
    char line[600];
    /* Fixed-width level column, so lines align: "INFO  ", "WARN  ", "ERROR ". */
    int ln = snprintf(line, sizeof line, "%-5s ", level ? level : "INFO");
    vsnprintf(line + ln, sizeof line - (size_t)ln, fmt, ap);
    evo_log_redact(line, sizeof line);

    printf("EVO boot: %s\n", line);
    fflush(stdout);

#ifdef EVO_BOOT_TRACE_POPUP
    /* #51: on-screen popup — opt-in (--breadcrumbs). Off by default; the
     * EVO_LOG_PATH file is the durable channel and is always written. */
    struct bl_note nt;
    memset(&nt, 0, sizeof nt);
    snprintf(nt.msg, sizeof nt.msg, "%s", line);
    sceKernelSendNotificationRequest(0, &nt, sizeof nt, 0);
#endif

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char stamped[680];
    int m = snprintf(stamped, sizeof stamped, "[%lld.%03ld] %s\n",
                     (long long)ts.tv_sec, ts.tv_nsec / 1000000L, line);
    if (m < 0)
        return;
    if ((size_t)m >= sizeof stamped)
        m = (int)sizeof stamped - 1;

    bl_ring_put(stamped, (size_t)m);

    pthread_mutex_lock(&g_q_lock);
    if (g_fp) {
        if (g_q_len + (size_t)m <= BL_QUEUE_CAP) {
            memcpy(g_q + g_q_len, stamped, (size_t)m);
            g_q_len += (size_t)m;
        } else {
            g_q_dropped++;
        }
        pthread_cond_signal(&g_q_cond);
    } else if (g_boot_len + (size_t)m < BL_BOOT_CAP) {
        memcpy(g_boot + g_boot_len, stamped, (size_t)m);
        g_boot_len += (size_t)m;
    } else {
        g_dropped++;
    }
    pthread_mutex_unlock(&g_q_lock);
}

void evo_boot_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bl_vlog(NULL, fmt, ap);
    va_end(ap);
}

/* level: 1 = WARN, 2 = ERROR; anything else is INFO. Every line is
 * "[seconds] LEVEL text" with LEVEL padded to 5 characters. */
void evo_log_level(int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bl_vlog(level == 2 ? "ERROR" : level == 1 ? "WARN" : NULL, fmt, ap);
    va_end(ap);
}

/* Wall-clock time as "2026-10-07T12:34:56Z" without libc time helpers (the
 * native-app libc is missing some). */
static void bl_wall_iso(char *out, size_t cap)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long s = (long long)ts.tv_sec;
    long long days = s / 86400, rem = s % 86400;
    if (rem < 0) { rem += 86400; days--; }
    long long z = days + 719468;                       /* civil-from-days */
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    long long d = doy - (153 * mp + 2) / 5 + 1;
    long long m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    snprintf(out, cap, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ", y, m, d,
             rem / 3600, (rem % 3600) / 60, rem % 60);
}

/* Keep evo.log from growing without bound on the stick: past BL_ROTATE_BYTES
 * the file is copied to evo.log.1 (one generation kept) and started afresh.
 * A copy, not rename(): the app module has no rename() guarantee. Caller holds
 * g_io_lock. */
static FILE *bl_rotate(FILE *fp)
{
    static char buf[64 * 1024];
    fflush(fp);
    fclose(fp);
    FILE *in = fopen(EVO_LOG_PATH, "r");
    FILE *out = in ? fopen(EVO_LOG_PATH ".1", "w") : NULL;
    if (in && out) {
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, in)) > 0)
            fwrite(buf, 1, n, out);
    }
    if (in) fclose(in);
    if (out) { fflush(out); fsync(fileno(out)); fclose(out); }
    FILE *fresh = fopen(EVO_LOG_PATH, (in && out) ? "w" : "a");   /* keep it if the copy failed */
    return fresh;
}

static void bl_session_header(FILE *fp, const char *why)
{
    char iso[32];
    struct timespec ts;
    bl_wall_iso(iso, sizeof iso);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(fp, "[%lld.%03ld] INFO  log %s: wall clock %s at this line\n",
            (long long)ts.tv_sec, ts.tv_nsec / 1000000L, why, iso);
}

void evo_boot_log_flush(void)
{
    if (!g_fp) {
        FILE *fp = fopen(EVO_LOG_PATH, "a");
        if (!fp)
            return;   /* /mnt/usb0 not reachable yet — try again next call */
        pthread_mutex_lock(&g_io_lock);
        fseek(fp, 0, SEEK_END);
        g_written = ftell(fp);
        if (g_written > BL_ROTATE_BYTES) {   /* a previous run left it big */
            FILE *fresh = bl_rotate(fp);
            if (fresh) { fp = fresh; g_written = 0; }
        }
        bl_session_header(fp, "session start");
        pthread_mutex_lock(&g_q_lock);
        if (g_dropped) {
            fprintf(fp, "[log] %d pre-mount line(s) dropped\n", g_dropped);
            g_dropped = 0;
        }
        if (g_boot_len) {
            fwrite(g_boot, 1, g_boot_len, fp);
            g_boot_len = 0;
        }
        g_fp = fp;
        pthread_mutex_unlock(&g_q_lock);
        pthread_mutex_unlock(&g_io_lock);

        if (!g_writer_up) {
            pthread_t t;
            if (pthread_create(&t, NULL, bl_writer, NULL) == 0) {
                pthread_detach(t);
                g_writer_up = 1;
            }
        }
        evo_log_server_start();   /* once; the sandbox (and network) is open now */
    }
    /* An explicit flush is a breadcrumb: it waits until the line is on the
     * stick. */
    bl_drain(1);
}

void evo_boot_log_kick(void)
{
    if (!g_fp) {
        evo_boot_log_flush();   /* still in phase 1: try to open the file */
        return;
    }
    if (!g_writer_up) {
        bl_drain(1);            /* no writer thread: behave as before */
        return;
    }
    pthread_mutex_lock(&g_q_lock);
    g_want_sync = 1;
    pthread_cond_signal(&g_q_cond);
    pthread_mutex_unlock(&g_q_lock);
}

void evo_boot_log_crash_drain(int fd)
{
    /* Signal context: no locks, no stdio. Best effort - a line being queued
     * at the instant of the crash may be torn. */
    if (fd < 0)
        return;
    if (g_q_len && g_q_len <= BL_QUEUE_CAP)
        (void)write(fd, g_q, g_q_len);
    else if (!g_fp && g_boot_len && g_boot_len <= BL_BOOT_CAP)
        (void)write(fd, g_boot, g_boot_len);
}

#else  /* host / payload */

void evo_boot_log(const char *fmt, ...) { (void)fmt; }
void evo_log_level(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void evo_boot_log_flush(void) {}
void evo_boot_log_kick(void) {}
void evo_boot_log_crash_drain(int fd) { (void)fd; }
unsigned long long evo_log_ring_total(void) { return 0; }
size_t evo_log_ring_read(unsigned long long *pos, char *buf, size_t cap,
                         int wait_ms, unsigned long long *missed)
{ (void)pos; (void)buf; (void)cap; (void)wait_ms; (void)missed; return 0; }
void evo_log_server_start(void) {}

#endif
