/*
 * evo_ntfs.c - read-only NTFS for drives the console does not mount (#217).
 * See include/evo_ntfs.h for what it does and the URL form.
 *
 * Written for EVO from the NTFS on-disk layout (boot sector, MFT records with
 * update-sequence fixups, attributes, data runs, attribute lists, $I30
 * directory indexes). Only libc calls EVO already imports: open/lseek/read,
 * no pread or ioctl (a missing import is a null call in the app module).
 *
 * Device reads go through one 4 KiB-aligned bounce buffer per volume: a raw
 * disk only accepts sector-aligned reads, and 4 KiB alignment suits both
 * 512-byte and 4K-sector drives.
 */
#include "evo_ntfs.h"
#include "evo_boot_log.h"
#include "evo_elfldr.h"
#include "evo_readdir.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#define DEV_ALIGN    4096u
#define BOUNCE_SIZE  (1024u * 1024u)
#define MAX_VOLS     16
#define MAX_DISKS    8
#define MAX_DEPTH    32          /* index tree depth / path components */
#define ROOT_RECORD  5
#define VOLUME_RECORD 3

enum {
    AT_ATTRIBUTE_LIST = 0x20, AT_FILE_NAME = 0x30, AT_VOLUME_NAME = 0x60, AT_DATA = 0x80,
    AT_INDEX_ROOT = 0x90, AT_INDEX_ALLOCATION = 0xA0, AT_END = 0xFFFFFFFFu,
};
#define ATTR_COMPRESSED 0x0001
#define ATTR_ENCRYPTED  0x4000
#define FN_DIRECTORY    0x10000000u
#define REF_MASK        0x0000FFFFFFFFFFFFull

/* ---- little-endian fields ---- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

/* ---- data runs ---- */
typedef struct { uint64_t vcn, len; int64_t lcn; } run_t;   /* lcn < 0: sparse */
typedef struct { run_t *r; int n, cap; } runs_t;

static int runs_add(runs_t *rs, uint64_t vcn, uint64_t len, int64_t lcn)
{
    if (rs->n == rs->cap) {
        int nc = rs->cap ? rs->cap * 2 : 16;
        run_t *nr = (run_t *)realloc(rs->r, (size_t)nc * sizeof *nr);
        if (!nr) return -1;
        rs->r = nr;
        rs->cap = nc;
    }
    rs->r[rs->n].vcn = vcn;
    rs->r[rs->n].len = len;
    rs->r[rs->n].lcn = lcn;
    rs->n++;
    return 0;
}

static void runs_sort(runs_t *rs)
{
    for (int i = 1; i < rs->n; ++i) {
        run_t t = rs->r[i];
        int j = i - 1;
        while (j >= 0 && rs->r[j].vcn > t.vcn) { rs->r[j + 1] = rs->r[j]; j--; }
        rs->r[j + 1] = t;
    }
}

/* ---- raw devices: a file / device node, or a disk served by the helper ---- */
/*
 * The app sandbox hides USB disk nodes, so on the console a disk is read
 * through helpers/ntfs_helper.c, a root payload on 127.0.0.1 (see
 * evo_ntfs_refresh). Images and the host tests use a plain fd.
 */
typedef struct { int fd, sock; } rawdev_t;

#define HELPER_PORT 9786          /* helpers/ntfs_helper.c, EVO_NTFS_HELPER_PORT */

static int sock_all(int s, void *p, size_t n, int out)
{
    uint8_t *b = (uint8_t *)p;
    while (n) {
        ssize_t r = out ? send(s, b, n, 0) : recv(s, b, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

/* A socket to a loopback port, with 10 s read/write timeouts (a stuck helper
 * must not freeze the browser). -1 when nothing listens. */
static int loopback_connect(int port)
{
    return evo_loopback_connect(port, 10000);
}

static int recv_line(int s, char *out, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        if (sock_all(s, &c, 1, 0) != 0) return -1;
        if (c == '\n') break;
        out[n++] = c;
    }
    out[n] = 0;
    return 0;
}

/* remote: `dev` is a disk name the helper opens ("da1"); else a path. */
static int rawdev_open(const char *dev, int remote, rawdev_t *d)
{
    d->fd = d->sock = -1;
    if (!remote) {
        d->fd = open(dev, O_RDONLY);
        return d->fd >= 0 ? 0 : -1;
    }
    d->sock = loopback_connect(HELPER_PORT);
    if (d->sock < 0) return -1;
    char line[64];
    const int n = snprintf(line, sizeof line, "OPEN %s\n", dev);
    if (sock_all(d->sock, line, (size_t)n, 1) != 0 || recv_line(d->sock, line, sizeof line) != 0 ||
        strcmp(line, "OK") != 0) {
        EVO_LOGW("ntfs", "helper could not open %s (%s)", dev, line);
        close(d->sock);
        d->sock = -1;
        return -1;
    }
    return 0;
}

static void rawdev_close(rawdev_t *d)
{
    if (d->fd >= 0) close(d->fd);
    if (d->sock >= 0) close(d->sock);
    d->fd = d->sock = -1;
}

/* `len` bytes at `off`, both DEV_ALIGN-aligned. Bytes read (short at the end
 * of the device), or -1. */
static long raw_read(rawdev_t *d, uint64_t off, uint8_t *buf, size_t len)
{
    if (d->sock >= 0) {
        uint8_t req[12];
        for (int i = 0; i < 8; ++i) req[i] = (uint8_t)(off >> (8 * i));
        for (int i = 0; i < 4; ++i) req[8 + i] = (uint8_t)(len >> (8 * i));
        uint8_t hdr[4];
        if (sock_all(d->sock, req, sizeof req, 1) != 0 || sock_all(d->sock, hdr, 4, 0) != 0) return -1;
        const int32_t n = (int32_t)rd32(hdr);
        if (n < 0 || (size_t)n > len) return -1;
        return n && sock_all(d->sock, buf, (size_t)n, 0) != 0 ? -1 : n;
    }
    if (lseek(d->fd, (off_t)off, SEEK_SET) != (off_t)off) return -1;
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(d->fd, buf + got, len - got);
        if (r < 0) { if (errno == EINTR) continue; return got ? (long)got : -1; }
        if (r == 0) break;
        got += (size_t)r;
    }
    return (long)got;
}

/* ---- volumes ---- */
typedef struct ntfs_vol {
    char name[24], url[40], label[96], dev[160];
    rawdev_t raw;
    uint64_t base;               /* volume start inside the device */
    uint8_t serial[8];
    uint32_t cluster, rec_size;
    runs_t mft;
    pthread_mutex_t io;
    uint8_t *bounce;
    int refs;                    /* 1 while listed + one per open handle */
    int gone;
    int disk;                    /* /dev/daN index, -1 for an image */
    unsigned id;                 /* unique per mount: keys the folder cache */
} ntfs_vol_t;

static pthread_mutex_t g_reg = PTHREAD_MUTEX_INITIALIZER;
static ntfs_vol_t *g_vols[MAX_VOLS];
static int g_nvols;
static unsigned g_next_vol_id;
static int g_disk_empty[MAX_DISKS];   /* probed, no NTFS: skip until it goes away */
static int g_disk_denied[MAX_DISKS];  /* open() refused: logged once */

static void vol_put(ntfs_vol_t *v)
{
    pthread_mutex_lock(&g_reg);
    const int left = --v->refs;
    pthread_mutex_unlock(&g_reg);
    if (left > 0) return;
    rawdev_close(&v->raw);
    free(v->mft.r);
    free(v->bounce);
    pthread_mutex_destroy(&v->io);
    free(v);
}

/* `len` bytes at volume offset `off`. Past the end of the device reads zero. */
static int dev_read(ntfs_vol_t *v, uint64_t off, void *dst, size_t len)
{
    uint8_t *out = (uint8_t *)dst;
    off += v->base;
    pthread_mutex_lock(&v->io);
    while (len) {
        const uint64_t a0 = off & ~(uint64_t)(DEV_ALIGN - 1);
        const size_t skip = (size_t)(off - a0);
        size_t want = skip + len;
        if (want > BOUNCE_SIZE) want = BOUNCE_SIZE;
        const size_t areq = (want + DEV_ALIGN - 1) & ~(size_t)(DEV_ALIGN - 1);
        const long rr = raw_read(&v->raw, a0, v->bounce, areq);
        if (rr < 0) goto fail;
        const size_t got = (size_t)rr;
        if (got < areq) memset(v->bounce + got, 0, areq - got);
        const size_t take = want - skip;
        memcpy(out, v->bounce + skip, take);
        out += take;
        off += take;
        len -= take;
    }
    pthread_mutex_unlock(&v->io);
    return 0;
fail:
    pthread_mutex_unlock(&v->io);
    return -1;
}

/* Through a run list: `len` bytes at byte `off` of the attribute. *hint keeps
 * the last run used, so a stream read in order finds its run at once. */
static int runs_read(ntfs_vol_t *v, const runs_t *rs, uint64_t off, void *dst, size_t len, int *hint)
{
    uint8_t *out = (uint8_t *)dst;
    int h = hint ? *hint : 0;
    while (len) {
        const uint64_t vcn = off / v->cluster;
        if (h < 0 || h >= rs->n || vcn < rs->r[h].vcn || vcn >= rs->r[h].vcn + rs->r[h].len) {
            int lo = 0, hi = rs->n - 1, found = -1;
            while (lo <= hi) {   /* sorted by vcn */
                const int mid = (lo + hi) / 2;
                if (vcn < rs->r[mid].vcn) hi = mid - 1;
                else if (vcn >= rs->r[mid].vcn + rs->r[mid].len) lo = mid + 1;
                else { found = mid; break; }
            }
            if (found < 0) return -1;
            h = found;
        }
        const run_t *r = &rs->r[h];
        const uint64_t run_off = (vcn - r->vcn) * v->cluster + off % v->cluster;
        const uint64_t avail = r->len * v->cluster - run_off;
        const size_t take = len < avail ? len : (size_t)avail;
        if (r->lcn < 0) memset(out, 0, take);
        else if (dev_read(v, (uint64_t)r->lcn * v->cluster + run_off, out, take) != 0) return -1;
        out += take;
        off += take;
        len -= take;
    }
    if (hint) *hint = h;
    return 0;
}

/* Update-sequence fixups: the last two bytes of every stride were swapped
 * for the sequence number when the record was written. */
static int fixup(uint8_t *b, uint32_t size)
{
    const uint16_t uo = rd16(b + 4), uc = rd16(b + 6);
    if (uc < 2 || (uint32_t)uo + 2u * uc > size) return -1;
    const uint32_t stride = size / (uint32_t)(uc - 1);
    if (stride < 256 || stride * (uint32_t)(uc - 1) != size) return -1;
    const uint16_t usn = rd16(b + uo);
    for (uint32_t i = 1; i < uc; ++i) {
        uint8_t *p = b + i * stride - 2;
        if (rd16(p) != usn) return -1;
        memcpy(p, b + uo + 2 * i, 2);
    }
    return 0;
}

static int decode_runs(const uint8_t *a, uint32_t alen, runs_t *out)
{
    uint64_t vcn = rd64(a + 0x10);
    const uint16_t ro = rd16(a + 0x20);
    if (ro >= alen) return -1;
    const uint8_t *p = a + ro, *e = a + alen;
    int64_t lcn = 0;
    while (p < e && *p) {
        const int ls = *p & 0x0F, os = *p >> 4;
        p++;
        if (ls == 0 || ls > 8 || os > 8 || p + ls + os > e) return -1;
        uint64_t len = 0;
        for (int i = 0; i < ls; ++i) len |= (uint64_t)p[i] << (8 * i);
        p += ls;
        if (os) {
            uint64_t d = 0;
            for (int i = 0; i < os; ++i) d |= (uint64_t)p[i] << (8 * i);
            if (os < 8 && (p[os - 1] & 0x80)) d |= ~0ull << (8 * os);   /* sign-extend */
            lcn += (int64_t)d;
            if (lcn < 0 || runs_add(out, vcn, len, lcn) != 0) return -1;
        } else if (runs_add(out, vcn, len, -1) != 0) {
            return -1;
        }
        p += os;
        vcn += len;
    }
    return 0;
}

static int read_record(ntfs_vol_t *v, uint64_t recno, uint8_t *buf)
{
    if (runs_read(v, &v->mft, recno * v->rec_size, buf, v->rec_size, NULL) != 0) return -1;
    if (memcmp(buf, "FILE", 4) != 0 || fixup(buf, v->rec_size) != 0) return -1;
    if (!(rd16(buf + 0x16) & 1)) return -1;   /* not in use */
    return 0;
}

/* Walks a record's attributes; returns the next one or NULL. */
static const uint8_t *attr_next(const uint8_t *rec, uint32_t size, const uint8_t *a)
{
    a = a ? a + rd32(a + 4) : rec + rd16(rec + 0x14);
    if (a < rec || a + 16 > rec + size) return NULL;
    const uint32_t type = rd32(a), len = rd32(a + 4);
    if (type == AT_END || len < 16 || a + len > rec + size) return NULL;
    return a;
}

/* Attribute name (UTF-16) equal to an ASCII name; NULL means unnamed. */
static int attr_name_is(const uint8_t *a, const char *name)
{
    const int nl = a[9];
    if (!name) return nl == 0;
    if (nl != (int)strlen(name)) return 0;
    const uint8_t *n = a + rd16(a + 10);
    for (int i = 0; i < nl; ++i)
        if (rd16(n + 2 * i) != (uint8_t)name[i]) return 0;
    return 1;
}

typedef struct {
    int found, resident, flags;
    uint8_t *res;
    uint32_t res_len;
    runs_t runs;
    uint64_t size, init_size;
} attr_t;

static void attr_free(attr_t *t) { free(t->res); free(t->runs.r); memset(t, 0, sizeof *t); }

static int attr_merge(attr_t *t, const uint8_t *a)
{
    const uint32_t alen = rd32(a + 4);
    t->found = 1;
    if (!a[8]) {   /* resident */
        const uint32_t vl = rd32(a + 0x10), vo = rd16(a + 0x14);
        if ((uint64_t)vo + vl > alen) return -1;
        free(t->res);
        t->res = (uint8_t *)malloc(vl ? vl : 1);
        if (!t->res) return -1;
        memcpy(t->res, a + vo, vl);
        t->res_len = vl;
        t->resident = 1;
        t->size = t->init_size = vl;
        return 0;
    }
    if (alen < 0x40) return -1;
    if (rd64(a + 0x10) == 0) {   /* first segment carries the sizes */
        t->flags = rd16(a + 0x0C);
        t->size = rd64(a + 0x30);
        t->init_size = rd64(a + 0x38);
    }
    return decode_runs(a, alen, &t->runs);
}

/* The attribute `type`/`name` of record `recno`, all its segments, following
 * an attribute list into extension records. */
static int load_attr(ntfs_vol_t *v, uint64_t recno, uint32_t type, const char *name, attr_t *out)
{
    memset(out, 0, sizeof *out);
    uint8_t *base = (uint8_t *)malloc(v->rec_size), *ext = NULL;
    if (!base) return -1;
    int rc = -1;
    if (read_record(v, recno, base) != 0) goto done;

    const uint8_t *list = NULL, *a = NULL;
    while ((a = attr_next(base, v->rec_size, a)) != NULL)
        if (rd32(a) == AT_ATTRIBUTE_LIST) { list = a; break; }

    if (!list) {
        a = NULL;
        while ((a = attr_next(base, v->rec_size, a)) != NULL)
            if (rd32(a) == type && attr_name_is(a, name) && attr_merge(out, a) != 0) goto done;
        rc = 0;
        goto done;
    }

    /* The list itself: resident, or (large) in runs of its own. */
    attr_t lt;
    memset(&lt, 0, sizeof lt);
    if (attr_merge(&lt, list) != 0) { attr_free(&lt); goto done; }
    uint8_t *lb = lt.res;
    uint64_t ll = lt.res_len;
    if (!lt.resident) {
        ll = lt.size;
        lb = (ll && ll < (16u << 20)) ? (uint8_t *)malloc(ll) : NULL;
        if (!lb || runs_read(v, &lt.runs, 0, lb, (size_t)ll, NULL) != 0) { free(lb); attr_free(&lt); goto done; }
    }
    ext = (uint8_t *)malloc(v->rec_size);
    uint64_t cached = ~0ull;
    int ok = ext != NULL;
    for (uint64_t p = 0; ok && p + 0x1A <= ll;) {
        const uint8_t *e = lb + p;
        const uint16_t elen = rd16(e + 4);
        if (elen < 0x1A || p + elen > ll) break;
        const uint8_t nl = e[6], no = e[7];
        int match = rd32(e) == type && (uint64_t)no + 2u * nl <= elen;
        if (match) {
            if (!name) match = nl == 0;
            else {
                match = nl == strlen(name);
                for (int i = 0; match && i < nl; ++i) match = rd16(e + no + 2 * i) == (uint8_t)name[i];
            }
        }
        if (match) {
            const uint64_t ref = rd64(e + 0x10) & REF_MASK;
            const uint64_t svcn = rd64(e + 8);
            const uint8_t *rec = base;
            if (ref != recno) {
                if (ref != cached) {
                    if (read_record(v, ref, ext) != 0) { ok = 0; break; }
                    cached = ref;
                }
                rec = ext;
            }
            const uint8_t *x = NULL;
            while ((x = attr_next(rec, v->rec_size, x)) != NULL) {
                if (rd32(x) != type || !attr_name_is(x, name)) continue;
                if (x[8] && rd64(x + 0x10) != svcn) continue;
                if (attr_merge(out, x) != 0) ok = 0;
                break;
            }
        }
        p += elen;
    }
    if (!lt.resident) free(lb);
    attr_free(&lt);
    if (ok) {
        runs_sort(&out->runs);
        rc = 0;
    }
done:
    free(ext);
    free(base);
    if (rc != 0) attr_free(out);
    return rc;
}

/* ---- names ---- */
static size_t utf16_to_utf8(const uint8_t *s, int units, char *out, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < units; ++i) {
        uint32_t c = rd16(s + 2 * i);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < units) {
            const uint32_t lo = rd16(s + 2 * (i + 1));
            if (lo >= 0xDC00 && lo <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); ++i; }
        }
        char b[4];
        size_t k;
        if (c < 0x80) { b[0] = (char)c; k = 1; }
        else if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 0x3F)); k = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | ((c >> 6) & 0x3F)); b[2] = (char)(0x80 | (c & 0x3F)); k = 3; }
        else { b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | ((c >> 12) & 0x3F)); b[2] = (char)(0x80 | ((c >> 6) & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F)); k = 4; }
        if (n + k + 1 > cap) break;
        memcpy(out + n, b, k);
        n += k;
    }
    if (cap) out[n] = 0;
    return n;
}

static int name_eq(const char *a, const char *b)   /* NTFS compares names case-insensitively */
{
    for (; *a && *b; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

/* ---- directories ---- */
typedef struct { char *name; int is_dir; uint64_t size, ref; } dent_t;
struct evo_ntfs_dir { ntfs_vol_t *v; dent_t *e; int n, cap, pos; };

typedef struct {
    ntfs_vol_t *v;
    const attr_t *alloc;
    uint32_t block, unit;
    int root;                       /* listing the volume root: hide $ files */
    struct evo_ntfs_dir *out;
} walk_t;

static void emit(walk_t *w, const uint8_t *fn, uint32_t klen, uint64_t ref)
{
    if (klen < 0x42) return;
    const int nl = fn[0x40], ns = fn[0x41];
    if (ns == 2 || 0x42u + 2u * nl > klen || nl == 0) return;   /* DOS 8.3 alias */
    char name[768];
    utf16_to_utf8(fn + 0x42, nl, name, sizeof name);
    if (!strcmp(name, ".") || (w->root && name[0] == '$')) return;
    struct evo_ntfs_dir *d = w->out;
    if (d->n == d->cap) {
        int nc = d->cap ? d->cap * 2 : 64;
        dent_t *ne = (dent_t *)realloc(d->e, (size_t)nc * sizeof *ne);
        if (!ne) return;
        d->e = ne;
        d->cap = nc;
    }
    dent_t *e = &d->e[d->n];
    e->name = strdup(name);
    if (!e->name) return;
    e->is_dir = (rd32(fn + 0x38) & FN_DIRECTORY) != 0;
    e->size = rd64(fn + 0x30);
    e->ref = ref;
    d->n++;
}

/* In-order walk of one index node: children before each key, so the result
 * comes out sorted and only live blocks are read. */
static int walk(walk_t *w, const uint8_t *p, const uint8_t *end, int depth)
{
    if (depth > MAX_DEPTH) return -1;
    while (p + 16 <= end) {
        const uint16_t elen = rd16(p + 8), klen = rd16(p + 10);
        const uint32_t fl = rd16(p + 12);
        if (elen < 16 || p + elen > end) return -1;
        if (fl & 1) {   /* a subnode: its VCN is the entry's last 8 bytes */
            if (!w->alloc || !w->alloc->found || elen < 24) return -1;
            const uint64_t vcn = rd64(p + elen - 8);
            uint8_t *blk = (uint8_t *)malloc(w->block);
            if (!blk) return -1;
            int ok = runs_read(w->v, &w->alloc->runs, vcn * w->unit, blk, w->block, NULL) == 0 &&
                     memcmp(blk, "INDX", 4) == 0 && fixup(blk, w->block) == 0;
            if (ok) {
                const uint8_t *nh = blk + 0x18;
                const uint32_t eo = rd32(nh), il = rd32(nh + 4);
                ok = 0x18u + il <= w->block && eo < il && walk(w, nh + eo, nh + il, depth + 1) == 0;
            }
            free(blk);
            if (!ok) return -1;
        }
        if (fl & 2) break;   /* the last entry has no key */
        if (16u + klen <= elen) emit(w, p + 16, klen, rd64(p) & REF_MASK);
        p += elen;
    }
    return 0;
}

static int list_dir(ntfs_vol_t *v, uint64_t recno, struct evo_ntfs_dir *out)
{
    attr_t root, alloc;
    if (load_attr(v, recno, AT_INDEX_ROOT, "$I30", &root) != 0 || !root.found || !root.resident ||
        root.res_len < 0x20) {
        if (root.found) attr_free(&root);
        return -1;
    }
    if (load_attr(v, recno, AT_INDEX_ALLOCATION, "$I30", &alloc) != 0) memset(&alloc, 0, sizeof alloc);
    walk_t w;
    w.v = v;
    w.alloc = &alloc;
    w.block = rd32(root.res + 8);
    w.unit = w.block >= v->cluster ? v->cluster : 512;
    w.root = recno == ROOT_RECORD;
    w.out = out;
    int rc = -1;
    if (w.block >= 512 && w.block <= 65536) {
        const uint8_t *nh = root.res + 0x10;
        const uint32_t eo = rd32(nh), il = rd32(nh + 4);
        if (0x10u + il <= root.res_len && eo < il) rc = walk(&w, nh + eo, nh + il, 0);
    }
    attr_free(&alloc);
    attr_free(&root);
    return rc;
}

static void dir_free(struct evo_ntfs_dir *d)
{
    for (int i = 0; i < d->n; ++i) free(d->e[i].name);
    free(d->e);
    d->e = NULL;
    d->n = d->cap = 0;
}

/* Folder listings, cached. Moving the cursor in the browser looks up the
 * selected file (its size) and its folder again and again; on a drive read
 * through the helper each lookup was several USB reads on the UI thread - a
 * visible stall per press. EVO never writes these volumes, so a listing stays
 * right until the drive is unplugged (a new mount gets a new id). */
#define DCACHE_SLOTS 16
#define DCACHE_SECS  120
typedef struct { unsigned vol; uint64_t rec; time_t at; dent_t *e; int n; } dcache_t;
static dcache_t g_dc[DCACHE_SLOTS];
static pthread_mutex_t g_dcm = PTHREAD_MUTEX_INITIALIZER;

static int dents_copy(const dent_t *src, int n, struct evo_ntfs_dir *out)
{
    out->e = (dent_t *)calloc(n ? (size_t)n : 1, sizeof *out->e);
    if (!out->e) return -1;
    out->cap = n;
    for (int i = 0; i < n; ++i) {
        out->e[i] = src[i];
        out->e[i].name = strdup(src[i].name);
        if (!out->e[i].name) { out->n = i; return -1; }
    }
    out->n = n;
    return 0;
}

static int list_dir_cached(ntfs_vol_t *v, uint64_t recno, struct evo_ntfs_dir *out)
{
    const time_t now = time(NULL);
    pthread_mutex_lock(&g_dcm);
    for (int i = 0; i < DCACHE_SLOTS; ++i) {
        dcache_t *c = &g_dc[i];
        if (c->e && c->vol == v->id && c->rec == recno && now - c->at < DCACHE_SECS) {
            const int rc = dents_copy(c->e, c->n, out);
            pthread_mutex_unlock(&g_dcm);
            return rc;
        }
    }
    pthread_mutex_unlock(&g_dcm);

    if (list_dir(v, recno, out) != 0) return -1;

    struct evo_ntfs_dir copy;
    memset(&copy, 0, sizeof copy);
    if (dents_copy(out->e, out->n, &copy) != 0) { dir_free(&copy); return 0; }
    pthread_mutex_lock(&g_dcm);
    int slot = 0;
    for (int i = 1; i < DCACHE_SLOTS; ++i)
        if (!g_dc[i].e || g_dc[i].at < g_dc[slot].at) slot = i;
    if (g_dc[slot].e) {
        for (int i = 0; i < g_dc[slot].n; ++i) free(g_dc[slot].e[i].name);
        free(g_dc[slot].e);
    }
    g_dc[slot].vol = v->id;
    g_dc[slot].rec = recno;
    g_dc[slot].at = now;
    g_dc[slot].e = copy.e;
    g_dc[slot].n = copy.n;
    pthread_mutex_unlock(&g_dcm);
    return 0;
}

/* ---- URLs and paths ---- */
int evo_ntfs_is_url(const char *path)
{
    return path && strncmp(path, EVO_NTFS_SCHEME, sizeof EVO_NTFS_SCHEME - 1) == 0;
}

/* The volume of a URL, with a reference held; *rest = the path inside it. */
static ntfs_vol_t *vol_of(const char *url, const char **rest)
{
    if (!evo_ntfs_is_url(url)) return NULL;
    const char *p = url + sizeof EVO_NTFS_SCHEME - 1;
    const char *slash = strchr(p, '/');
    const size_t nl = slash ? (size_t)(slash - p) : strlen(p);
    ntfs_vol_t *v = NULL;
    pthread_mutex_lock(&g_reg);
    for (int i = 0; i < g_nvols; ++i)
        if (strlen(g_vols[i]->name) == nl && !strncmp(g_vols[i]->name, p, nl)) {
            v = g_vols[i];
            v->refs++;
            break;
        }
    pthread_mutex_unlock(&g_reg);
    *rest = slash ? slash + 1 : "";
    return v;
}

/* Record number of a path inside the volume ("" = root). */
static int resolve(ntfs_vol_t *v, const char *path, uint64_t *rec, int *is_dir, uint64_t *size)
{
    uint64_t cur = ROOT_RECORD;
    int dir = 1;
    uint64_t sz = 0;
    char part[768];
    int depth = 0;
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        if (!dir || ++depth > MAX_DEPTH) return -1;
        const char *s = strchr(path, '/');
        const size_t n = s ? (size_t)(s - path) : strlen(path);
        if (n >= sizeof part) return -1;
        memcpy(part, path, n);
        part[n] = 0;
        path += n;
        struct evo_ntfs_dir d;
        memset(&d, 0, sizeof d);
        if (list_dir_cached(v, cur, &d) != 0) { dir_free(&d); return -1; }
        int found = 0;
        for (int i = 0; i < d.n && !found; ++i)
            if (name_eq(d.e[i].name, part)) {
                cur = d.e[i].ref;
                dir = d.e[i].is_dir;
                sz = d.e[i].size;
                found = 1;
            }
        dir_free(&d);
        if (!found) return -1;
    }
    *rec = cur;
    if (is_dir) *is_dir = dir;
    if (size) *size = sz;
    return 0;
}

evo_ntfs_dir_t *evo_ntfs_opendir(const char *url)
{
    const char *path;
    ntfs_vol_t *v = vol_of(url, &path);
    if (!v) return NULL;
    uint64_t rec;
    int is_dir = 0;
    evo_ntfs_dir_t *d = (evo_ntfs_dir_t *)calloc(1, sizeof *d);
    if (!d || resolve(v, path, &rec, &is_dir, NULL) != 0 || !is_dir || list_dir_cached(v, rec, d) != 0) {
        if (d) { dir_free(d); free(d); }
        vol_put(v);
        return NULL;
    }
    d->v = v;
    return d;
}

int evo_ntfs_readdir(evo_ntfs_dir_t *d, const char **name, int *is_dir, uint64_t *size)
{
    if (!d || d->pos >= d->n) return 0;
    const dent_t *e = &d->e[d->pos++];
    if (name) *name = e->name;
    if (is_dir) *is_dir = e->is_dir;
    if (size) *size = e->size;
    return 1;
}

void evo_ntfs_closedir(evo_ntfs_dir_t *d)
{
    if (!d) return;
    dir_free(d);
    if (d->v) vol_put(d->v);
    free(d);
}

/* ---- files ---- */
struct evo_ntfs_file { ntfs_vol_t *v; attr_t data; int hint; };

evo_ntfs_file_t *evo_ntfs_open(const char *url)
{
    const char *path;
    ntfs_vol_t *v = vol_of(url, &path);
    if (!v) return NULL;
    uint64_t rec;
    int is_dir = 1;
    evo_ntfs_file_t *f = (evo_ntfs_file_t *)calloc(1, sizeof *f);
    if (!f || resolve(v, path, &rec, &is_dir, NULL) != 0 || is_dir ||
        load_attr(v, rec, AT_DATA, NULL, &f->data) != 0 || !f->data.found) {
        if (f) { attr_free(&f->data); free(f); }
        vol_put(v);
        return NULL;
    }
    if (f->data.flags & (ATTR_COMPRESSED | ATTR_ENCRYPTED)) {
        EVO_LOGW("ntfs", "%s is %s - not supported", url,
                 (f->data.flags & ATTR_ENCRYPTED) ? "encrypted" : "compressed");
        attr_free(&f->data);
        free(f);
        vol_put(v);
        return NULL;
    }
    f->v = v;
    return f;
}

uint64_t evo_ntfs_size(const evo_ntfs_file_t *f) { return f ? f->data.size : 0; }

long evo_ntfs_read(evo_ntfs_file_t *f, void *buf, size_t n, uint64_t off)
{
    if (!f) return -1;
    const uint64_t size = f->data.size;
    if (off >= size || n == 0) return 0;
    if (n > size - off) n = (size_t)(size - off);
    if (f->data.resident) {
        memcpy(buf, f->data.res + off, n);
        return (long)n;
    }
    /* Past the initialized size a file reads as zeros. */
    size_t real = n;
    if (off + n > f->data.init_size) real = off >= f->data.init_size ? 0 : (size_t)(f->data.init_size - off);
    if (real && runs_read(f->v, &f->data.runs, off, buf, real, &f->hint) != 0) return -1;
    if (real < n) memset((uint8_t *)buf + real, 0, n - real);
    return (long)n;
}

void evo_ntfs_close(evo_ntfs_file_t *f)
{
    if (!f) return;
    attr_free(&f->data);
    if (f->v) vol_put(f->v);
    free(f);
}

int evo_ntfs_read_file(const char *url, unsigned char **out, size_t *len, size_t max)
{
    *out = NULL;
    *len = 0;
    evo_ntfs_file_t *f = evo_ntfs_open(url);
    if (!f) return -1;
    const uint64_t size = evo_ntfs_size(f);
    unsigned char *buf = size <= max ? (unsigned char *)malloc(size ? (size_t)size : 1) : NULL;
    long got = buf ? evo_ntfs_read(f, buf, (size_t)size, 0) : -1;
    evo_ntfs_close(f);
    if (!buf || got != (long)size) {
        free(buf);
        return -1;
    }
    *out = buf;
    *len = (size_t)size;
    return 0;
}

int evo_ntfs_stat(const char *url, int *is_dir, uint64_t *size)
{
    const char *path;
    ntfs_vol_t *v = vol_of(url, &path);
    if (!v) return -1;
    uint64_t rec, sz = 0;
    int dir = 0;
    int rc = resolve(v, path, &rec, &dir, &sz);
    if (rc == 0 && !dir) {   /* the index's size can be stale; $DATA is not */
        attr_t a;
        if (load_attr(v, rec, AT_DATA, NULL, &a) == 0 && a.found) sz = a.size;
        attr_free(&a);
    }
    vol_put(v);
    if (rc != 0) return -1;
    if (is_dir) *is_dir = dir;
    if (size) *size = dir ? 0 : sz;
    return 0;
}

/* ---- finding volumes ---- */

/* One aligned 4 KiB block of a device into buf. */
static int raw_block(rawdev_t *d, uint64_t off, uint8_t *buf)
{
    return raw_read(d, off, buf, DEV_ALIGN) >= 512 ? 0 : -1;
}

/* The 512-byte sector at `off` (any alignment). */
static int raw_sector(rawdev_t *fd, uint64_t off, uint8_t *sec, uint8_t *tmp)
{
    const uint64_t a0 = off & ~(uint64_t)(DEV_ALIGN - 1);
    if (raw_block(fd, a0, tmp) != 0 || off - a0 + 512 > DEV_ALIGN) return -1;
    memcpy(sec, tmp + (off - a0), 512);
    return 0;
}

static int is_pow2(uint64_t x) { return x && !(x & (x - 1)); }

/* An NTFS boot sector at `base`: fills the geometry. */
static int probe_boot(rawdev_t *fd, uint64_t base, uint8_t *tmp, uint32_t *cluster, uint32_t *rec,
                      uint64_t *mft_lcn, uint8_t serial[8])
{
    uint8_t b[512];
    if (raw_sector(fd, base, b, tmp) != 0) return -1;
    if (memcmp(b + 3, "NTFS    ", 8) != 0 || b[510] != 0x55 || b[511] != 0xAA) return -1;
    const uint32_t bps = rd16(b + 0x0B);
    const uint8_t spc = b[0x0D];
    const uint32_t spcv = spc > 0x80 ? 1u << (256 - spc) : spc;
    const uint64_t cl = (uint64_t)bps * spcv;
    if (!is_pow2(bps) || bps < 256 || bps > 4096 || !is_pow2(cl) || cl > (2u << 20)) return -1;
    const int8_t c = (int8_t)b[0x40];
    const uint64_t rs = c < 0 ? (1ull << -c) : (uint64_t)c * cl;
    if (!is_pow2(rs) || rs < 256 || rs > 65536) return -1;
    *cluster = (uint32_t)cl;
    *rec = (uint32_t)rs;
    *mft_lcn = rd64(b + 0x30);
    memcpy(serial, b + 0x48, 8);
    return 0;
}

static ntfs_vol_t *mount_volume(const char *dev, int remote, uint64_t base, const char *name, int disk)
{
    ntfs_vol_t *v = (ntfs_vol_t *)calloc(1, sizeof *v);
    if (!v) return NULL;
    const int opened = rawdev_open(dev, remote, &v->raw);
    v->bounce = (uint8_t *)malloc(BOUNCE_SIZE + DEV_ALIGN);
    pthread_mutex_init(&v->io, NULL);
    v->refs = 1;
    v->disk = disk;
    v->id = ++g_next_vol_id;
    v->base = base;
    snprintf(v->dev, sizeof v->dev, "%s", dev);
    snprintf(v->name, sizeof v->name, "%s", name);
    snprintf(v->url, sizeof v->url, "%s%s", EVO_NTFS_SCHEME, name);
    uint64_t mft_lcn = 0;
    uint8_t *rec = NULL;
    if (opened != 0 || !v->bounce ||
        probe_boot(&v->raw, base, v->bounce, &v->cluster, &v->rec_size, &mft_lcn, v->serial) != 0)
        goto fail;

    /* $MFT's own runs: record 0 sits at the MFT start; its $DATA maps the rest. */
    rec = (uint8_t *)malloc(v->rec_size);
    if (!rec || dev_read(v, mft_lcn * v->cluster, rec, v->rec_size) != 0 || memcmp(rec, "FILE", 4) != 0 ||
        fixup(rec, v->rec_size) != 0)
        goto fail;
    const uint8_t *a = NULL;
    int has_list = 0;
    while ((a = attr_next(rec, v->rec_size, a)) != NULL) {
        if (rd32(a) == AT_ATTRIBUTE_LIST) has_list = 1;
        if (rd32(a) == AT_DATA && attr_name_is(a, NULL) && a[8] && decode_runs(a, rd32(a + 4), &v->mft) != 0)
            goto fail;
    }
    runs_sort(&v->mft);
    if (!v->mft.n) goto fail;
    if (has_list) {   /* a fragmented MFT: the rest of its runs are in extension records */
        attr_t m;
        if (load_attr(v, 0, AT_DATA, NULL, &m) == 0 && m.found && m.runs.n) {
            free(v->mft.r);
            v->mft = m.runs;
            m.runs.r = NULL;
        }
        attr_free(&m);
    }
    free(rec);
    rec = NULL;

    /* Root must list, or this is not a volume we can read. */
    struct evo_ntfs_dir d;
    memset(&d, 0, sizeof d);
    if (list_dir(v, ROOT_RECORD, &d) != 0) { dir_free(&d); goto fail; }
    dir_free(&d);

    attr_t lab;
    if (load_attr(v, VOLUME_RECORD, AT_VOLUME_NAME, NULL, &lab) == 0 && lab.found && lab.resident && lab.res_len >= 2)
        utf16_to_utf8(lab.res, (int)(lab.res_len / 2), v->label, sizeof v->label);
    attr_free(&lab);
    if (!v->label[0]) snprintf(v->label, sizeof v->label, "%s", name);
    EVO_LOGI("ntfs", "mounted %s (%s) from %s at %llu: %u-byte clusters, %u-byte records", v->url, v->label,
             dev, (unsigned long long)base, v->cluster, v->rec_size);
    return v;
fail:
    free(rec);
    vol_put(v);
    return NULL;
}

static int add_vol(ntfs_vol_t *v)
{
    pthread_mutex_lock(&g_reg);
    int ok = g_nvols < MAX_VOLS;
    if (ok) g_vols[g_nvols++] = v;
    pthread_mutex_unlock(&g_reg);
    if (!ok) vol_put(v);
    return ok;
}

static int add_device_disk(const char *path, const char *name, int disk, int remote)
{
    rawdev_t rd;
    if (rawdev_open(path, remote, &rd) != 0) return -1;
    rawdev_t *fd = &rd;
    uint8_t *tmp = (uint8_t *)malloc(DEV_ALIGN), *ents = NULL;
    uint8_t s0[512], hdr[512];
    int added = 0;
    uint32_t cl, rs;
    uint64_t lcn;
    uint8_t ser[8];
    char vname[24];
    if (!tmp || raw_sector(fd, 0, s0, tmp) != 0) goto done;

    if (probe_boot(fd, 0, tmp, &cl, &rs, &lcn, ser) == 0) {   /* a volume on the whole disk */
        ntfs_vol_t *v = mount_volume(path, remote, 0, name, disk);
        if (v) added += add_vol(v);
        goto done;
    }
    if (s0[510] != 0x55 || s0[511] != 0xAA) goto done;
    int gpt = 0;
    for (int i = 0; i < 4; ++i) {
        const uint8_t *e = s0 + 0x1BE + 16 * i;
        if (e[4] == 0xEE) gpt = 1;
        if (e[4] != 0x07) continue;   /* NTFS / exFAT: the boot sector says which */
        const uint64_t base = (uint64_t)rd32(e + 8) * 512;
        if (probe_boot(fd, base, tmp, &cl, &rs, &lcn, ser) != 0) continue;
        snprintf(vname, sizeof vname, "%sp%d", name, i + 1);
        ntfs_vol_t *v = mount_volume(path, remote, base, vname, disk);
        if (v) added += add_vol(v);
    }
    if (gpt) {
        static const uint32_t kLba[2] = {512, 4096};
        for (int k = 0; k < 2; ++k) {
            const uint32_t lb = kLba[k];
            if (raw_sector(fd, lb, hdr, tmp) != 0 || memcmp(hdr, "EFI PART", 8) != 0) continue;
            const uint64_t elba = rd64(hdr + 0x48);
            uint32_t count = rd32(hdr + 0x50);
            const uint32_t esz = rd32(hdr + 0x54);
            if (esz < 128 || esz > 1024 || count == 0) break;
            if (count > 128) count = 128;
            const size_t bytes = (size_t)count * esz;
            ents = (uint8_t *)malloc(bytes + DEV_ALIGN);
            if (!ents) break;
            for (size_t off = 0; off < bytes; off += 512)
                if (raw_sector(fd, elba * lb + off, ents + off, tmp) != 0) { count = (uint32_t)(off / esz); break; }
            for (uint32_t i = 0; i < count; ++i) {
                const uint8_t *e = ents + (size_t)i * esz;
                int used = 0;
                for (int j = 0; j < 16; ++j) used |= e[j];
                if (!used) continue;
                const uint64_t base = rd64(e + 32) * lb;
                if (probe_boot(fd, base, tmp, &cl, &rs, &lcn, ser) != 0) continue;
                snprintf(vname, sizeof vname, "%sp%u", name, i + 1);
                ntfs_vol_t *v = mount_volume(path, remote, base, vname, disk);
                if (v) added += add_vol(v);
            }
            break;
        }
    }
done:
    free(ents);
    free(tmp);
    rawdev_close(&rd);
    return added;
}

int evo_ntfs_add_device(const char *path, const char *name)
{
    const int n = add_device_disk(path, name, -1, 0);
    return n < 0 ? 0 : n;
}

/* Drops the volumes of disk `disk` (or of image `name` when disk < 0). */
static void drop_disk(int disk)
{
    ntfs_vol_t *gone[MAX_VOLS];
    int ng = 0;
    pthread_mutex_lock(&g_reg);
    for (int i = 0; i < g_nvols;) {
        if (g_vols[i]->disk == disk) {
            gone[ng++] = g_vols[i];
            g_vols[i] = g_vols[--g_nvols];
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&g_reg);
    for (int i = 0; i < ng; ++i) {
        EVO_LOGI("ntfs", "%s gone (drive removed)", gone[i]->url);
        vol_put(gone[i]);
    }
}

static int disk_listed(int disk)
{
    int any = 0;
    pthread_mutex_lock(&g_reg);
    for (int i = 0; i < g_nvols; ++i) any |= g_vols[i]->disk == disk;
    pthread_mutex_unlock(&g_reg);
    return any;
}

/* Once per run: which disk nodes this process can see. The app sandbox has
 * its own view of /dev, and a disk the system lists may not be in it. */
static void log_dev_view(void)
{
    static int done;
    if (done) return;
    done = 1;
    evo_dir_t *d = evo_opendir("/dev");
    if (!d) {
        EVO_LOGW("ntfs", "cannot list /dev (errno %d)", errno);
        return;
    }
    char seen[256];
    size_t n = 0;
    seen[0] = 0;
    struct dirent *de;
    while ((de = evo_readdir(d)) != NULL) {
        if (strncmp(de->d_name, "da", 2) && strncmp(de->d_name, "ugen", 4) && strncmp(de->d_name, "usb", 3)) continue;
        const int w = snprintf(seen + n, sizeof seen - n, "%s%s", n ? " " : "", de->d_name);
        if (w < 0 || (size_t)w >= sizeof seen - n) break;
        n += (size_t)w;
    }
    evo_closedir(d);
    EVO_LOGI("ntfs", "/dev as EVO sees it: %s", n ? seen : "(no da/ugen/usb nodes)");
}

#ifdef EVO_APP_MODULE
/* ---- disks through the helper payload (helpers/ntfs_helper.c) ---- */
/*
 * EVO's /dev has no disk nodes (measured 2026-11-03: usbctl, usb, ugen* only,
 * while a root payload sees da1 and da1s1). So the helper, embedded in EVO,
 * is sent to the ELF loader on 127.0.0.1:9021 when the set of USB devices
 * changes; it lists the disks and serves read-only blocks on 127.0.0.1. It
 * exits 30 s after EVO stops talking to it. Remote volumes use disk ids
 * REMOTE_DISK + N for daN.
 */
extern const unsigned char evo_ntfs_helper_elf[];
extern const size_t evo_ntfs_helper_elf_len;

#define ELF_LOADER_PORT EVO_ELFLDR_PORT
#define REMOTE_DISK    100
#define HELPER_TRIES    3          /* starts per change of the USB device set */

static int g_remote_empty[MAX_DISKS];

static int helper_start(void)
{
    return evo_elfldr_send(evo_ntfs_helper_elf, evo_ntfs_helper_elf_len, 10000);
}

static int helper_list(char *out, size_t cap)
{
    int s = loopback_connect(HELPER_PORT);
    if (s < 0) return -1;
    const int rc = (sock_all(s, "LIST\n", 5, 1) == 0 && recv_line(s, out, cap) == 0) ? 0 : -1;
    close(s);
    return rc;
}

/* The USB devices EVO can see (ugen nodes): plugging or pulling a drive
 * changes it, which is when the disks are listed again. */
static void usb_signature(char *out, size_t cap)
{
    size_t n = 0;
    out[0] = 0;
    evo_dir_t *d = evo_opendir("/dev");
    if (!d) return;
    struct dirent *de;
    while ((de = evo_readdir(d)) != NULL) {
        if (strncmp(de->d_name, "ugen", 4)) continue;
        const int w = snprintf(out + n, cap - n, "%s ", de->d_name);
        if (w < 0 || (size_t)w >= cap - n) break;
        n += (size_t)w;
    }
    evo_closedir(d);
}

static void refresh_remote(void)
{
    static char s_sig[256];
    static int s_first = 1, s_pending, s_tries;
    static time_t s_started;
    char sig[256], list[512];
    usb_signature(sig, sizeof sig);
    const int changed = strcmp(sig, s_sig) != 0;
    if (!s_first && !s_pending && !changed) return;
    if (changed) s_tries = 0;

    if (helper_list(list, sizeof list) != 0) {
        const time_t now = time(NULL);
        if (s_tries < HELPER_TRIES && (!s_pending || now - s_started >= 5)) {
            s_tries++;
            s_started = now;
            if (helper_start() == 0)
                EVO_LOGI("ntfs", "disk helper sent to the ELF loader (%zu bytes)", evo_ntfs_helper_elf_len);
            else
                EVO_LOGW("ntfs", "no ELF loader on 127.0.0.1:%d - NTFS drives cannot be read", ELF_LOADER_PORT);
        }
        /* Try again on the next refresh; after HELPER_TRIES, only when the
         * USB devices change. */
        s_pending = s_tries < HELPER_TRIES;
        if (!s_pending) { snprintf(s_sig, sizeof s_sig, "%s", sig); s_first = 0; }
        return;
    }
    s_pending = 0;
    s_first = 0;
    snprintf(s_sig, sizeof s_sig, "%s", sig);

    int present[MAX_DISKS] = {0};
    for (const char *p = list; *p;) {
        while (*p == ' ') p++;
        if (p[0] == 'd' && p[1] == 'a' && p[2] >= '0' && p[2] <= '9') {
            const int n = atoi(p + 2);
            if (n >= 0 && n < MAX_DISKS) present[n] = 1;
        }
        while (*p && *p != ' ') p++;
    }
    for (int n = 0; n < MAX_DISKS; ++n) {
        if (!present[n]) {
            if (disk_listed(REMOTE_DISK + n)) drop_disk(REMOTE_DISK + n);
            g_remote_empty[n] = 0;
            continue;
        }
        if (disk_listed(REMOTE_DISK + n) || g_remote_empty[n]) continue;
        char name[16];
        snprintf(name, sizeof name, "da%d", n);
        const int added = add_device_disk(name, name, REMOTE_DISK + n, 1);
        g_remote_empty[n] = added <= 0;
        EVO_LOGI("ntfs", "%s through the helper: %d NTFS volume%s", name, added > 0 ? added : 0, added == 1 ? "" : "s");
    }
}
#endif

int evo_ntfs_refresh(void)
{
    char path[32], name[16];
    log_dev_view();
    for (int n = 0; n < MAX_DISKS; ++n) {
        snprintf(path, sizeof path, "/dev/da%d", n);
        int fd = open(path, O_RDONLY);
        const int err = fd < 0 ? errno : 0;
        if (fd < 0 && err == ENOENT) {   /* nothing on this port (any more) */
            if (disk_listed(n)) drop_disk(n);
            g_disk_empty[n] = 0;
            g_disk_denied[n] = 0;
            continue;
        }
        if (fd >= 0) close(fd);
        if (disk_listed(n) || g_disk_empty[n]) continue;
        snprintf(name, sizeof name, "da%d", n);
        int added = fd >= 0 ? add_device_disk(path, name, n, 0) : 0;
        if (fd < 0 && !g_disk_denied[n]) {
            g_disk_denied[n] = 1;
            EVO_LOGW("ntfs", "%s: open failed (errno %d) - trying its partition nodes", path, err);
        }
        if (added <= 0) {
            /* The system's own partition nodes (daNs1 for MBR, daNp1 for GPT):
             * a volume starts at their offset 0. */
            static const char *const kSuffix[] = {"s1", "s2", "s3", "s4", "p1", "p2", "p3", "p4"};
            for (size_t k = 0; k < sizeof kSuffix / sizeof kSuffix[0]; ++k) {
                char ppath[40], pname[24];
                snprintf(ppath, sizeof ppath, "/dev/da%d%s", n, kSuffix[k]);
                snprintf(pname, sizeof pname, "da%d%s", n, kSuffix[k]);
                int pfd = open(ppath, O_RDONLY);
                if (pfd < 0) {
                    if (errno != ENOENT) EVO_LOGW("ntfs", "%s: open failed (errno %d)", ppath, errno);
                    continue;
                }
                close(pfd);
                const int got = add_device_disk(ppath, pname, n, 0);
                if (got > 0) added += got;
            }
        }
        g_disk_empty[n] = added <= 0;
        if (added <= 0) EVO_LOGI("ntfs", "%s: no NTFS volume", path);
    }
#ifdef EVO_APP_MODULE
    refresh_remote();
#endif
#ifdef EVO_USB_REMOTE
    /* Development: an NTFS image stands in for a drive (tools/make_ntfs_image.sh). */
    static int s_img_tried;
    if (!s_img_tried) {
        s_img_tried = 1;
        add_device_disk("/data/evoplayer/test/ntfs.img", "img0", MAX_DISKS, 0);
    }
#endif
    return evo_ntfs_volume_count();
}

int evo_ntfs_volume_count(void)
{
    pthread_mutex_lock(&g_reg);
    const int n = g_nvols;
    pthread_mutex_unlock(&g_reg);
    return n;
}

const char *evo_ntfs_volume_url(int i)
{
    pthread_mutex_lock(&g_reg);
    const char *u = (i >= 0 && i < g_nvols) ? g_vols[i]->url : NULL;
    pthread_mutex_unlock(&g_reg);
    return u;
}

const char *evo_ntfs_volume_label(int i)
{
    pthread_mutex_lock(&g_reg);
    const char *l = (i >= 0 && i < g_nvols) ? g_vols[i]->label : NULL;
    pthread_mutex_unlock(&g_reg);
    return l;
}
