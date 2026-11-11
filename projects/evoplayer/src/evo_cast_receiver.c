/*
 * evo_cast_receiver.c - DIAL (DIscovery And Launch) cast receiver for EVO Player.
 *
 * Implements SSDP discovery (UDP 1900 on 239.255.255.250) and DIAL HTTP
 * server (TCP 8765) as described in the DIAL-multiscreen protocol and CastDesk.
 */
#include "evo_cast_receiver.h"
#include "evo_boot_log.h"
#include "evo_data_path.h"
#include "evo_net.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#ifndef NO_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

#define SSDP_MULTICAST_ADDR "239.255.255.250"
#define SSDP_PORT           1900
#define SSDP_MAX_AGE        1800
#define MAX_REQ_BUF         (64 * 1024)
#define MAX_ADDITIONAL_DATA 16

typedef struct {
    char key[64];
    char value[256];
} evo_cast_kv_t;

static pthread_mutex_t s_cast_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    s_running = 0;
static volatile int    s_app_running = 0;
static int             s_dial_port = EVO_CAST_DEFAULT_PORT;
static char            s_friendly_name[128] = "EVO Player";
static char            s_device_id[64] = "";
static evo_cast_launch_cb s_on_launch = NULL;
static evo_cast_stop_cb   s_on_stop = NULL;
static void           *s_userdata = NULL;

static char           *s_dash_mpd = NULL;
static size_t          s_dash_mpd_len = 0;
static char            s_cast_media_title[256] = {0};
static char            s_direct_media_url[4096] = {0};
/* The real googlevideo URLs of the cast's video and audio. The MPD names them
 * http://127.0.0.1:<port>/cast/v and /cast/a, and the DIAL server answers
 * those with a 302 to these. See parse_innertube_streams for why. */
static char           *s_cast_seg_url[2] = { NULL, NULL };

static evo_cast_kv_t   s_add_data[MAX_ADDITIONAL_DATA];
static int             s_add_data_count = 0;

static char            s_screen_id[128] = {0};
static char            s_lounge_token[512] = {0};
static char            s_lounge_sid[128] = {0};
static char            s_lounge_gsid[128] = {0};
static int             s_lounge_rid = 1;
static int             s_lounge_ofs = 0;
static int             s_lounge_aid = 0;
static char            s_last_cast_vid[64] = {0};
static pthread_t       s_lounge_th = 0;
static volatile int    s_lounge_running = 0;

static pthread_t       s_ssdp_th;
static pthread_t       s_dial_th;
static int             s_ssdp_fd = -1;
static int             s_dial_fd = -1;

/* ---- UUID / Device ID ---- */

static void generate_uuid_v4(char *out, size_t cap)
{
    unsigned char b[16];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    srand((unsigned)(tv.tv_sec ^ tv.tv_usec ^ (long)getpid()));
    for (int i = 0; i < 16; i++) {
        b[i] = (unsigned char)(rand() & 0xFF);
    }
    b[6] = (b[6] & 0x0F) | 0x40; /* version 4 */
    b[8] = (b[8] & 0x3F) | 0x80; /* variant 1 */
    snprintf(out, cap,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void load_or_create_device_id(void)
{
    pthread_mutex_lock(&s_cast_lock);
    if (s_device_id[0] != '\0') {
        pthread_mutex_unlock(&s_cast_lock);
        return;
    }

    const char *path = evo_data_path("cast_device_id.txt");
    FILE *f = fopen(path, "r");
    if (f) {
        char buf[128] = {0};
        if (fgets(buf, sizeof(buf), f)) {
            char *p = buf;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            char *end = p + strlen(p) - 1;
            while (end > p && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
                *end = '\0';
                end--;
            }
            if (strlen(p) >= 32) {
                snprintf(s_device_id, sizeof(s_device_id), "%s", p);
            }
        }
        fclose(f);
    }

    if (s_device_id[0] == '\0') {
        generate_uuid_v4(s_device_id, sizeof(s_device_id));
        FILE *wf = fopen(path, "w");
        if (wf) {
            fprintf(wf, "%s\n", s_device_id);
            fclose(wf);
        }
    }
    pthread_mutex_unlock(&s_cast_lock);
}

/* ---- Local IP Resolution ---- */

void evo_cast_receiver_get_local_ip(char *out, size_t cap)
{
    out[0] = '\0';
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(80);
    to.sin_addr.s_addr = htonl(0x08080808u); /* 8.8.8.8 */
    struct sockaddr_in me;
    socklen_t ml = sizeof me;
    if (connect(s, (struct sockaddr *)&to, sizeof to) == 0 && /* connect-ok: UDP, sends nothing */
        getsockname(s, (struct sockaddr *)&me, &ml) == 0 && me.sin_addr.s_addr != 0) {
        uint32_t a = ntohl(me.sin_addr.s_addr);
        snprintf(out, cap, "%u.%u.%u.%u", (a >> 24) & 255, (a >> 16) & 255, (a >> 8) & 255, a & 255);
    }
    close(s);
}

/* ---- URL & XML helpers ---- */

static void xml_escape(const char *in, char *out, size_t cap)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 6 < cap; i++) {
        switch (in[i]) {
            case '&': memcpy(&out[j], "&amp;", 5); j += 5; break;
            case '<': memcpy(&out[j], "&lt;", 4); j += 4; break;
            case '>': memcpy(&out[j], "&gt;", 4); j += 4; break;
            case '\"': memcpy(&out[j], "&quot;", 6); j += 6; break;
            case '\'': memcpy(&out[j], "&apos;", 6); j += 6; break;
            default: out[j++] = in[i]; break;
        }
    }
    out[j] = '\0';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *src, char *dst, size_t cap)
{
    size_t i = 0, j = 0;
    while (src[i] && j + 1 < cap) {
        if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            int h1 = hex_val(src[i + 1]);
            int h2 = hex_val(src[i + 2]);
            if (h1 >= 0 && h2 >= 0) {
                dst[j++] = (char)((h1 << 4) | h2);
                i += 3;
            } else {
                dst[j++] = src[i++];
            }
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

int evo_cast_parse_param(const char *data, const char *key, char *out, size_t cap)
{
    if (!data || !key || !out || cap == 0) return 0;
    out[0] = '\0';
    size_t klen = strlen(key);
    const char *p = data;
    while (*p) {
        if ((p == data || *(p - 1) == '&' || *(p - 1) == '?') &&
            strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *val_start = p + klen + 1;
            const char *val_end = strchr(val_start, '&');
            size_t vlen = val_end ? (size_t)(val_end - val_start) : strlen(val_start);
            char raw[512];
            if (vlen >= sizeof(raw)) vlen = sizeof(raw) - 1;
            memcpy(raw, val_start, vlen);
            raw[vlen] = '\0';
            url_decode(raw, out, cap);
            return 1;
        }
        p++;
    }
    return 0;
}

int evo_cast_parse_lounge_video(const char *text, char *out_vid, size_t vid_cap, char *out_time, size_t time_cap)
{
    if (!text || !out_vid || vid_cap < 12) return 0;
    out_vid[0] = '\0';
    if (out_time && time_cap > 0) out_time[0] = '\0';

    /* Look for "videoId" or "videoIds" (with or without backslash escape) */
    const char *p = text;
    while ((p = strstr(p, "videoId")) != NULL) {
        /* Advance past "videoId" or "videoIds" */
        p += 7;
        if (*p == 's' || *p == 'S') p++;

        /* Skip quotes, backslashes, colons, brackets, spaces */
        while (*p && (*p == '"' || *p == '\\' || *p == ':' || *p == '[' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
            p++;
        }

        /* Check for 11-char YouTube ID */
        size_t len = 0;
        while (p[len] && (
               (p[len] >= 'a' && p[len] <= 'z') ||
               (p[len] >= 'A' && p[len] <= 'Z') ||
               (p[len] >= '0' && p[len] <= '9') ||
               p[len] == '-' || p[len] == '_')) {
            len++;
        }

        if (len == 11) {
            memcpy(out_vid, p, 11);
            out_vid[11] = '\0';
            break;
        }
        p += (len > 0 ? len : 1);
    }

    if (!out_vid[0]) {
        return 0;
    }

    /* Check for currentTime */
    if (out_time && time_cap > 0) {
        const char *t = strstr(text, "currentTime");
        if (t) {
            t += 11;
            while (*t && (*t == '"' || *t == '\\' || *t == ':' || *t == ' ' || *t == '\t')) {
                t++;
            }
            size_t tlen = 0;
            while (t[tlen] && (
                   (t[tlen] >= '0' && t[tlen] <= '9') ||
                   t[tlen] == '.')) {
                tlen++;
            }
            if (tlen > 0 && tlen < time_cap) {
                memcpy(out_time, t, tlen);
                out_time[tlen] = '\0';
            }
        }
    }
    return 1;
}

int evo_cast_register_pairing_code(const char *pairing_code)
{
    if (!pairing_code || !pairing_code[0]) return -1;
    pthread_mutex_lock(&s_cast_lock);
    char screen_id[128] = {0};
    char lounge_token[512] = {0};
    snprintf(screen_id, sizeof(screen_id), "%s", s_screen_id);
    snprintf(lounge_token, sizeof(lounge_token), "%s", s_lounge_token);
    pthread_mutex_unlock(&s_cast_lock);

    if (!screen_id[0] || !lounge_token[0]) {
        evo_log("cast: cannot register pairing code (screen_id or token not ready)");
        return -1;
    }

    char post_data[512];
    snprintf(post_data, sizeof(post_data),
        "pairing_code=%s&screen_id=%s&access_type=permanent",
        pairing_code, screen_id);

    char hdr_token[600];
    snprintf(hdr_token, sizeof(hdr_token), "X-YouTube-LoungeId-Token: %s", lounge_token);
    const char *hdrs[] = {
        "Content-Type: application/x-www-form-urlencoded",
        hdr_token
    };
    char *body = NULL; size_t len = 0; int status = 0;
    int rc = evo_net_http_post_sync("https://www.youtube.com/api/lounge/pairing/register_pairing_code",
                                    post_data, hdrs, 2, &body, &len, &status);
    evo_log("cast: Registered pairingCode %s with Lounge -> rc=%d status=%d resp: %.200s",
            pairing_code, rc, status, body ? body : "(empty)");
    if (body) free(body);
    return (rc == EVO_NET_OK && status == 200) ? 0 : -1;
}

/* Parse form urlencoded pairs and store them as additionalData */
static void parse_and_store_additional_data(const char *body)
{
    if (!body || !body[0]) return;
    const char *p = body;
    while (*p) {
        const char *eq = strchr(p, '=');
        if (!eq) break;
        const char *amp = strchr(eq, '&');
        size_t klen = (size_t)(eq - p);
        size_t vlen = amp ? (size_t)(amp - (eq + 1)) : strlen(eq + 1);

        char kbuf[64], vbuf[256];
        if (klen >= sizeof(kbuf)) klen = sizeof(kbuf) - 1;
        if (vlen >= sizeof(vbuf)) vlen = sizeof(vbuf) - 1;
        memcpy(kbuf, p, klen); kbuf[klen] = '\0';
        memcpy(vbuf, eq + 1, vlen); vbuf[vlen] = '\0';

        char dec_k[64], dec_v[256];
        url_decode(kbuf, dec_k, sizeof(dec_k));
        url_decode(vbuf, dec_v, sizeof(dec_v));

        evo_cast_receiver_set_additional_data(dec_k, dec_v);

        if (!amp) break;
        p = amp + 1;
    }
}

/* ---- Public Status & Data APIs ---- */

int evo_cast_receiver_is_running(void)
{
    pthread_mutex_lock(&s_cast_lock);
    int r = s_running;
    pthread_mutex_unlock(&s_cast_lock);
    return r;
}

int evo_cast_receiver_port(void)
{
    pthread_mutex_lock(&s_cast_lock);
    int p = s_dial_port;
    pthread_mutex_unlock(&s_cast_lock);
    return p;
}

const char* evo_cast_receiver_device_id(void)
{
    return s_device_id;
}

const char* evo_cast_receiver_friendly_name(void)
{
    return s_friendly_name;
}

void evo_cast_receiver_set_friendly_name(const char *name)
{
    if (!name || !name[0]) return;
    pthread_mutex_lock(&s_cast_lock);
    snprintf(s_friendly_name, sizeof(s_friendly_name), "%s", name);
    pthread_mutex_unlock(&s_cast_lock);
}

void evo_cast_receiver_set_callbacks(evo_cast_launch_cb on_launch, evo_cast_stop_cb on_stop, void *userdata)
{
    pthread_mutex_lock(&s_cast_lock);
    s_on_launch = on_launch;
    s_on_stop = on_stop;
    s_userdata = userdata;
    pthread_mutex_unlock(&s_cast_lock);
}

void evo_cast_receiver_set_additional_data(const char *key, const char *value)
{
    if (!key || !key[0]) return;
    pthread_mutex_lock(&s_cast_lock);
    for (int i = 0; i < s_add_data_count; i++) {
        if (strcmp(s_add_data[i].key, key) == 0) {
            snprintf(s_add_data[i].value, sizeof(s_add_data[i].value), "%s", value ? value : "");
            pthread_mutex_unlock(&s_cast_lock);
            return;
        }
    }
    if (s_add_data_count < MAX_ADDITIONAL_DATA) {
        snprintf(s_add_data[s_add_data_count].key, sizeof(s_add_data[0].key), "%s", key);
        snprintf(s_add_data[s_add_data_count].value, sizeof(s_add_data[0].value), "%s", value ? value : "");
        s_add_data_count++;
    }
    pthread_mutex_unlock(&s_cast_lock);
}

void evo_cast_receiver_clear_additional_data(void)
{
    pthread_mutex_lock(&s_cast_lock);
    s_add_data_count = 0;
    pthread_mutex_unlock(&s_cast_lock);
}

int evo_cast_receiver_app_is_running(void)
{
    pthread_mutex_lock(&s_cast_lock);
    int r = s_app_running;
    pthread_mutex_unlock(&s_cast_lock);
    return r;
}

void evo_cast_receiver_set_app_running(int running)
{
    pthread_mutex_lock(&s_cast_lock);
    s_app_running = running;
    if (!running) {
        s_add_data_count = 0;
    }
    pthread_mutex_unlock(&s_cast_lock);
}

/* ---- SSDP Worker ---- */

static void send_ssdp_reply(int fd, const struct sockaddr_in *client, const char *st, const char *local_ip, int port)
{
    char date_buf[64];
    time_t now = time(NULL);
    struct tm gmt;
    gmtime_r(&now, &gmt);
    strftime(date_buf, sizeof(date_buf), "%a, %d %b %Y %H:%M:%S GMT", &gmt);

    char reply[1024];
    int len = snprintf(reply, sizeof(reply),
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=%d\r\n"
        "DATE: %s\r\n"
        "EXT:\r\n"
        "LOCATION: http://%s:%d/ssdp/device-desc.xml\r\n"
        "SERVER: FreeBSD/13.6 UPnP/1.1 EVOPlayer/1.0\r\n"
        "ST: %s\r\n"
        "USN: uuid:%s::%s\r\n"
        "BOOTID.UPNP.ORG: 1\r\n"
        "CONFIGID.UPNP.ORG: 1\r\n"
        "\r\n",
        SSDP_MAX_AGE, date_buf, local_ip, port, st, s_device_id, st);

    sendto(fd, reply, (size_t)len, 0, (const struct sockaddr *)client, sizeof(*client));
}

static void send_ssdp_notify(int fd, const char *local_ip, int port, const char *nts)
{
    static const char *targets[] = {
        "upnp:rootdevice",
        "urn:dial-multiscreen-org:device:dial:1",
        "urn:dial-multiscreen-org:service:dial:1",
    };

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(SSDP_PORT);
    dest.sin_addr.s_addr = inet_addr(SSDP_MULTICAST_ADDR);

    for (size_t i = 0; i < sizeof(targets)/sizeof(targets[0]); i++) {
        char msg[1024];
        int len = snprintf(msg, sizeof(msg),
            "NOTIFY * HTTP/1.1\r\n"
            "HOST: %s:%d\r\n"
            "CACHE-CONTROL: max-age=%d\r\n"
            "LOCATION: http://%s:%d/ssdp/device-desc.xml\r\n"
            "NT: %s\r\n"
            "NTS: %s\r\n"
            "SERVER: FreeBSD/13.6 UPnP/1.1 EVOPlayer/1.0\r\n"
            "USN: uuid:%s::%s\r\n"
            "BOOTID.UPNP.ORG: 1\r\n"
            "CONFIGID.UPNP.ORG: 1\r\n"
            "\r\n",
            SSDP_MULTICAST_ADDR, SSDP_PORT, SSDP_MAX_AGE, local_ip, port,
            targets[i], nts, s_device_id, targets[i]);

        sendto(fd, msg, (size_t)len, 0, (const struct sockaddr *)&dest, sizeof(dest));
    }
}

static void *ssdp_thread(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        evo_log("cast: cannot create SSDP socket (%d)", errno);
        return NULL;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(SSDP_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        evo_log("cast: SSDP bind port %d failed (errno %d)", SSDP_PORT, errno);
        close(fd);
        return NULL;
    }

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(SSDP_MULTICAST_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
        evo_log("cast: SSDP join multicast group failed (errno %d)", errno);
    }

    s_ssdp_fd = fd;
    evo_log("cast: SSDP discovery running on UDP %d", SSDP_PORT);

    char local_ip[32];
    evo_cast_receiver_get_local_ip(local_ip, sizeof(local_ip));
    if (local_ip[0]) {
        send_ssdp_notify(fd, local_ip, s_dial_port, "ssdp:alive");
    }

    time_t last_notify = time(NULL);

    while (s_running) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, 1000);
        if (!s_running) break;

        time_t now = time(NULL);
        if (now - last_notify >= 120) {
            last_notify = now;
            evo_cast_receiver_get_local_ip(local_ip, sizeof(local_ip));
            if (local_ip[0]) {
                send_ssdp_notify(fd, local_ip, s_dial_port, "ssdp:alive");
            }
        }

        if (pr <= 0 || !(pfd.revents & POLLIN)) continue;

        char buf[2048];
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&client, &clen);
        if (n <= 0) continue;
        buf[n] = '\0';

        if (strncasecmp(buf, "M-SEARCH", 8) != 0) continue;

        /* Find ST header */
        char st[256] = {0};
        char *line = strtok(buf, "\r\n");
        while (line) {
            if (strncasecmp(line, "ST:", 3) == 0) {
                const char *val = line + 3;
                while (*val == ' ' || *val == '\t') val++;
                snprintf(st, sizeof(st), "%s", val);
                break;
            }
            line = strtok(NULL, "\r\n");
        }

        if (!st[0]) continue;

        evo_cast_receiver_get_local_ip(local_ip, sizeof(local_ip));
        if (!local_ip[0]) continue;

        if (strcasecmp(st, "ssdp:all") == 0 ||
            strcasecmp(st, "upnp:rootdevice") == 0 ||
            strcasecmp(st, "urn:dial-multiscreen-org:device:dial:1") == 0 ||
            strcasecmp(st, "urn:dial-multiscreen-org:service:dial:1") == 0 ||
            strncasecmp(st, "uuid:", 5) == 0) {
            send_ssdp_reply(fd, &client, st, local_ip, s_dial_port);
        }
    }

    evo_cast_receiver_get_local_ip(local_ip, sizeof(local_ip));
    if (local_ip[0]) {
        send_ssdp_notify(fd, local_ip, s_dial_port, "ssdp:byebye");
    }

    close(fd);
    s_ssdp_fd = -1;
    return NULL;
}

/* ---- DIAL HTTP Server ---- */

static const char *find_case_str(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return NULL;
    size_t nlen = strlen(needle);
    if (nlen == 0) return haystack;
    while (*haystack) {
        if (strncasecmp(haystack, needle, nlen) == 0) return haystack;
        haystack++;
    }
    return NULL;
}

static void handle_dial_client(int cfd, const char *local_ip, int port)
{
    char buf[MAX_REQ_BUF];
    ssize_t total = 0;
    char *hdr_end = NULL;

    /* Set 5-second socket timeout */
    struct timeval tv = { 5, 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    while (total < (ssize_t)(sizeof(buf) - 1)) {
        ssize_t n = read(cfd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) break;
    }

    if (!hdr_end) {
        close(cfd);
        return;
    }

    /* Extract Content-Length if present */
    size_t content_len = 0;
    const char *cl = find_case_str(buf, "Content-Length:");
    if (cl) {
        content_len = (size_t)strtoul(cl + 15, NULL, 10);
    }

    size_t body_recvd = (size_t)(buf + total - (hdr_end + 4));
    while (body_recvd < content_len && total < (ssize_t)(sizeof(buf) - 1)) {
        ssize_t n = read(cfd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        body_recvd += n;
    }

    char method[16] = {0};
    char path[256] = {0};
    sscanf(buf, "%15s %255s", method, path);

    /* Strip query string from path for routing */
    char route_path[256];
    snprintf(route_path, sizeof(route_path), "%s", path);
    char *q = strchr(route_path, '?');
    if (q) *q = '\0';
    size_t rlen = strlen(route_path);
    while (rlen > 1 && route_path[rlen - 1] == '/') {
        route_path[--rlen] = '\0';
    }

    const char *body = hdr_end + 4;
    evo_log("cast: HTTP %s %s", method, path);

    /* Route: OPTIONS (CORS preflight) */
    if (strcasecmp(method, "OPTIONS") == 0) {
        const char *resp =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
            "Access-Control-Allow-Headers: *\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        (void)write(cfd, resp, strlen(resp));
        close(cfd);
        return;
    }

    /* Route: GET /ssdp/device-desc.xml (UPnP Device Description) */
    if (strcasecmp(method, "GET") == 0 &&
        (strcmp(route_path, "/ssdp/device-desc.xml") == 0 || strcmp(route_path, "/dd.xml") == 0)) {
        char esc_name[256];
        pthread_mutex_lock(&s_cast_lock);
        xml_escape(s_friendly_name, esc_name, sizeof(esc_name));
        pthread_mutex_unlock(&s_cast_lock);

        char xml_body[2048];
        int xlen = snprintf(xml_body, sizeof(xml_body),
            "<?xml version=\"1.0\"?>\n"
            "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\n"
            "  <specVersion><major>1</major><minor>0</minor></specVersion>\n"
            "  <URLBase>http://%s:%d</URLBase>\n"
            "  <device>\n"
            "    <deviceType>urn:dial-multiscreen-org:device:dial:1</deviceType>\n"
            "    <friendlyName>%s</friendlyName>\n"
            "    <manufacturer>EVO Player</manufacturer>\n"
            "    <modelName>EVO Player Cast Receiver</modelName>\n"
            "    <UDN>uuid:%s</UDN>\n"
            "    <serviceList>\n"
            "      <service>\n"
            "        <serviceType>urn:dial-multiscreen-org:service:dial:1</serviceType>\n"
            "        <serviceId>urn:dial-multiscreen-org:serviceId:dial</serviceId>\n"
            "        <controlURL>/ssdp/notfound</controlURL>\n"
            "        <eventSubURL>/ssdp/notfound</eventSubURL>\n"
            "        <SCPDURL>/ssdp/notfound</SCPDURL>\n"
            "      </service>\n"
            "    </serviceList>\n"
            "  </device>\n"
            "</root>\n",
            local_ip, port, esc_name, s_device_id);

        char resp[2560];
        int rlen2 = snprintf(resp, sizeof(resp),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/xml; charset=utf-8\r\n"
            "Application-URL: http://%s:%d/apps/\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n%s",
            local_ip, port, xlen, xml_body);

        (void)write(cfd, resp, (size_t)rlen2);
        close(cfd);
        return;
    }

    /* Route: GET /cast/v and /cast/a - redirect to the cast's googlevideo URL.
     * FFmpeg follows the 302 and keeps the byte range it asked for. */
    if (strcasecmp(method, "GET") == 0 &&
        (strcmp(route_path, "/cast/v") == 0 || strcmp(route_path, "/cast/a") == 0)) {
        int which = (route_path[6] == 'v') ? 0 : 1;
        char *resp = NULL;
        pthread_mutex_lock(&s_cast_lock);
        if (s_cast_seg_url[which]) {
            size_t cap = strlen(s_cast_seg_url[which]) + 256;
            resp = (char *)malloc(cap);
            if (resp) {
                snprintf(resp, cap,
                    "HTTP/1.1 302 Found\r\n"
                    "Location: %s\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n", s_cast_seg_url[which]);
            }
        }
        pthread_mutex_unlock(&s_cast_lock);
        if (resp) {
            (void)write(cfd, resp, strlen(resp));
            free(resp);
        } else {
            const char *err = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            (void)write(cfd, err, strlen(err));
        }
        close(cfd);
        return;
    }

    /* Route: GET /cast/stream.mpd or /apps/<name>/stream.mpd (DASH MPD Manifest) */
    if (strcasecmp(method, "GET") == 0 &&
        (strcmp(route_path, "/cast/stream.mpd") == 0 ||
         (strncmp(route_path, "/apps/", 6) == 0 && strstr(route_path, "stream.mpd") != NULL))) {
        pthread_mutex_lock(&s_cast_lock);
        size_t mlen = s_dash_mpd ? s_dash_mpd_len : 0;
        if (mlen == 0) {
            pthread_mutex_unlock(&s_cast_lock);
            const char *err = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            (void)write(cfd, err, strlen(err));
            close(cfd);
            return;
        }
        char hdr[512];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/dash+xml\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n", mlen);
        (void)write(cfd, hdr, (size_t)hlen);
        (void)write(cfd, s_dash_mpd, mlen);
        pthread_mutex_unlock(&s_cast_lock);
        close(cfd);
        return;
    }

    /* Routes under /apps/<app_name> */
    if (strncmp(route_path, "/apps/", 6) == 0) {
        char app_name[64] = "YouTube";
        const char *sub = route_path + 6;
        const char *slash = strchr(sub, '/');
        size_t n = slash ? (size_t)(slash - sub) : strlen(sub);
        if (n > 0 && n < sizeof(app_name)) {
            memcpy(app_name, sub, n);
            app_name[n] = '\0';
        }

        /* Route: GET /apps/<name> (App Status) or /apps/<name>/run */
        if (strcasecmp(method, "GET") == 0 && (slash == NULL || strcmp(slash, "/run") == 0)) {
            pthread_mutex_lock(&s_cast_lock);
            int running = s_app_running || (s_lounge_running && s_screen_id[0] != '\0');
            char add_xml[1024] = {0};
            if (s_add_data_count > 0) {
                strcat(add_xml, "  <additionalData>\n");
                for (int i = 0; i < s_add_data_count; i++) {
                    char ek[128], ev[256];
                    xml_escape(s_add_data[i].key, ek, sizeof(ek));
                    xml_escape(s_add_data[i].value, ev, sizeof(ev));
                    char line[512];
                    snprintf(line, sizeof(line), "    <%s>%s</%s>\n", ek, ev, ek);
                    strncat(add_xml, line, sizeof(add_xml) - strlen(add_xml) - 1);
                }
                strncat(add_xml, "  </additionalData>\n", sizeof(add_xml) - strlen(add_xml) - 1);
            } else if (s_screen_id[0] && s_lounge_token[0]) {
                strcat(add_xml, "  <additionalData>\n");
                char line1[256], line2[256], line3[600], line4[128];
                snprintf(line1, sizeof(line1), "    <screenId>%s</screenId>\n", s_screen_id);
                snprintf(line2, sizeof(line2), "    <deviceId>%s</deviceId>\n", s_device_id);
                snprintf(line3, sizeof(line3), "    <loungeToken>%s</loungeToken>\n", s_lounge_token);
                snprintf(line4, sizeof(line4), "    <loungeTokenRefreshIntervalMs>1500000</loungeTokenRefreshIntervalMs>\n");
                strncat(add_xml, line1, sizeof(add_xml) - strlen(add_xml) - 1);
                strncat(add_xml, line2, sizeof(add_xml) - strlen(add_xml) - 1);
                strncat(add_xml, line3, sizeof(add_xml) - strlen(add_xml) - 1);
                strncat(add_xml, line4, sizeof(add_xml) - strlen(add_xml) - 1);
                strncat(add_xml, "  </additionalData>\n", sizeof(add_xml) - strlen(add_xml) - 1);
            }
            pthread_mutex_unlock(&s_cast_lock);

            char xml_body[2048];
            int xlen = snprintf(xml_body, sizeof(xml_body),
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<service xmlns=\"urn:dial-multiscreen-org:schemas:dial\" dialVer=\"1.7\">\n"
                "  <name>%s</name>\n"
                "  <options allowStop=\"true\"/>\n"
                "  <state>%s</state>\n"
                "%s"
                "%s"
                "</service>\n",
                app_name,
                running ? "running" : "stopped",
                running ? "  <link rel=\"run\" href=\"run\"/>\n" : "",
                add_xml);

            char resp[2560];
            int rlen2 = snprintf(resp, sizeof(resp),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/xml; charset=utf-8\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n%s",
                xlen, xml_body);

            (void)write(cfd, resp, (size_t)rlen2);
            close(cfd);
            return;
        }

        /* Route: POST /apps/<name> (Launch Request) */
        if (strcasecmp(method, "POST") == 0 && slash == NULL) {
            evo_log("cast: phone requested %s launch: %s", app_name, body);

            char pcode[128] = {0};
            if (evo_cast_parse_param(body, "pairingCode", pcode, sizeof(pcode)) && pcode[0]) {
                evo_cast_register_pairing_code(pcode);
            }

            pthread_mutex_lock(&s_cast_lock);
            int was_running = s_app_running || (s_lounge_running && s_screen_id[0] != '\0');
            s_app_running = 1;
            s_add_data_count = 0;
            evo_cast_launch_cb cb = s_on_launch;
            void *ud = s_userdata;
            pthread_mutex_unlock(&s_cast_lock);

            if (cb) {
                cb(body, ud);
            }

            char resp[512];
            int rlen2 = snprintf(resp, sizeof(resp),
                "HTTP/1.1 %s\r\n"
                "Location: http://%s:%d/apps/%s/run\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n",
                was_running ? "200 OK" : "201 Created",
                local_ip, port, app_name);

            (void)write(cfd, resp, (size_t)rlen2);
            close(cfd);
            return;
        }

        /* Route: DELETE /apps/<name>/run (Stop Request) */
        if (strcasecmp(method, "DELETE") == 0 && slash && strcmp(slash, "/run") == 0) {
            evo_log("cast: phone requested %s stop", app_name);

            pthread_mutex_lock(&s_cast_lock);
            s_app_running = 0;
            s_add_data_count = 0;
            evo_cast_stop_cb cb = s_on_stop;
            void *ud = s_userdata;
            pthread_mutex_unlock(&s_cast_lock);

            if (cb) {
                cb(ud);
            }

            const char *resp =
                "HTTP/1.1 200 OK\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            (void)write(cfd, resp, strlen(resp));
            close(cfd);
            return;
        }

        /* Route: POST /apps/<name>/dial_data */
        if (strcasecmp(method, "POST") == 0 && slash && strcmp(slash, "/dial_data") == 0) {
            evo_log("cast: dial_data received: %s", body);
            parse_and_store_additional_data(body);

            const char *resp =
                "HTTP/1.1 200 OK\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                "Access-Control-Allow-Headers: *\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 2\r\n"
                "Connection: close\r\n\r\nOK";
            (void)write(cfd, resp, strlen(resp));
            close(cfd);
            return;
        }
    }

    /* 404 Not Found */
    const char *resp404 =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    (void)write(cfd, resp404, strlen(resp404));
    close(cfd);
}

static void *dial_thread(void *arg)
{
    (void)arg;
    int preferred_port = s_dial_port;
    int lfd = -1;
    int bound_port = preferred_port;

    for (int k = 0; k < EVO_CAST_FALLBACK_COUNT; k++) {
        int try_port = preferred_port + k;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) break;

        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons((uint16_t)try_port);
        sin.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) == 0 && listen(fd, 8) == 0) {
            lfd = fd;
            bound_port = try_port;
            break;
        }
        close(fd);
    }

    if (lfd < 0) {
        evo_log("cast: could not bind DIAL HTTP server on port %d..%d",
                preferred_port, preferred_port + EVO_CAST_FALLBACK_COUNT - 1);
        return NULL;
    }

    s_dial_fd = lfd;
    pthread_mutex_lock(&s_cast_lock);
    s_dial_port = bound_port;
    pthread_mutex_unlock(&s_cast_lock);

    evo_log("cast: DIAL HTTP server listening on TCP %d", bound_port);

    char local_ip[32];

    while (s_running) {
        struct pollfd pfd = { lfd, POLLIN, 0 };
        int pr = poll(&pfd, 1, 500);
        if (!s_running) break;
        if (pr <= 0 || !(pfd.revents & POLLIN)) continue;

        struct sockaddr_in cin;
        socklen_t clen = sizeof(cin);
        int cfd = accept(lfd, (struct sockaddr *)&cin, &clen);
        if (cfd < 0) continue;

        evo_cast_receiver_get_local_ip(local_ip, sizeof(local_ip));
        handle_dial_client(cfd, local_ip[0] ? local_ip : "127.0.0.1", bound_port);
    }

    close(lfd);
    s_dial_fd = -1;
    return NULL;
}

#ifndef NO_OPENSSL
static int lounge_send_command(const char *cmd, const char *extra_params)
{
    pthread_mutex_lock(&s_cast_lock);
    char sid[128], gsid[128], tok[512], dev_id[128];
    snprintf(sid, sizeof(sid), "%s", s_lounge_sid);
    snprintf(gsid, sizeof(gsid), "%s", s_lounge_gsid);
    snprintf(tok, sizeof(tok), "%s", s_lounge_token);
    snprintf(dev_id, sizeof(dev_id), "%s", s_device_id);
    int rid = s_lounge_rid++;
    int ofs = s_lounge_ofs++;
    int aid = s_lounge_aid;
    pthread_mutex_unlock(&s_cast_lock);

    if (!sid[0] || !tok[0]) {
        evo_log("cast: lounge_send_command('%s') failed: session not bound", cmd);
        return -1;
    }

    char url[1024];
    long now_ms = (long)time(NULL) * 1000;
    snprintf(url, sizeof(url),
        "https://www.youtube.com/api/lounge/bc/bind?device=LOUNGE_SCREEN&id=%s&name=%s&loungeIdToken=%s&VER=8&SID=%s&RID=%d&AID=%d&gsessionid=%s&zx=%ld",
        dev_id, "EVO%20Player", tok, sid, rid, aid, gsid, now_ms);

    char body[1024];
    snprintf(body, sizeof(body),
        "count=1&ofs=%d&req0__sc=%s%s",
        ofs, cmd, extra_params ? extra_params : "");

    char hdr_token[600];
    snprintf(hdr_token, sizeof(hdr_token), "X-YouTube-LoungeId-Token: %s", tok);
    const char *hdrs[] = {
        "Content-Type: application/x-www-form-urlencoded",
        hdr_token
    };

    char *resp_body = NULL;
    size_t resp_len = 0;
    int status = 0;
    int rc = evo_net_http_post_sync(url, body, hdrs, 2, &resp_body, &resp_len, &status);
    evo_log("cast: lounge_send_command('%s', body='%s') -> status=%d rc=%d (RID=%d ofs=%d) resp: %.200s",
            cmd, body, status, rc, rid, ofs, resp_body ? resp_body : "(null)");
    if (resp_body) free(resp_body);
    return (rc == EVO_NET_OK && status == 200) ? 0 : -1;
}
#endif

/* Handles one chunk of a Lounge channel stream (NUL-terminated, writable). */
typedef void (*lounge_chunk_fn)(char *chunk);

static void screen_on_chunk(char *chunk);

/* Streams one Lounge channel GET until it ends, YouTube goes quiet for 45 s,
 * or *alive (or s_running) drops. Returns the HTTP status (200 once the
 * stream ran), or -1 when the connection failed. */
static int lounge_listen_channel(const char *poll_url, const char *token,
                                 lounge_chunk_fn on_chunk, volatile int *alive)
{
#ifndef NO_OPENSSL
    char host[128] = "www.youtube.com";
    int port = 443;
    const char *path = poll_url;
    if (strncmp(poll_url, "https://", 8) == 0) {
        const char *p = poll_url + 8;
        const char *slash = strchr(p, '/');
        if (slash) {
            size_t hlen = (size_t)(slash - p);
            if (hlen < sizeof(host)) {
                memcpy(host, p, hlen);
                host[hlen] = '\0';
            }
            path = slash;
        }
    }

    struct addrinfo hints, *res = NULL;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        evo_log("cast: Lounge channel DNS lookup failed for %s", host);
        return -1;
    }

    int sock = evo_net_connect_list(res, 5000);
    freeaddrinfo(res);
    if (sock < 0) {
        evo_log("cast: Lounge channel connect failed for %s:%d", host, port);
        return -1;
    }

    /* Handshake and header reads: short timeout. The stream itself is read
     * through poll() below so a stop request is seen within half a second. */
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    SSL_CTX *ctx = evo_net_get_ssl_ctx();
    if (!ctx) {
        evo_log("cast: Lounge channel no SSL_CTX");
        close(sock);
        return -1;
    }
    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        evo_log("cast: Lounge channel SSL_new failed");
        close(sock);
        return -1;
    }
    SSL_set_tlsext_host_name(ssl, host);
    SSL_set_fd(ssl, sock);
    int sc = SSL_connect(ssl);
    if (sc <= 0) {
        evo_log("cast: Lounge channel SSL_connect failed (err=%d)", SSL_get_error(ssl, sc));
        SSL_free(ssl);
        close(sock);
        return -1;
    }

    char req[2048];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: EVOPlayer-PS5/1.0\r\n"
        "Accept: */*\r\n"
        "X-YouTube-LoungeId-Token: %s\r\n"
        "Connection: keep-alive\r\n\r\n",
        path, host, token ? token : "");
    if (SSL_write(ssl, req, rlen) <= 0) {
        evo_log("cast: Lounge channel SSL_write failed");
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(sock);
        return -1;
    }

    /* Read HTTP response header */
    char hbuf[2048];
    int htotal = 0;
    int status = 0;
    while (htotal < (int)sizeof(hbuf) - 1) {
        int r = SSL_read(ssl, hbuf + htotal, 1);
        if (r <= 0) {
            int err = SSL_get_error(ssl, r);
            evo_log("cast: Lounge channel header read failed r=%d err=%d", r, err);
            break;
        }
        htotal += r;
        hbuf[htotal] = '\0';
        char *end = strstr(hbuf, "\r\n\r\n");
        if (end) break;
    }
    if (sscanf(hbuf, "HTTP/1.%*d %d", &status) != 1) {
        status = 0;
    }

    evo_log("cast: Lounge channel status=%d htotal=%d", status, htotal);

    if (status != 200) {
        evo_log("cast: Lounge channel unexpected status %d: %.200s", status, hbuf);
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(sock);
        return status;
    }

    /* Stream chunks as they arrive from YouTube. Wait in poll() (500 ms
     * slices) rather than a long blocking SSL_read, so evo_cast_receiver_stop()
     * - which runs on the soft-close path - never waits on this thread for
     * more than a moment. YouTube sends a noop about every 30 s; 45 s of
     * silence means the channel is dead. */
    int idle_ms = 0;
    while (s_running && *alive) {
        if (SSL_pending(ssl) <= 0) {
            struct pollfd pfd = { sock, POLLIN, 0 };
            int pr = poll(&pfd, 1, 500);
            if (pr < 0) {
                evo_log("cast: Lounge channel poll failed (errno %d)", errno);
                break;
            }
            if (pr == 0) {
                idle_ms += 500;
                if (idle_ms >= 45000) {
                    evo_log("cast: Lounge channel idle 45 s, reconnecting");
                    break;
                }
                continue;
            }
        }
        idle_ms = 0;
        char chunk[4096];
        int r = SSL_read(ssl, chunk, sizeof(chunk) - 1);
        if (r <= 0) {
            int err = SSL_get_error(ssl, r);
            evo_log("cast: Lounge channel stream read ended (r=%d, ssl_err=%d)", r, err);
            break;
        }
        chunk[r] = '\0';
        /* Not logged: logging every chunk's body flooded evo.log (and the USB
         * writes that costs) for as long as EVO was open. */
        on_chunk(chunk);
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(sock);
    return 200;
#else
    (void)poll_url;
    (void)token;
    (void)on_chunk;
    (void)alive;
    return -1;
#endif
}

/* Highest message id ("[[N," or ",[N,") in a Lounge chunk, or `cur`. */
static int lounge_max_aid(const char *chunk, int cur)
{
    for (const char *p = chunk; *p; p++) {
        if ((*p == '[' && p[1] == '[') || (*p == ',' && p[1] == '[')) {
            char *end = NULL;
            long v = strtol(p + 2, &end, 10);
            if (end && *end == ',' && v > cur) cur = (int)v;
        }
    }
    return cur;
}

/* Events for the main thread (evo_cast_take_events). */
static volatile int s_cast_events = 0;

static void cast_post_event(int ev)
{
    pthread_mutex_lock(&s_cast_lock);
    s_cast_events |= ev;
    pthread_mutex_unlock(&s_cast_lock);
}

int evo_cast_take_events(void)
{
    pthread_mutex_lock(&s_cast_lock);
    int ev = s_cast_events;
    s_cast_events = 0;
    pthread_mutex_unlock(&s_cast_lock);
    return ev;
}

/* Remotes (phones) on EVO's screen, by Lounge id, from remoteConnected /
 * remoteDisconnected. Lounge thread only. */
#define MAX_REMOTES 8
static char s_remotes[MAX_REMOTES][64];
static int  s_remote_count = 0;

/* The "id" of the Lounge message object starting at msg (escaped ids inside
 * its "device" string do not match the unescaped pattern). */
static int lounge_msg_id(const char *msg, char *out, size_t cap)
{
    const char *end = strstr(msg, "}]");
    const char *p = strstr(msg, "\"id\":\"");
    if (!p || (end && p > end)) return 0;
    p += 6;
    size_t n = 0;
    while (p[n] && p[n] != '"' && n < cap - 1) { out[n] = p[n]; n++; }
    out[n] = '\0';
    return n > 0;
}

static void track_remotes(const char *chunk)
{
    char id[64];
    for (const char *p = strstr(chunk, "[\"remoteConnected\",{"); p; p = strstr(p + 1, "[\"remoteConnected\",{")) {
        if (!lounge_msg_id(p, id, sizeof(id))) continue;
        int known = 0;
        for (int i = 0; i < s_remote_count; i++) if (!strcmp(s_remotes[i], id)) known = 1;
        if (!known && s_remote_count < MAX_REMOTES)
            snprintf(s_remotes[s_remote_count++], sizeof(s_remotes[0]), "%s", id);
        evo_log("cast: Lounge remoteConnected %s (%d remote(s))", id, s_remote_count);
    }
    for (const char *p = strstr(chunk, "[\"remoteDisconnected\",{"); p; p = strstr(p + 1, "[\"remoteDisconnected\",{")) {
        if (!lounge_msg_id(p, id, sizeof(id))) id[0] = '\0';
        for (int i = 0; i < s_remote_count; i++) {
            if (!strcmp(s_remotes[i], id)) {
                memmove(s_remotes[i], s_remotes[i + 1], (size_t)(s_remote_count - i - 1) * sizeof(s_remotes[0]));
                s_remote_count--;
                break;
            }
        }
        evo_log("cast: Lounge remoteDisconnected %s (%d remote(s) left)", id, s_remote_count);
        if (s_remote_count == 0) cast_post_event(EVO_CAST_EV_PHONE_LEFT);
    }
}

/* One chunk of EVO's own screen channel (the session the phone talks to). */
static void screen_on_chunk(char *chunk)
{
#ifndef NO_OPENSSL
        /* Update gsessionid from stream if YouTube assigned a new one */
        const char *ss = strstr(chunk, "[\"S\",\"");
        if (ss) {
            ss += 6;
            const char *se = strchr(ss, '"');
            if (se && (size_t)(se - ss) < sizeof(s_lounge_gsid)) {
                pthread_mutex_lock(&s_cast_lock);
                memcpy(s_lounge_gsid, ss, (size_t)(se - ss));
                s_lounge_gsid[se - ss] = '\0';
                pthread_mutex_unlock(&s_cast_lock);
                evo_log("cast: Lounge gsessionid updated: %s", s_lounge_gsid);
            }
        }
        /* Update sid from stream if present */
        const char *cs = strstr(chunk, "[\"c\",\"");
        if (cs) {
            cs += 6;
            const char *ce = strchr(cs, '"');
            if (ce && (size_t)(ce - cs) < sizeof(s_lounge_sid)) {
                pthread_mutex_lock(&s_cast_lock);
                memcpy(s_lounge_sid, cs, (size_t)(ce - cs));
                s_lounge_sid[ce - cs] = '\0';
                pthread_mutex_unlock(&s_cast_lock);
                evo_log("cast: Lounge sid updated: %s", s_lounge_sid);
            }
        }
        /* Track latest message AID */
        pthread_mutex_lock(&s_cast_lock);
        s_lounge_aid = lounge_max_aid(chunk, s_lounge_aid);
        pthread_mutex_unlock(&s_cast_lock);

        /* The phone left the session ("Disconnect" in the YouTube app). A
         * browser cast has no other way back to EVO: CastService closes it.
         * Only when the last remote goes: an old session of the same or
         * another phone timing out must not close a cast that is running. */
        track_remotes(chunk);

        /* Reply to getNowPlaying */
        if (strstr(chunk, "\"getNowPlaying\"")) {
            evo_log("cast: Lounge received getNowPlaying, sending nowPlaying reply");
            lounge_send_command("nowPlaying", NULL);
        }

        /* Notice getDiscoveryDeviceId */
        if (strstr(chunk, "\"getDiscoveryDeviceId\"")) {
            evo_log("cast: Lounge received getDiscoveryDeviceId");
        }

        /* Check for video cast */
        char vid[64] = {0};
        char t_off[32] = {0};
        if (evo_cast_parse_lounge_video(chunk, vid, sizeof(vid), t_off, sizeof(t_off)) && vid[0]) {
            int is_set_playlist = (strstr(chunk, "\"setPlaylist\"") != NULL);
            int is_new_vid = (strcmp(vid, s_last_cast_vid) != 0);

            if (is_set_playlist || is_new_vid) {
                evo_log("cast: Lounge parsed videoId=%s, t=%s (setPlaylist=%d new=%d)",
                        vid, t_off, is_set_playlist, is_new_vid);
                pthread_mutex_lock(&s_cast_lock);
                snprintf(s_last_cast_vid, sizeof(s_last_cast_vid), "%s", vid);
                evo_cast_launch_cb cb = s_on_launch;
                void *ud = s_userdata;
                pthread_mutex_unlock(&s_cast_lock);

                char launch_payload[256];
                snprintf(launch_payload, sizeof(launch_payload), "v=%s&t=%s", vid, t_off);
                if (cb) {
                    cb(launch_payload, ud);
                }
                char np_extra[256];
                snprintf(np_extra, sizeof(np_extra), "&req0_videoId=%s&req0_currentTime=%s&req0_state=1",
                         vid, t_off[0] ? t_off : "0");
                lounge_send_command("nowPlaying", np_extra);
            } else {
                evo_log("cast: Lounge ignoring echo of current video %s", vid);
            }
        } else if (strstr(chunk, "\"stopVideo\"")) {
            evo_log("cast: Lounge parsed stopVideo");
            pthread_mutex_lock(&s_cast_lock);
            s_last_cast_vid[0] = '\0';
            evo_cast_stop_cb scb = s_on_stop;
            void *ud = s_userdata;
            pthread_mutex_unlock(&s_cast_lock);
            if (scb) {
                scb(ud);
            }
            lounge_send_command("nowPlaying", "&req0_state=-1");
        }
#else
    (void)chunk;
#endif
}

/* Sleep between Lounge retries, waking every 250 ms to honour a stop. The
 * wait doubles per consecutive failure (1 s .. 60 s) so an offline console
 * does not hammer YouTube, or evo.log with net failures, every two seconds. */
static void lounge_backoff_nap(int *fails)
{
    int n = *fails < 6 ? *fails : 6;
    int ms = 1000 << n;
    if (ms > 60000) ms = 60000;
    (*fails)++;
    for (int waited = 0; waited < ms && s_running && s_lounge_running; waited += 250)
        usleep(250 * 1000);
}

static void *lounge_thread(void *arg)
{
    (void)arg;
    int fails = 0;
    evo_log("cast: Lounge background listener started");

    while (s_running && s_lounge_running) {
        /* Step 1: Ensure screen_id */
        pthread_mutex_lock(&s_cast_lock);
        int has_sid = (s_screen_id[0] != '\0');
        pthread_mutex_unlock(&s_cast_lock);

        if (!has_sid) {
            char *body = NULL; size_t len = 0; int status = 0;
            int rc = evo_net_http_get_sync("https://www.youtube.com/api/lounge/pairing/generate_screen_id",
                                           NULL, 0, &body, &len, &status);
            if (rc == EVO_NET_OK && status == 200 && body && len > 0) {
                char *p = body;
                while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '"') p++;
                char *end = p + strlen(p);
                while (end > p && (end[-1] == ' ' || end[-1] == '\r' || end[-1] == '\n' || end[-1] == '"')) end--;
                *end = '\0';
                pthread_mutex_lock(&s_cast_lock);
                snprintf(s_screen_id, sizeof(s_screen_id), "%s", p);
                pthread_mutex_unlock(&s_cast_lock);
                evo_log("cast: Lounge screen_id acquired: %s", p);
            }
            if (body) free(body);
            if (!s_running || !s_lounge_running) break;
            pthread_mutex_lock(&s_cast_lock);
            has_sid = (s_screen_id[0] != '\0');
            pthread_mutex_unlock(&s_cast_lock);
            if (!has_sid) {
                lounge_backoff_nap(&fails);
                continue;
            }
        }

        /* Step 2: Ensure lounge_token */
        pthread_mutex_lock(&s_cast_lock);
        int has_tok = (s_lounge_token[0] != '\0');
        char cur_sid[128];
        snprintf(cur_sid, sizeof(cur_sid), "%s", s_screen_id);
        pthread_mutex_unlock(&s_cast_lock);

        if (!has_tok) {
            char post_data[256];
            snprintf(post_data, sizeof(post_data), "screen_ids=%s", cur_sid);
            const char *hdrs[] = { "Content-Type: application/x-www-form-urlencoded" };
            char *body = NULL; size_t len = 0; int status = 0;
            int rc = evo_net_http_post_sync("https://www.youtube.com/api/lounge/pairing/get_lounge_token_batch",
                                            post_data, hdrs, 1, &body, &len, &status);
            if (rc == EVO_NET_OK && status == 200 && body) {
                cJSON *root = cJSON_Parse(body);
                if (root) {
                    cJSON *screens = cJSON_GetObjectItemCaseSensitive(root, "screens");
                    if (screens && cJSON_IsArray(screens) && cJSON_GetArraySize(screens) > 0) {
                        cJSON *first = cJSON_GetArrayItem(screens, 0);
                        cJSON *lt = cJSON_GetObjectItemCaseSensitive(first, "loungeToken");
                        if (lt && lt->valuestring && lt->valuestring[0]) {
                            pthread_mutex_lock(&s_cast_lock);
                            snprintf(s_lounge_token, sizeof(s_lounge_token), "%s", lt->valuestring);
                            pthread_mutex_unlock(&s_cast_lock);
                            evo_log("cast: Lounge token acquired");
                        }
                    }
                    cJSON_Delete(root);
                }
            }
            if (body) free(body);
            if (!s_running || !s_lounge_running) break;
            pthread_mutex_lock(&s_cast_lock);
            has_tok = (s_lounge_token[0] != '\0');
            pthread_mutex_unlock(&s_cast_lock);
            if (!has_tok) {
                lounge_backoff_nap(&fails);
                continue;
            }
        }

        /* Step 3: Initial bind if needed */
        pthread_mutex_lock(&s_cast_lock);
        int has_bsid = (s_lounge_sid[0] != '\0');
        char cur_tok[512];
        snprintf(cur_tok, sizeof(cur_tok), "%s", s_lounge_token);
        pthread_mutex_unlock(&s_cast_lock);

        if (!has_bsid) {
            pthread_mutex_lock(&s_cast_lock);
            int bind_rid = s_lounge_rid++;
            pthread_mutex_unlock(&s_cast_lock);

            char url[1024];
            long now_ms = (long)time(NULL) * 1000;
            snprintf(url, sizeof(url),
                "https://www.youtube.com/api/lounge/bc/bind?device=LOUNGE_SCREEN&id=%s&name=%s&loungeIdToken=%s&VER=8&RID=%d&zx=%ld",
                s_device_id, "EVO%20Player", cur_tok, bind_rid, now_ms);

            char hdr_token[600];
            snprintf(hdr_token, sizeof(hdr_token), "X-YouTube-LoungeId-Token: %s", cur_tok);
            const char *b_hdrs[] = {
                "Content-Type: application/x-www-form-urlencoded",
                hdr_token
            };

            char *body = NULL; size_t len = 0; int status = 0;
            int rc = evo_net_http_post_sync(url, "count=0", b_hdrs, 2, &body, &len, &status);
            if (rc == EVO_NET_OK && status == 200 && body) {
                const char *cs = strstr(body, "[\"c\",\"");
                const char *ss = strstr(body, "[\"S\",\"");
                if (cs && ss) {
                    cs += 6;
                    const char *ce = strchr(cs, '"');
                    ss += 6;
                    const char *se = strchr(ss, '"');
                    if (ce && se && (size_t)(ce - cs) < sizeof(s_lounge_sid) && (size_t)(se - ss) < sizeof(s_lounge_gsid)) {
                        pthread_mutex_lock(&s_cast_lock);
                        memcpy(s_lounge_sid, cs, (size_t)(ce - cs));
                        s_lounge_sid[ce - cs] = '\0';
                        memcpy(s_lounge_gsid, ss, (size_t)(se - ss));
                        s_lounge_gsid[se - ss] = '\0';
                        s_lounge_aid = 0;
                        s_lounge_ofs = 0;
                        pthread_mutex_unlock(&s_cast_lock);
                        evo_log("cast: Lounge session bound (SID: %s)", s_lounge_sid);
                    }
                }
            }
            if (body) free(body);
            if (!s_running || !s_lounge_running) break;
            pthread_mutex_lock(&s_cast_lock);
            has_bsid = (s_lounge_sid[0] != '\0');
            pthread_mutex_unlock(&s_cast_lock);
            if (!has_bsid) {
                lounge_backoff_nap(&fails);
                continue;
            }
        }

        /* Step 4: Long-poll channel for commands using direct streaming */
        char poll_url[1024];
        long now_ms = (long)time(NULL) * 1000;
        pthread_mutex_lock(&s_cast_lock);
        char session_sid[128], session_gsid[128];
        snprintf(session_sid, sizeof(session_sid), "%s", s_lounge_sid);
        snprintf(session_gsid, sizeof(session_gsid), "%s", s_lounge_gsid);
        int cur_aid = s_lounge_aid;
        pthread_mutex_unlock(&s_cast_lock);

        snprintf(poll_url, sizeof(poll_url),
            "https://www.youtube.com/api/lounge/bc/bind?device=LOUNGE_SCREEN&id=%s&name=%s&loungeIdToken=%s&VER=8&RID=rpc&SID=%s&CI=0&AID=%d&gsessionid=%s&TYPE=xmlhttp&zx=%ld",
            s_device_id, "EVO%20Player", cur_tok, session_sid, cur_aid, session_gsid, now_ms);

        int ret = lounge_listen_channel(poll_url, cur_tok, screen_on_chunk, &s_lounge_running);
        if (!s_running || !s_lounge_running) break;
        if (ret == 200) fails = 0;

        if (ret == 400 || ret == 404) {
            pthread_mutex_lock(&s_cast_lock);
            s_lounge_sid[0] = '\0';
            s_lounge_gsid[0] = '\0';
            s_lounge_aid = 0;
            s_lounge_ofs = 0;
            s_lounge_rid = 1;
            pthread_mutex_unlock(&s_cast_lock);
            evo_log("cast: Lounge session expired (status %d), re-establishing", ret);
            lounge_backoff_nap(&fails);
        } else if (ret != 200) {
            evo_log("cast: Lounge channel exited (code %d), retrying in 1s", ret);
            lounge_backoff_nap(&fails);
        }
    }

    evo_log("cast: Lounge background listener stopped");
    return NULL;
}

/* ---- Browser cast page watcher ----
 *
 * A browser cast opens youtube.com/tv in Sony's browser dialog, which tells
 * EVO nothing about the page. EVO puts its own pairingCode in the page URL
 * (as a phone's DIAL launch does); the page registers that code for its Lounge
 * screen, and EVO looks the screen up with /pairing/get_screen. No request
 * from the browser to EVO is needed: Sony's browser never posted to
 * additionalDataUrl, even on 127.0.0.1. EVO then joins the screen as a silent
 * REMOTE_CONTROL and reads its nowPlaying messages: once the cast video has
 * played, a nowPlaying without a videoId (the video ended) or with another one
 * (autoplay moved on) means the video is over. */
static volatile int s_page_on = 0;       /* armed by evo_cast_page_watch_start */
static pthread_t    s_page_th;
static int          s_page_th_valid = 0;
static char         s_page_video[32];
static char         s_page_code[64];     /* the pairingCode in the page URL */
static char         s_page_time[32];     /* start offset in seconds ("" = 0) */
static int          s_page_seen = 0;     /* the cast video showed up in nowPlaying */
static int          s_page_aid = 0;

static void page_on_chunk(char *chunk)
{
    s_page_aid = lounge_max_aid(chunk, s_page_aid);
    for (char *p = strstr(chunk, "[\"nowPlaying\",{"); p; p = strstr(p + 1, "[\"nowPlaying\",{")) {
        char *obj = p + 14;
        char *end = strstr(obj, "}]");
        char save = 0;
        if (end) { save = end[1]; end[1] = '\0'; }
        char vid[32] = {0};
        const char *v = strstr(obj, "\"videoId\":\"");
        if (v) {
            v += 11;
            size_t n = 0;
            while (v[n] && v[n] != '"' && n < sizeof(vid) - 1) { vid[n] = v[n]; n++; }
        }
        if (end) end[1] = save;

        pthread_mutex_lock(&s_cast_lock);
        int ended = 0;
        if (vid[0] && strcmp(vid, s_page_video) == 0) {
            if (!s_page_seen) evo_log("cast: page is playing %s", vid);
            s_page_seen = 1;
        } else if (s_page_seen) {
            evo_log("cast: page video %s over (nowPlaying videoId='%s')", s_page_video, vid);
            s_page_seen = 0;
            ended = 1;
        }
        pthread_mutex_unlock(&s_cast_lock);
        if (ended) cast_post_event(EVO_CAST_EV_PAGE_ENDED);
    }
    /* state 0 = ended (onStateChange carries no videoId; seen ties it to
     * the cast video). */
    for (char *p = strstr(chunk, "[\"onStateChange\",{"); p; p = strstr(p + 1, "[\"onStateChange\",{")) {
        char *end = strstr(p, "}]");
        char *st = strstr(p, "\"state\":\"0\"");
        if (!st || (end && st > end)) continue;
        pthread_mutex_lock(&s_cast_lock);
        int ended = s_page_seen;
        s_page_seen = 0;
        pthread_mutex_unlock(&s_cast_lock);
        if (ended) {
            evo_log("cast: page video %s ended (state 0)", s_page_video);
            cast_post_event(EVO_CAST_EV_PAGE_ENDED);
        }
    }
}

#ifndef NO_OPENSSL
/* Joins `screen` as a REMOTE_CONTROL and streams its channel until the watch
 * is disarmed. Returns once the session is gone or cannot be set up. */
static void page_watch_session(const char *tok)
{
    char *body = NULL; size_t len = 0; int status = 0; int rc;
    char base[1024];
    snprintf(base, sizeof(base),
        "https://www.youtube.com/api/lounge/bc/bind?device=REMOTE_CONTROL&mdx-version=3&ui=1&v=2"
        "&name=EVO%%20Player&app=youtube-desktop&loungeIdToken=%s&id=%s-w&VER=8&CVER=1",
        tok, s_device_id);
    char hdr_token[600];
    snprintf(hdr_token, sizeof(hdr_token), "X-YouTube-LoungeId-Token: %s", tok);
    const char *bhdr[] = { "Content-Type: application/x-www-form-urlencoded", hdr_token };

    char url[1400];
    snprintf(url, sizeof(url), "%s&RID=1&zx=%ld", base, (long)time(NULL) * 1000);
    body = NULL; len = 0; status = 0;
    rc = evo_net_http_post_sync(url, "count=0", bhdr, 2, &body, &len, &status);
    char sid[128] = {0}, gsid[128] = {0};
    if (rc == EVO_NET_OK && status == 200 && body) {
        const char *c = strstr(body, "[\"c\",\"");
        const char *s = strstr(body, "[\"S\",\"");
        if (c) sscanf(c + 6, "%127[^\"]", sid);
        if (s) sscanf(s + 6, "%127[^\"]", gsid);
        s_page_aid = 0;
        page_on_chunk(body);   /* the bind reply already carries nowPlaying */
    }
    free(body);
    if (!sid[0] || !gsid[0]) {
        evo_log("cast: page watch: bind failed (rc=%d status=%d)", rc, status);
        return;
    }
    evo_log("cast: page watch: joined the page's Lounge screen (SID %s)", sid);

    /* A page opened with a pairingCode waits for a remote to say what to
     * play (it ignores v= in the URL on the PS5): send the cast video, as
     * the phone would after a DIAL launch. */
    {
        char req[512];
        snprintf(req, sizeof(req),
                 "count=1&ofs=0&req0__sc=setPlaylist&req0_videoId=%s&req0_currentTime=%s"
                 "&req0_currentIndex=0&req0_videoIds=%s&req0_listId=",
                 s_page_video, s_page_time[0] ? s_page_time : "0", s_page_video);
        snprintf(url, sizeof(url), "%s&RID=2&SID=%s&gsessionid=%s&zx=%ld",
                 base, sid, gsid, (long)time(NULL) * 1000);
        body = NULL; len = 0; status = 0;
        rc = evo_net_http_post_sync(url, req, bhdr, 2, &body, &len, &status);
        evo_log("cast: page watch: setPlaylist %s t=%s -> rc=%d status=%d",
                s_page_video, s_page_time[0] ? s_page_time : "0", rc, status);
        free(body);
    }

    int fails = 0;
    while (s_running && s_page_on && fails < 3) {
        snprintf(url, sizeof(url), "%s&RID=rpc&SID=%s&gsessionid=%s&CI=0&AID=%d&TYPE=xmlhttp&zx=%ld",
                 base, sid, gsid, s_page_aid, (long)time(NULL) * 1000);
        int ret = lounge_listen_channel(url, tok, page_on_chunk, &s_page_on);
        if (ret == 200) { fails = 0; continue; }
        if (ret == 400 || ret == 404) break;   /* session gone: the page closed */
        fails++;
    }
}
#endif

static void *page_watch_thread(void *arg)
{
    (void)arg;
#ifndef NO_OPENSSL
    evo_log("cast: page watch: waiting for the page to register pairing code %s", s_page_code);
    char post[128];
    snprintf(post, sizeof(post), "pairing_code=%s", s_page_code);
    const char *hdr[] = { "Content-Type: application/x-www-form-urlencoded" };
    char tok[512] = {0};
    char screen[128] = {0};
    /* get_screen answers 404 until the page has loaded and registered the
     * code (about 4 s on the PC); Sony's browser loads slower. */
    for (int tries = 0; s_running && s_page_on && tries < 60 && !tok[0]; tries++) {
        for (int ms = 0; ms < 1500 && s_running && s_page_on; ms += 250) usleep(250 * 1000);
        char *body = NULL; size_t len = 0; int status = 0;
        int rc = evo_net_http_post_sync("https://www.youtube.com/api/lounge/pairing/get_screen",
                                        post, hdr, 1, &body, &len, &status);
        if (rc == EVO_NET_OK && status == 200 && body) {
            cJSON *root = cJSON_Parse(body);
            cJSON *sc = root ? cJSON_GetObjectItemCaseSensitive(root, "screen") : NULL;
            cJSON *lt = sc ? cJSON_GetObjectItemCaseSensitive(sc, "loungeToken") : NULL;
            cJSON *si = sc ? cJSON_GetObjectItemCaseSensitive(sc, "screenId") : NULL;
            if (lt && lt->valuestring) snprintf(tok, sizeof(tok), "%s", lt->valuestring);
            if (si && si->valuestring) snprintf(screen, sizeof(screen), "%s", si->valuestring);
            cJSON_Delete(root);
        }
        free(body);
    }
    if (!tok[0]) {
        if (s_page_on) evo_log("cast: page watch: the page never registered its code; no end detection");
    } else {
        evo_log("cast: page watch: page screen %s", screen);
        page_watch_session(tok);
    }
#endif
    evo_log("cast: page watch: stopped");
    return NULL;
}

void evo_cast_new_pairing_code(char *out, size_t cap)
{
    generate_uuid_v4(out, cap);
}

static void page_watch_join(void)
{
    if (s_page_th_valid) {
        pthread_join(s_page_th, NULL);
        s_page_th_valid = 0;
    }
}

void evo_cast_page_watch_start(const char *video_id, const char *pairing_code, const char *start_time)
{
    s_page_on = 0;
    page_watch_join();
    pthread_mutex_lock(&s_cast_lock);
    /* Digits and one dot only: it goes into a form body. */
    s_page_time[0] = '\0';
    if (start_time && strspn(start_time, "0123456789.") == strlen(start_time) && strlen(start_time) < sizeof(s_page_time))
        snprintf(s_page_time, sizeof(s_page_time), "%s", start_time);
    snprintf(s_page_video, sizeof(s_page_video), "%s", video_id ? video_id : "");
    snprintf(s_page_code, sizeof(s_page_code), "%s", pairing_code ? pairing_code : "");
    s_page_seen = 0;
    s_cast_events &= ~EVO_CAST_EV_PAGE_ENDED;
    pthread_mutex_unlock(&s_cast_lock);
    if (!s_running || !s_page_video[0] || !s_page_code[0]) return;

    s_page_on = 1;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 512 * 1024);
    if (pthread_create(&s_page_th, &at, page_watch_thread, NULL) == 0) {
        s_page_th_valid = 1;
    } else {
        s_page_on = 0;
        evo_log("cast: page watch: thread failed to start");
    }
    pthread_attr_destroy(&at);
}

void evo_cast_page_watch_stop(void)
{
    /* Not joined here: the thread can be inside a TLS request for a few
     * seconds, and this runs on the main thread. It sees the flag within
     * half a second of the request ending; the next start or the receiver
     * stop joins it. */
    s_page_on = 0;
}

int evo_cast_page_watch_active(void)
{
    return s_page_on;
}

/* ---- Lifecycle Management ---- */

int evo_cast_receiver_start(const char *friendly_name, int preferred_port)
{
    pthread_mutex_lock(&s_cast_lock);
    if (s_running) {
        pthread_mutex_unlock(&s_cast_lock);
        return 0;
    }

    if (friendly_name && friendly_name[0]) {
        snprintf(s_friendly_name, sizeof(s_friendly_name), "%s", friendly_name);
    }
    if (preferred_port > 0 && preferred_port <= 65535) {
        s_dial_port = preferred_port;
    }
    s_running = 1;
    pthread_mutex_unlock(&s_cast_lock);

    load_or_create_device_id();

    pthread_attr_t at;
    pthread_attr_init(&at);
    /* 512 KB: handle_dial_client keeps a 64 KB request buffer on the stack
     * and can then run an HTTPS request (pairing code) through OpenSSL; the
     * Lounge thread runs TLS and cJSON. 128 KB left too little headroom. */
    pthread_attr_setstacksize(&at, 512 * 1024);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_JOINABLE);

    if (pthread_create(&s_dial_th, &at, dial_thread, NULL) != 0) {
        evo_log("cast: failed to spawn DIAL thread");
        pthread_attr_destroy(&at);
        s_running = 0;
        return -1;
    }

    if (pthread_create(&s_ssdp_th, &at, ssdp_thread, NULL) != 0) {
        evo_log("cast: failed to spawn SSDP thread");
        s_running = 0;
        pthread_join(s_dial_th, NULL);
        pthread_attr_destroy(&at);
        return -1;
    }

    s_lounge_running = 1;
    if (pthread_create(&s_lounge_th, &at, lounge_thread, NULL) != 0) {
        evo_log("cast: failed to spawn Lounge thread");
    }

    pthread_attr_destroy(&at);
    evo_log("cast: receiver started (name: \"%s\", port: %d)", s_friendly_name, s_dial_port);
    return 0;
}

void evo_cast_receiver_stop(void)
{
    pthread_mutex_lock(&s_cast_lock);
    if (!s_running) {
        pthread_mutex_unlock(&s_cast_lock);
        return;
    }
    s_running = 0;
    s_lounge_running = 0;
    pthread_mutex_unlock(&s_cast_lock);

    /* The worker threads own their sockets: each polls with a short timeout,
     * sees s_running drop and closes its own fd. Closing them here as well
     * closed the same descriptor number twice, and the second close could hit
     * a file or socket another subsystem had opened in between. */
    pthread_join(s_ssdp_th, NULL);
    pthread_join(s_dial_th, NULL);
    if (s_lounge_th) {
        pthread_join(s_lounge_th, NULL);
        s_lounge_th = 0;
    }
    s_page_on = 0;
    page_watch_join();

    pthread_mutex_lock(&s_cast_lock);
    s_last_cast_vid[0] = '\0';
    if (s_dash_mpd) {
        free(s_dash_mpd);
        s_dash_mpd = NULL;
        s_dash_mpd_len = 0;
    }
    for (int i = 0; i < 2; i++) {
        free(s_cast_seg_url[i]);
        s_cast_seg_url[i] = NULL;
    }
    pthread_mutex_unlock(&s_cast_lock);

    evo_log("cast: receiver stopped");
}

int evo_cast_receiver_set_stream_mpd(const char *mpd_xml, const char *title)
{
    pthread_mutex_lock(&s_cast_lock);
    if (s_dash_mpd) {
        free(s_dash_mpd);
        s_dash_mpd = NULL;
        s_dash_mpd_len = 0;
    }
    if (mpd_xml) {
        s_dash_mpd = strdup(mpd_xml);
        s_dash_mpd_len = s_dash_mpd ? strlen(s_dash_mpd) : 0;
    }
    if (title && title[0]) {
        snprintf(s_cast_media_title, sizeof(s_cast_media_title), "%s", title);
    }
    s_direct_media_url[0] = '\0';
    pthread_mutex_unlock(&s_cast_lock);
    return 0;
}

int evo_cast_receiver_set_direct_stream(const char *url, const char *title)
{
    pthread_mutex_lock(&s_cast_lock);
    if (url) {
        snprintf(s_direct_media_url, sizeof(s_direct_media_url), "%s", url);
    } else {
        s_direct_media_url[0] = '\0';
    }
    if (title && title[0]) {
        snprintf(s_cast_media_title, sizeof(s_cast_media_title), "%s", title);
    }
    if (s_dash_mpd) {
        free(s_dash_mpd);
        s_dash_mpd = NULL;
        s_dash_mpd_len = 0;
    }
    pthread_mutex_unlock(&s_cast_lock);
    return 0;
}

const char* evo_cast_receiver_stream_title(void)
{
    return s_cast_media_title;
}

static void extract_codec_from_mime(const char *mime, char *out, size_t cap, const char *fallback)
{
    if (!out || cap == 0) return;
    const char *p = mime ? strstr(mime, "codecs=\"") : NULL;
    if (p) {
        p += 8;
        const char *end = strchr(p, '"');
        if (end && (size_t)(end - p) < cap) {
            size_t len = (size_t)(end - p);
            memcpy(out, p, len);
            out[len] = '\0';
            return;
        }
    }
    snprintf(out, cap, "%s", fallback ? fallback : "");
}

struct yt_segment_range {
    uint64_t start;
    uint64_t end;
};

struct mpd_builder {
    char *buf;
    size_t len;
    size_t cap;
};

static void mpd_append(struct mpd_builder *b, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);
    if (needed <= 0) {
        va_end(args);
        return;
    }
    if (b->len + (size_t)needed + 1 > b->cap) {
        size_t new_cap = b->cap ? b->cap * 2 : 16384;
        while (b->len + (size_t)needed + 1 > new_cap) {
            new_cap *= 2;
        }
        char *nb = (char*)realloc(b->buf, new_cap);
        if (!nb) {
            va_end(args);
            return;
        }
        b->buf = nb;
        b->cap = new_cap;
    }
    vsnprintf(b->buf + b->len, b->cap - b->len, fmt, args);
    b->len += (size_t)needed;
    va_end(args);
}

static int parse_range_obj(cJSON *obj, uint64_t *out_start, uint64_t *out_end)
{
    if (!obj || !out_start || !out_end) return -1;
    cJSON *s = cJSON_GetObjectItemCaseSensitive(obj, "start");
    cJSON *e = cJSON_GetObjectItemCaseSensitive(obj, "end");
    if (!s || !e) return -1;
    *out_start = s->valuestring ? (uint64_t)strtoull(s->valuestring, NULL, 10) : (uint64_t)s->valuedouble;
    *out_end = e->valuestring ? (uint64_t)strtoull(e->valuestring, NULL, 10) : (uint64_t)e->valuedouble;
    return (*out_end >= *out_start) ? 0 : -1;
}

static int fetch_and_parse_sidx(const char *url, uint64_t idx_start, uint64_t idx_end,
                                struct yt_segment_range **out_ranges, int *out_count)
{
    if (!url || idx_end <= idx_start || !out_ranges || !out_count) return -1;
    *out_ranges = NULL;
    *out_count = 0;

    char range_hdr[128];
    snprintf(range_hdr, sizeof(range_hdr), "Range: bytes=%" PRIu64 "-%" PRIu64, idx_start, idx_end);
    const char *hdrs[] = {
        range_hdr,
        "User-Agent: com.google.android.youtube/20.02.35 (Linux; U; Android 11) gzip"
    };

    char *body = NULL;
    size_t body_len = 0;
    int status = 0;
    int rc = evo_net_http_get_sync(url, hdrs, 2, &body, &body_len, &status);
    if (rc != EVO_NET_OK || !body || body_len < 24) {
        if (body) free(body);
        return -1;
    }

    const uint8_t *p = (const uint8_t*)body;
    if (memcmp(p + 4, "sidx", 4) != 0) {
        free(body);
        return -1;
    }

    uint8_t version = p[8];
    size_t pos = 20;
    uint64_t first_offset = 0;

    if (version == 0) {
        if (pos + 8 > body_len) { free(body); return -1; }
        first_offset = ((uint64_t)p[pos+4] << 24) | ((uint64_t)p[pos+5] << 16) |
                       ((uint64_t)p[pos+6] << 8)  | (uint64_t)p[pos+7];
        pos += 8;
    } else {
        if (pos + 16 > body_len) { free(body); return -1; }
        first_offset = ((uint64_t)p[pos+8]  << 56) | ((uint64_t)p[pos+9]  << 48) |
                       ((uint64_t)p[pos+10] << 40) | ((uint64_t)p[pos+11] << 32) |
                       ((uint64_t)p[pos+12] << 24) | ((uint64_t)p[pos+13] << 16) |
                       ((uint64_t)p[pos+14] << 8)  | (uint64_t)p[pos+15];
        pos += 16;
    }

    if (pos + 4 > body_len) { free(body); return -1; }
    uint16_t ref_count = ((uint16_t)p[pos+2] << 8) | (uint16_t)p[pos+3];
    pos += 4;

    if (ref_count == 0 || pos + (size_t)ref_count * 12 > body_len) {
        free(body);
        return -1;
    }

    struct yt_segment_range *ranges = (struct yt_segment_range*)calloc(ref_count, sizeof(struct yt_segment_range));
    if (!ranges) {
        free(body);
        return -1;
    }

    uint64_t cur_offset = idx_end + 1 + first_offset;
    for (int i = 0; i < (int)ref_count; i++) {
        uint32_t ref_size = ((uint32_t)(p[pos] & 0x7F) << 24) |
                            ((uint32_t)p[pos+1] << 16) |
                            ((uint32_t)p[pos+2] << 8)  |
                            (uint32_t)p[pos+3];
        pos += 12;
        ranges[i].start = cur_offset;
        ranges[i].end = cur_offset + (uint64_t)ref_size - 1;
        cur_offset += ref_size;
    }

    free(body);
    *out_ranges = ranges;
    *out_count = (int)ref_count;
    return 0;
}

static int parse_innertube_streams(cJSON *root, const char *video_id,
                                   char *out_url, size_t url_cap,
                                   char *out_title, size_t title_cap)
{
    if (!root || !video_id || !out_url || url_cap == 0) return -1;

    /* Check playabilityStatus */
    cJSON *ps = cJSON_GetObjectItemCaseSensitive(root, "playabilityStatus");
    if (ps) {
        cJSON *st = cJSON_GetObjectItemCaseSensitive(ps, "status");
        if (st && st->valuestring && strcasecmp(st->valuestring, "OK") != 0) {
            cJSON *reason = cJSON_GetObjectItemCaseSensitive(ps, "reason");
            evo_log("cast: YouTube video %s not playable: %s (%s)",
                    video_id,
                    st->valuestring,
                    reason && reason->valuestring ? reason->valuestring : "");
            return -1;
        }
    }

    /* Extract title & duration */
    char title_buf[256] = "YouTube Video";
    double duration_sec = 300.0;
    cJSON *vd = cJSON_GetObjectItemCaseSensitive(root, "videoDetails");
    if (vd) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(vd, "title");
        if (t && t->valuestring && t->valuestring[0]) {
            snprintf(title_buf, sizeof(title_buf), "%s", t->valuestring);
        }
        cJSON *len_sec = cJSON_GetObjectItemCaseSensitive(vd, "lengthSeconds");
        if (len_sec) {
            if (len_sec->valuestring) duration_sec = atof(len_sec->valuestring);
            else if (len_sec->valuedouble > 0) duration_sec = len_sec->valuedouble;
        }
    }
    if (duration_sec <= 0.0) duration_sec = 300.0;

    if (out_title && title_cap > 0 && out_title[0] == '\0') {
        snprintf(out_title, title_cap, "%s", title_buf);
    }

    cJSON *sd = cJSON_GetObjectItemCaseSensitive(root, "streamingData");
    if (!sd) {
        evo_log("cast: No streamingData in YouTube response for %s", video_id);
        return -1;
    }

    /* Check for live HLS stream */
    cJSON *hls = cJSON_GetObjectItemCaseSensitive(sd, "hlsManifestUrl");
    if (hls && hls->valuestring && hls->valuestring[0]) {
        snprintf(out_url, url_cap, "%s", hls->valuestring);
        evo_cast_receiver_set_direct_stream(hls->valuestring, title_buf);
        evo_log("cast: Resolved YouTube %s -> HLS live stream: %s", video_id, out_url);
        return 0;
    }

    /* Search adaptiveFormats for best video and audio with sidx indexRange */
    cJSON *adaptive = cJSON_GetObjectItemCaseSensitive(sd, "adaptiveFormats");
    cJSON *best_video = NULL;
    cJSON *best_audio = NULL;
    int best_height = 0;
    int best_video_br = 0;
    int best_video_avc = 0;
    int best_audio_br = 0;

    if (adaptive && cJSON_IsArray(adaptive)) {
        int count = cJSON_GetArraySize(adaptive);
        for (int i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(adaptive, i);
            cJSON *u = cJSON_GetObjectItemCaseSensitive(item, "url");
            if (!u || !u->valuestring || !u->valuestring[0]) continue;
            cJSON *m = cJSON_GetObjectItemCaseSensitive(item, "mimeType");
            if (!m || !m->valuestring) continue;
            cJSON *init_r = cJSON_GetObjectItemCaseSensitive(item, "initRange");
            cJSON *idx_r = cJSON_GetObjectItemCaseSensitive(item, "indexRange");
            if (!init_r || !idx_r) continue;

            const char *mime = m->valuestring;
            if (strncmp(mime, "video/mp4", 9) == 0) {
                cJSON *h = cJSON_GetObjectItemCaseSensitive(item, "height");
                int height = h ? h->valueint : 0;
                cJSON *br = cJSON_GetObjectItemCaseSensitive(item, "bitrate");
                int bitrate = br ? br->valueint : 0;
                /* video/mp4 carries both H.264 (avc1, up to 1080p) and AV1
                 * (av01, up to 8K). H.264 runs on the hardware decoder; AV1
                 * decodes in software (dav1d), which handles 4K since #94/#95.
                 * So: the tallest picture up to 2160p; at the same height
                 * H.264 wins (hardware), then the higher bitrate. */
                int is_avc = strstr(mime, "avc1") != NULL;
                int is_av1 = strstr(mime, "av01") != NULL;
                if (!is_avc && !is_av1) continue;
                if (height > 2160) continue;
                if (!best_video ||
                    height > best_height ||
                    (height == best_height &&
                     (is_avc > best_video_avc ||
                      (is_avc == best_video_avc && bitrate > best_video_br)))) {
                    best_height = height;
                    best_video_br = bitrate;
                    best_video_avc = is_avc;
                    best_video = item;
                }
            } else if (strncmp(mime, "audio/mp4", 9) == 0) {
                cJSON *br = cJSON_GetObjectItemCaseSensitive(item, "bitrate");
                int bitrate = br ? br->valueint : 0;
                if (bitrate > best_audio_br) {
                    best_audio_br = bitrate;
                    best_audio = item;
                }
            }
        }
    }

    /* Build DASH MPD if both video and audio streams were found with segment ranges */
    if (best_video && best_audio) {
        uint64_t v_init_s = 0, v_init_e = 0, v_idx_s = 0, v_idx_e = 0;
        uint64_t a_init_s = 0, a_init_e = 0, a_idx_s = 0, a_idx_e = 0;
        parse_range_obj(cJSON_GetObjectItemCaseSensitive(best_video, "initRange"), &v_init_s, &v_init_e);
        parse_range_obj(cJSON_GetObjectItemCaseSensitive(best_video, "indexRange"), &v_idx_s, &v_idx_e);
        parse_range_obj(cJSON_GetObjectItemCaseSensitive(best_audio, "initRange"), &a_init_s, &a_init_e);
        parse_range_obj(cJSON_GetObjectItemCaseSensitive(best_audio, "indexRange"), &a_idx_s, &a_idx_e);

        const char *v_url = cJSON_GetObjectItemCaseSensitive(best_video, "url")->valuestring;
        const char *v_mime = cJSON_GetObjectItemCaseSensitive(best_video, "mimeType")->valuestring;
        int v_w = cJSON_GetObjectItemCaseSensitive(best_video, "width") ? cJSON_GetObjectItemCaseSensitive(best_video, "width")->valueint : 1920;
        int v_h = cJSON_GetObjectItemCaseSensitive(best_video, "height") ? cJSON_GetObjectItemCaseSensitive(best_video, "height")->valueint : 1080;
        int v_br = cJSON_GetObjectItemCaseSensitive(best_video, "bitrate") ? cJSON_GetObjectItemCaseSensitive(best_video, "bitrate")->valueint : 2500000;

        const char *a_url = cJSON_GetObjectItemCaseSensitive(best_audio, "url")->valuestring;
        const char *a_mime = cJSON_GetObjectItemCaseSensitive(best_audio, "mimeType")->valuestring;
        int a_br = cJSON_GetObjectItemCaseSensitive(best_audio, "bitrate") ? cJSON_GetObjectItemCaseSensitive(best_audio, "bitrate")->valueint : 128000;
        cJSON *asr = cJSON_GetObjectItemCaseSensitive(best_audio, "audioSampleRate");
        int a_rate = (asr && asr->valuestring) ? atoi(asr->valuestring) : 44100;
        if (a_rate <= 0) a_rate = 44100;

        struct yt_segment_range *v_segs = NULL;
        int v_seg_count = 0;
        struct yt_segment_range *a_segs = NULL;
        int a_seg_count = 0;

        int v_rc = fetch_and_parse_sidx(v_url, v_idx_s, v_idx_e, &v_segs, &v_seg_count);
        int a_rc = fetch_and_parse_sidx(a_url, a_idx_s, a_idx_e, &a_segs, &a_seg_count);

        if (v_rc == 0 && a_rc == 0 && v_seg_count > 0 && a_seg_count > 0) {
            char v_codec[64] = "avc1.640028";
            extract_codec_from_mime(v_mime, v_codec, sizeof(v_codec), "avc1.640028");
            char a_codec[64] = "mp4a.40.2";
            extract_codec_from_mime(a_mime, a_codec, sizeof(a_codec), "mp4a.40.2");

            /* The BaseURLs are short local URLs that 302 to googlevideo, not
             * the googlevideo URLs themselves. FFmpeg's dash demuxer writes a
             * BaseURL back into the XML tree (resolve_content_path ->
             * xmlNodeSetContent), and the bundled libxml2 re-parses each '&'
             * of the query as an entity ("unterminated entity reference"). The
             * URL lost its signature and every cast got 403 on its first
             * segment, so no DASH cast ever played - only itag 18 (360p),
             * which never goes through an MPD. Reproduced with
             * tools/host-ffmpeg-probe.sh's ffprobe; the 302 opens there. */
            int port_l = evo_cast_receiver_port();
            char *v_esc = (char*)malloc(128);
            char *a_esc = (char*)malloc(128);
            char *v_real = strdup(v_url);
            char *a_real = strdup(a_url);
            if (v_esc && a_esc && v_real && a_real) {
                snprintf(v_esc, 128, "http://127.0.0.1:%d/cast/v", port_l);
                snprintf(a_esc, 128, "http://127.0.0.1:%d/cast/a", port_l);
                pthread_mutex_lock(&s_cast_lock);
                free(s_cast_seg_url[0]);
                free(s_cast_seg_url[1]);
                s_cast_seg_url[0] = v_real;
                s_cast_seg_url[1] = a_real;
                v_real = a_real = NULL;
                pthread_mutex_unlock(&s_cast_lock);

                struct mpd_builder b = {0};
                mpd_append(&b,
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" mediaPresentationDuration=\"PT%.3fS\" minBufferTime=\"PT1S\" profiles=\"urn:mpeg:dash:profile:isoff-main:2011\">\n"
                    "  <Period duration=\"PT%.3fS\">\n"
                    "    <AdaptationSet contentType=\"video\" mimeType=\"video/mp4\" codecs=\"%s\">\n"
                    "      <Representation id=\"v\" bandwidth=\"%d\" width=\"%d\" height=\"%d\">\n"
                    "        <BaseURL>%s</BaseURL>\n"
                    "        <SegmentList>\n"
                    "          <Initialization range=\"%" PRIu64 "-%" PRIu64 "\"/>\n",
                    duration_sec, duration_sec,
                    v_codec, v_br, v_w, v_h,
                    v_esc, v_init_s, v_init_e);

                for (int i = 0; i < v_seg_count; i++) {
                    mpd_append(&b, "          <SegmentURL mediaRange=\"%" PRIu64 "-%" PRIu64 "\"/>\n",
                               v_segs[i].start, v_segs[i].end);
                }

                mpd_append(&b,
                    "        </SegmentList>\n"
                    "      </Representation>\n"
                    "    </AdaptationSet>\n"
                    "    <AdaptationSet contentType=\"audio\" mimeType=\"audio/mp4\" codecs=\"%s\">\n"
                    "      <Representation id=\"a\" bandwidth=\"%d\" audioSamplingRate=\"%d\">\n"
                    "        <BaseURL>%s</BaseURL>\n"
                    "        <SegmentList>\n"
                    "          <Initialization range=\"%" PRIu64 "-%" PRIu64 "\"/>\n",
                    a_codec, a_br, a_rate,
                    a_esc, a_init_s, a_init_e);

                for (int i = 0; i < a_seg_count; i++) {
                    mpd_append(&b, "          <SegmentURL mediaRange=\"%" PRIu64 "-%" PRIu64 "\"/>\n",
                               a_segs[i].start, a_segs[i].end);
                }

                mpd_append(&b,
                    "        </SegmentList>\n"
                    "      </Representation>\n"
                    "    </AdaptationSet>\n"
                    "  </Period>\n"
                    "</MPD>\n");

                evo_cast_receiver_set_stream_mpd(b.buf, title_buf);

                /* Also save to local file for direct file opening fallback */
                const char *cfg_mpd = evo_data_path("cast_yt.mpd");
                FILE *mf = fopen(cfg_mpd, "w");
                if (mf) {
                    if (b.buf) fputs(b.buf, mf);
                    fclose(mf);
                }

                int port = evo_cast_receiver_port();
                snprintf(out_url, url_cap, "http://127.0.0.1:%d/apps/YouTube/stream.mpd", port);
                evo_log("cast: Resolved YouTube %s -> DASH %dp (%s) / %s (%d video / %d audio segs): %s",
                        video_id, v_h, v_codec, a_codec, v_seg_count, a_seg_count, out_url);

                if (b.buf) free(b.buf);
                free(v_esc);
                free(a_esc);
                free(v_segs);
                free(a_segs);
                return 0;
            }
            free(v_esc);
            free(a_esc);
            free(v_real);
            free(a_real);
        } else {
            evo_log("cast: sidx fetch failed (v_rc=%d a_rc=%d), falling back to progressive format", v_rc, a_rc);
        }

        if (v_segs) free(v_segs);
        if (a_segs) free(a_segs);
    }

    /* Fallback: Check combined progressive formats (prefer highest resolution) */
    cJSON *formats = cJSON_GetObjectItemCaseSensitive(sd, "formats");
    cJSON *best_prog = NULL;
    int best_prog_height = 0;
    if (formats && cJSON_IsArray(formats)) {
        int count = cJSON_GetArraySize(formats);
        for (int i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(formats, i);
            cJSON *u = cJSON_GetObjectItemCaseSensitive(item, "url");
            if (!u || !u->valuestring || !u->valuestring[0]) continue;
            cJSON *h = cJSON_GetObjectItemCaseSensitive(item, "height");
            int height = h ? h->valueint : 0;
            if (height > best_prog_height || !best_prog) {
                best_prog_height = height;
                best_prog = item;
            }
        }
    }

    if (best_prog) {
        cJSON *u = cJSON_GetObjectItemCaseSensitive(best_prog, "url");
        if (u && u->valuestring && u->valuestring[0]) {
            snprintf(out_url, url_cap, "%s", u->valuestring);
            evo_cast_receiver_set_direct_stream(u->valuestring, title_buf);
            evo_log("cast: Resolved YouTube %s -> progressive format (%dp): %s",
                    video_id, best_prog_height, out_url);
            return 0;
        }
    }

    return -1;
}

int evo_cast_resolve_youtube(const char *video_id, char *out_url, size_t url_cap, char *out_title, size_t title_cap)
{
    if (!video_id || !video_id[0] || !out_url || url_cap == 0) {
        return -1;
    }
    /* The id can come from any device on the LAN (DIAL POST body) and goes
     * straight into a JSON request: accept only a real 11-char YouTube id. */
    if (strlen(video_id) != 11) return -1;
    for (const char *c = video_id; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '-' || *c == '_'))
            return -1;
    }

    if (out_title && title_cap > 0) {
        out_title[0] = '\0';
    }

    /* Attempt 1: ANDROID mobile client (clientVersion 20.02.35 - bypasses bot verification, provides direct 4K/1080p adaptive stream URLs) */
    char payload_android[512];
    snprintf(payload_android, sizeof(payload_android),
        "{\"videoId\":\"%s\",\"context\":{\"client\":{\"clientName\":\"ANDROID\",\"clientVersion\":\"20.02.35\",\"androidSdkVersion\":30,\"hl\":\"en\",\"gl\":\"US\"}},\"contentCheckOk\":true,\"racyCheckOk\":true}",
        video_id);

    const char *headers_android[] = {
        "Content-Type: application/json",
        "User-Agent: com.google.android.youtube/20.02.35 (Linux; U; Android 11) gzip",
        "X-YouTube-Client-Name: 3",
        "X-YouTube-Client-Version: 20.02.35"
    };

    char *res_body = NULL;
    size_t res_len = 0;
    int status = 0;

    int rc = evo_net_http_post_sync("https://www.youtube.com/youtubei/v1/player",
                                    payload_android, headers_android, 4, &res_body, &res_len, &status);
    if (rc == EVO_NET_OK && status == 200 && res_body) {
        cJSON *root = cJSON_Parse(res_body);
        free(res_body);
        res_body = NULL;
        if (root) {
            if (parse_innertube_streams(root, video_id, out_url, url_cap, out_title, title_cap) == 0) {
                cJSON_Delete(root);
                return 0;
            }
            cJSON_Delete(root);
        }
    } else {
        if (res_body) { free(res_body); res_body = NULL; }
        evo_log("cast: ANDROID mobile client POST failed (rc=%d, status=%d)", rc, status);
    }

    evo_log("cast: ANDROID mobile client failed for %s, trying ANDROID_VR fallback...", video_id);

    /* Attempt 2: ANDROID_VR fallback (provides DASH 4K/1080p MPD when allowed) */
    char payload_vr[512];
    snprintf(payload_vr, sizeof(payload_vr),
        "{\"videoId\":\"%s\",\"context\":{\"client\":{\"clientName\":\"ANDROID_VR\",\"clientVersion\":\"1.61.48\",\"deviceModel\":\"Quest 3\"}}}",
        video_id);

    const char *headers_vr[] = {
        "Content-Type: application/json",
        "User-Agent: Mozilla/5.0"
    };

    rc = evo_net_http_post_sync("https://www.youtube.com/youtubei/v1/player",
                                payload_vr, headers_vr, 2, &res_body, &res_len, &status);
    if (rc == EVO_NET_OK && status == 200 && res_body) {
        cJSON *root = cJSON_Parse(res_body);
        free(res_body);
        res_body = NULL;
        if (root) {
            if (parse_innertube_streams(root, video_id, out_url, url_cap, out_title, title_cap) == 0) {
                cJSON_Delete(root);
                return 0;
            }
            cJSON_Delete(root);
        }
    } else {
        if (res_body) { free(res_body); res_body = NULL; }
    }

    evo_log("cast: No playable video streams could be resolved for YouTube %s", video_id);
    return -1;
}
