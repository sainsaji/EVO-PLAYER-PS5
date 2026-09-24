/*
 * provider_iptv.c — the IPTV provider (#90).
 *
 * This is the provider the seam is proven against, and it was chosen for that
 * job because it is the least forgiving of the five: its catalog is one flat
 * text file that can hold ten thousand rows, its items are live streams with
 * no duration and no seek, and it has no API to hide behind - a bad M3U line
 * is the provider's whole problem to solve.
 *
 * WHAT IT DOES
 *
 *   catalog   an M3U/M3U8 playlist, fetched once and held in memory. The
 *             #EXTINF `group-title` attribute becomes a folder, so a 5000-line
 *             playlist browses as thirty groups rather than one endless list.
 *   search    substring over channel names, case-insensitive.
 *   resolve   a channel id becomes its URL, marked live.
 *   EPG       optional XMLTV now-and-next. Two strings per channel. A full
 *             grid is explicitly out of #90's scope.
 *
 * WHAT IT DOES NOT DO
 *
 * It draws nothing. The channel grid the user sees is assets/providers/iptv/,
 * fetched at runtime and bound to the data model this file fills - that is the
 * entire point of the exercise. If this file ever grows a layout decision,
 * the seam has failed.
 *
 * CONFIG
 *
 * iptv.conf in the store, key=value, following emby.conf's shape:
 *
 *   playlist=http://192.168.0.5:8099/iptv.m3u
 *   xmltv=http://192.168.0.5:8099/epg.xml
 *   bundle=http://192.168.0.5:8099/ui/iptv
 *
 * There is deliberately no default playlist. A hardcoded LAN literal is what
 * addon_emby.c shipped with and it was wrong there too: it makes a provider
 * look configured when it is not, and the first thing the user sees is a
 * timeout against somebody else's router.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "evo_provider.h"
#include "evo_net.h"
#include "evo_data_path.h"

#define IPTV_CONF "iptv.conf"

/*
 * USB fallback, same as addon_emby.c's EMBY_CONF_USB and for the same reason:
 * the console's FTP server serves /mnt/usb0, and /data is behind the
 * self-unjail. Dropping a config file on the USB stick is the only way to
 * configure a provider on hardware without a settings screen, and the settings
 * screens are per-provider stories that come after this one.
 *
 * /data wins when it exists, so once a setup screen writes there this is
 * ignored.
 */
#define IPTV_CONF_USB "/mnt/usb0/.evo_iptv.conf"

/*
 * A playlist row. Kept narrow on purpose: this is multiplied by the channel
 * count, and a 10000-channel playlist at evo_provider_item_t's ~1.5 KB would
 * be 15 MB of catalog for a screen that shows twelve rows at a time. The
 * item struct is filled in on demand, per page, from these.
 */
typedef struct channel {
    char  *name;       /* heap, never NULL after a successful parse */
    char  *url;        /* heap */
    char  *group;      /* heap, "" when the row had no group-title */
    char  *logo;       /* heap, "" when it had no tvg-logo */
    char  *tvg_id;     /* heap, "" when it had no tvg-id; the EPG key */
    char  *now;        /* heap or NULL - filled by the XMLTV pass */
    char  *next;       /* heap or NULL */
} channel_t;

typedef struct group {
    char name[96];
    int  first;        /* index of the first channel in this group */
    int  count;
} group_t;

static struct {
    char  playlist_url[EVO_PROVIDER_MAX_URL];
    char  xmltv_url[EVO_PROVIDER_MAX_URL];
    char  bundle_url[EVO_PROVIDER_MAX_URL];

    channel_t *ch;
    int        ch_count;
    int        ch_cap;

    group_t   *gr;
    int        gr_count;
    int        gr_cap;

    int   loaded;          /* a playlist has been parsed                  */
    int   loading;         /* a fetch is in flight                        */
    int   epg_loaded;
} G;

/* ------------------------------------------------------------------------- */
/* Small string helpers                                                      */
/* ------------------------------------------------------------------------- */

static char *dup_str(const char *s)
{
    if (!s) s = "";
    size_t n = strlen(s);
    char *o = (char *)malloc(n + 1);
    if (o) memcpy(o, s, n + 1);
    return o;
}

static char *dup_range(const char *b, const char *e)
{
    if (!b || !e || e < b) return dup_str("");
    size_t n = (size_t)(e - b);
    char *o = (char *)malloc(n + 1);
    if (!o) return NULL;
    memcpy(o, b, n);
    o[n] = '\0';
    return o;
}

static void trim(char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' ||
                 s[n - 1] == ' '  || s[n - 1] == '\t')) s[--n] = '\0';
    size_t lead = 0;
    while (s[lead] == ' ' || s[lead] == '\t') lead++;
    if (lead) memmove(s, s + lead, n - lead + 1);
}

static int ci_contains(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) return 1;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return 1;
    }
    return 0;
}

/*
 * Pull attr="value" out of an #EXTINF line. The attributes are unordered and
 * optional, and a value can legitimately contain a comma - which is why this
 * cannot just split the line on commas, the mistake every quick M3U parser
 * makes and the reason channel names come out truncated at the first comma.
 */
static char *extinf_attr(const char *line, const char *attr)
{
    size_t al = strlen(attr);
    for (const char *p = line; (p = strstr(p, attr)) != NULL; p += al) {
        /* Must be preceded by whitespace, so tvg-name does not match name. */
        if (p != line && p[-1] != ' ' && p[-1] != '\t') continue;
        const char *q = p + al;
        while (*q == ' ') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ') q++;
        if (*q != '"') continue;
        q++;
        const char *end = strchr(q, '"');
        if (!end) return NULL;
        return dup_range(q, end);
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

static void load_conf(void)
{
    FILE *f = fopen(evo_data_path(IPTV_CONF), "r");
    if (!f) f = fopen(IPTV_CONF_USB, "r");
    if (!f) return;
    char line[EVO_PROVIDER_MAX_URL + 64];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *k = line, *v = eq + 1;
        if      (strcmp(k, "playlist") == 0) snprintf(G.playlist_url, sizeof G.playlist_url, "%s", v);
        else if (strcmp(k, "xmltv")    == 0) snprintf(G.xmltv_url,    sizeof G.xmltv_url,    "%s", v);
        else if (strcmp(k, "bundle")   == 0) snprintf(G.bundle_url,   sizeof G.bundle_url,   "%s", v);
    }
    fclose(f);
}

int provider_iptv_save_conf(void)
{
    FILE *f = fopen(evo_data_path(IPTV_CONF), "w");
    if (!f) return -1;
    fprintf(f, "playlist=%s\n", G.playlist_url);
    fprintf(f, "xmltv=%s\n",    G.xmltv_url);
    fprintf(f, "bundle=%s\n",   G.bundle_url);
    fclose(f);
    return 0;
}

void provider_iptv_set_playlist(const char *url)
{
    snprintf(G.playlist_url, sizeof G.playlist_url, "%s", url ? url : "");
    /* A new playlist invalidates everything parsed from the old one. */
    G.loaded = 0;
}

void provider_iptv_set_bundle(const char *url)
{
    snprintf(G.bundle_url, sizeof G.bundle_url, "%s", url ? url : "");
}

void provider_iptv_set_xmltv(const char *url)
{
    snprintf(G.xmltv_url, sizeof G.xmltv_url, "%s", url ? url : "");
    G.epg_loaded = 0;
}

const char *provider_iptv_playlist_url(void) { return G.playlist_url; }

/* ------------------------------------------------------------------------- */
/* Storage                                                                   */
/* ------------------------------------------------------------------------- */

static void free_channels(void)
{
    for (int i = 0; i < G.ch_count; ++i) {
        free(G.ch[i].name); free(G.ch[i].url); free(G.ch[i].group);
        free(G.ch[i].logo); free(G.ch[i].tvg_id);
        free(G.ch[i].now);  free(G.ch[i].next);
    }
    free(G.ch); G.ch = NULL; G.ch_count = G.ch_cap = 0;
    free(G.gr); G.gr = NULL; G.gr_count = G.gr_cap = 0;
    G.loaded = 0;
    G.epg_loaded = 0;
}

static int push_channel(channel_t c)
{
    if (G.ch_count == G.ch_cap) {
        int nc = G.ch_cap ? G.ch_cap * 2 : 128;
        channel_t *n = (channel_t *)realloc(G.ch, (size_t)nc * sizeof *n);
        if (!n) return -1;
        G.ch = n; G.ch_cap = nc;
    }
    G.ch[G.ch_count++] = c;
    return 0;
}

/*
 * Groups are built after the channels are parsed, by sorting the channel array
 * by group name. Sorting rather than a hash means a group's channels are
 * contiguous, so a group page is a slice with no indirection - which is what
 * keeps a 5000-channel playlist's page build free of per-row scanning.
 */
static int cmp_by_group(const void *a, const void *b)
{
    const channel_t *x = (const channel_t *)a, *y = (const channel_t *)b;
    int g = strcmp(x->group, y->group);
    if (g) return g;
    return strcmp(x->name, y->name);
}

static int build_groups(void)
{
    G.gr_count = 0;
    if (G.ch_count == 0) return 0;

    qsort(G.ch, (size_t)G.ch_count, sizeof *G.ch, cmp_by_group);

    for (int i = 0; i < G.ch_count; ) {
        const char *g = G.ch[i].group;
        int j = i;
        while (j < G.ch_count && strcmp(G.ch[j].group, g) == 0) j++;

        if (G.gr_count == G.gr_cap) {
            int nc = G.gr_cap ? G.gr_cap * 2 : 32;
            group_t *n = (group_t *)realloc(G.gr, (size_t)nc * sizeof *n);
            if (!n) return -1;
            G.gr = n; G.gr_cap = nc;
        }
        group_t *gp = &G.gr[G.gr_count++];
        snprintf(gp->name, sizeof gp->name, "%s", g && *g ? g : "Ungrouped");
        gp->first = i;
        gp->count = j - i;
        i = j;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* M3U parse                                                                 */
/* ------------------------------------------------------------------------- */
/*
 * Tolerant by design. A real-world playlist has CRLF mixed with LF, blank
 * lines, #EXTGRP on its own line, comment lines nobody documents, and
 * #EXTINF rows whose URL is three lines further down. The rule applied here is
 * the only one that survives all of that: an #EXTINF arms a pending row, the
 * next line that is not a directive completes it, and anything unpaired at EOF
 * is dropped.
 */
static int parse_m3u(const char *body, size_t len)
{
    free_channels();

    const char *p   = body;
    const char *end = body + len;

    char *pend_name = NULL, *pend_group = NULL, *pend_logo = NULL, *pend_tvg = NULL;
    char *extgrp = NULL;    /* #EXTGRP applies until the next one */
    int   armed = 0;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        size_t n = (size_t)(le - p);
        while (n && (p[n - 1] == '\r')) n--;

        if (n == 0) { p = nl ? nl + 1 : end; continue; }

        /* A stack line buffer would cap the channel name; these rows are long
         * and the cost of one alloc per line here is invisible next to the
         * network fetch that produced the body. */
        char *line = dup_range(p, p + n);
        if (!line) goto oom;
        trim(line);

        if (strncmp(line, "#EXTINF", 7) == 0) {
            free(pend_name); free(pend_group); free(pend_logo); free(pend_tvg);
            pend_group = extinf_attr(line, "group-title");
            pend_logo  = extinf_attr(line, "tvg-logo");
            pend_tvg   = extinf_attr(line, "tvg-id");

            /* The display name is everything after the LAST comma on the line,
             * because the attributes before it may contain commas of their
             * own. Falls back to tvg-name, then to the URL's basename later. */
            const char *comma = strrchr(line, ',');
            pend_name = comma ? dup_str(comma + 1) : extinf_attr(line, "tvg-name");
            if (pend_name) trim(pend_name);
            armed = 1;

        } else if (strncmp(line, "#EXTGRP", 7) == 0) {
            const char *c = strchr(line, ':');
            free(extgrp);
            extgrp = c ? dup_str(c + 1) : NULL;
            if (extgrp) trim(extgrp);

        } else if (line[0] == '#') {
            /* #EXTM3U, #PLAYLIST, vendor comments - nothing to do. */

        } else if (armed) {
            channel_t c;
            memset(&c, 0, sizeof c);
            c.url    = dup_str(line);
            c.name   = pend_name  ? pend_name  : dup_str(line);
            c.group  = pend_group ? pend_group : dup_str(extgrp ? extgrp : "");
            c.logo   = pend_logo  ? pend_logo  : dup_str("");
            c.tvg_id = pend_tvg   ? pend_tvg   : dup_str("");
            pend_name = pend_group = pend_logo = pend_tvg = NULL;
            armed = 0;

            if (!c.url || !c.name || !c.group || !c.logo || !c.tvg_id) {
                free(c.url); free(c.name); free(c.group);
                free(c.logo); free(c.tvg_id);
                free(line);
                goto oom;
            }
            if (!c.name[0]) { free(c.name); c.name = dup_str(c.url); }

            if (push_channel(c) != 0) { free(line); goto oom; }

        } else {
            /*
             * A bare URL with no #EXTINF. A plain .m3u (as opposed to an
             * extended one) is nothing but these, so they are channels too -
             * named after the last path component, which is all there is.
             */
            channel_t c;
            memset(&c, 0, sizeof c);
            const char *slash = strrchr(line, '/');
            c.url    = dup_str(line);
            c.name   = dup_str(slash && slash[1] ? slash + 1 : line);
            c.group  = dup_str(extgrp ? extgrp : "");
            c.logo   = dup_str("");
            c.tvg_id = dup_str("");
            if (!c.url || !c.name || !c.group || !c.logo || !c.tvg_id ||
                push_channel(c) != 0) {
                free(c.url); free(c.name); free(c.group);
                free(c.logo); free(c.tvg_id);
                free(line);
                goto oom;
            }
        }

        free(line);
        p = nl ? nl + 1 : end;
    }

    free(pend_name); free(pend_group); free(pend_logo); free(pend_tvg);
    free(extgrp);

    if (build_groups() != 0) { free_channels(); return -1; }
    G.loaded = 1;
    return G.ch_count;

oom:
    free(pend_name); free(pend_group); free(pend_logo); free(pend_tvg);
    free(extgrp);
    free_channels();
    return -1;
}

/* ------------------------------------------------------------------------- */
/* XMLTV now-and-next                                                        */
/* ------------------------------------------------------------------------- */
/*
 * Deliberately a scan, not an XML parse.
 *
 * An XMLTV file for a few hundred channels is tens of megabytes of <programme>
 * elements covering a week, of which this wants two per channel. Handing that
 * to libxml2 would build a DOM many times the size of the only two fields it
 * is after, on a console where the flexible pool is the thing that runs out.
 * So: walk the bytes, and for each <programme> read only start, channel and
 * the first <title>. Anything malformed is skipped, not diagnosed - a broken
 * EPG must cost the channel list nothing.
 *
 * evo_net caps a response body, so an oversized EPG simply does not arrive and
 * now/next stay empty. That is the honest failure: the channel list is what
 * matters and it is already on screen.
 */
static int64_t xmltv_time(const char *s)
{
    /* "20260924123000 +0000" - 14 digits, optional offset. */
    if (!s) return -1;
    int y, mo, d, h, mi, se;
    if (sscanf(s, "%4d%2d%2d%2d%2d%2d", &y, &mo, &d, &h, &mi, &se) != 6)
        return -1;

    /* Days from the civil epoch (Howard Hinnant's algorithm). timegm() is not
     * portable here and mktime() would apply the console's local zone to a
     * timestamp that already carries its own offset. */
    int yy = y - (mo <= 2);
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    int64_t t = days * 86400 + h * 3600 + mi * 60 + se;

    /* Trailing " +0530" / " -0400". */
    const char *sp = strchr(s, ' ');
    if (sp) {
        while (*sp == ' ') sp++;
        if ((*sp == '+' || *sp == '-') && strlen(sp) >= 5) {
            int sign = (*sp == '-') ? -1 : 1;
            int oh = (sp[1] - '0') * 10 + (sp[2] - '0');
            int om = (sp[3] - '0') * 10 + (sp[4] - '0');
            t -= sign * (oh * 3600 + om * 60);
        }
    }
    return t;
}

/* Value of attr="..." inside a single element's text. */
static int tag_attr(const char *el, const char *el_end, const char *attr,
                    char *out, size_t out_sz)
{
    size_t al = strlen(attr);
    for (const char *p = el; p + al < el_end; ++p) {
        if (strncmp(p, attr, al) != 0) continue;
        if (p != el && p[-1] != ' ') continue;
        const char *q = p + al;
        if (q >= el_end || *q != '=') continue;
        q++;
        if (q >= el_end || *q != '"') continue;
        q++;
        const char *e = memchr(q, '"', (size_t)(el_end - q));
        if (!e) return -1;
        size_t n = (size_t)(e - q);
        if (n >= out_sz) n = out_sz - 1;
        memcpy(out, q, n);
        out[n] = '\0';
        return 0;
    }
    return -1;
}

static void parse_xmltv(const char *body, size_t len)
{
    if (!G.loaded || G.ch_count == 0) return;

    int64_t now = (int64_t)time(NULL);
    const char *p = body, *end = body + len;

    /*
     * One pass. For each programme, if its channel is one of ours, keep it as
     * `now` when it straddles the current time and as `next` when it is the
     * earliest thing starting after it. No sorting, no buffering the file.
     */
    int64_t *next_start = (int64_t *)calloc((size_t)G.ch_count, sizeof(int64_t));
    if (!next_start) return;

    while (p < end) {
        const char *op = strstr(p, "<programme");
        if (!op || op >= end) break;
        const char *close = strstr(op, "</programme>");
        const char *pe = close ? close : end;

        char start[32] = {0}, stop[32] = {0}, chan[96] = {0};
        const char *hdr_end = memchr(op, '>', (size_t)(pe - op));
        if (!hdr_end) break;

        tag_attr(op, hdr_end, "start",   start, sizeof start);
        tag_attr(op, hdr_end, "stop",    stop,  sizeof stop);
        tag_attr(op, hdr_end, "channel", chan,  sizeof chan);

        if (chan[0] && start[0]) {
            int64_t ts = xmltv_time(start);
            int64_t te = stop[0] ? xmltv_time(stop) : ts + 1800;

            const char *tb = strstr(hdr_end, "<title");
            char title[128] = {0};
            if (tb && tb < pe) {
                const char *tg = memchr(tb, '>', (size_t)(pe - tb));
                const char *tc = tg ? strstr(tg, "</title>") : NULL;
                if (tg && tc && tc > tg + 1) {
                    size_t n = (size_t)(tc - tg - 1);
                    if (n >= sizeof title) n = sizeof title - 1;
                    memcpy(title, tg + 1, n);
                    title[n] = '\0';
                    trim(title);
                }
            }

            if (title[0] && ts > 0) {
                for (int i = 0; i < G.ch_count; ++i) {
                    if (!G.ch[i].tvg_id[0] || strcmp(G.ch[i].tvg_id, chan) != 0)
                        continue;
                    if (ts <= now && now < te) {
                        free(G.ch[i].now);
                        G.ch[i].now = dup_str(title);
                    } else if (ts > now &&
                               (next_start[i] == 0 || ts < next_start[i])) {
                        next_start[i] = ts;
                        free(G.ch[i].next);
                        G.ch[i].next = dup_str(title);
                    }
                    break;
                }
            }
        }

        p = close ? close + 12 : end;
    }

    free(next_start);
    G.epg_loaded = 1;
}

/* ------------------------------------------------------------------------- */
/* Item ids                                                                  */
/* ------------------------------------------------------------------------- */
/*
 * Ids are opaque to EVO but they still have to survive a round trip through a
 * data model and back, so they are text:
 *
 *   "g:<group name>"  a folder
 *   "c:<index>"       a channel, by its position in the sorted array
 *
 * The index is only valid for as long as the parsed playlist is: a refresh
 * re-sorts and renumbers. resolve() therefore checks the index is in range and
 * fails cleanly rather than playing whatever now sits there.
 */
static int channel_index(const char *item_id)
{
    if (!item_id || item_id[0] != 'c' || item_id[1] != ':') return -1;
    char *endp = NULL;
    long v = strtol(item_id + 2, &endp, 10);
    if (!endp || *endp) return -1;
    if (v < 0 || v >= G.ch_count) return -1;
    return (int)v;
}

static void fill_channel_item(evo_provider_item_t *it, int i, const char *parent)
{
    evo_provider_item_clear(it);
    snprintf(it->id, sizeof it->id, "c:%d", i);
    if (parent) snprintf(it->parent_id, sizeof it->parent_id, "%s", parent);
    snprintf(it->title, sizeof it->title, "%s", G.ch[i].name);
    snprintf(it->art_url, sizeof it->art_url, "%s", G.ch[i].logo);
    if (G.ch[i].now)
        snprintf(it->now_title, sizeof it->now_title, "%s", G.ch[i].now);
    if (G.ch[i].next)
        snprintf(it->next_title, sizeof it->next_title, "%s", G.ch[i].next);
    /* The group is the useful subtitle at the root, where rows from different
     * groups are mixed; inside a group it would repeat the folder name. */
    if (!parent || parent[0] != 'g')
        snprintf(it->subtitle, sizeof it->subtitle, "%s", G.ch[i].group);
    else if (G.ch[i].now)
        snprintf(it->subtitle, sizeof it->subtitle, "%s", G.ch[i].now);

    it->kind = EVO_MEDIA_STREAM;
    it->is_live = 1;
    it->duration_sec = 0;
}

static void fill_group_item(evo_provider_item_t *it, int g)
{
    evo_provider_item_clear(it);
    snprintf(it->id, sizeof it->id, "g:%s", G.gr[g].name);
    snprintf(it->title, sizeof it->title, "%s", G.gr[g].name);
    snprintf(it->subtitle, sizeof it->subtitle, "%d channel%s",
             G.gr[g].count, G.gr[g].count == 1 ? "" : "s");
    it->kind = EVO_MEDIA_FOLDER;
    it->is_folder = 1;
}

/* ------------------------------------------------------------------------- */
/* Page emission                                                             */
/* ------------------------------------------------------------------------- */
/*
 * One page of items is built on the heap and handed to the callback, then
 * freed. It is not kept: the caller either copies what it wants into a data
 * model or it does not, and holding a second copy of a 5000-row catalog for
 * the sake of not re-filling 512 structs is the wrong trade.
 */
static int emit_page(const char *parent_id, int page,
                     evo_provider_items_cb cb, void *ud)
{
    int total = 0, base = 0, is_group_page = 0, gidx = -1;

    if (!parent_id || !*parent_id) {
        /*
         * At the root: groups if there are any real ones, otherwise the
         * channels themselves. A playlist with no group-title anywhere would
         * otherwise show a single "Ungrouped" folder the user has to open for
         * no reason.
         */
        int real_groups = 0;
        for (int i = 0; i < G.gr_count; ++i)
            if (strcmp(G.gr[i].name, "Ungrouped") != 0) real_groups++;
        if (real_groups > 0 && G.gr_count > 1) {
            total = G.gr_count;
            is_group_page = 1;
        } else {
            total = G.ch_count;
        }
    } else if (parent_id[0] == 'g' && parent_id[1] == ':') {
        for (int i = 0; i < G.gr_count; ++i) {
            if (strcmp(G.gr[i].name, parent_id + 2) == 0) { gidx = i; break; }
        }
        if (gidx < 0) { if (cb) cb(0, NULL, 0, 0, ud); return 0; }
        total = G.gr[gidx].count;
        base  = G.gr[gidx].first;
    } else {
        /* A channel has no children. */
        if (cb) cb(1, NULL, 0, 0, ud);
        return 0;
    }

    if (page < 0) page = 0;
    int off = page * EVO_PROVIDER_PAGE_MAX;
    if (off >= total) { if (cb) cb(1, NULL, 0, 0, ud); return 0; }
    int n = total - off;
    if (n > EVO_PROVIDER_PAGE_MAX) n = EVO_PROVIDER_PAGE_MAX;

    evo_provider_item_t *items =
        (evo_provider_item_t *)calloc((size_t)n, sizeof *items);
    if (!items) { if (cb) cb(0, NULL, 0, 0, ud); return -1; }

    for (int k = 0; k < n; ++k) {
        if (is_group_page) fill_group_item(&items[k], off + k);
        else               fill_channel_item(&items[k], base + off + k, parent_id);
    }

    if (cb) cb(1, items, n, (off + n) < total, ud);
    free(items);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Fetch                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct pending {
    char parent_id[EVO_PROVIDER_MAX_ITEM_ID];
    char query[128];
    int  page;
    int  is_search;
    evo_provider_items_cb cb;
    void *ud;
} pending_t;

static void emit_search(const char *query, int page,
                        evo_provider_items_cb cb, void *ud);

static void on_epg(int success, int status, const char *body, size_t len, void *ud)
{
    (void)ud;
    if (success && status == 200 && body && len)
        parse_xmltv(body, len);
    else
        G.epg_loaded = 1;   /* do not retry every page turn */
}

static void kick_epg(void)
{
    if (G.epg_loaded || !G.xmltv_url[0]) return;
    /* Marked loaded up front: one attempt per playlist load. A retry loop on a
     * 30 MB EPG that will never fit the body cap is a frame-rate bug. */
    G.epg_loaded = 1;
    evo_net_request_async("GET", G.xmltv_url, NULL, NULL, 0, on_epg, NULL);
}

static void on_playlist(int success, int status, const char *body, size_t len,
                        void *ud)
{
    pending_t *pd = (pending_t *)ud;
    G.loading = 0;

    if (!success || status < 200 || status >= 300 || !body || len == 0) {
        if (pd && pd->cb) pd->cb(0, NULL, 0, 0, pd->ud);
        free(pd);
        return;
    }

    if (parse_m3u(body, len) < 0) {
        if (pd && pd->cb) pd->cb(0, NULL, 0, 0, pd->ud);
        free(pd);
        return;
    }

    kick_epg();

    if (pd) {
        if (pd->is_search) emit_search(pd->query, pd->page, pd->cb, pd->ud);
        else               emit_page(pd->parent_id, pd->page, pd->cb, pd->ud);
        free(pd);
    }
}

static int fetch_playlist(pending_t *pd)
{
    if (!G.playlist_url[0]) { free(pd); return -1; }
    if (G.loading)          { free(pd); return -2; }
    G.loading = 1;
    int rc = evo_net_request_async("GET", G.playlist_url, NULL, NULL, 0,
                                   on_playlist, pd);
    if (rc != 0) { G.loading = 0; free(pd); return -3; }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Search                                                                    */
/* ------------------------------------------------------------------------- */

static void emit_search(const char *query, int page,
                        evo_provider_items_cb cb, void *ud)
{
    /* Two passes so the page array is sized exactly: the match count is not
     * knowable without walking, and over-allocating for 5000 channels to
     * return twelve is the sort of thing the memory ceiling notices. */
    int total = 0;
    for (int i = 0; i < G.ch_count; ++i)
        if (ci_contains(G.ch[i].name, query)) total++;

    if (page < 0) page = 0;
    int off = page * EVO_PROVIDER_PAGE_MAX;
    if (off >= total) { if (cb) cb(1, NULL, 0, 0, ud); return; }
    int n = total - off;
    if (n > EVO_PROVIDER_PAGE_MAX) n = EVO_PROVIDER_PAGE_MAX;

    evo_provider_item_t *items =
        (evo_provider_item_t *)calloc((size_t)n, sizeof *items);
    if (!items) { if (cb) cb(0, NULL, 0, 0, ud); return; }

    int seen = 0, k = 0;
    for (int i = 0; i < G.ch_count && k < n; ++i) {
        if (!ci_contains(G.ch[i].name, query)) continue;
        if (seen++ < off) continue;
        fill_channel_item(&items[k++], i, NULL);
    }

    if (cb) cb(1, items, k, (off + k) < total, ud);
    free(items);
}

/* ------------------------------------------------------------------------- */
/* Vtable                                                                    */
/* ------------------------------------------------------------------------- */

static int iptv_init(void)
{
    memset(&G, 0, sizeof G);
    load_conf();
    return 0;   /* no network here - this runs during boot */
}

static void iptv_shutdown(void)
{
    free_channels();
}

static int iptv_is_configured(void)
{
    return G.playlist_url[0] ? 1 : 0;
}

static int iptv_list_catalog(const char *parent_id, int page,
                             evo_provider_items_cb cb, void *ud)
{
    if (!G.playlist_url[0]) return -1;

    if (G.loaded)
        return emit_page(parent_id, page, cb, ud);

    pending_t *pd = (pending_t *)calloc(1, sizeof *pd);
    if (!pd) return -2;
    if (parent_id) snprintf(pd->parent_id, sizeof pd->parent_id, "%s", parent_id);
    pd->page = page;
    pd->cb = cb;
    pd->ud = ud;
    return fetch_playlist(pd);
}

static int iptv_search(const char *query, int page,
                       evo_provider_items_cb cb, void *ud)
{
    if (!G.playlist_url[0]) return -1;

    if (G.loaded) { emit_search(query, page, cb, ud); return 0; }

    pending_t *pd = (pending_t *)calloc(1, sizeof *pd);
    if (!pd) return -2;
    pd->is_search = 1;
    if (query) snprintf(pd->query, sizeof pd->query, "%s", query);
    pd->page = page;
    pd->cb = cb;
    pd->ud = ud;
    return fetch_playlist(pd);
}

static int iptv_resolve(const char *item_id, evo_provider_resolve_cb cb, void *ud)
{
    int i = channel_index(item_id);
    if (i < 0) return -1;

    evo_stream_choice_t c;
    evo_provider_stream_choice_clear(&c);
    snprintf(c.url, sizeof c.url, "%s", G.ch[i].url);
    snprintf(c.label, sizeof c.label, "Live");
    c.is_live = 1;

    /*
     * The container hint is taken from the URL's extension because that is all
     * there is before the open - and it is only a hint: an IPTV endpoint that
     * ends in .m3u8 routinely serves a plain TS, and one with no extension at
     * all is the common case. FFmpeg probes for real; this is for the OSD.
     */
    const char *q = strchr(G.ch[i].url, '?');
    const char *dot = NULL;
    for (const char *p = G.ch[i].url; *p && (!q || p < q); ++p)
        if (*p == '.') dot = p;
    if (dot) {
        size_t n = q ? (size_t)(q - dot - 1) : strlen(dot + 1);
        if (n > 0 && n < sizeof c.container)
            snprintf(c.container, sizeof c.container, "%.*s", (int)n, dot + 1);
    }

    /* Synchronous, but still delivered through the callback so callers have
     * exactly one code path. */
    if (cb) cb(1, &c, 1, ud);
    return 0;
}

static const char *iptv_ui_bundle_url(void)
{
    return G.bundle_url[0] ? G.bundle_url : NULL;
}

const evo_provider_t evo_provider_iptv = {
    .id            = "iptv",
    .name          = "IPTV",
    .icon          = "icon_emby.png",   /* the shared provider rail slot (#90) */
    .caps          = EVO_PROVIDER_CAP_CATALOG | EVO_PROVIDER_CAP_SEARCH |
                     EVO_PROVIDER_CAP_RESOLVE | EVO_PROVIDER_CAP_UI |
                     EVO_PROVIDER_CAP_LIVE,
    .api_version   = EVO_PROVIDER_API_VERSION,
    .init          = iptv_init,
    .shutdown      = iptv_shutdown,
    .is_configured = iptv_is_configured,
    .auth          = NULL,
    .list_catalog  = iptv_list_catalog,
    .search        = iptv_search,
    .resolve       = iptv_resolve,
    .report_progress = NULL,
    .ui_bundle_url = iptv_ui_bundle_url,
};
