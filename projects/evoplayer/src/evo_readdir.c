/*
 * Module: evo_readdir - see include/evo_readdir.h.
 */

#include "evo_readdir.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef EVO_APP_MODULE

#ifndef O_DIRECTORY
#define O_DIRECTORY 0x00020000
#endif

/* FreeBSD getdents(2), wrapped by libc with no fstatfs/malloc/fdopendir in the
 * way - the layer that survives the app sandbox (probe, 2026-09-02). Declared
 * here because <dirent.h> only exposes it under _WANT_ macros on some SDKs. */
int getdents(int fd, char *buf, int nbytes);

struct evo_dir {
    /* buf first so it keeps calloc()'s alignment - getdents wants the
     * struct dirent records it writes to be naturally aligned. */
    char buf[8192];
    int  fd;
    int  len;   /* valid bytes in buf */
    int  pos;   /* cursor into buf    */
};

evo_dir_t *evo_opendir(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return NULL;

    evo_dir_t *d = calloc(1, sizeof *d);
    if (!d) {
        int e = errno;
        close(fd);
        errno = e;
        return NULL;
    }
    d->fd = fd;
    return d;
}

struct dirent *evo_readdir(evo_dir_t *d)
{
    if (!d)
        return NULL;

    for (;;) {
        if (d->pos >= d->len) {
            int n = getdents(d->fd, d->buf, (int)sizeof d->buf);
            if (n <= 0)
                return NULL;   /* end of directory, or error */
            d->len = n;
            d->pos = 0;
        }

        struct dirent *de = (struct dirent *)(d->buf + d->pos);
        if (de->d_reclen == 0)
            return NULL;       /* malformed block - stop rather than spin */
        d->pos += de->d_reclen;

        /* getdents can leave empty slots (d_type == 0 && d_namlen == 0) in the
         * stream; readdir() hides them. Real entries always have a name. */
        if (de->d_namlen == 0)
            continue;

        return de;
    }
}

void evo_closedir(evo_dir_t *d)
{
    if (!d)
        return;
    if (d->fd >= 0)
        close(d->fd);
    free(d);
}

#else /* !EVO_APP_MODULE - plain libc, unsandboxed elfldr payload */

struct evo_dir {
    DIR *dir;
};

evo_dir_t *evo_opendir(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir)
        return NULL;

    evo_dir_t *d = calloc(1, sizeof *d);
    if (!d) {
        int e = errno;
        closedir(dir);
        errno = e;
        return NULL;
    }
    d->dir = dir;
    return d;
}

struct dirent *evo_readdir(evo_dir_t *d)
{
    return d ? readdir(d->dir) : NULL;
}

void evo_closedir(evo_dir_t *d)
{
    if (!d)
        return;
    if (d->dir)
        closedir(d->dir);
    free(d);
}

#endif /* EVO_APP_MODULE */
