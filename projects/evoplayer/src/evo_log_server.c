/*
 * evo_log_server.c - live log over HTTP, port EVO_LOG_SERVER_PORT (9780).
 *
 * Read-only view of what evo_boot_log() writes to /mnt/usb0/evo.log, without a
 * USB stick pull or FTP. Every line is already credential-redacted before it
 * reaches the ring, so nothing here needs to filter.
 *
 *   GET /              the input page if a keyboard is open, else tails the log
 *   GET /log           tails the log in the browser
 *   GET /stream        Server-Sent Events, one event per line
 *   GET /raw           plain text, kept open (curl -N http://<ps5>:9780/raw)
 *   GET /snapshot      snapshot of the ring (the last ~128 KiB), then closes
 *   GET /report        the phone page for sending a problem report (#229)
 *   GET /report.txt    that report as a download: version, model, stats, settings
 *                      and the end of evo.log from disk (evo_report_write)
 *   GET /stats         one JSON snapshot of memory, read-ahead, network and
 *                      playback (evo_stats.c) - tools/evo-dash.py graphs it
 *   GET /input         the phone page: type or paste text for the console's
 *                      open keyboard (evo_keyboard_open_phone) and send it
 *   GET /input/state   {"open","seq","title","text","max","secret"} for that page
 *   POST /input        the text as the body; needs the X-EVO-Input header
 *
 * Case-insensitive paths and trailing slashes are accepted for /input, /input/state,
 * /log, etc. Unknown GET paths redirect (302) to /input.
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
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
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
#include "evo_build_id.h"
#include "evo_data_path.h"
#include "evo_hw.h"
#include "evo_keyboard.h"
#include "evo_prev_run.h"

#ifndef EVO_PLAYER_VERSION
#define EVO_PLAYER_VERSION "dev"
#endif

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

/* The phone page for long text entry (evo_keyboard_open_phone). No external
 * files, no libraries: it has to load on any phone browser on the LAN. */
static const char k_input_page[] =
"<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>EVO input</title><style>"
":root{color-scheme:dark light;--bg:#111;--fg:#eee;--dim:#999;--card:#1c1c1c;--line:#333;--ac:#4da3ff}"
"@media(prefers-color-scheme:light){:root{--bg:#f5f5f5;--fg:#222;--dim:#666;--card:#fff;--line:#ddd;--ac:#0a66d6}}"
"body{margin:0;font:17px/1.4 system-ui,sans-serif;background:var(--bg);color:var(--fg)}"
"main{max-width:560px;margin:0 auto;padding:20px 16px}h1{font-size:20px;margin:0 0 4px}"
"#sub{color:var(--dim);margin:0 0 16px}#cnt{color:var(--dim);font-size:14px;text-align:right;margin:4px 0 0}"
"textarea{width:100%;box-sizing:border-box;min-height:140px;font:16px/1.4 ui-monospace,monospace;padding:10px;"
"border:1px solid var(--line);border-radius:8px;background:var(--card);color:var(--fg)}"
"textarea.sec{-webkit-text-security:disc;text-security:disc;min-height:56px}"
"button{width:100%;margin-top:12px;padding:14px;font:inherit;font-weight:600;border:0;border-radius:8px;background:var(--ac);color:#fff}"
"button:disabled{opacity:.4}#msg{margin-top:12px;min-height:1.4em}"
"</style><main><h1>EVO Player</h1><p id=sub>Checking...</p>"
"<textarea id=t autocapitalize=off autocorrect=off autocomplete=off spellcheck=false placeholder='Type or paste here'></textarea>"
"<p id=cnt></p><button id=b disabled>Send to PS5</button><p id=msg></p></main><script>"
"var T=document.getElementById('t'),B=document.getElementById('b'),S=document.getElementById('sub'),M=document.getElementById('msg'),C=document.getElementById('cnt'),"
"seq=-1,sec=false,dirty=false,busy=false;"
"function cnt(){C.textContent=T.maxLength>0?T.value.length+' / '+T.maxLength:''}"
"function poll(){if(busy)return;fetch('/input/state',{cache:'no-store'}).then(function(r){return r.json()}).then(function(s){"
"if(!s.open){seq=-1;B.disabled=true;S.textContent='Nothing is asking for text right now. Open a text box on the PS5 first.';return}"
"if(s.seq!==seq){var fresh=seq!==-1;seq=s.seq;dirty=false;sec=!!s.secret;T.className=sec?'sec':'';T.maxLength=s.max;T.value=s.text;"
"if(fresh)M.textContent='New prompt on the PS5.';T.focus()}"
"else if(!dirty&&T.value!==s.text)T.value=s.text;"
"S.textContent=s.title;B.disabled=false;cnt()"
"}).catch(function(){B.disabled=true;S.textContent='Cannot reach the PS5'})}"
"T.oninput=function(){dirty=true;cnt()};"
"B.onclick=function(){busy=true;B.disabled=true;M.textContent='Sending...';"
"fetch('/input',{method:'POST',headers:{'X-EVO-Input':'1','X-EVO-Prompt':String(seq),'Content-Type':'text/plain;charset=utf-8'},body:sec?T.value:T.value.trim()})"
".then(function(r){busy=false;"
"if(r.ok){M.textContent='Sent. Check your PS5.';T.value='';dirty=false;seq=-2}"
"else M.textContent=r.status==409?'The PS5 moved on to another prompt. Check it and send again.':'Could not send.';"
"poll()})"
".catch(function(){busy=false;M.textContent='Could not reach the PS5.';B.disabled=false})};"
"poll();setInterval(poll,1000);</script>";

/* #229: the phone page for sending a problem report. It loads /report.txt once,
 * shows how big it is and its last warnings and errors, and offers Download,
 * Share (only where the browser allows it on plain http) and Copy. No external
 * files: it has to load on any phone browser on the LAN. */
static const char k_report_page[] =
"<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>EVO problem report</title><style>"
":root{color-scheme:dark light;--bg:#111;--fg:#eee;--dim:#999;--card:#1c1c1c;--line:#333;--ac:#4da3ff;--e:#ff6b6b;--w:#ffc857}"
"@media(prefers-color-scheme:light){:root{--bg:#f5f5f5;--fg:#222;--dim:#666;--card:#fff;--line:#ddd;--ac:#0a66d6;--e:#c62828;--w:#a66a00}}"
"body{margin:0;font:17px/1.45 system-ui,sans-serif;background:var(--bg);color:var(--fg)}"
"main{max-width:640px;margin:0 auto;padding:20px 16px}h1{font-size:21px;margin:0 0 6px}h2{font-size:16px;margin:22px 0 6px}"
"p{margin:6px 0}.dim{color:var(--dim)}ol{padding-left:22px;margin:6px 0}"
".b{display:block;width:100%;box-sizing:border-box;margin-top:10px;padding:14px;font:inherit;font-weight:600;border:0;"
"border-radius:8px;background:var(--ac);color:#fff;text-align:center;text-decoration:none}.b.alt{background:var(--card);color:var(--fg);border:1px solid var(--line)}"
"#sum{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:10px 12px;margin-top:12px}"
"pre{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:8px;font:12px/1.4 ui-monospace,monospace;"
"white-space:pre-wrap;word-break:break-word;max-height:320px;overflow:auto}.E{color:var(--e)}.W{color:var(--w)}"
"textarea{position:absolute;left:-9999px}"
"</style><main><h1>EVO Player: report a problem</h1>"
"<p>This page gets EVO's log from your PS5, so you can send it with your bug report. No USB stick needed.</p>"
"<ol><li>Tap <b>Download report</b> (or Share / Copy).</li>"
"<li>Attach the file to your report on GitHub or Discord, and say what you did just before the problem.</li></ol>"
"<p id=runs>Report: <a href='/report'>this run</a> &middot; <a href='/report?run=last'>the last run</a> "
"<span class=dim>(pick the last run if EVO crashed or froze before you reopened it)</span></p>"
"<div id=sum>Loading the report from the PS5...</div>"
"<a class=b id=dl href='/report.txt' download>Download report</a>"
"<button class='b alt' id=sh hidden>Share</button><button class='b alt' id=cp>Copy text</button><p id=msg class=dim></p>"
"<h2>Last warnings and errors</h2><pre id=last class=dim>-</pre>"
"<p class=dim>Passwords, tokens and keys are removed from the report. Server addresses, folder and file names stay in, "
"because they help find the problem. <a href='/log'>Live log</a></p></main><textarea id=ta></textarea><script>"
"var S=document.getElementById('sum'),M=document.getElementById('msg'),LA=document.getElementById('last'),"
"SH=document.getElementById('sh'),TA=document.getElementById('ta'),txt='',name='evo-report.txt',"
"LR=location.search.indexOf('run=last')>=0,U='/report.txt'+(LR?'?run=last':'');"
"document.getElementById('dl').href=U;document.getElementById('runs').children[LR?1:0].style.fontWeight='700';"
"fetch(U,{cache:'no-store'}).then(function(r){var d=r.headers.get('Content-Disposition')||'',m=/filename=\"([^\"]+)\"/.exec(d);"
"if(m)name=m[1];document.getElementById('dl').setAttribute('download',name);return r.text()}).then(function(t){txt=t;"
"var ls=t.split('\\n'),e=0,w=0,bad=[];for(var i=0;i<ls.length;i++){var l=ls[i];"
"if(/^\\[[0-9.]+\\] ERROR /.test(l)){e++;bad.push(['E',l])}else if(/^\\[[0-9.]+\\] WARN /.test(l)){w++;bad.push(['W',l])}}"
"var b=(ls[1]||'')+'. '+(ls[2]||'');S.textContent=(LR?'Last run report: ':'Ready: ')+Math.round(t.length/1024)+' KB, '+e+' error'+(e==1?'':'s')+', '+w+' warning'+(w==1?'':'s')+'. '+b;"
"LA.textContent='';LA.className='';bad=bad.slice(-30);if(!bad.length)LA.textContent='None.';"
"for(var j=0;j<bad.length;j++){var s=document.createElement('div');s.className=bad[j][0];s.textContent=bad[j][1];LA.appendChild(s)}"
"try{var f=new File([t],name,{type:'text/plain'});if(navigator.canShare&&navigator.canShare({files:[f]})){SH.hidden=false;"
"SH.onclick=function(){navigator.share({files:[f],title:'EVO Player report'}).catch(function(){})}}}catch(x){}"
"}).catch(function(){S.textContent='Could not reach the PS5. Is EVO still open, and is this phone on the same network?'});"
"document.getElementById('cp').onclick=function(){if(!txt){M.textContent='The report has not loaded yet.';return}"
"function done(){M.textContent='Copied. Paste it into your report.'}"
"if(navigator.clipboard&&window.isSecureContext){navigator.clipboard.writeText(txt).then(done,function(){M.textContent='Copy failed - use Download.'});return}"
"TA.value=txt;TA.select();try{document.execCommand('copy');done()}catch(x){M.textContent='Copy failed - use Download.'}};"
"</script>";

typedef struct {
    int tail_now;
    int min_level;      /* 0 all, 1 WARN and ERROR, 2 ERROR only */
    int last_run;       /* /report.txt?run=last: the previous run's report */
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
        else if (!strncmp(q, "run=last", 8))
            o->last_run = 1;
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

/* The client hung up (a curl --max-time, a closed tab). A filtered stream may
 * write nothing for hours, so a failed send cannot be how this is noticed: four
 * such leftovers held every slot and the server answered 503 (2026-11-03). */
static int client_gone(int fd)
{
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, 0) <= 0) return 0;
    if (p.revents & (POLLHUP | POLLERR | POLLNVAL)) return 1;
    char c;
    const ssize_t r = recv(fd, &c, 1, MSG_PEEK);   /* readable: does not block */
    return r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR);
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
        if (client_gone(fd)) return;
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

/* Case-insensitive prefix test; the native libc is not trusted with strcasecmp. */
static int ci_prefix(const char *s, const char *p)
{
    for (; *p; ++s, ++p) {
        char a = *s;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != *p) return 0;
    }
    return 1;
}

/* Lower-case ASCII and drop trailing slashes (except root "/"). */
static void normalize_path(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    while (*src && i + 1 < cap) {
        char c = *src++;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        dst[i++] = c;
    }
    dst[i] = '\0';
    while (i > 1 && dst[i - 1] == '/') {
        dst[--i] = '\0';
    }
}

/* The value of header `name` (lower case, with the colon) in a request head, or NULL. */
static const char *header_value(const char *req, const char *name)
{
    const char *l = strstr(req, "\r\n");
    while (l && l[2] != '\r') {
        l += 2;
        if (ci_prefix(l, name)) {
            l += strlen(name);
            while (*l == ' ') l++;
            return l;
        }
        l = strstr(l, "\r\n");
    }
    return NULL;
}

/* Quote and escape for a JSON string; control characters become spaces. */
static size_t json_esc(char *out, size_t cap, const char *in)
{
    size_t k = 0;
    for (const char *p = in; *p && k + 3 < cap; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[k++] = '\\'; out[k++] = (char)c; }
        else if (c < 0x20)         out[k++] = ' ';
        else                       out[k++] = (char)c;
    }
    out[k] = '\0';
    return k;
}

/* GET /input/state - the open prompt: its id, what it asks for, and what is in the field now. */
static void serve_input_state(int fd)
{
    evo_keyboard_phone_state_t st;
    char title[320], text[1100], body[1700], head[200];
    int n;
    if (evo_keyboard_phone_state(&st)) {
        json_esc(title, sizeof title, st.prompt);
        json_esc(text, sizeof text, st.text);
        n = snprintf(body, sizeof body,
                     "{\"open\":1,\"seq\":%u,\"title\":\"%s\",\"text\":\"%s\",\"max\":%d,\"secret\":%d}",
                     st.seq, title, text, st.max_len, st.secret);
    } else {
        n = snprintf(body, sizeof body, "{\"open\":0}");
    }
    snprintf(head, sizeof head,
             "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
             "Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", n);
    send_str(fd, head);
    send_all(fd, body, (size_t)n);
}

/*
 * POST /input - the text goes to the open keyboard and is submitted, as if it
 * had been typed there. The custom header makes it a request a web page on
 * another site cannot send without a preflight this server never answers, so
 * only the page above (or curl -H 'X-EVO-Input: 1') can type into the console.
 * X-EVO-Prompt names the prompt the text is for (the "seq" of /input/state); a
 * request for a prompt that has since closed gets 409, so the user name never
 * lands in the password box. Without the header it answers whatever is open.
 * An empty body is valid (an empty password, "clear to reset").
 * The text itself is never logged.
 */
static void serve_input_post(int fd, const char *head, const char *body, size_t blen, int too_big)
{
    if (!header_value(head, "x-evo-input:")) {
        send_str(fd, "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n");
        return;
    }
    if (too_big || blen >= 512) {                   /* 512 is the keyboard's own limit */
        send_str(fd, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
        return;
    }
    evo_keyboard_phone_state_t st;
    const char *pv = header_value(head, "x-evo-prompt:");
    if (!evo_keyboard_phone_state(&st) || (pv && (unsigned)strtoul(pv, NULL, 10) != st.seq)) {
        send_str(fd, "HTTP/1.1 409 Conflict\r\nConnection: close\r\n\r\nthat text prompt is not open\n");
        return;
    }
    char text[512];
    memcpy(text, body ? body : "", blen);
    text[blen] = '\0';
    if (!evo_keyboard_phone_submit(st.seq, text, 1)) {
        send_str(fd, "HTTP/1.1 409 Conflict\r\nConnection: close\r\n\r\nthat text prompt is not open\n");
        return;
    }
    evo_log("logsrv: phone input accepted (%zu chars)", blen);
    send_str(fd, "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\nok\n");
}

/* ---- #229: the problem report ------------------------------------------ */

typedef struct { evo_report_sink_fn sink; void *ud; long total; int stop; } rp_t;

static void rp_put(rp_t *r, const char *s, size_t n)
{
    if (r->stop || !n) return;
    if (r->sink(r->ud, s, n) != 0) { r->stop = 1; return; }
    r->total += (long)n;
}

static void rp_printf(rp_t *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void rp_printf(rp_t *r, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    rp_put(r, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

static long rp_file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    fclose(f);
    return n;
}

/* The last `max` bytes of a text file, from a line start, each line redacted
 * again: a log written by an older build may predate a redaction rule. */
static void rp_file_tail(rp_t *r, const char *title, const char *path, long max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    long size = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : 0;
    long start = size > max ? size - max : 0;
    fseek(f, start, SEEK_SET);
    rp_printf(r, "\n=== %s: %s (%s%ld of %ld bytes) ===\n", title, path,
              start ? "last " : "", size - start, size);
    char line[1024];
    int skip = start > 0;          /* the first line is cut in half: drop it */
    while (!r->stop && fgets(line, sizeof line, f)) {
        if (skip) { skip = line[strlen(line) - 1] != '\n'; continue; }
        evo_log_redact(line, sizeof line);
        rp_put(r, line, strlen(line));
    }
    fclose(f);
}

long evo_report_write_run(evo_report_sink_fn sink, void *ud, int last_run)
{
    rp_t r = { sink, ud, 0, 0 };
    evo_boot_log_flush();          /* everything logged so far is on disk */

    char path[160];
    const int have_file = evo_log_file_path(path, sizeof path);
    const evo_prev_run_t prev = evo_prev_run_state();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    rp_printf(&r, "EVO Player problem report - %s\n", last_run ? "LAST run" : "this run");
    rp_printf(&r, "version %s, build %s, %s, up %lld s\n", EVO_PLAYER_VERSION, EVO_BUILD_ID,
              evo_hw_model_name(), (long long)ts.tv_sec);
    rp_printf(&r, "last run: %s\n", evo_prev_run_text(prev));
    rp_printf(&r, "log file: %s\n", have_file ? path : "none (no USB stick and /data not writable)");

    if (!last_run) {
        char stats[2048];
        size_t sn = evo_stats_json(stats, sizeof stats);
        rp_printf(&r, "\n=== stats ===\n");
        rp_put(&r, stats, sn);
        rp_put(&r, "\n", 1);
    }

    /* Settings are numbers and the log port: no names, addresses or passwords.
     * The network shares file has passwords and is never included. */
    rp_file_tail(&r, "settings", evo_data_path("evo_player_settings.cfg"), 8192);

    if (last_run) {
        /* The run before this one, saved at start (evo_prev_run.c): a crash
         * stays reportable however long the new session has been running. */
        if (prev == EVO_PREV_RUN_NONE)
            rp_printf(&r, "\n=== no earlier run is saved (the log was empty, cleared or missing) ===\n");
        else
            rp_file_tail(&r, "last run log", evo_prev_run_path(), EVO_REPORT_LOG_BYTES);
    } else if (have_file) {
        long budget = EVO_REPORT_LOG_BYTES;
        long cur = rp_file_size(path);
        if (cur >= 0 && cur < budget) {
            /* Room left: the end of the rotated file too, so a crash just
             * before a rotation is still in the report. */
            char old[176];
            snprintf(old, sizeof old, "%s.1", path);
            rp_file_tail(&r, "older log", old, budget - cur);
        }
        rp_file_tail(&r, "log", path, budget);
    } else {
        /* No file: what is still in memory, this session only. */
        rp_printf(&r, "\n=== log (memory, this session only) ===\n");
        unsigned long long pos = 0, end = evo_log_ring_total(), missed = 0;
        char buf[LS_CHUNK];
        while (!r.stop && pos < end) {
            size_t n = evo_log_ring_read(&pos, buf, sizeof buf, 0, &missed);
            if (!n) break;
            rp_put(&r, buf, n);
        }
    }
    rp_printf(&r, "\n=== end of report ===\n");
    return r.stop ? -1 : r.total;
}

long evo_report_write(evo_report_sink_fn sink, void *ud)
{
    return evo_report_write_run(sink, ud, 0);
}

static int rp_file_sink(void *ud, const char *data, size_t n)
{
    return fwrite(data, 1, n, (FILE *)ud) == n ? 0 : -1;
}

int evo_report_save_run(char *out, size_t cap, int last_run)
{
    const char *name = last_run ? "evo_report_last_run.txt" : "evo_report.txt";
    char usb[64], data[192];
    snprintf(usb, sizeof usb, "/mnt/usb0/%s", name);
    /* A copy: evo_data_path() hands back one shared buffer, and writing the
     * report calls it again (the settings file) - the toast then named that. */
    snprintf(data, sizeof data, "%s", evo_data_path(name));
    const char *paths[2] = { usb, data };
    for (int i = 0; i < 2; ++i) {
        FILE *f = fopen(paths[i], "w");
        if (!f) continue;
        const long n = evo_report_write_run(rp_file_sink, f, last_run);
        const int closed = fclose(f) == 0;
        if (n > 0 && closed) {
            if (out && cap) snprintf(out, cap, "%s", paths[i]);
            EVO_LOGI("report", "saved %ld bytes to %s", n, paths[i]);
            return 1;
        }
        EVO_LOGE("report", "writing %s failed", paths[i]);
    }
    if (out && cap) out[0] = '\0';
    return 0;
}

int evo_report_save(char *out, size_t cap)
{
    return evo_report_save_run(out, cap, 0);
}

static int rp_sock_sink(void *ud, const char *data, size_t n)
{
    return send_all((int)(intptr_t)ud, data, n);
}

/* GET /report.txt[?run=last] - the report as a file the phone saves. */
static void serve_report_txt(int fd, int last_run)
{
    char head[320];
    snprintf(head, sizeof head,
             "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
             "Content-Disposition: attachment; filename=\"evo-report-%s%s.txt\"\r\n"
             "Cache-Control: no-store\r\nConnection: close\r\n\r\n", last_run ? "last-run-" : "", EVO_BUILD_ID);
    if (send_str(fd, head) != 0) return;
    const long n = evo_report_write_run(rp_sock_sink, (void *)(intptr_t)fd, last_run);
    EVO_LOGI("report", "%s sent to a phone (%ld bytes)", last_run ? "last-run report" : "report", n);
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

    /* A GET is one segment; a POST /input may arrive as head then body, so read
     * until the head is complete and Content-Length bytes of body are in. */
    char req[4096];
    size_t got = 0;
    const char *body = NULL;
    size_t blen = 0, want = 0;
    int too_big = 0;
    for (;;) {
        ssize_t g = recv(fd, req + got, sizeof req - 1 - got, 0);
        if (g <= 0) break;
        got += (size_t)g;
        req[got] = '\0';
        char *he = strstr(req, "\r\n\r\n");
        if (!he) {
            if (got >= sizeof req - 1) break;
            continue;
        }
        if (strncmp(req, "POST ", 5) != 0) break;
        const char *cv = header_value(req, "content-length:");
        long clen = cv ? atol(cv) : 0;
        size_t head = (size_t)(he + 4 - req);
        if (clen < 0 || head + (size_t)clen > sizeof req - 1) { too_big = 1; break; }
        body = he + 4;
        want = (size_t)clen;
        blen = got - head < want ? got - head : want;
        if (got >= head + want) break;
    }
    if (body && blen < want) too_big = 1;       /* the client stalled mid-body: never submit half a URL */
    if (got > 0) {
        char method[8] = "", target[512] = "";
        sscanf(req, "%7s %511s", method, target);
        char *q = strchr(target, '?');
        if (q) *q++ = 0;
        ls_opts_t o;
        parse_query(q, &o);

        char norm[512];
        normalize_path(norm, target, sizeof norm);

        pthread_mutex_lock(&s_mx);
        const int busy = s_clients >= LS_MAX_CLIENTS;
        if (!busy) s_clients++;
        pthread_mutex_unlock(&s_mx);

        const int post_input = !strcmp(method, "POST") && !strcmp(norm, "/input");
        if (strcmp(method, "GET") != 0 && !post_input)
            send_str(fd, "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n");
        else if (busy)
            send_str(fd, "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\ntoo many log clients\n");
        else {
            if (post_input) {
                serve_input_post(fd, req, body, blen, too_big);
            } else if (!strcmp(norm, "/input")) {
                char head[160];
                snprintf(head, sizeof head,
                         "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                         "Cache-Control: no-store\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                         sizeof k_input_page - 1);
                send_str(fd, head);
                send_all(fd, k_input_page, sizeof k_input_page - 1);
            } else if (!strcmp(norm, "/input/state")) {
                serve_input_state(fd);
            } else if (!strcmp(norm, "/")) {
                if (evo_keyboard_phone_info(NULL, 0)) {
                    char head[160];
                    snprintf(head, sizeof head,
                             "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                             "Cache-Control: no-store\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                             sizeof k_input_page - 1);
                    send_str(fd, head);
                    send_all(fd, k_input_page, sizeof k_input_page - 1);
                } else {
                    char head[160];
                    snprintf(head, sizeof head,
                             "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                             "Cache-Control: no-cache\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                             sizeof k_page - 1);
                    send_str(fd, head);
                    send_all(fd, k_page, sizeof k_page - 1);
                }
            } else if (!strcmp(norm, "/log")) {
                char head[160];
                snprintf(head, sizeof head,
                         "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                         "Cache-Control: no-cache\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                         sizeof k_page - 1);
                send_str(fd, head);
                send_all(fd, k_page, sizeof k_page - 1);
            } else if (!strcmp(norm, "/report")) {
                char head[160];
                snprintf(head, sizeof head,
                         "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                         "Cache-Control: no-store\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                         sizeof k_report_page - 1);
                send_str(fd, head);
                send_all(fd, k_report_page, sizeof k_report_page - 1);
            } else if (!strcmp(norm, "/report.txt")) {
                serve_report_txt(fd, o.last_run);
            } else if (!strcmp(norm, "/stats")) {
                char body[2048], head[192];
                size_t n = evo_stats_json(body, sizeof body);
                snprintf(head, sizeof head,
                         "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                         "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\n"
                         "Content-Length: %zu\r\nConnection: close\r\n\r\n", n);
                send_str(fd, head);
                send_all(fd, body, n);
            } else if (!strcmp(norm, "/stream"))
                serve_stream(fd, 's', &o);
            else if (!strcmp(norm, "/raw"))
                serve_stream(fd, 'r', &o);
            else if (!strcmp(norm, "/snapshot"))
                serve_snapshot(fd, &o);
            else
                send_str(fd, "HTTP/1.1 302 Found\r\nLocation: /input\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            pthread_mutex_lock(&s_mx);
            s_clients--;
            pthread_mutex_unlock(&s_mx);
        }
    }
    close(fd);
    return NULL;
}

/*
 * The listener is owned by one manager thread (#227). It binds the saved port,
 * falls back to the next nine when that is busy, keeps retrying the saved port
 * and swaps back as soon as it is free. s_actual is the one port everything else
 * (phone link, QR, settings card) reads. One log line per outcome, not per try.
 */
#define LS_FALLBACKS     9
#define LS_RETRY_SECS    3
#define LS_PORT_MIN      1024
#define LS_PORT_MAX      65535

static pthread_mutex_t s_pm = PTHREAD_MUTEX_INITIALIZER;
static int s_saved = EVO_LOG_SERVER_PORT;
static int s_actual;                 /* 0 = not listening */
static int s_state = EVO_LOGSRV_STARTING;
static volatile int s_wake;          /* the saved port changed: retry now */
static int s_cfg;                    /* the saved port is known (settings were read) */
#define LS_CFG_WAIT_SECS 4          /* no settings file: bind the default after this */

static int bind_port(int port, int *err)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { *err = errno; return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 4) != 0) {
        *err = errno;
        close(fd);
        return -1;
    }
    struct timeval tv = { 1, 0 };    /* accept wakes each second to reconcile */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

static long now_secs(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec;
}

static void *server_thread(void *arg)
{
    (void)arg;
    int lfd = -1;
    long next_try = 0;
    const long t0 = now_secs();
    int logged = -1;                 /* state*100000+port of the last log line */
    for (;;) {
        pthread_mutex_lock(&s_pm);
        int want = s_saved;
        int have = s_actual;
        int wake = s_wake;
        s_wake = 0;
        pthread_mutex_unlock(&s_pm);

        long now = now_secs();
        pthread_mutex_lock(&s_pm);
        const int cfg = s_cfg;
        pthread_mutex_unlock(&s_pm);
        if (!cfg && now < t0 + LS_CFG_WAIT_SECS) {   /* do not bind the default port just to move */
            usleep(100 * 1000);
            continue;
        }
        if ((lfd < 0 || have != want) && (wake || now >= next_try)) {
            next_try = now + LS_RETRY_SECS;
            int err = 0, busy = 0, nfd = bind_port(want, &err);
            int port = want, state = EVO_LOGSRV_SAVED;
            if (nfd < 0 && err == EADDRINUSE) busy = 1;
            if (nfd < 0 && lfd < 0) {
                for (int k = 1; k <= LS_FALLBACKS && nfd < 0; k++) {
                    if (want + k > LS_PORT_MAX) break;
                    int e2 = 0;
                    nfd = bind_port(want + k, &e2);
                    if (nfd >= 0) { port = want + k; state = EVO_LOGSRV_FALLBACK; }
                    else if (e2 == EADDRINUSE) busy = 1;
                }
            }
            if (nfd >= 0) {
                if (lfd >= 0) close(lfd);
                lfd = nfd;
                pthread_mutex_lock(&s_pm);
                s_actual = port;
                s_state = state;
                pthread_mutex_unlock(&s_pm);
                if (logged != state * 100000 + port) {
                    logged = state * 100000 + port;
                    if (state == EVO_LOGSRV_SAVED)
                        evo_log("logsrv: listening on :%d", port);
                    else
                        evo_log("logsrv: :%d is busy (errno=%d), using :%d", want, err, port);
                }
            } else if (lfd < 0) {
                int st = busy ? EVO_LOGSRV_NONE : EVO_LOGSRV_STARTING;
                pthread_mutex_lock(&s_pm);
                s_actual = 0;
                s_state = st;
                pthread_mutex_unlock(&s_pm);
                if (logged != st * 100000) {
                    logged = st * 100000;
                    if (busy)
                        evo_log("logsrv: no free port in :%d-:%d, retrying", want, want + LS_FALLBACKS);
                    else
                        evo_log("logsrv: cannot listen yet (errno=%d), retrying", err);
                }
            }
            /* else: the saved port is still busy and the fallback keeps serving */
        }
        if (lfd < 0) { usleep(250 * 1000); continue; }

        /* accept() on the console does not honour SO_RCVTIMEO, so a port change would
         * wait for the next connection; poll() wakes the loop to reconcile instead. */
        struct pollfd pfd = { lfd, POLLIN, 0 };
        if (poll(&pfd, 1, 250) <= 0) continue;
        int c = accept(lfd, NULL, NULL);
        if (c < 0) continue;
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 128 * 1024);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &a, conn_thread, (void *)(intptr_t)c) != 0)
            close(c);
        pthread_attr_destroy(&a);
    }
    return NULL;
}

void evo_log_server_start(void)
{
    if (s_started) return;
    s_started = 1;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 128 * 1024);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, server_thread, NULL) != 0) {
        s_started = 0;
        evo_log("logsrv: thread create failed");
    }
    pthread_attr_destroy(&at);
}

int evo_log_server_port(void)       { pthread_mutex_lock(&s_pm); int v = s_actual; pthread_mutex_unlock(&s_pm); return v; }
int evo_log_server_saved_port(void) { pthread_mutex_lock(&s_pm); int v = s_saved;  pthread_mutex_unlock(&s_pm); return v; }
int evo_log_server_state(void)      { pthread_mutex_lock(&s_pm); int v = s_state;  pthread_mutex_unlock(&s_pm); return v; }

void evo_log_server_set_port(int port)
{
    if (port < LS_PORT_MIN || port > LS_PORT_MAX) return;
    pthread_mutex_lock(&s_pm);
    s_saved = port;
    s_cfg = 1;
    s_wake = 1;
    pthread_mutex_unlock(&s_pm);
}

int evo_log_server_probe_port(int port)
{
    if (port < LS_PORT_MIN || port > LS_PORT_MAX) return EINVAL;
    if (port == evo_log_server_port()) return 0;
    int err = 0;
    int fd = bind_port(port, &err);
    if (fd < 0) return err ? err : EADDRINUSE;
    close(fd);
    return 0;
}

int evo_log_server_try_port(int port)
{
    int rc = evo_log_server_probe_port(port);
    if (rc == 0) evo_log_server_set_port(port);
    return rc;
}

#endif /* EVO_APP_MODULE */
