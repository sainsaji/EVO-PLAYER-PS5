/*
 * evo_iobench.c - why does a big copy to /data start at 34 MB/s and settle at 2?
 *
 * A temporary diagnostic, driven by `evo-remote.sh` -> `iobench [case ...]`.
 * FTP writes 3 GB to /data/Media at a flat 79 MB/s, so the drive and the
 * filesystem are not the limit; something about EVO's own write loop is. Each
 * case below writes the same 2 GiB to the same place and differs in exactly one
 * thing, so the log says which one matters.
 *
 * Per 128 MiB it logs the interval throughput and the worst single write, and
 * a case gives up early once three intervals in a row are under 6 MB/s - no
 * point spending 15 minutes proving a collapse we have already seen.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <unistd.h>

#include "evo_boot_trace.h"

#define BENCH_SRC     "/mnt/usb0/media/Demos/GTAVI_An_Extended_Look.mp4"
#define BENCH_TOTAL   ((unsigned long long)2048 * 1024 * 1024)
#define BENCH_REPORT  ((unsigned long long)128 * 1024 * 1024)
#define BENCH_MAXBUF  ((size_t)8 * 1024 * 1024)

/* one bit per behaviour under test, so cases compose */
#define F_PREALLOC    0x01   /* ftruncate(fd, total) before the first write */
#define F_BIGCHUNK    0x02   /* 8 MiB writes instead of 1 MiB */
#define F_ODIRECT     0x04   /* O_DIRECT, aligned buffer */
#define F_PERIODIC_FS 0x08   /* fsync() every 64 MiB */
#define F_READ_USB    0x10   /* feed the writes from a real USB read */
#define F_OSYNC       0x20   /* O_SYNC on the destination */
#define F_SMALLCHUNK  0x40   /* 64 KiB writes - the size ftpsrv uses */
#define F_REOPEN      0x80   /* close + reopen the same file every 256 MiB */
#define F_MULTIFILE   0x100  /* a fresh file every 256 MiB */
#define F_USB_DST     0x200  /* write to /mnt/usb0 instead of /data */
#define F_MMAP        0x400  /* ftruncate + a sliding mmap window, not write() */

static volatile int g_bench_running;

static unsigned long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long long)tv.tv_sec * 1000ULL + (unsigned long long)tv.tv_usec / 1000ULL;
}

/* The roll boundary for F_REOPEN / F_MULTIFILE. */
#define BENCH_ROLL ((unsigned long long)256 * 1024 * 1024)

static void bench_dst_path(char *out, size_t n, unsigned flags, int seq)
{
    const char *dir = (flags & F_USB_DST) ? "/mnt/usb0" : "/data/Media";
    if (flags & F_MULTIFILE)
        snprintf(out, n, "%s/_iobench_%d.tmp", dir, seq);
    else
        snprintf(out, n, "%s/_iobench.tmp", dir);
}

/*
 * write() is accounted somewhere we cannot see: whatever we do to the fd, the
 * process gets throttled after a fixed 1152 MB. A file mapping dirties pages
 * through the VM object instead, which is a different path through the kernel -
 * worth one measurement before concluding the budget is unavoidable.
 */
#define MMAP_WINDOW ((size_t)64 * 1024 * 1024)

static void run_case_mmap(const char *name, unsigned flags)
{
    unsigned long long written = 0, interval_start_bytes = 0;
    unsigned long long t0, interval_t0, worst = 0, worst_overall = 0;
    char               dstPath[256];
    char              *src;
    int                fd, slow_streak = 0;
    size_t             chunk = (size_t)1024 * 1024;

    src = (char *)malloc(chunk);
    if (!src)
        return;
    memset(src, 0xA5, chunk);

    bench_dst_path(dstPath, sizeof dstPath, flags, 0);
    remove(dstPath);
    fd = open(dstPath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        evo_bt("iobench[%s]: open dst failed errno=%d", name, errno);
        free(src);
        return;
    }
    if (ftruncate(fd, (off_t)BENCH_TOTAL) != 0) {
        evo_bt("iobench[%s]: ftruncate failed errno=%d", name, errno);
        close(fd);
        remove(dstPath);
        free(src);
        return;
    }

    evo_bt("iobench[%s]: start mmap window=%zuMB", name, MMAP_WINDOW / (1024 * 1024));
    t0 = interval_t0 = now_ms();

    while (written < BENCH_TOTAL) {
        unsigned long long winBase = written;
        size_t             winLen = MMAP_WINDOW;
        unsigned long long tmap, tsync;
        size_t             off;
        void              *p;

        if (winBase + winLen > BENCH_TOTAL)
            winLen = (size_t)(BENCH_TOTAL - winBase);

        tmap = now_ms();
        p = mmap(NULL, winLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)winBase);
        if (p == (void *)-1) {
            evo_bt("iobench[%s]: mmap at %lluMB failed errno=%d",
                   name, winBase / (1024 * 1024), errno);
            break;
        }
        tmap = now_ms() - tmap;

        for (off = 0; off < winLen; off += chunk) {
            size_t             n = (off + chunk > winLen) ? (winLen - off) : chunk;
            unsigned long long tc = now_ms();
            memcpy((char *)p + off, src, n);
            tc = now_ms() - tc;
            if (tc > worst)
                worst = tc;
            if (tc > worst_overall)
                worst_overall = tc;
        }

        tsync = now_ms();
        msync(p, winLen, MS_SYNC);
        munmap(p, winLen);
        tsync = now_ms() - tsync;

        written += winLen;
        evo_bt("iobench[%s]: window at %lluMB  map %llu ms  msync+unmap %llu ms  worst memcpy %llu ms",
               name, written / (1024 * 1024), tmap, tsync, worst);

        if (written - interval_start_bytes >= BENCH_REPORT) {
            unsigned long long dt = now_ms() - interval_t0;
            double mbps = dt ? ((double)(written - interval_start_bytes) / 1048576.0) / ((double)dt / 1000.0) : 0.0;
            evo_bt("iobench[%s]: at %4lluMB  %6.1f MB/s", name, written / (1024 * 1024), mbps);
            if (mbps < 6.0 && ++slow_streak >= 3) {
                evo_bt("iobench[%s]: collapsed, giving up at %lluMB", name, written / (1024 * 1024));
                break;
            }
            if (mbps >= 6.0)
                slow_streak = 0;
            interval_start_bytes = written;
            interval_t0 = now_ms();
        }
        worst = 0;
    }

    {
        unsigned long long dt = now_ms() - t0;
        double mbps = dt ? ((double)written / 1048576.0) / ((double)dt / 1000.0) : 0.0;
        close(fd);
        evo_bt("iobench[%s]: DONE %lluMB in %llu ms = %.1f MB/s avg, worst memcpy %llu ms",
               name, written / (1024 * 1024), dt, mbps, worst_overall);
    }
    remove(dstPath);
    free(src);
}

static void run_case(const char *name, unsigned flags)
{
    if (flags & F_MMAP) {
        run_case_mmap(name, flags);
        return;
    }
    size_t             chunk = (flags & F_BIGCHUNK)   ? (size_t)8 * 1024 * 1024
                             : (flags & F_SMALLCHUNK) ? (size_t)64 * 1024
                                                      : (size_t)1024 * 1024;
    unsigned long long written = 0, interval_start_bytes = 0, roll_at = BENCH_ROLL;
    unsigned long long t0, interval_t0, worst = 0, worst_overall = 0;
    unsigned long long read_ms_total = 0;
    int                oflags = O_WRONLY | O_CREAT | O_TRUNC;
    int                fd, srcFd = -1, slow_streak = 0, failed = 0, seq = 0;
    char               dstPath[256];
    char              *raw, *buf;

    raw = (char *)malloc(BENCH_MAXBUF + 4096);
    if (!raw) {
        evo_bt("iobench[%s]: no buffer", name);
        return;
    }
    /* O_DIRECT wants a sector-aligned buffer; align unconditionally so the
     * cases differ only in the flag being measured. */
    buf = (char *)(((unsigned long long)raw + 4095ULL) & ~4095ULL);
    memset(buf, 0xA5, BENCH_MAXBUF);

    if (flags & F_ODIRECT)
        oflags |= O_DIRECT;
    if (flags & F_OSYNC)
        oflags |= O_SYNC;

    bench_dst_path(dstPath, sizeof dstPath, flags, seq);
    remove(dstPath);
    fd = open(dstPath, oflags, 0644);
    if (fd < 0) {
        evo_bt("iobench[%s]: open dst '%s' failed errno=%d", name, dstPath, errno);
        free(raw);
        return;
    }

    if (flags & F_READ_USB) {
        srcFd = open(BENCH_SRC, O_RDONLY);
        if (srcFd < 0) {
            evo_bt("iobench[%s]: open src failed errno=%d", name, errno);
            close(fd);
            remove(dstPath);
            free(raw);
            return;
        }
    }

    if (flags & F_PREALLOC) {
        unsigned long long tp = now_ms();
        int rc = ftruncate(fd, (off_t)BENCH_TOTAL);
        evo_bt("iobench[%s]: ftruncate(%lluMB) rc=%d errno=%d in %llu ms",
               name, BENCH_TOTAL / (1024 * 1024), rc, rc ? errno : 0, now_ms() - tp);
        if (rc == 0)
            lseek(fd, 0, SEEK_SET);
    }

    evo_bt("iobench[%s]: start chunk=%zuKB flags=0x%02x", name, chunk / 1024, flags);
    t0 = interval_t0 = now_ms();

    while (written < BENCH_TOTAL) {
        size_t             want = chunk;
        ssize_t            n;
        unsigned long long tw;

        if (written + want > BENCH_TOTAL)
            want = (size_t)(BENCH_TOTAL - written);

        if (srcFd >= 0) {
            unsigned long long tr = now_ms();
            ssize_t            got = read(srcFd, buf, want);
            read_ms_total += now_ms() - tr;
            if (got <= 0) {
                evo_bt("iobench[%s]: src exhausted at %lluMB", name, written / (1024 * 1024));
                break;
            }
            want = (size_t)got;
        }

        tw = now_ms();
        n = write(fd, buf, want);
        tw = now_ms() - tw;
        if (tw > worst)
            worst = tw;
        if (tw > worst_overall)
            worst_overall = tw;

        if (n != (ssize_t)want) {
            evo_bt("iobench[%s]: write short n=%zd errno=%d at %lluMB",
                   name, n, errno, written / (1024 * 1024));
            failed = 1;
            break;
        }
        written += (unsigned long long)n;

        if ((flags & (F_REOPEN | F_MULTIFILE)) && written >= roll_at) {
            unsigned long long tr = now_ms();
            close(fd);
            if (flags & F_MULTIFILE)
                bench_dst_path(dstPath, sizeof dstPath, flags, ++seq);
            fd = open(dstPath, (flags & F_MULTIFILE) ? (O_WRONLY | O_CREAT | O_TRUNC)
                                                     : (O_WRONLY | O_APPEND), 0644);
            if (fd < 0) {
                evo_bt("iobench[%s]: reopen '%s' failed errno=%d", name, dstPath, errno);
                failed = 1;
                break;
            }
            evo_bt("iobench[%s]: rolled fd at %lluMB in %llu ms",
                   name, written / (1024 * 1024), now_ms() - tr);
            roll_at += BENCH_ROLL;
        }

        if ((flags & F_PERIODIC_FS) && (written % ((unsigned long long)64 * 1024 * 1024)) == 0) {
            unsigned long long tf = now_ms();
            fsync(fd);
            evo_bt("iobench[%s]: fsync at %lluMB took %llu ms",
                   name, written / (1024 * 1024), now_ms() - tf);
        }

        if (written - interval_start_bytes >= BENCH_REPORT) {
            unsigned long long dt = now_ms() - interval_t0;
            double mbps = dt ? ((double)(written - interval_start_bytes) / 1048576.0) / ((double)dt / 1000.0) : 0.0;
            evo_bt("iobench[%s]: at %4lluMB  %6.1f MB/s  worst write %llu ms",
                   name, written / (1024 * 1024), mbps, worst);
            if (mbps < 6.0) {
                if (++slow_streak >= 3) {
                    evo_bt("iobench[%s]: collapsed, giving up at %lluMB", name, written / (1024 * 1024));
                    break;
                }
            } else {
                slow_streak = 0;
            }
            worst = 0;
            interval_start_bytes = written;
            interval_t0 = now_ms();
        }
    }

    {
        unsigned long long dt = now_ms() - t0;
        double mbps = dt ? ((double)written / 1048576.0) / ((double)dt / 1000.0) : 0.0;
        unsigned long long tc = now_ms();
        close(fd);
        evo_bt("iobench[%s]: DONE %lluMB in %llu ms = %.1f MB/s avg, worst write %llu ms, "
               "read %llu ms total, close %llu ms, failed=%d",
               name, written / (1024 * 1024), dt, mbps, worst_overall,
               read_ms_total, now_ms() - tc, failed);
    }

    if (srcFd >= 0)
        close(srcFd);
    while (seq >= 0) {
        bench_dst_path(dstPath, sizeof dstPath, flags, seq);
        remove(dstPath);
        if (!(flags & F_MULTIFILE))
            break;
        --seq;
    }
    free(raw);
}

static void *bench_thread(void *arg)
{
    char *spec = (char *)arg;
    char *tok, *save = NULL;

    evo_bt("iobench: BEGIN spec='%s'", spec ? spec : "all");

    for (tok = strtok_r(spec, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (strcmp(tok, "plain") == 0)        run_case("plain", 0);
        else if (strcmp(tok, "big") == 0)     run_case("big", F_BIGCHUNK);
        else if (strcmp(tok, "pre") == 0)     run_case("pre", F_PREALLOC);
        else if (strcmp(tok, "prebig") == 0)  run_case("prebig", F_PREALLOC | F_BIGCHUNK);
        else if (strcmp(tok, "odirect") == 0) run_case("odirect", F_ODIRECT);
        else if (strcmp(tok, "osync") == 0)   run_case("osync", F_OSYNC);
        else if (strcmp(tok, "fsync") == 0)   run_case("fsync", F_PERIODIC_FS);
        else if (strcmp(tok, "copy") == 0)    run_case("copy", F_READ_USB);
        else if (strcmp(tok, "precopy") == 0) run_case("precopy", F_READ_USB | F_PREALLOC);
        else if (strcmp(tok, "small") == 0)   run_case("small", F_SMALLCHUNK);
        else if (strcmp(tok, "reopen") == 0)  run_case("reopen", F_REOPEN);
        else if (strcmp(tok, "multi") == 0)   run_case("multi", F_MULTIFILE);
        else if (strcmp(tok, "usbdst") == 0)  run_case("usbdst", F_USB_DST);
        else if (strcmp(tok, "mmapw") == 0)   run_case("mmapw", F_MMAP);
        else evo_bt("iobench: unknown case '%s'", tok);
    }

    evo_bt("iobench: END");
    free(spec);
    g_bench_running = 0;
    return NULL;
}

void evo_iobench_run(const char *spec)
{
    pthread_t th;
    char     *copy;
    size_t    n;

    if (g_bench_running) {
        evo_bt("iobench: already running");
        return;
    }

    if (!spec || !spec[0])
        spec = "plain,pre,big,prebig,copy,precopy";
    n = strlen(spec) + 1;
    copy = (char *)malloc(n);
    if (!copy)
        return;
    memcpy(copy, spec, n);

    g_bench_running = 1;
    if (pthread_create(&th, NULL, bench_thread, copy) != 0) {
        g_bench_running = 0;
        free(copy);
        evo_bt("iobench: pthread_create failed");
        return;
    }
    pthread_detach(th);
}
