/* evo_playlist_sniff.c - see evo_playlist_sniff.h. */
#include "evo_playlist_sniff.h"

#include <string.h>

/* memmem is not in every libc this builds against; the buffer is a few KB. */
static int has(const char *buf, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    if (m == 0 || n < m) return 0;
    for (size_t i = 0; i + m <= n; ++i)
        if (buf[i] == needle[0] && memcmp(buf + i, needle, m) == 0)
            return 1;
    return 0;
}

int evo_playlist_sniff_is_channel_list(const char *buf, size_t n)
{
    if (!buf || n < 7) return 0;

    size_t i = 0;
    if (n >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF)
        i = 3;                                         /* UTF-8 byte order mark */
    while (i < n && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n'))
        i++;
    if (n - i < 7 || memcmp(buf + i, "#EXTM3U", 7) != 0) return 0;

    if (has(buf + i, n - i, "#EXT-X-")) return 0;      /* HLS: FFmpeg plays it */
    return has(buf + i, n - i, "#EXTINF");
}
