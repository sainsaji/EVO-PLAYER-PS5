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
#include "evo_provider_log.h"
#include "evo_favorites.h"

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
    char  *lang;       /* heap or NULL - detected language code e.g. "EN" */
} channel_t;

typedef struct group {
    char name[96];
    int  first;        /* index of the first channel in this group */
    int  count;
} group_t;

typedef struct lang_entry {
    char code[8];
    char name[32];
    int  count;
} lang_entry_t;

static struct {
    char  playlist_url[EVO_PROVIDER_MAX_URL];
    char  xmltv_url[EVO_PROVIDER_MAX_URL];
    char  bundle_url[EVO_PROVIDER_MAX_URL];
    char  last_url[EVO_PROVIDER_MAX_URL];

    channel_t *ch;
    int        ch_count;
    int        ch_cap;

    group_t   *gr;
    int        gr_count;
    int        gr_cap;

    lang_entry_t langs[32];
    int          lang_count;

    /*
     * A playlist file found on the USB stick, if any. Zero-config entry point:
     * drop iptv.m3u on the stick and EVO finds it, no URL to type on a D-pad
     * keyboard and no conf file to hand-write. Empty when there is none.
     */
    char  playlist_file[512];

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
        if (*q == '"' || *q == '\'') {
            char quote = *q++;
            const char *end = strchr(q, quote);
            if (!end) return NULL;
            return dup_range(q, end);
        } else if (*q && *q != ' ' && *q != ',') {
            /* Unquoted value up to space, comma, or end of line */
            const char *end = q;
            while (*end && *end != ' ' && *end != '\t' && *end != ',' && *end != '\r' && *end != '\n') end++;
            if (end > q) return dup_range(q, end);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

/* Defined further down, next to the rest of the parse and fetch machinery. */
static int  parse_m3u(const char *body, size_t len);
static void kick_epg(void);

/*
 * Names we accept for a playlist dropped on the USB stick, in priority order.
 *
 * The stick is how this audience already moves files around - they are copying
 * media onto it anyway - so a playlist file is the one configuration route
 * that needs no UI, no keyboard and no server. It loses to an explicit
 * playlist= in the conf, so a configured URL is never overridden by a stale
 * file someone forgot about.
 */
static const char *const kUsbPlaylistNames[] = {
    "/mnt/usb0/iptv.m3u",
    "/mnt/usb0/iptv.m3u8",
    "/mnt/usb0/playlist.m3u",
    "/mnt/usb0/playlist.m3u8",
    "/mnt/usb0/channels.m3u",
    "/mnt/usb0/channels.m3u8",
    NULL
};

/* First readable candidate, or NULL. */
static const char *find_usb_playlist(void)
{
    for (int i = 0; kUsbPlaylistNames[i]; ++i) {
        FILE *f = fopen(kUsbPlaylistNames[i], "rb");
        if (f) { fclose(f); return kUsbPlaylistNames[i]; }
    }
    return NULL;
}

/*
 * Read a local playlist and parse it. No network, so this is synchronous and
 * the catalog is ready on return - which is why iptv_list_catalog can emit the
 * first page in the same call rather than waiting for a callback.
 *
 * Capped at EVO_BUNDLE-ish size for the same reason evo_net caps a body: a
 * playlist is text, and a 64 MiB file on the stick is a mistake, not a
 * playlist. 16 MiB is roughly 200k channels.
 */
#define IPTV_MAX_LOCAL_PLAYLIST (16u << 20)

static int load_playlist_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    if (n <= 0 || (unsigned long)n > IPTV_MAX_LOCAL_PLAYLIST) {
        PROV_LOG("iptv: '%s' is %ld bytes - refused", path, n);
        fclose(f);
        return -1;
    }
    rewind(f);

    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;

    int rc = parse_m3u(buf, got);
    free(buf);

    PROV_LOG("iptv: local playlist '%s' -> %d channels, %d groups",
             path, G.ch_count, G.gr_count);
    return rc;
}

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
        if (strcmp(k, "playlist") == 0) {
            /* Ignore stale local dev server URLs from previous testing (e.g. :8099) */
            if (!strstr(v, ":8099")) {
                if (v[0] == '/') snprintf(G.playlist_file, sizeof G.playlist_file, "%s", v);
                else {
                    snprintf(G.playlist_url,  sizeof G.playlist_url,  "%s", v);
                    if (!G.last_url[0]) {
                        snprintf(G.last_url,  sizeof G.last_url,      "%s", v);
                    }
                }
            }
        }
        else if (strcmp(k, "last_url") == 0) {
            if (!strstr(v, ":8099")) {
                snprintf(G.last_url, sizeof G.last_url, "%s", v);
            }
        }
        else if (strcmp(k, "xmltv") == 0) snprintf(G.xmltv_url, sizeof G.xmltv_url, "%s", v);
    }
    fclose(f);
}

/*
 * Pick the playlist source. Called from init() and from every rebind.
 */
static void resolve_playlist_source(void)
{
    /* If a local path is set in playlist_url, move to playlist_file */
    if (G.playlist_url[0] == '/') {
        snprintf(G.playlist_file, sizeof G.playlist_file, "%s", G.playlist_url);
        G.playlist_url[0] = 0;
    }
}

int provider_iptv_save_conf(void)
{
    FILE *f = fopen(evo_data_path(IPTV_CONF), "w");
    if (!f) return -1;
    if (G.playlist_url[0])
        fprintf(f, "playlist=%s\n", G.playlist_url);
    else if (G.playlist_file[0])
        fprintf(f, "playlist=%s\n", G.playlist_file);
    else
        fprintf(f, "playlist=\n");

    if (G.last_url[0])
        fprintf(f, "last_url=%s\n", G.last_url);

    if (G.xmltv_url[0])
        fprintf(f, "xmltv=%s\n", G.xmltv_url);
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
        free(G.ch[i].lang);
    }
    free(G.ch); G.ch = NULL; G.ch_count = G.ch_cap = 0;
    free(G.gr); G.gr = NULL; G.gr_count = G.gr_cap = 0;
    G.lang_count = 0;
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

static const char *kLangCodes[] = {
    "EN", "English",
    "ES", "Spanish",
    "FR", "French",
    "DE", "German",
    "IT", "Italian",
    "PT", "Portuguese",
    "AR", "Arabic",
    "TR", "Turkish",
    "RU", "Russian",
    "HI", "Hindi",
    "NL", "Dutch",
    "PL", "Polish",
    "GR", "Greek",
    "RO", "Romanian",
    "SV", "Swedish",
    "NO", "Norwegian",
    "DA", "Danish",
    "FI", "Finnish",
    NULL, NULL
};

static const char *get_lang_name(const char *code)
{
    if (!code || !*code) return "Unknown";
    for (int i = 0; kLangCodes[i]; i += 2) {
        if (strcasecmp(code, kLangCodes[i]) == 0)
            return kLangCodes[i + 1];
    }
    return code;
}

static char *detect_channel_language(const char *name, const char *group, const char *attr_lang)
{
    if (attr_lang && *attr_lang) {
        char buf[8] = {0};
        size_t n = strlen(attr_lang);
        if (n >= 2) {
            buf[0] = (char)toupper((unsigned char)attr_lang[0]);
            buf[1] = (char)toupper((unsigned char)attr_lang[1]);
            return dup_str(buf);
        }
    }
    static const char *prefixes[] = {
        "[EN]", "EN:", "UK:", "US:", "EN|", "ENG|",
        "[ES]", "ES:", "SPA|", "SPAIN|",
        "[FR]", "FR:", "FRA|", "FRANCE|",
        "[DE]", "DE:", "GER|", "GERMANY|",
        "[IT]", "IT:", "ITA|", "ITALY|",
        "[PT]", "PT:", "POR|",
        "[AR]", "AR:", "ARABIC|",
        "[TR]", "TR:", "TURK|",
        "[RU]", "RU:", "RUS|",
        "[HI]", "HI:", "HINDI|", "IN:",
        NULL
    };
    static const char *mapped_code[] = {
        "EN", "EN", "EN", "EN", "EN", "EN",
        "ES", "ES", "ES", "ES",
        "FR", "FR", "FR", "FR",
        "DE", "DE", "DE", "DE",
        "IT", "IT", "IT", "IT",
        "PT", "PT", "PT",
        "AR", "AR", "AR",
        "TR", "TR", "TR",
        "RU", "RU", "RU",
        "HI", "HI", "HI", "HI",
        NULL
    };

    const char *targets[2] = { name, group };
    for (int t = 0; t < 2; ++t) {
        if (!targets[t]) continue;
        for (int i = 0; prefixes[i]; ++i) {
            if (strstr(targets[t], prefixes[i]))
                return dup_str(mapped_code[i]);
        }
    }
    return dup_str("");
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
    G.lang_count = 0;
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

    /* Index available languages across all channels */
    for (int i = 0; i < G.ch_count; ++i) {
        if (!G.ch[i].lang || !G.ch[i].lang[0]) continue;
        int found = -1;
        for (int l = 0; l < G.lang_count; ++l) {
            if (strcasecmp(G.langs[l].code, G.ch[i].lang) == 0) {
                found = l;
                break;
            }
        }
        if (found >= 0) {
            G.langs[found].count++;
        } else if (G.lang_count < 32) {
            int l = G.lang_count++;
            snprintf(G.langs[l].code, sizeof G.langs[l].code, "%s", G.ch[i].lang);
            snprintf(G.langs[l].name, sizeof G.langs[l].name, "%s", get_lang_name(G.ch[i].lang));
            G.langs[l].count = 1;
        }
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

    char *pend_name = NULL, *pend_group = NULL, *pend_logo = NULL, *pend_tvg = NULL, *pend_lang = NULL;
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
            free(pend_name); free(pend_group); free(pend_logo); free(pend_tvg); free(pend_lang);
            pend_group = extinf_attr(line, "group-title");
            pend_logo  = extinf_attr(line, "tvg-logo");
            if (!pend_logo) pend_logo = extinf_attr(line, "logo");
            pend_tvg   = extinf_attr(line, "tvg-id");
            pend_lang  = extinf_attr(line, "tvg-language");
            if (!pend_lang) pend_lang = extinf_attr(line, "tvg-country");
            if (!pend_lang) pend_lang = extinf_attr(line, "language");

            /* The display name is everything after the LAST comma on the line,
             * because the attributes before it may contain commas of their
             * own. Falls back to tvg-name, then to the URL's basename later. */
            const char *comma = strrchr(line, ',');
            pend_name = comma ? dup_str(comma + 1) : extinf_attr(line, "tvg-name");
            if (pend_name)  trim(pend_name);
            if (pend_logo)  trim(pend_logo);
            if (pend_group) trim(pend_group);
            if (pend_tvg)   trim(pend_tvg);
            if (pend_lang)  trim(pend_lang);
            armed = 1;

        } else if (strncmp(line, "#EXTGRP", 7) == 0) {
            const char *c = strchr(line, ':');
            free(extgrp);
            extgrp = c ? dup_str(c + 1) : NULL;
            if (extgrp) trim(extgrp);

        } else if (line[0] == '#') {
            /* #EXTM3U: check if header provides XMLTV EPG URL */
            if (strncmp(line, "#EXTM3U", 7) == 0 && !G.xmltv_url[0]) {
                char *tvg = extinf_attr(line, "x-tvg-url");
                if (!tvg) tvg = extinf_attr(line, "url-tvg");
                if (tvg) {
                    snprintf(G.xmltv_url, sizeof G.xmltv_url, "%s", tvg);
                    PROV_LOG("iptv: auto-discovered EPG URL from M3U: %s", G.xmltv_url);
                    free(tvg);
                }
            }

        } else if (armed) {
            channel_t c;
            memset(&c, 0, sizeof c);
            c.url    = dup_str(line);
            c.name   = pend_name  ? pend_name  : dup_str(line);
            c.group  = pend_group ? pend_group : dup_str(extgrp ? extgrp : "");
            c.logo   = pend_logo  ? pend_logo  : dup_str("");
            c.tvg_id = pend_tvg   ? pend_tvg   : dup_str("");
            c.lang   = detect_channel_language(c.name, c.group, pend_lang);
            free(pend_lang);
            pend_name = pend_group = pend_logo = pend_tvg = pend_lang = NULL;
            armed = 0;

            if (!c.url || !c.name || !c.group || !c.logo || !c.tvg_id) {
                free(c.url); free(c.name); free(c.group);
                free(c.logo); free(c.tvg_id); free(c.lang);
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
            c.lang   = detect_channel_language(c.name, c.group, NULL);
            if (!c.url || !c.name || !c.group || !c.logo || !c.tvg_id ||
                push_channel(c) != 0) {
                free(c.url); free(c.name); free(c.group);
                free(c.logo); free(c.tvg_id); free(c.lang);
                free(line);
                goto oom;
            }
        }

        free(line);
        p = nl ? nl + 1 : end;
    }

    free(pend_name); free(pend_group); free(pend_logo); free(pend_tvg); free(pend_lang);
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

static void xml_decode_entities(char *s)
{
    if (!s) return;
    char *r = s, *w = s;
    while (*r) {
        if (*r == '&') {
            if (strncmp(r, "&amp;", 5) == 0) {
                *w++ = '&'; r += 5;
            } else if (strncmp(r, "&quot;", 6) == 0) {
                *w++ = '"'; r += 6;
            } else if (strncmp(r, "&apos;", 6) == 0) {
                *w++ = '\''; r += 6;
            } else if (strncmp(r, "&#39;", 5) == 0) {
                *w++ = '\''; r += 5;
            } else if (strncmp(r, "&lt;", 4) == 0) {
                *w++ = '<'; r += 4;
            } else if (strncmp(r, "&gt;", 4) == 0) {
                *w++ = '>'; r += 4;
            } else {
                *w++ = *r++;
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static int channel_name_match(const char *a, const char *b)
{
    if (!a || !b) return 0;
    if (strcasecmp(a, b) == 0) return 1;
    /* Compare ignoring non-alphanumeric characters and whitespace */
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (*pa || *pb) {
        while (*pa && !isalnum(*pa)) pa++;
        while (*pb && !isalnum(*pb)) pb++;
        if (!*pa || !*pb) break;
        if (tolower(*pa) != tolower(*pb)) return 0;
        pa++;
        pb++;
    }
    while (*pa && !isalnum(*pa)) pa++;
    while (*pb && !isalnum(*pb)) pb++;
    return (!*pa && !*pb);
}

static void parse_xmltv(const char *body, size_t len)
{
    if (!G.loaded || G.ch_count == 0) return;

    const char *end = body + len;

    /*
     * Pass 1: Parse <channel id="..."> elements.
     * Maps the XMLTV channel id to our playlist channels by matching
     * <display-name> (or id) to G.ch[i].name. Also fills logos if missing.
     */
    const char *cp = body;
    int mapped_count = 0;
    while (cp < end) {
        const char *oc = strstr(cp, "<channel");
        if (!oc || oc >= end) break;
        const char *close = strstr(oc, "</channel>");
        const char *ce = close ? close : end;

        char chan_id[96] = {0};
        const char *hdr_end = memchr(oc, '>', (size_t)(ce - oc));
        if (hdr_end) {
            tag_attr(oc, hdr_end, "id", chan_id, sizeof chan_id);
            xml_decode_entities(chan_id);
            trim(chan_id);

            char dname[128] = {0};
            const char *db = strstr(hdr_end, "<display-name");
            if (db && db < ce) {
                const char *dg = memchr(db, '>', (size_t)(ce - db));
                const char *dc = dg ? strstr(dg, "</display-name>") : NULL;
                if (dg && dc && dc > dg + 1) {
                    size_t n = (size_t)(dc - dg - 1);
                    if (n >= sizeof dname) n = sizeof dname - 1;
                    memcpy(dname, dg + 1, n);
                    dname[n] = '\0';
                    xml_decode_entities(dname);
                    trim(dname);
                }
            }

            char icon_src[256] = {0};
            const char *ic = strstr(hdr_end, "<icon");
            if (ic && ic < ce) {
                const char *ic_end = memchr(ic, '>', (size_t)(ce - ic));
                if (ic_end) {
                    tag_attr(ic, ic_end, "src", icon_src, sizeof icon_src);
                }
            }

            if (chan_id[0]) {
                for (int i = 0; i < G.ch_count; ++i) {
                    int match = 0;
                    if (dname[0] && channel_name_match(G.ch[i].name, dname)) {
                        match = 1;
                    } else if (channel_name_match(G.ch[i].name, chan_id)) {
                        match = 1;
                    }
                    if (match) {
                        if (!G.ch[i].tvg_id[0]) {
                            free(G.ch[i].tvg_id);
                            G.ch[i].tvg_id = dup_str(chan_id);
                            mapped_count++;
                        }
                        if ((!G.ch[i].logo || !G.ch[i].logo[0]) && icon_src[0]) {
                            free(G.ch[i].logo);
                            G.ch[i].logo = dup_str(icon_src);
                        }
                    }
                }
            }
        }
        cp = close ? close + 10 : (hdr_end ? hdr_end + 1 : end);
    }
    PROV_LOG("iptv: XMLTV pass 1 mapped %d/%d channels by display-name/id", mapped_count, G.ch_count);

    /*
     * Pass 2: Parse <programme> elements.
     * Matches programme channel to G.ch[i].tvg_id or G.ch[i].name.
     */
    int64_t now = (int64_t)time(NULL);
    int64_t *next_start = (int64_t *)calloc((size_t)G.ch_count, sizeof(int64_t));
    if (!next_start) return;

    const char *p = body;
    int progs_found = 0;
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
        xml_decode_entities(chan);
        trim(chan);

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
                    xml_decode_entities(title);
                    trim(title);
                }
            }

            if (title[0] && ts > 0) {
                for (int i = 0; i < G.ch_count; ++i) {
                    int match = (G.ch[i].tvg_id[0] && strcmp(G.ch[i].tvg_id, chan) == 0);
                    if (!match && G.ch[i].name[0]) {
                        match = channel_name_match(G.ch[i].name, chan);
                    }
                    if (!match) continue;
                    progs_found++;
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
    PROV_LOG("iptv: XMLTV pass 2 matched %d programmes", progs_found);
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

    int is_fav = (favorites_is_favorite(G.ch[i].url) || favorites_is_favorite(G.ch[i].name));
    if (is_fav)
        snprintf(it->title, sizeof it->title, "★ %s", G.ch[i].name);
    else
        snprintf(it->title, sizeof it->title, "%s", G.ch[i].name);

    snprintf(it->art_url, sizeof it->art_url, "%s", G.ch[i].logo);
    if (G.ch[i].now && G.ch[i].now[0])
        snprintf(it->now_title, sizeof it->now_title, "%s", G.ch[i].now);
    if (G.ch[i].next && G.ch[i].next[0])
        snprintf(it->next_title, sizeof it->next_title, "%s", G.ch[i].next);

    /* Current program display: if now/next are available, show on subtitle */
    if (G.ch[i].now && G.ch[i].now[0]) {
        if (G.ch[i].next && G.ch[i].next[0])
            snprintf(it->subtitle, sizeof it->subtitle, "%s | Next: %s", G.ch[i].now, G.ch[i].next);
        else
            snprintf(it->subtitle, sizeof it->subtitle, "%s", G.ch[i].now);
    } else if (G.ch[i].group && G.ch[i].group[0]) {
        if (G.ch[i].lang && G.ch[i].lang[0])
            snprintf(it->subtitle, sizeof it->subtitle, "[%s] %s", G.ch[i].lang, G.ch[i].group);
        else
            snprintf(it->subtitle, sizeof it->subtitle, "%s", G.ch[i].group);
    } else if (G.ch[i].lang && G.ch[i].lang[0]) {
        snprintf(it->subtitle, sizeof it->subtitle, "[%s]", G.ch[i].lang);
    }

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
    int is_special_root = 0;
    int is_fav_page = 0;
    int is_langs_page = 0;
    char filter_lang[8] = {0};

    if (!parent_id || !*parent_id) {
        /*
         * At the root: special folders (★ FAVORITES and LANGUAGES if present),
         * followed by groups (or channels if playlist has no groups).
         */
        int real_groups = 0;
        for (int i = 0; i < G.gr_count; ++i)
            if (strcmp(G.gr[i].name, "Ungrouped") != 0) real_groups++;

        int num_special = 1 + (G.lang_count > 0 ? 1 : 0);
        if (real_groups > 0) {
            total = num_special + G.gr_count;
            is_group_page = 1;
        } else {
            total = num_special + G.ch_count;
        }
        is_special_root = num_special;
    } else if (strcmp(parent_id, "fav") == 0) {
        is_fav_page = 1;
        for (int i = 0; i < G.ch_count; ++i) {
            if (favorites_is_favorite(G.ch[i].url) || favorites_is_favorite(G.ch[i].name))
                total++;
        }
    } else if (strcmp(parent_id, "langs") == 0) {
        is_langs_page = 1;
        total = G.lang_count;
    } else if (strncmp(parent_id, "lang:", 5) == 0) {
        snprintf(filter_lang, sizeof filter_lang, "%s", parent_id + 5);
        for (int i = 0; i < G.ch_count; ++i) {
            if (G.ch[i].lang && strcasecmp(G.ch[i].lang, filter_lang) == 0)
                total++;
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
    if (off >= total || total == 0) { if (cb) cb(1, NULL, 0, 0, ud); return 0; }
    int n = total - off;
    if (n > EVO_PROVIDER_PAGE_MAX) n = EVO_PROVIDER_PAGE_MAX;

    evo_provider_item_t *items =
        (evo_provider_item_t *)calloc((size_t)n, sizeof *items);
    if (!items) { if (cb) cb(0, NULL, 0, 0, ud); return -1; }

    if (is_special_root > 0) {
        for (int k = 0; k < n; ++k) {
            int idx = off + k;
            if (idx == 0) {
                evo_provider_item_clear(&items[k]);
                snprintf(items[k].id, sizeof items[k].id, "fav");
                snprintf(items[k].title, sizeof items[k].title, "★ FAVORITES");
                snprintf(items[k].subtitle, sizeof items[k].subtitle, "Starred channels");
                items[k].kind = EVO_MEDIA_FOLDER;
                items[k].is_folder = 1;
            } else if (G.lang_count > 0 && idx == 1) {
                evo_provider_item_clear(&items[k]);
                snprintf(items[k].id, sizeof items[k].id, "langs");
                snprintf(items[k].title, sizeof items[k].title, "LANGUAGES");
                snprintf(items[k].subtitle, sizeof items[k].subtitle, "%d detected languages", G.lang_count);
                items[k].kind = EVO_MEDIA_FOLDER;
                items[k].is_folder = 1;
            } else {
                int item_idx = idx - is_special_root;
                if (is_group_page) fill_group_item(&items[k], item_idx);
                else               fill_channel_item(&items[k], item_idx, parent_id);
            }
        }
    } else if (is_fav_page) {
        int seen = 0, k = 0;
        for (int i = 0; i < G.ch_count && k < n; ++i) {
            if (favorites_is_favorite(G.ch[i].url) || favorites_is_favorite(G.ch[i].name)) {
                if (seen++ >= off) {
                    fill_channel_item(&items[k++], i, parent_id);
                }
            }
        }
    } else if (is_langs_page) {
        for (int k = 0; k < n; ++k) {
            int l = off + k;
            evo_provider_item_clear(&items[k]);
            snprintf(items[k].id, sizeof items[k].id, "lang:%s", G.langs[l].code);
            snprintf(items[k].title, sizeof items[k].title, "%s", G.langs[l].name);
            snprintf(items[k].subtitle, sizeof items[k].subtitle, "%d channel%s",
                     G.langs[l].count, G.langs[l].count == 1 ? "" : "s");
            items[k].kind = EVO_MEDIA_FOLDER;
            items[k].is_folder = 1;
        }
    } else if (filter_lang[0]) {
        int seen = 0, k = 0;
        for (int i = 0; i < G.ch_count && k < n; ++i) {
            if (G.ch[i].lang && strcasecmp(G.ch[i].lang, filter_lang) == 0) {
                if (seen++ >= off) {
                    fill_channel_item(&items[k++], i, parent_id);
                }
            }
        }
    } else {
        for (int k = 0; k < n; ++k) {
            if (is_group_page) fill_group_item(&items[k], off + k);
            else               fill_channel_item(&items[k], base + off + k, parent_id);
        }
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

__attribute__((weak)) void evo_rmlui_provider_reload(void);

static void on_epg(int success, int status, const char *body, size_t len, void *ud)
{
    (void)ud;
    PROV_LOG("iptv: on_epg result success=%d status=%d len=%zu", success, status, len);
    if (success && status == 200 && body && len) {
        parse_xmltv(body, len);
        PROV_LOG("iptv: parse_xmltv finished, requesting UI level reload to display EPG");
        if (evo_rmlui_provider_reload) {
            evo_rmlui_provider_reload();
        }
    } else {
        G.epg_loaded = 1;   /* do not retry every page turn */
    }
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
    PROV_LOG("iptv: fetch_playlist url='%s' loading=%d", G.playlist_url, G.loading);
    if (!G.playlist_url[0]) { free(pd); return -1; }
    if (G.loading)          { free(pd); return -2; }
    G.loading = 1;
    PROV_LOG("iptv: calling evo_net_request_async for playlist");
    int rc = evo_net_request_async("GET", G.playlist_url, NULL, NULL, 0,
                                   on_playlist, pd);
    PROV_LOG("iptv: evo_net_request_async returned %d", rc);
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
    /* Only stats a handful of paths - no network, so it is safe during boot. */
    resolve_playlist_source();
    return 0;
}

static void iptv_shutdown(void)
{
    free_channels();
}

static int iptv_is_configured(void)
{
    /* Either a URL to fetch or a file on the stick. The file is what makes the
     * provider usable before any setup screen exists. */
    return (G.playlist_url[0] || G.playlist_file[0]) ? 1 : 0;
}

static int iptv_list_catalog(const char *parent_id, int page,
                             evo_provider_items_cb cb, void *ud)
{
    PROV_LOG("iptv: iptv_list_catalog parent='%s' page=%d loaded=%d url='%s'",
             parent_id ? parent_id : "", page, G.loaded, G.playlist_url);
    if (!G.playlist_url[0] && !G.playlist_file[0]) return -1;

    if (G.loaded)
        return emit_page(parent_id, page, cb, ud);

    /*
     * A local playlist parses synchronously, so the first page is emitted in
     * this call - no fetch, no callback, no spinner.
     */
    if (!G.playlist_url[0] && G.playlist_file[0]) {
        if (load_playlist_file(G.playlist_file) < 0) return -1;
        kick_epg();
        return emit_page(parent_id, page, cb, ud);
    }

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
    if (!G.playlist_url[0] && !G.playlist_file[0]) return -1;

    if (!G.loaded && !G.playlist_url[0] && G.playlist_file[0]) {
        if (load_playlist_file(G.playlist_file) < 0) return -1;
        kick_epg();
    }

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

    evo_stream_choice_t choices[2];
    memset(choices, 0, sizeof choices);
    int count = 1;

    evo_provider_stream_choice_clear(&choices[0]);
    snprintf(choices[0].url, sizeof choices[0].url, "%s", G.ch[i].url);
    choices[0].is_live = 1;

    const char *url = G.ch[i].url;
    int is_hls = (strstr(url, ".m3u8") != NULL || strstr(url, "m3u8") != NULL);
    int is_ts = (strstr(url, ".ts") != NULL);

    if (is_hls) {
        snprintf(choices[0].label, sizeof choices[0].label, "HLS (Adaptive)");
        snprintf(choices[0].container, sizeof choices[0].container, "hls");
    } else if (is_ts) {
        snprintf(choices[0].label, sizeof choices[0].label, "MPEG-TS (Live)");
        snprintf(choices[0].container, sizeof choices[0].container, "mpegts");
    } else {
        snprintf(choices[0].label, sizeof choices[0].label, "Live Stream");
    }

    /* Container hint from URL if not already set */
    if (!choices[0].container[0]) {
        const char *q = strchr(url, '?');
        const char *dot = NULL;
        for (const char *p = url; *p && (!q || p < q); ++p)
            if (*p == '.') dot = p;
        if (dot) {
            size_t n = q ? (size_t)(q - dot - 1) : strlen(dot + 1);
            if (n > 0 && n < sizeof choices[0].container)
                snprintf(choices[0].container, sizeof choices[0].container, "%.*s", (int)n, dot + 1);
        }
    }

    /* If URL has standard .m3u8 or .ts extension, offer the alternative format as fallback choice */
    if (is_hls && strstr(url, ".m3u8")) {
        evo_provider_stream_choice_clear(&choices[1]);
        snprintf(choices[1].url, sizeof choices[1].url, "%s", url);
        char *ext = strstr(choices[1].url, ".m3u8");
        if (ext) {
            ext[1] = 't';
            ext[2] = 's';
            memmove(ext + 3, ext + 5, strlen(ext + 5) + 1);
            snprintf(choices[1].label, sizeof choices[1].label, "MPEG-TS Fallback");
            snprintf(choices[1].container, sizeof choices[1].container, "mpegts");
            choices[1].is_live = 1;
            count = 2;
        }
    } else if (is_ts && strstr(url, ".ts")) {
        evo_provider_stream_choice_clear(&choices[1]);
        snprintf(choices[1].url, sizeof choices[1].url, "%s", url);
        char *ext = strstr(choices[1].url, ".ts");
        if (ext && (strlen(choices[1].url) + 3 < sizeof choices[1].url)) {
            char rest[256] = {0};
            snprintf(rest, sizeof rest, "%s", ext + 3);
            snprintf(ext, sizeof choices[1].url - (ext - choices[1].url), ".m3u8%s", rest);
            snprintf(choices[1].label, sizeof choices[1].label, "HLS Fallback");
            snprintf(choices[1].container, sizeof choices[1].container, "hls");
            choices[1].is_live = 1;
            count = 2;
        }
    }

    /* Synchronous, but delivered through callback per contract */
    if (cb) cb(1, choices, count, ud);
    return 0;
}

static const char *iptv_ui_bundle_url(void)
{
    return G.bundle_url[0] ? G.bundle_url : NULL;
}

/* ------------------------------------------------------------------------- */
/* CAP_CONFIG - the typed playlist URL                                       */
/* ------------------------------------------------------------------------- */

static const char *iptv_get_source(void)
{
    /* Show the URL if one is set; otherwise the last URL typed if any;
     * otherwise the USB file that is standing in for it. */
    if (G.playlist_url[0]) return G.playlist_url;
    if (G.last_url[0])     return G.last_url;
    if (G.playlist_file[0]) return G.playlist_file;
    return "";
}

static int iptv_set_source(const char *value)
{
    if (!value) return -1;

    /* Empty clears the URL and falls back to a USB playlist if there is one -
     * which is how a user undoes a typo without having to retype anything. */
    if (!*value) {
        G.playlist_url[0] = 0;
        G.loaded = 0;
        free_channels();
        resolve_playlist_source();
        provider_iptv_save_conf();
        PROV_LOG("iptv: source cleared; usb fallback='%s'", G.playlist_file);
        return 0;
    }

    /* A local path is legitimate - someone may point this at a file on the
     * stick by hand - so accept either, and reject anything that is neither. */
    int is_url  = (strncmp(value, "http://", 7) == 0 ||
                   strncmp(value, "https://", 8) == 0);
    int is_path = (value[0] == '/');
    if (!is_url && !is_path) {
        PROV_LOG("iptv: refused source '%s' (not http(s):// or an absolute path)",
                 value);
        return -1;
    }
    if (strlen(value) >= sizeof G.playlist_url) {
        PROV_LOG("iptv: refused source - %zu chars, max %zu",
                 strlen(value), sizeof G.playlist_url - 1);
        return -1;
    }

    free_channels();          /* also clears G.loaded / G.epg_loaded */
    G.loading = 0;

    if (is_url) {
        snprintf(G.playlist_url, sizeof G.playlist_url, "%s", value);
        snprintf(G.last_url,     sizeof G.last_url,     "%s", value);
        G.playlist_file[0] = 0;
    } else {
        G.playlist_url[0] = 0;
        snprintf(G.playlist_file, sizeof G.playlist_file, "%s", value);
    }

    provider_iptv_save_conf();
    PROV_LOG("iptv: source set to '%s'", value);
    return 0;
}

const evo_provider_t evo_provider_iptv = {
    .id            = "iptv",
    .name          = "IPTV",
    .icon          = "icon_emby.png",   /* the shared provider rail slot (#90) */
    .caps          = EVO_PROVIDER_CAP_CATALOG | EVO_PROVIDER_CAP_SEARCH |
                     EVO_PROVIDER_CAP_RESOLVE |
                     EVO_PROVIDER_CAP_LIVE    | EVO_PROVIDER_CAP_CONFIG,
    .api_version   = EVO_PROVIDER_API_VERSION,
    .init          = iptv_init,
    .shutdown      = iptv_shutdown,
    .is_configured = iptv_is_configured,
    .auth          = NULL,
    .list_catalog  = iptv_list_catalog,
    .search        = iptv_search,
    .resolve       = iptv_resolve,
    .report_progress = NULL,
    .ui_bundle_url = NULL,
    .get_source    = iptv_get_source,
    .set_source    = iptv_set_source,
};
