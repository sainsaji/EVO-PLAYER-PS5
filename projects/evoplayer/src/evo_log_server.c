/*
 * evo_log_server.c - live log over HTTP, port EVO_LOG_SERVER_PORT (9780).
 *
 * Read-only view of what evo_boot_log() writes to /mnt/usb0/evo.log, without a
 * USB stick pull or FTP. Every line is already credential-redacted before it
 * reaches the ring, so nothing here needs to filter.
 *
 *   GET /              a small page that tails the log in the browser
 *   GET /stream        Server-Sent Events, one event per line
 *   GET /raw           plain text, kept open (curl -N http://<ps5>:9780/raw)
 *   GET /log           snapshot of the ring (the last ~128 KiB), then closes
 *   GET /stats         one JSON snapshot of memory, read-ahead, network and
 *                      playback (evo_stats.c) - tools/evo-dash.py graphs it
 *
 * Query options (stream, raw, log):
 *   tail=0        only lines logged from now on (default: the whole ring first)
 *   grep=<text>   only lines containing <text> (case-sensitive, no wildcards)
 *   level=warn    only WARN and ERROR lines (evo_log_warn / evo_log_error);
 *   level=error   only ERROR lines
 *
 * A slow or stalled client never holds up the logger: it reads from the ring at
 * its own pace and, if the ring laps it, gets a "[log] N bytes missed" line.
 * At most LS_MAX_CLIENTS connections at once. Compiled into app-module builds
 * only.
 */
#if defined(EVO_APP_MODULE)

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>

#include "evo_boot_log.h"

#define LS_MAX_CLIENTS 4
#define LS_CHUNK       8192

size_t evo_stats_json(char *out, size_t cap);

static int s_started;
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static int s_clients;

static const char k_page[] =
"<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>EVO log</title><style>"
":root{color-scheme:dark light;--bg:#111;--fg:#ddd;--dim:#888;--bar:#1c1c1c;--line:#2a2a2a;--e:#ff6b6b;--w:#ffc857;--chip:#2c3a4d}"
"@media(prefers-color-scheme:light){:root{--bg:#fff;--fg:#222;--dim:#777;--bar:#f1f1f1;--line:#e4e4e4;--e:#c62828;--w:#a66a00;--chip:#dbe6f3}}"
"body{margin:0;font:13px/1.4 ui-monospace,Consolas,monospace;background:var(--bg);color:var(--fg)}"
"#bar{position:sticky;top:0;z-index:1;display:flex;flex-wrap:wrap;gap:8px;align-items:center;padding:6px 8px;background:var(--bar);border-bottom:1px solid var(--line)}"
"#bar input[type=text]{flex:1;min-width:120px;background:var(--bg);color:var(--fg);border:1px solid var(--line);padding:4px 6px;font:inherit}"
"#bar button,#bar select{font:inherit;padding:4px 10px}#st{color:var(--dim)}"
".r{display:grid;grid-template-columns:96px 52px minmax(0,auto) 1fr;gap:0 8px;padding:1px 8px;border-bottom:1px solid var(--line);align-items:baseline}"
".r>*{min-width:0}.t{color:var(--dim)}.m{white-space:pre-wrap;word-break:break-word}"
".l{font-weight:700;font-size:11px}.INFO .l{color:var(--dim);font-weight:400}.WARN .l,.WARN .m{color:var(--w)}.ERROR .l,.ERROR .m{color:var(--e)}"
".g{background:var(--chip);border-radius:3px;padding:0 5px;font-size:11px;white-space:nowrap}.g:empty{display:none}.g{cursor:pointer}"
".x{grid-column:1/-1;color:var(--dim);white-space:pre-wrap}"
"@media(max-width:640px){.r{grid-template-columns:72px 44px 1fr}.m{grid-column:1/-1}}"
"</style>"
"<div id=bar><input type=text id=f placeholder='filter: text, or tag:pio'>"
"<select id=v><option value=0>all levels<option value=1>warn + error<option value=2>error only</select>"
"<label><input type=checkbox id=u checked> wall time</label>"
"<button id=p>Pause</button><button id=c>Clear</button><span id=st>connecting</span></div>"
"<div id=log></div><script>"
"var L=document.getElementById('log'),st=document.getElementById('st'),F=document.getElementById('f'),"
"V=document.getElementById('v'),U=document.getElementById('u'),paused=false,es=null,MAX=4000,off=null,"
"RX=/^\\[(\\d+(?:\\.\\d+)?)\\] (INFO|WARN|ERROR) ?(.*)$/,TG=/^([A-Za-z][\\w.-]{1,24}): ?/,WC=/wall clock (\\S+Z)/;"
"function p2(n,w){n=String(n);while(n.length<w)n='0'+n;return n}"
"function fmt(s){if(off!==null&&U.checked){var d=new Date(off+s*1000);return p2(d.getHours(),2)+':'+p2(d.getMinutes(),2)+':'+p2(d.getSeconds(),2)+'.'+p2(d.getMilliseconds(),3)}"
"var m=Math.floor(s/60);return p2(m,2)+':'+p2((s-m*60).toFixed(3),6)}"
"function vis(d){var q=F.value,lv=+V.value,dl=+d.dataset.lv;if(dl<lv)return false;if(!q)return true;"
"if(q.indexOf('tag:')==0)return d.dataset.tag==q.slice(4);return d.textContent.indexOf(q)>=0}"
"function add(t){var d=document.createElement('div'),m=RX.exec(t);"
"if(!m){d.className='x';d.dataset.lv=0;d.dataset.tag='';d.textContent=t}else{"
"var lv=m[2],msg=m[3],tg='',x=TG.exec(msg);if(x){tg=x[1];msg=msg.slice(x[0].length)}"
"var w=WC.exec(msg);if(w&&/^log /.test(msg)){var a=Date.parse(w[1]);if(!isNaN(a))off=a-parseFloat(m[1])*1000}"
"d.className='r '+lv;d.dataset.lv=lv=='ERROR'?2:lv=='WARN'?1:0;d.dataset.tag=tg;d.dataset.s=m[1];"
"var c=[['t',fmt(parseFloat(m[1]))],['l',lv],['g',tg],['m',msg]];"
"for(var i=0;i<4;i++){var e=document.createElement('span');e.className=c[i][0];e.textContent=c[i][1];"
"if(c[i][0]=='g'&&tg)e.onclick=function(){F.value='tag:'+this.textContent;refilter()};d.appendChild(e)}}"
"L.appendChild(d);d.hidden=!vis(d);while(L.childNodes.length>MAX)L.removeChild(L.firstChild);"
"if(!paused&&!d.hidden)window.scrollTo(0,document.body.scrollHeight)}"
"function refilter(){for(var i=0;i<L.childNodes.length;i++){var d=L.childNodes[i];d.hidden=!vis(d);"
"if(d.dataset.s)d.firstChild.textContent=fmt(parseFloat(d.dataset.s))}}"
"function open(){if(es)es.close();es=new EventSource('/stream');"
"es.onopen=function(){st.textContent='live'};es.onerror=function(){st.textContent='reconnecting'};"
"es.onmessage=function(e){add(e.data)}}"
"F.oninput=refilter;V.onchange=refilter;U.onchange=refilter;"
"document.getElementById('p').onclick=function(){paused=!paused;this.textContent=paused?'Resume':'Pause'};"
"document.getElementById('c').onclick=function(){L.textContent=''};"
"open();</script>";

typedef struct {
    int tail_now;
    int min_level;      /* 0 all, 1 WARN and ERROR, 2 ERROR only */
    char grep[96];
} ls_opts_t;

/* "[12.345] WARN text" -> 1, "[12.345] ERROR text" -> 2, anything else 0 */
static int line_level(const char *s)
{
    const char *p = strstr(s, "] ");
    if (!p || s[0] != '[') return 0;
    p += 2;
    if (!strncmp(p, "ERROR ", 6)) return 2;
    if (!strncmp(p, "WARN ", 5)) return 1;
    return 0;
}

static void parse_query(const char *q, ls_opts_t *o)
{
    memset(o, 0, sizeof *o);
    while (q && *q) {
        if (!strncmp(q, "tail=0", 6))
            o->tail_now = 1;
        else if (!strncmp(q, "level=warn", 10))
            o->min_level = 1;
        else if (!strncmp(q, "level=error", 11))
            o->min_level = 2;
        else if (!strncmp(q, "grep=", 5)) {
            const char *v = q + 5;
            size_t n = 0;
            while (*v && *v != '&' && n + 1 < sizeof o->grep) {
                if (v[0] == '%' && v[1] && v[2]) {
                    char h[3] = { v[1], v[2], 0 };
                    o->grep[n++] = (char)strtol(h, NULL, 16);
                    v += 3;
                } else {
                    o->grep[n++] = *v == '+' ? ' ' : *v;
                    v++;
                }
            }
            o->grep[n] = 0;
        }
        q = strchr(q, '&');
        if (q) q++;
    }
}

static int send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int send_str(int fd, const char *s) { return send_all(fd, s, strlen(s)); }

/* Send the lines in buf[0..n) that match o->grep, framed per `mode`:
 * 's' = SSE events, anything else = raw text. */
static int emit(int fd, char mode, const ls_opts_t *o, char *buf, size_t n)
{
    char out[LS_CHUNK + 1024];
    size_t used = 0;
    size_t i = 0;
    while (i < n) {
        char *nl = memchr(buf + i, '\n', n - i);
        size_t len = nl ? (size_t)(nl - (buf + i)) : n - i;
        char saved = buf[i + len];
        buf[i + len] = 0;
        if ((!o->grep[0] || strstr(buf + i, o->grep)) &&
            (!o->min_level || line_level(buf + i) >= o->min_level)) {
            size_t need = len + 16;
            if (used + need > sizeof out) {
                if (send_all(fd, out, used) != 0) return -1;
                used = 0;
            }
            if (mode == 's')
                used += (size_t)snprintf(out + used, sizeof out - used, "data: %s\n\n", buf + i);
            else
                used += (size_t)snprintf(out + used, sizeof out - used, "%s\n", buf + i);
            if (used > sizeof out) used = sizeof out;
        }
        buf[i + len] = saved;
        i += len + 1;
    }
    return used ? send_all(fd, out, used) : 0;
}

static void serve_stream(int fd, char mode, const ls_opts_t *o)
{
    if (mode == 's')
        send_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                     "Cache-Control: no-cache\r\nConnection: close\r\n"
                     "Access-Control-Allow-Origin: *\r\n\r\nretry: 2000\n\n");
    else
        send_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                     "Cache-Control: no-cache\r\nConnection: close\r\n"
                     "Access-Control-Allow-Origin: *\r\n\r\n");

    unsigned long long pos = o->tail_now ? evo_log_ring_total() : 0, missed = 0, reported = 0;
    char buf[LS_CHUNK];
    int idle = 0;
    for (;;) {
        size_t n = evo_log_ring_read(&pos, buf, sizeof buf, 1000, &missed);
        if (missed != reported) {
            char note[96];
            snprintf(note, sizeof note, "[log] %llu bytes missed - client too slow\n",
                     missed - reported);
            reported = missed;
            if (emit(fd, mode, &(ls_opts_t){0}, note, strlen(note)) != 0) return;
        }
        if (n) {
            idle = 0;
            if (emit(fd, mode, o, buf, n) != 0) return;
        } else if (++idle >= 10) {
            idle = 0;   /* keepalive; also how a dead client is noticed */
            if (mode == 's' && send_str(fd, ": ping\n\n") != 0) return;
        }
    }
}

static void serve_snapshot(int fd, const ls_opts_t *o)
{
    send_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                 "Cache-Control: no-cache\r\nConnection: close\r\n\r\n");
    unsigned long long pos = 0, end = evo_log_ring_total(), missed = 0;
    char buf[LS_CHUNK];
    while (pos < end) {
        size_t n = evo_log_ring_read(&pos, buf, sizeof buf, 0, &missed);
        if (!n) break;
        if (emit(fd, 'r', o, buf, n) != 0) return;
    }
}

static void *conn_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    tv.tv_sec = 10;   /* a stalled client is dropped, not waited on forever */
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif

    char req[1024];
    ssize_t got = recv(fd, req, sizeof req - 1, 0);
    if (got > 0) {
        req[got] = 0;
        char method[8] = "", target[512] = "";
        sscanf(req, "%7s %511s", method, target);
        char *q = strchr(target, '?');
        if (q) *q++ = 0;
        ls_opts_t o;
        parse_query(q, &o);

        pthread_mutex_lock(&s_mx);
        const int busy = s_clients >= LS_MAX_CLIENTS;
        if (!busy) s_clients++;
        pthread_mutex_unlock(&s_mx);

        if (strcmp(method, "GET") != 0)
            send_str(fd, "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n");
        else if (busy)
            send_str(fd, "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\ntoo many log clients\n");
        else {
            if (!strcmp(target, "/")) {
                char head[160];
                snprintf(head, sizeof head,
                         "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                         "Content-Length: %zu\r\nConnection: close\r\n\r\n", sizeof k_page - 1);
                send_str(fd, head);
                send_all(fd, k_page, sizeof k_page - 1);
            } else if (!strcmp(target, "/stats")) {
                char body[2048], head[192];
                size_t n = evo_stats_json(body, sizeof body);
                snprintf(head, sizeof head,
                         "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                         "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\n"
                         "Content-Length: %zu\r\nConnection: close\r\n\r\n", n);
                send_str(fd, head);
                send_all(fd, body, n);
            } else if (!strcmp(target, "/stream"))
                serve_stream(fd, 's', &o);
            else if (!strcmp(target, "/raw"))
                serve_stream(fd, 'r', &o);
            else if (!strcmp(target, "/log"))
                serve_snapshot(fd, &o);
            else
                send_str(fd, "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
            pthread_mutex_lock(&s_mx);
            s_clients--;
            pthread_mutex_unlock(&s_mx);
        }
    }
    close(fd);
    return NULL;
}

static void *accept_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            evo_log("logsrv: accept errno=%d - stopped", errno);
            break;
        }
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 128 * 1024);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &a, conn_thread, (void *)(intptr_t)c) != 0)
            close(c);
        pthread_attr_destroy(&a);
    }
    close(lfd);
    return NULL;
}

void evo_log_server_start(void)
{
    if (s_started) return;
    s_started = 1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { evo_log("logsrv: socket errno=%d", errno); return; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(EVO_LOG_SERVER_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 4) != 0) {
        evo_log("logsrv: bind/listen :%d failed errno=%d", EVO_LOG_SERVER_PORT, errno);
        close(fd);
        return;
    }
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 128 * 1024);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &at, accept_thread, (void *)(intptr_t)fd);
    pthread_attr_destroy(&at);
    if (rc != 0)
        close(fd);
    evo_log("logsrv: listening on :%d (thread rc=%d)", EVO_LOG_SERVER_PORT, rc);
}

#endif /* EVO_APP_MODULE */
