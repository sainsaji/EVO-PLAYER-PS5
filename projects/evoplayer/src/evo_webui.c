/*
 * evo_webui.c - a provider's own web UI in the PS5's system browser (#101).
 *
 * Compiled into every app-module build; a provider with
 * EVO_PROVIDER_CAP_WEBUI (Emby) opens through evo_webui_open(). Everything
 * lands in evo.log (and klog) with a "web:" prefix.
 *
 * What the hardware runs established (2026-09-26, FW 12.70):
 *   run 1  the dialog loads, opens from PPSA99039 and draws over the AGC plane;
 *          the library's own CallbackInitParam route is a dead end (a guessed
 *          block is accepted, but the dialog never closes on the callback URL
 *          and GetResult refuses every callback-result buffer, 0x80b8000a).
 *   run 2  a page reaches a loopback listener in EVO by fetch() and by
 *          navigation, and sceWebBrowserDialogClose() on a RUNNING dialog
 *          dismisses it.
 *   run 3  Mode 2 (Custom) takes a rectangle, so the page can sit beside EVO's
 *          nav rail and look embedded. parts/control 0xFF is refused.
 *   run 4  Emby's web UI, proxied through EVO with the hook injected: pressing
 *          Play in Emby plays the file in EVO's own player.
 *
 * With `proxy=` set, EVO is a reverse proxy for that server on
 * 127.0.0.1:<port>: the browser loads the site THROUGH EVO, EVO injects
 * /evo/hook.js into its index.html, and the hook catches the site's own player
 * the moment it sets a media source. It sends that stream URL to /evo/play
 * instead; EVO closes the dialog, plays the stream in its own player, and
 * reopens the site on the page the user was on once playback ends.
 *
 * Dev override: the trigger file /mnt/usb0/evo_web_probe, read once at boot,
 * opens a session without going through a provider. One key=value per line:
 *
 *   url=/web/index.html          page to open. A path is taken relative to the
 *                                proxy; a full http:// URL is opened as is
 *   proxy=http[s]://<host>:<port>  upstream to proxy. Optional
 *   port=8686                    loopback port (default 8686)
 *   layout=<n>                   open in k_layouts[n-1] (Custom mode) instead
 *                                of the full-screen default
 *   layouts=1, hold=<s>          run 3: cycle every layout, <s> seconds each
 *
 * Rules (CLAUDE.md, #101): no memory scans, no sceVideoOutOpen. The server
 * binds loopback only and lives only while the probe runs.
 */
#if defined(EVO_APP_MODULE)

#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <openssl/ssl.h>

#include "evo_webui.h"
#include "evo_boot_trace.h"
#include "evo_provider.h"     /* evo_provider_parse_web_source */
#include "evo_data_path.h"    /* evo_data_path, evo_mkdir - the saved page storage */

/* ---- libSceWebBrowserDialog, laid out after SharpProspero
 *      Interop/Dialog/WebBrowserDialog.cs (sizes 48 / 328 / 256). ---- */

#define SCE_SYSMODULE_WEB_BROWSER_DIALOG 0x00AB
#define WBD_MAGIC 0xC0D1A109u

enum { CDLG_STATUS_NONE = 0, CDLG_STATUS_INITIALIZED = 1,
       CDLG_STATUS_RUNNING = 2, CDLG_STATUS_FINISHED = 3 };

typedef struct {
    uint64_t size;
    uint8_t  reserved[36];
    uint32_t magic;               /* WBD_MAGIC + &this block */
} evo_cdlg_base_t;

typedef struct {
    evo_cdlg_base_t base;         /* 0   */
    uint64_t size;                /* 48  */
    int32_t  mode;                /* 56  1 = default, 2 = custom rect */
    int32_t  user_id;             /* 60  */
    const char *url;              /* 64  */
    void    *callback_init;       /* 72  layout unknown - left NULL */
    uint16_t width, height;       /* 80  custom mode only */
    uint16_t pos_x, pos_y;        /* 84  */
    uint32_t parts;               /* 88  */
    uint16_t header_width;        /* 92  */
    uint16_t header_x, header_y;  /* 94  */
    uint16_t pad0;                /* 98  */
    uint32_t control;             /* 100 */
    void    *ime_param;           /* 104 */
    void    *webview_param;       /* 112 */
    uint32_t animation;           /* 120 */
    uint8_t  reserved[202];       /* 124 */
    uint16_t tail_pad;            /* 326 */
} evo_wbd_param_t;

typedef struct {
    int32_t  result;
    int32_t  pad0;
    void    *callback_result;     /* 8 */
    uint8_t  reserved[240];
} evo_wbd_result_t;

_Static_assert(sizeof(evo_cdlg_base_t) == 48, "CommonDialogBaseParam");
_Static_assert(__builtin_offsetof(evo_wbd_param_t, callback_init) == 72, "CallbackInitParam @72");
_Static_assert(__builtin_offsetof(evo_wbd_param_t, control) == 100, "Control @100");
_Static_assert(__builtin_offsetof(evo_wbd_param_t, animation) == 120, "Animation @120");
_Static_assert(sizeof(evo_wbd_param_t) == 328, "WebBrowserDialogParam");
_Static_assert(sizeof(evo_wbd_result_t) == 256, "WebBrowserDialogResult");

extern int sceWebBrowserDialogInitialize(void);
extern int sceWebBrowserDialogOpen(evo_wbd_param_t *param);
extern int sceWebBrowserDialogUpdateStatus(void);
extern int sceWebBrowserDialogGetResult(evo_wbd_result_t *result);
extern int sceWebBrowserDialogClose(void);
extern int sceWebBrowserDialogTerminate(void);

extern int sceSysmoduleLoadModule(uint16_t id);
extern int sceCommonDialogInitialize(void);
extern int sceUserServiceGetInitialUser(int *user_id);
extern int sceUserServiceGetLoginUserIdList(int *user_ids);

/* ---- layouts (run 3) ----
 * Coordinates are in the 1920x1080 UI space; EVO's nav rail is 108 wide
 * (navbar.rcss), so a rect starting at x=108 leaves it visible. The Parts /
 * Control bit values are undocumented; 1-3 open, 0xFF/0xFF is refused. */
typedef struct {
    const char *name;
    int32_t  mode;
    uint16_t x, y, w, h;
    uint32_t parts;
    uint32_t control;
} probe_layout_t;

static const probe_layout_t k_layouts[] = {
    { "1: beside the rail, parts=0 control=0",      2, 108,  0, 1812, 1080, 0x0, 0x0 },
    { "2: beside the rail, parts=0x7 control=0",    2, 108,  0, 1812, 1080, 0x7, 0x0 },
    { "3: inset card, parts=0 control=0x1",         2, 148, 40, 1732, 1000, 0x0, 0x1 },
    { "4: inset card, parts=0xFF control=0xFF",     2, 148, 40, 1732, 1000, 0xFF, 0xFF },
};
#define LAYOUT_COUNT ((int)(sizeof k_layouts / sizeof k_layouts[0]))

/* ---- state ---- */

enum probe_state {
    P_OFF,          /* no trigger, finished, or the base path refused */
    P_WAIT,         /* counting frames before (re)opening */
    P_RUN,          /* dialog up */
    P_CLOSING,      /* handoff received, EVO closed the dialog, waiting for it */
    P_PLAYING,      /* dialog gone, EVO's player has the stream */
};

#define OPEN_DELAY_FRAMES   120      /* ~2 s at 60 fps */
#define RUN_LOG_EVERY       1800     /* heartbeat while the dialog is up */
#define DEFAULT_PORT        8686

static int  s_preload_rc = -1;       /* sysmodule load */
static int  s_init_done  = 0;        /* sceWebBrowserDialogInitialize ok */
static int  s_checked    = 0;        /* trigger file read */
static enum probe_state s_state = P_OFF;
static int  s_frames     = 0;
static int  s_last_status = -1;
static int  s_closed_by_evo = 0;
static int  s_port       = DEFAULT_PORT;
static int  s_layout     = -1;       /* fixed layout index, -1 = default mode */
static int  s_cycle      = 0;        /* run 3: cycle k_layouts */
static int  s_hold_frames = 25 * 60;

static char s_url[1024];
static char s_hook_profile[16];      /* "" = media server, "nuvio" */
static char s_open_url[2048];        /* what the dialog opens first */
static char s_reopen_url[2048];      /* where it reopens after playback */

/* The stream EVO's player should start, handed to Application by
 * evo_webui_take_play() once the dialog is fully closed. */
static int  s_play_pending = 0;
static char s_play_url[2048];
static char s_play_title[256];

/* Fixed address for the whole dialog: the magic is derived from the block's
 * address, and the url pointer must stay valid while the browser runs. */
static evo_wbd_param_t s_param __attribute__((aligned(16)));

#define LOG(...) do { evo_bt("web: " __VA_ARGS__); evo_boot_log_flush(); } while (0)

/* ---- shared with the server threads (s_mx) ---- */

typedef struct {
    int  enabled;
    char host[128];                  /* a name or an IPv4 address */
    int  port;
    int  tls;                        /* https upstream */
    char base[176];                  /* scheme://host[:port], no trailing slash */
} upstream_t;

static upstream_t      s_up;
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static int  s_listen_fd  = -1;
static volatile int s_srv_stop = 0;
static int  s_handoff_ready = 0;
static int  s_close_req = 0;         /* the page's "Back to EVO" button */
static char s_h_url[2048];
static char s_h_title[256];
static char s_h_return[2048];

/*
 * The page's localStorage, kept by EVO. The PS5's browser dialog starts every
 * opening with empty storage (hardware, 2026-09-26: an addon saved in Nuvio
 * was gone the next time it opened), so a site would forget its sign-in and
 * settings each time. The hook restores this copy before the site's own code
 * runs and posts it back as it changes. One file per upstream site: every web
 * provider shares the browser origin 127.0.0.1:<port>, and Emby's and
 * Jellyfin's web clients even use the same keys.
 */
#define STORE_MAX (8u << 20)
static char            s_store_path[512];   /* set on the main thread at open */
static pthread_mutex_t s_store_mx = PTHREAD_MUTEX_INITIALIZER;

/* evo_boot_log is not thread-safe: server threads queue lines here and the
 * main thread writes them out in evo_webui_pump(). */
#define TLOG_LINES 64
static char s_tlog[TLOG_LINES][240];
static int  s_tlog_n = 0;
static int  s_tlog_dropped = 0;

static void tlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tlog(const char *fmt, ...)
{
    char line[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&s_mx);
    if (s_tlog_n < TLOG_LINES)
        memcpy(s_tlog[s_tlog_n++], line, sizeof line);
    else
        s_tlog_dropped++;
    pthread_mutex_unlock(&s_mx);
}

static void tlog_drain(void)
{
    char lines[TLOG_LINES][240];
    int n, dropped;
    pthread_mutex_lock(&s_mx);
    n = s_tlog_n;
    dropped = s_tlog_dropped;
    memcpy(lines, s_tlog, sizeof(lines[0]) * (size_t)n);
    s_tlog_n = 0;
    s_tlog_dropped = 0;
    pthread_mutex_unlock(&s_mx);
    for (int i = 0; i < n; i++)
        LOG("%s", lines[i]);
    if (dropped)
        LOG("(%d server log lines dropped)", dropped);
}

/* ---- libc stand-ins ----
 * Only libc calls EVO already makes elsewhere: the native-app libc does not
 * export everything a FreeBSD libc does, and a missing one is a call through
 * a null import - a SIGSEGV at addr 0 (strcasestr did exactly that on run 4;
 * evo_net.c carries its own copy for the same reason). */

static char *ci_strstr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n))
            return (char *)hay;
    return NULL;
}

static long parse_long(const char *s)
{
    long v = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

/* ---- trigger, user ---- */

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

static int parse_upstream(const char *v)
{
    if (evo_provider_parse_web_source(v, s_up.host, sizeof s_up.host,
                                      &s_up.port, &s_up.tls, 80) != 0)
        return -1;
    int dflt = s_up.tls ? 443 : 80;
    if (s_up.port == dflt)
        snprintf(s_up.base, sizeof s_up.base, "%s://%s", s_up.tls ? "https" : "http", s_up.host);
    else
        snprintf(s_up.base, sizeof s_up.base, "%s://%s:%d", s_up.tls ? "https" : "http",
                 s_up.host, s_up.port);
    s_up.enabled = 1;
    return 0;
}

static int read_trigger(void)
{
    FILE *f = fopen("/mnt/usb0/evo_web_probe", "r");
    if (!f) return 0;
    char line[1024];
    int first = 1;
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == '#') { first = 0; continue; }
        int v = 0;
        if (!strncmp(line, "url=", 4))
            snprintf(s_url, sizeof s_url, "%s", line + 4);
        else if (!strncmp(line, "proxy=", 6)) {
            if (parse_upstream(line + 6) != 0)
                LOG("trigger: bad proxy= '%s' (need http[s]://<host>:<port>)", line + 6);
        } else if (sscanf(line, "port=%d", &v) == 1) {
            if (v > 1024 && v < 65536) s_port = v;
        } else if (sscanf(line, "layout=%d", &v) == 1) {
            if (v >= 1 && v <= LAYOUT_COUNT) s_layout = v - 1;
        } else if (!strncmp(line, "layouts=", 8)) {
            s_cycle = line[8] == '1';
        } else if (sscanf(line, "hold=%d", &v) == 1) {
            if (v >= 5 && v <= 600) s_hold_frames = v * 60;
        } else if (first && !strchr(line, '='))
            snprintf(s_url, sizeof s_url, "%s", line);
        first = 0;
    }
    fclose(f);
    if (!s_url[0])
        snprintf(s_url, sizeof s_url, "%s", s_up.enabled ? "/web/index.html" : "");
    if (!s_url[0]) return 0;
    if (s_url[0] == '/')
        snprintf(s_open_url, sizeof s_open_url, "http://127.0.0.1:%d%s", s_port, s_url);
    else if (s_up.enabled)
        snprintf(s_open_url, sizeof s_open_url, "%s", s_url);
    else    /* runs 1-3: tell a plain page where EVO listens */
        snprintf(s_open_url, sizeof s_open_url, "%s%cevo=http://127.0.0.1:%d",
                 s_url, strchr(s_url, '?') ? '&' : '?', s_port);
    return 1;
}

static int pick_user(void)
{
    int uid = -1;
    int rc = sceUserServiceGetInitialUser(&uid);
    if (rc == 0 && uid != -1 && uid != 0xFF)
        return uid;
    LOG("sceUserServiceGetInitialUser -> 0x%08x uid=0x%08x", (unsigned)rc, (unsigned)uid);
    int list[4] = { -1, -1, -1, -1 };
    rc = sceUserServiceGetLoginUserIdList(list);
    LOG("sceUserServiceGetLoginUserIdList -> 0x%08x [0x%08x ...]", (unsigned)rc, (unsigned)list[0]);
    return list[0];
}

static int ensure_initialized(void)
{
    if (s_init_done) return 0;
    int rc = sceWebBrowserDialogInitialize();
    LOG("sceWebBrowserDialogInitialize (post-unjail) -> 0x%08x", (unsigned)rc);
    if (rc == 0) s_init_done = 1;
    return rc;
}

/* ---- the injected hook ----
 * Runs before the site's own scripts. Any media source the site sets that
 * looks like a stream (Emby/Jellyfin shapes: /Videos/<id>/stream..., master /
 * main .m3u8, /Audio/<id>/universal) goes to EVO instead. Written without
 * double quotes or backslashes so it can sit in a C string as is. */
static const char k_hook_js[] =
"(function(){"
"if(window.__evoHook)return;window.__evoHook=1;var sent=0;"
/* Profile from our own <script src>: "" = media server (Emby/Jellyfin),
 * "nuvio" = Nuvio. Nuvio plays addon/debrid links from any host, so every
 * source counts - but only on its player element (<video id=videoPlayer>
 * inside #player): the home screen's trailers set a src too. It also asks
 * canPlayType before it will try a source, and the PS5's WebKit says no to
 * MKV/HEVC/DTS - EVO's player decides that, so the answer is always yes. */
"var NUVIO=String((document.currentScript&&document.currentScript.src)||'').indexOf('p=nuvio')>=0;"
/* Storage: restore EVO's saved copy once per dialog session (sessionStorage
 * survives reloads within one opening, and a reload must not roll back what
 * the page changed since the last save), before the site reads anything. Then
 * save whenever it changes: every 5 s, synchronously before a handoff or Back
 * to EVO, and by beacon when the page goes away. Values over 512 KB are
 * caches, not settings, and are left out. */
"var lastSnap='';"
"function snap(){var o={};try{for(var i=0;i<localStorage.length;i++){var k=localStorage.key(i),v=localStorage.getItem(k);"
" if(v!==null&&v.length<=524288)o[k]=v;}}catch(e){}return JSON.stringify(o);}"
"try{if(!sessionStorage.getItem('__evoRestored')){var sx=new XMLHttpRequest();sx.open('GET','/evo/storage',false);sx.send(null);"
" if(sx.status===200){var so=JSON.parse(sx.responseText||'{}');localStorage.clear();"
"  for(var sk in so){if(Object.prototype.hasOwnProperty.call(so,sk))localStorage.setItem(sk,so[sk]);}"
"  sessionStorage.setItem('__evoRestored','1');}}}catch(e){}"
"lastSnap=snap();"
"function saveNow(kind){try{var sv=snap();if(sv===lastSnap)return;lastSnap=sv;"
" if(kind===2&&navigator.sendBeacon){navigator.sendBeacon('/evo/storage',sv);return;}"
" var x=new XMLHttpRequest();x.open('POST','/evo/storage',kind!==1);x.setRequestHeader('Content-Type','application/json');x.send(sv);}catch(e){}}"
"setInterval(function(){saveNow(0);},5000);"
"window.addEventListener('pagehide',function(){saveNow(2);});"
"document.addEventListener('visibilitychange',function(){if(document.hidden)saveNow(2);});"
"var lastState=null;"
/* Diagnostics into evo.log ("web: page: ..."): the keys the controller really
 * sends, the site's route changes, clicks and script errors - the only view of
 * a page running in the console's browser. Capped per page. */
"var logN=0;function evoLog(m){if(logN++>300)return;try{var x=new XMLHttpRequest();"
" x.open('GET','/evo/log?m='+encodeURIComponent(String(m).substring(0,200)),true);x.send(null);}catch(e){}}"
"window.addEventListener('error',function(e){evoLog('js error: '+e.message+' @'+String(e.filename||'').split('/').pop()+':'+e.lineno);});"
"window.addEventListener('unhandledrejection',function(e){var r=e.reason;evoLog('rejection: '+(r&&(r.message||r)));});"
"document.addEventListener('keydown',function(e){evoLog('key '+e.key+' code='+e.keyCode);},true);"
"function descEl(t){if(!t||!t.tagName)return '?';var c=String(t.className&&t.className.baseVal!==undefined?t.className.baseVal:t.className||'');"
" return t.tagName.toLowerCase()+(t.id?'#'+t.id:'')+(c?'.'+c.split(' ').slice(0,2).join('.'):'');}"
"document.addEventListener('click',function(e){evoLog('click '+descEl(e.target));},true);"
"function playerShown(){var p=document.getElementById('player');return !!(p&&p.style.display==='block');}"
"function okEl(el){if(!NUVIO)return 1;return !!(el&&(el.id==='videoPlayer'||(el.closest&&el.closest('#player'))));}"
"if(NUVIO){HTMLMediaElement.prototype.canPlayType=function(){return 'probably';};}"
/* Nuvio's hold-able buttons (Play, add-to-library, episode / season / more-
 * like cards) start a press-and-hold timer on activation and only act on the
 * key-up. A click - which is what the PS5 browser's X is - never sends one,
 * so Play did nothing (reproduced in desktop Chromium too; Enter works). On
 * those, turn the click into Enter down + up on the focused element. */
"var nvTitle='';function nvNoteTitle(){var tt=document.querySelector('#stream .stream-route-title'),"
" lg=document.querySelector('#stream .stream-route-logo'),ep=document.querySelector('#stream .stream-route-episode-code');"
" var t=String((tt&&tt.textContent)||(lg&&lg.getAttribute('alt'))||'').trim();"
" if(t){if(ep&&ep.textContent)t=t+' - '+String(ep.textContent).trim();nvTitle=t;}}"
/* Picking a stream (click or Enter) is the last moment the stream screen,
 * and so the title, is on the page: Nuvio clears it while the player mounts. */
"if(NUVIO){document.addEventListener('keydown',function(e){if(e.keyCode===13)nvNoteTitle();},true);}"
"if(NUVIO){document.addEventListener('click',function(e){nvNoteTitle();"
" var t=e.target&&e.target.closest?e.target.closest('[data-action=playDefault],[data-action=toggleLibrary],"
".series-episode-card,.series-season-btn,.detail-morelike-card,.stream-route-card'):null;"
" if(!t)return;e.preventDefault();e.stopImmediatePropagation();"
" function k(ty){var ev=new KeyboardEvent(ty,{key:'Enter',code:'Enter',bubbles:true,cancelable:true});"
"  try{Object.defineProperty(ev,'keyCode',{get:function(){return 13;}});"
"   Object.defineProperty(ev,'which',{get:function(){return 13;}});}catch(x){}"
"  (document.activeElement||document.body).dispatchEvent(ev);}"
" k('keydown');setTimeout(function(){k('keyup');},60);},true);}"
/* Where to come back to: the last page that is not the site's own player.
 * By the time the hook fires, the site has already routed to its player
 * (Emby: #!/videoosd/...), and reopening THAT leaves an empty player with no
 * history to go back through. */
"var good=location.href;"
/* Emby: #!/videoosd/..., #!/nowplaying. Jellyfin: #/video, #/queue - matched
 * whole, so #/videos... (a library page) still counts as a place to return to. */
"function isPlayerUrl(h){h=String(h).toLowerCase();"
" if(h.indexOf('videoosd')>=0||h.indexOf('nowplaying')>=0)return 1;"
" var r=['#/video','#!/video','#/queue','#!/queue'];"
" for(var k=0;k<r.length;k++){var i=h.indexOf(r[k]);if(i>=0){var c=h.charAt(i+r[k].length);"
"  if(c===''||c==='?')return 1;}}"
" return 0;}"
"function note(){if(!isPlayerUrl(location.href))good=location.href;}"
"window.addEventListener('hashchange',note);window.addEventListener('popstate',note);"
"['pushState','replaceState'].forEach(function(k){var o=history[k];"
" history[k]=function(s){if(s&&typeof s==='object'){lastState=s;if(s.route)evoLog('route '+s.route);}var r=o.apply(this,arguments);note();return r;};});"
"function isStream(u){var l=String(u||'').toLowerCase();"
" if(l.indexOf('blob:')==0||l.indexOf('data:')==0)return 0;"
" if(NUVIO)return l.indexOf('http')==0||l.indexOf('/')==0;"
" if(l.indexOf('/subtitles/')>=0||l.indexOf('/images/')>=0||l.indexOf('/attachments/')>=0)return 0;"
" if(l.indexOf('/videos/')<0&&l.indexOf('/audio/')<0)return 0;"
" return l.indexOf('/stream')>=0||l.indexOf('master.m3u8')>=0||l.indexOf('main.m3u8')>=0||"
"  l.indexOf('/universal')>=0||l.indexOf('live.m3u8')>=0;}"
"function itemId(u){var l=String(u).toLowerCase();var i=l.indexOf('/videos/');"
" if(i<0)i=l.indexOf('/audio/');if(i<0)return '';var p=String(u).substring(i+1).split('/');return p[1]||'';}"
"function handoff(u){evoLog('handoff '+String(u).substring(0,150));if(sent)return 1;sent=1;setTimeout(function(){sent=0;},30000);"
" var abs=new URL(u,location.href).href,id=itemId(abs),done=0;"
" function go(t){if(done)return;done=1;saveNow(1);"
"  fetch('/evo/play?url='+encodeURIComponent(abs)+'&title='+encodeURIComponent(t||'')+"
"   '&return='+encodeURIComponent(good));}"
" if(NUVIO){var pr=(lastState&&lastState.params)||{},ks=['title','name','metaTitle','itemTitle','showTitle'],t='';"
"  for(var q=0;q<ks.length&&!t;q++){if(typeof pr[ks[q]]==='string')t=pr[ks[q]];}"
/* The stream screen, still in the DOM under the player, has it in text. */
"  nvNoteTitle();if(nvTitle)t=nvTitle;"
"  go(t||document.title);return 1;}"
" setTimeout(function(){go(document.title);},1500);"
" try{var ac=window.ApiClient;if(ac&&id){ac.getItem(ac.getCurrentUserId(),id).then(function(it){"
"  var n=(it&&it.Name)||'';if(it&&it.SeriesName)n=it.SeriesName+' - '+n;go(n);},"
"  function(){go(document.title);});}}catch(e){}"
" return 1;}"
"var d=Object.getOwnPropertyDescriptor(HTMLMediaElement.prototype,'src');"
"if(d&&d.set)Object.defineProperty(HTMLMediaElement.prototype,'src',{configurable:true,get:d.get,"
" set:function(v){if(okEl(this)&&isStream(v)&&handoff(v))return;d.set.call(this,v);}});"
"var sd=Object.getOwnPropertyDescriptor(HTMLSourceElement.prototype,'src');"
"if(sd&&sd.set)Object.defineProperty(HTMLSourceElement.prototype,'src',{configurable:true,get:sd.get,"
" set:function(v){if(okEl(this.parentNode)&&isStream(v)&&handoff(v))return;sd.set.call(this,v);}});"
"var sa=Element.prototype.setAttribute;Element.prototype.setAttribute=function(n,v){"
" if(String(n).toLowerCase()=='src'&&(this instanceof HTMLMediaElement||this instanceof HTMLSourceElement)"
"  &&okEl(this instanceof HTMLSourceElement?this.parentNode:this)&&isStream(v)&&handoff(v))return;return sa.apply(this,arguments);};"
"function isManifest(u){var l=String(u||'').toLowerCase();"
" if(NUVIO)return playerShown()&&(l.indexOf('.m3u8')>=0||l.indexOf('.mpd')>=0);"
" return isStream(u)&&l.indexOf('.m3u8')>=0;}"
"var xo=XMLHttpRequest.prototype.open;XMLHttpRequest.prototype.open=function(m,u){"
" if(isManifest(u))handoff(u);return xo.apply(this,arguments);};"
"if(window.fetch){var fo=window.fetch;window.fetch=function(i){var u=(typeof i==='string')?i:(i&&i.url);"
" if(isManifest(u))handoff(u);return fo.apply(this,arguments);};}"
/* Catch-all: whatever path set the source (innerHTML, a <source> child, a
 * cloned element...), every media element fires loadstart and play. Media
 * events do not bubble, but they do go through the capture phase. */
"function grab(e){var el=e.target;if(!(el instanceof HTMLMediaElement))return;"
" evoLog(e.type+' on '+descEl(el)+' src='+String(el.currentSrc||el.getAttribute('src')||'').substring(0,120)+' ok='+okEl(el));"
" if(!okEl(el))return;"
" var u=el.currentSrc||el.getAttribute('src')||'';"
" if(!u){var s=el.querySelector('source');u=s?s.getAttribute('src')||'':'';}"
" if(isStream(u)&&handoff(u)){try{el.pause();el.removeAttribute('src');el.load();}catch(x){}}}"
"document.addEventListener('loadstart',grab,true);document.addEventListener('play',grab,true);"
/* The way out. The embedded layout switches the browser's own controls off,
 * and the only other exit is the PS button - the close path that has
 * panicked the console. Re-added on a timer in case the site rebuilds the
 * page body. */
"function addClose(){if(!document.body||document.getElementById('__evoClose'))return;"
" var b=document.createElement('div');b.id='__evoClose';b.textContent='Back to EVO';"
" b.setAttribute('style','position:fixed;right:28px;bottom:28px;z-index:2147483647;padding:14px 30px;"
"border-radius:30px;background:rgba(0,0,0,.75);color:#fff;font:24px sans-serif;cursor:pointer;"
"border:2px solid rgba(255,255,255,.55)');"
" b.onclick=function(){saveNow(1);fetch('/evo/close');};document.body.appendChild(b);}"
"setInterval(addClose,2000);"
"if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',addClose);else addClose();"
"})();";

/* The tag injected after <head>; the profile rides in the query string, where
 * the hook reads it back from document.currentScript. */
static size_t hook_tag(char *out, size_t cap)
{
    int n = snprintf(out, cap, "<script src=\"/evo/hook.js%s%s\"></script>",
                     s_hook_profile[0] ? "?p=" : "", s_hook_profile);
    return n > 0 ? (size_t)n : 0;
}

/* ---- HTTP plumbing ---- */

static void set_timeouts(int fd, int secs)
{
    struct timeval tv = { secs, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

static int send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* ---- the upstream connection ----
 * Plain TCP or TLS, resolved per connection with getaddrinfo - the same calls
 * evo_net.c makes for IPTV's https playlists, so nothing new is imported from
 * the native-app libc. No certificate verification, as in evo_net.c. */

typedef struct {
    int  fd;
    SSL *ssl;
} uconn_t;

static SSL_CTX        *s_ssl_ctx = NULL;
static pthread_mutex_t s_ssl_mx = PTHREAD_MUTEX_INITIALIZER;

static SSL_CTX *ssl_ctx(void)
{
    pthread_mutex_lock(&s_ssl_mx);
    if (!s_ssl_ctx) {
        SSL_library_init();
        SSL_load_error_strings();
        s_ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (s_ssl_ctx) SSL_CTX_set_mode(s_ssl_ctx, SSL_MODE_AUTO_RETRY);
    }
    pthread_mutex_unlock(&s_ssl_mx);
    return s_ssl_ctx;
}

static void u_close(uconn_t *u)
{
    if (u->ssl) { SSL_shutdown(u->ssl); SSL_free(u->ssl); u->ssl = NULL; }
    if (u->fd >= 0) { close(u->fd); u->fd = -1; }
}

/* 0 on success; on failure *why says which step. */
static int u_open(uconn_t *u, const char **why)
{
    u->fd = -1;
    u->ssl = NULL;
    char port[8];
    snprintf(port, sizeof port, "%d", s_up.port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(s_up.host, port, &hints, &res) != 0 || !res) { *why = "dns"; return -1; }
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        int fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        set_timeouts(fd, 30);
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) { u->fd = fd; break; }
        close(fd);
    }
    freeaddrinfo(res);
    if (u->fd < 0) { *why = "connect"; return -1; }
    if (s_up.tls) {
        SSL_CTX *ctx = ssl_ctx();
        u->ssl = ctx ? SSL_new(ctx) : NULL;
        if (!u->ssl) { *why = "tls setup"; u_close(u); return -1; }
        SSL_set_tlsext_host_name(u->ssl, s_up.host);
        SSL_set_fd(u->ssl, u->fd);
        if (SSL_connect(u->ssl) <= 0) { *why = "tls handshake"; u_close(u); return -1; }
    }
    return 0;
}

static ssize_t u_recv(uconn_t *u, char *buf, size_t n)
{
    if (u->ssl) {
        int r = SSL_read(u->ssl, buf, n > 0x7fffffff ? 0x7fffffff : (int)n);
        return r > 0 ? r : -1;
    }
    return recv(u->fd, buf, n, 0);
}

static int u_send_all(uconn_t *u, const char *p, size_t n)
{
    if (!u->ssl) return send_all(u->fd, p, n);
    while (n) {
        int w = SSL_write(u->ssl, p, n > 0x7fffffff ? 0x7fffffff : (int)n);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* read_head for the upstream side. */
static int u_read_head(uconn_t *u, char *buf, size_t cap, size_t *got, size_t *head)
{
    *got = 0;
    while (*got < cap - 1) {
        ssize_t r = u_recv(u, buf + *got, cap - 1 - *got);
        if (r <= 0) return -1;
        *got += (size_t)r;
        buf[*got] = 0;
        char *e = strstr(buf, "\r\n\r\n");
        if (e) { *head = (size_t)(e - buf) + 4; return 0; }
    }
    return -1;
}

/* Read until the end of the header block. *got = bytes in buf, *head = offset
 * of the first body byte. 0 on success. */
static int read_head(int fd, char *buf, size_t cap, size_t *got, size_t *head)
{
    *got = 0;
    while (*got < cap - 1) {
        ssize_t r = recv(fd, buf + *got, cap - 1 - *got, 0);
        if (r <= 0) return -1;
        *got += (size_t)r;
        buf[*got] = 0;
        char *e = strstr(buf, "\r\n\r\n");
        if (e) { *head = (size_t)(e - buf) + 4; return 0; }
    }
    return -1;
}

/* Value of header `name` inside the head [buf, buf+hl), copied to out. */
static int header_value(const char *buf, size_t hl, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    const char *p = strstr(buf, "\r\n");
    while (p && (size_t)(p - buf) + 2 < hl) {
        p += 2;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ') v++;
            const char *e = strstr(v, "\r\n");
            size_t l = e ? (size_t)(e - v) : strlen(v);
            if (l >= cap) l = cap - 1;
            memcpy(out, v, l);
            out[l] = 0;
            return 1;
        }
        p = strstr(p, "\r\n");
    }
    return 0;
}

static void respond(int fd, const char *status, const char *type, const char *body, size_t len)
{
    char head[512];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n", status, type, len);
    if (send_all(fd, head, (size_t)n) == 0 && len)
        send_all(fd, body, len);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Query parameter `key` from path, URL-decoded into out. */
static int query_param(const char *path, const char *key, char *out, size_t cap)
{
    const char *q = strchr(path, '?');
    size_t kl = strlen(key);
    out[0] = 0;
    while (q) {
        q++;
        if (!strncmp(q, key, kl) && q[kl] == '=') {
            const char *v = q + kl + 1;
            size_t o = 0;
            while (*v && *v != '&' && o + 1 < cap) {
                if (*v == '%' && hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
                    out[o++] = (char)(hexval(v[1]) * 16 + hexval(v[2]));
                    v += 3;
                } else {
                    out[o++] = (*v == '+') ? ' ' : *v;
                    v++;
                }
            }
            out[o] = 0;
            return 1;
        }
        q = strchr(q, '&');
    }
    return 0;
}

/* A URL on our own origin, rewritten onto the upstream, so the player talks
 * to the media server directly instead of through the proxy. */
static void to_upstream(const char *in, char *out, size_t cap)
{
    char local[64];
    int ll = snprintf(local, sizeof local, "http://127.0.0.1:%d", s_port);
    if (s_up.enabled && !strncmp(in, local, (size_t)ll))
        snprintf(out, cap, "%s%s", s_up.base, in + ll);
    else
        snprintf(out, cap, "%s", in);
}

static void serve_evo(int fd, const char *path)
{
    if (!strncmp(path, "/evo/hook.js", 12)) {
        respond(fd, "200 OK", "application/javascript", k_hook_js, sizeof k_hook_js - 1);
    } else if (!strncmp(path, "/evo/close", 10)) {
        tlog("close requested by the page");
        pthread_mutex_lock(&s_mx);
        s_close_req = 1;
        pthread_mutex_unlock(&s_mx);
        respond(fd, "204 No Content", "text/plain", "", 0);
    } else if (!strncmp(path, "/evo/log", 8)) {
        char m[240];
        query_param(path, "m", m, sizeof m);
        tlog("page: %s", m);
        respond(fd, "204 No Content", "text/plain", "", 0);
    } else if (!strncmp(path, "/evo/ping", 9) || !strncmp(path, "/ping", 5)) {
        respond(fd, "200 OK", "application/json", "{\"evo\":\"pong\"}", 14);
    } else if (!strncmp(path, "/evo/play", 9) || !strncmp(path, "/play", 5)) {
        char raw[2048], url[2048], title[256], ret[2048];
        query_param(path, "url", raw, sizeof raw);
        query_param(path, "title", title, sizeof title);
        query_param(path, "return", ret, sizeof ret);
        to_upstream(raw, url, sizeof url);
        tlog("handoff: title='%s'", title);
        tlog("handoff: url=%.200s", url);
        tlog("handoff: return=%.200s", ret);
        pthread_mutex_lock(&s_mx);
        snprintf(s_h_url, sizeof s_h_url, "%s", url);
        snprintf(s_h_title, sizeof s_h_title, "%s", title);
        snprintf(s_h_return, sizeof s_h_return, "%s", ret);
        s_handoff_ready = url[0] != 0;
        pthread_mutex_unlock(&s_mx);
        static const char body[] =
            "<!doctype html><html><body style=\"background:#101418;color:#6fdc8c;"
            "font:40px sans-serif;margin:64px\"><h1>Playing in EVO...</h1></body></html>";
        respond(fd, "200 OK", "text/html; charset=utf-8", body, sizeof body - 1);
    } else {
        respond(fd, "404 Not Found", "text/plain", "not found", 9);
    }
}

/* GET /evo/storage: the saved copy ("{}" when there is none). POST: replace
 * it with the request body. Written in place - the app module has no rename()
 * guarantee - under s_store_mx; a torn file only costs a JSON parse failure,
 * which the hook ignores. */
static void serve_storage(int fd, const char *method, char *req, size_t got, size_t hl)
{
    char path[512];
    pthread_mutex_lock(&s_store_mx);
    snprintf(path, sizeof path, "%s", s_store_path);
    pthread_mutex_unlock(&s_store_mx);
    if (!path[0]) {
        respond(fd, "200 OK", "application/json", "{}", 2);
        return;
    }

    if (!strcmp(method, "POST")) {
        char cl[32];
        long n = header_value(req, hl, "Content-Length", cl, sizeof cl) ? parse_long(cl) : -1;
        if (n < 0 || n > (long)STORE_MAX) {
            respond(fd, "413 Payload Too Large", "text/plain", "too large", 9);
            return;
        }
        char *b = malloc((size_t)n + 1);
        if (!b) { respond(fd, "500 Internal Server Error", "text/plain", "oom", 3); return; }
        size_t have = got - hl;
        if (have > (size_t)n) have = (size_t)n;
        memcpy(b, req + hl, have);
        while (have < (size_t)n) {
            ssize_t r = recv(fd, b + have, (size_t)n - have, 0);
            if (r <= 0) break;
            have += (size_t)r;
        }
        int ok = 0;
        if (have == (size_t)n) {
            pthread_mutex_lock(&s_store_mx);
            FILE *f = fopen(path, "wb");
            if (f) {
                ok = fwrite(b, 1, have, f) == have;
                fclose(f);
            }
            pthread_mutex_unlock(&s_store_mx);
        }
        free(b);
        tlog("storage: saved %ld bytes -> %s", n, ok ? "ok" : "FAILED");
        respond(fd, ok ? "204 No Content" : "500 Internal Server Error", "text/plain", "", 0);
        return;
    }

    pthread_mutex_lock(&s_store_mx);
    FILE *f = fopen(path, "rb");
    char *b = NULL;
    size_t n = 0;
    if (f) {
        size_t cap = 65536;
        b = malloc(cap);
        while (b) {
            if (n == cap) {
                if (cap >= STORE_MAX) break;
                char *nb = realloc(b, cap * 2);
                if (!nb) break;
                b = nb;
                cap *= 2;
            }
            size_t r = fread(b + n, 1, cap - n, f);
            if (r == 0) break;
            n += r;
        }
        fclose(f);
    }
    pthread_mutex_unlock(&s_store_mx);
    if (b && n) {
        respond(fd, "200 OK", "application/json", b, n);
        tlog("storage: restored %zu bytes", n);
    } else {
        respond(fd, "200 OK", "application/json", "{}", 2);
    }
    free(b);
}

static int is_html_entry(const char *path)
{
    size_t n = strcspn(path, "?#");
    if (n == 1 && path[0] == '/') return 1;
    if (n == 5 && !strncmp(path, "/web/", 5)) return 1;
    return n >= 10 && !strncmp(path + n - 10, "index.html", 10);
}

/* Undo Transfer-Encoding: chunked in place. Returns the new length. */
static size_t dechunk(char *b, size_t n)
{
    size_t r = 0, w = 0;
    while (r < n) {
        size_t len = 0;
        int h;
        while (r < n && (h = hexval((unsigned char)b[r])) >= 0) { len = len * 16 + (size_t)h; r++; }
        char *eol = memchr(b + r, '\n', n - r);
        if (!eol) break;
        r = (size_t)(eol - b) + 1;
        if (len == 0 || r + len > n) break;
        memmove(b + w, b + r, len);
        w += len;
        r += len + 2;          /* chunk data + CRLF */
    }
    return w;
}

static void proxy_request(int cfd, char *req, size_t got, size_t hl,
                          const char *method, const char *path)
{
    uconn_t u;
    const char *why = "";
    if (u_open(&u, &why) != 0) {
        tlog("proxy: %s to %s failed errno=%d (%.100s)", why, s_up.base, errno, path);
        respond(cfd, "502 Bad Gateway", "text/plain", "upstream unreachable", 20);
        return;
    }

    /* Request: HTTP/1.0 + Connection: close + identity encoding, so the
     * response is a plain byte stream ending at EOF (no keep-alive, no gzip,
     * normally no chunking) that can be relayed or rewritten simply. */
    size_t cap = hl + 512;
    char *out = malloc(cap);
    if (!out) { u_close(&u); return; }
    /* Host carries the port only when it is not the scheme's default: a
     * reverse proxy matches its virtual host on the bare name. */
    char hosthdr[160];
    if (s_up.port == (s_up.tls ? 443 : 80))
        snprintf(hosthdr, sizeof hosthdr, "%s", s_up.host);
    else
        snprintf(hosthdr, sizeof hosthdr, "%s:%d", s_up.host, s_up.port);
    size_t o = (size_t)snprintf(out, cap, "%s %s HTTP/1.0\r\nHost: %s\r\n"
                                "Connection: close\r\nAccept-Encoding: identity\r\n",
                                method, path, hosthdr);
    const char *p = strstr(req, "\r\n") + 2;
    while ((size_t)(p - req) < hl - 2) {
        const char *e = strstr(p, "\r\n");
        size_t l = (size_t)(e - p);
        if (strncasecmp(p, "Host:", 5) && strncasecmp(p, "Connection:", 11) &&
            strncasecmp(p, "Accept-Encoding:", 16) && strncasecmp(p, "Keep-Alive:", 11) &&
            strncasecmp(p, "Upgrade:", 8) && strncasecmp(p, "Proxy-", 6) &&
            strncasecmp(p, "TE:", 3) && o + l + 2 < cap) {
            memcpy(out + o, p, l + 2);
            o += l + 2;
        }
        p = e + 2;
    }
    memcpy(out + o, "\r\n", 2);
    o += 2;
    int ok = u_send_all(&u, out, o) == 0;
    free(out);

    /* Request body: what arrived with the head, then the rest. */
    char cl[32];
    long body = header_value(req, hl, "Content-Length", cl, sizeof cl) ? parse_long(cl) : 0;
    long have = (long)(got - hl);
    if (ok && have > 0) ok = u_send_all(&u, req + hl, (size_t)have) == 0;
    for (long left = body - have; ok && left > 0; ) {
        char tmp[16384];
        ssize_t r = recv(cfd, tmp, left < (long)sizeof tmp ? (size_t)left : sizeof tmp, 0);
        if (r <= 0) { ok = 0; break; }
        ok = u_send_all(&u, tmp, (size_t)r) == 0;
        left -= r;
    }

    /* Response head. */
    size_t rcap = 65536, rgot = 0, rhl = 0;
    char *resp = ok ? malloc(rcap) : NULL;
    if (!resp || u_read_head(&u, resp, rcap, &rgot, &rhl) != 0) {
        tlog("proxy: no response for %s %.120s", method, path);
        free(resp);
        u_close(&u);
        return;
    }
    int status = 0;
    sscanf(resp, "HTTP/%*s %d", &status);
    char ctype[128] = "", te[64] = "";
    header_value(resp, rhl, "Content-Type", ctype, sizeof ctype);
    header_value(resp, rhl, "Transfer-Encoding", te, sizeof te);
    int inject = status == 200 && is_html_entry(path) && strstr(ctype, "text/html") != NULL;
    int chunked = ci_strstr(te, "chunked") != NULL;

    /* Rewrite the head: drop hop-by-hop headers, point absolute redirects back
     * at the proxy, and - when the body is being rewritten - drop the length
     * and chunking headers, which are recomputed below. */
    char *nh = malloc(rhl + 256);
    if (!nh) { free(resp); u_close(&u); return; }
    size_t nho;
    const char *q = resp;
    const char *qe = strstr(q, "\r\n");
    memcpy(nh, q, (size_t)(qe - q) + 2);
    nho = (size_t)(qe - q) + 2;
    q = qe + 2;
    while ((size_t)(q - resp) < rhl - 2) {
        qe = strstr(q, "\r\n");
        size_t l = (size_t)(qe - q);
        int drop = !strncasecmp(q, "Connection:", 11) || !strncasecmp(q, "Keep-Alive:", 11) ||
                   (inject && (!strncasecmp(q, "Content-Length:", 15) ||
                               !strncasecmp(q, "Transfer-Encoding:", 18)));
        if (!drop && !strncasecmp(q, "Location:", 9)) {
            const char *v = q + 9;
            while (*v == ' ') v++;
            size_t bl = strlen(s_up.base);
            if (!strncmp(v, s_up.base, bl)) {
                nho += (size_t)snprintf(nh + nho, 64, "Location: http://127.0.0.1:%d", s_port);
                memcpy(nh + nho, v + bl, (size_t)(qe - v) - bl + 2);
                nho += (size_t)(qe - v) - bl + 2;
                drop = 2;
            }
        }
        if (!drop) {
            memcpy(nh + nho, q, l + 2);
            nho += l + 2;
        }
        q = qe + 2;
    }

    if (!inject) {
        memcpy(nh + nho, "Connection: close\r\n\r\n", 21);
        nho += 21;
        int sent = send_all(cfd, nh, nho) == 0 &&
                   (rgot == rhl || send_all(cfd, resp + rhl, rgot - rhl) == 0);
        while (sent) {
            ssize_t r = u_recv(&u, resp, rcap);
            if (r <= 0) break;
            sent = send_all(cfd, resp, (size_t)r) == 0;
        }
    } else {
        /* The whole page, then the hook right after <head ...>, so it runs
         * before anything the site loads. */
        size_t bcap = 8u << 20, blen = rgot - rhl;
        char *b = malloc(bcap + 1);     /* + NUL for the <head search */
        if (b) {
            memcpy(b, resp + rhl, blen);
            for (;;) {
                if (blen == bcap) break;
                ssize_t r = u_recv(&u, b + blen, bcap - blen);
                if (r <= 0) break;
                blen += (size_t)r;
            }
            if (chunked) blen = dechunk(b, blen);
            b[blen] = 0;
            char *ins = ci_strstr(b, "<head");
            ins = ins ? memchr(ins, '>', blen - (size_t)(ins - b)) : NULL;
            size_t at = ins ? (size_t)(ins - b) + 1 : 0;
            char tag[96];
            size_t tl = hook_tag(tag, sizeof tag);
            nho += (size_t)snprintf(nh + nho, 96, "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                                    blen + tl);
            if (send_all(cfd, nh, nho) == 0 && send_all(cfd, b, at) == 0 &&
                send_all(cfd, tag, tl) == 0)
                send_all(cfd, b + at, blen - at);
            tlog("proxy: injected hook into %.100s (%zu bytes%s)", path, blen,
                 chunked ? ", was chunked" : "");
            free(b);
        }
    }
    free(nh);
    free(resp);
    u_close(&u);
}

static void *conn_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    set_timeouts(fd, 30);
    size_t cap = 65536, got = 0, hl = 0;
    char *req = malloc(cap);
    if (req && read_head(fd, req, cap, &got, &hl) == 0) {
        char method[16] = "", path[4096] = "";
        sscanf(req, "%15s %4095s", method, path);
        if (!strncmp(path, "/evo/storage", 12))
            serve_storage(fd, method, req, got, hl);
        else if (!strncmp(path, "/evo/", 5) || !s_up.enabled)
            serve_evo(fd, path);
        else
            proxy_request(fd, req, got, hl, method, path);
    }
    free(req);
    close(fd);
    return NULL;
}

static void *accept_thread(void *arg)
{
    (void)arg;
    while (!s_srv_stop) {
        int c = accept(s_listen_fd, NULL, NULL);
        if (c >= 0 && s_srv_stop) { close(c); break; }
        if (c < 0) {
            if (s_srv_stop) break;
            if (errno == EINTR) continue;
            tlog("server: accept errno=%d - stopping", errno);
            break;
        }
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 256 * 1024);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &a, conn_thread, (void *)(intptr_t)c) != 0)
            close(c);
        pthread_attr_destroy(&a);
    }
    return NULL;
}

static void server_start(void)
{
    if (s_listen_fd >= 0) return;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { LOG("server: socket errno=%d", errno); return; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)s_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 16) != 0) {
        LOG("server: bind/listen 127.0.0.1:%d failed errno=%d", s_port, errno);
        close(fd);
        return;
    }
    s_listen_fd = fd;
    s_srv_stop = 0;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &at, accept_thread, NULL);
    pthread_attr_destroy(&at);
    LOG("server: 127.0.0.1:%d ready%s%s (thread rc=%d)", s_port,
        s_up.enabled ? ", proxying " : "", s_up.enabled ? s_up.base : "", rc);
}

static void server_stop(void)
{
    if (s_listen_fd < 0) return;
    s_srv_stop = 1;
    /* Wake the accept thread with a connection of our own; it sees the stop
     * flag and exits. Then the socket can go. */
    int w = socket(AF_INET, SOCK_STREAM, 0);
    if (w >= 0) {
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)s_port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connect(w, (struct sockaddr *)&a, sizeof a);
        close(w);
    }
    close(s_listen_fd);
    s_listen_fd = -1;
    LOG("server: stopped");
}

/* ---- the dialog ---- */

static void open_dialog(const char *url)
{
    int uid = pick_user();
    if (ensure_initialized() != 0) {
        LOG("NO-GO at Initialize");
        s_state = P_OFF;
        return;
    }
    server_start();
    evo_mkdir(evo_data_path("webui"));
    char leaf[200];
    snprintf(leaf, sizeof leaf, "webui/%s_%d.json", s_up.enabled ? s_up.host : "local",
             s_up.enabled ? s_up.port : s_port);
    pthread_mutex_lock(&s_store_mx);
    snprintf(s_store_path, sizeof s_store_path, "%s", evo_data_path(leaf));
    pthread_mutex_unlock(&s_store_mx);
    pthread_mutex_lock(&s_mx);
    s_close_req = 0;                     /* nothing stale from a previous page */
    s_handoff_ready = 0;
    pthread_mutex_unlock(&s_mx);
    memset(&s_param, 0, sizeof s_param);
    s_param.base.size  = sizeof s_param.base;
    s_param.base.magic = (uint32_t)(WBD_MAGIC + (uint64_t)(uintptr_t)&s_param.base);
    s_param.size       = sizeof s_param;
    s_param.mode       = 1;              /* Default */
    s_param.user_id    = uid;
    s_param.url        = url;
    if (s_layout >= 0) {
        const probe_layout_t *L = &k_layouts[s_layout];
        s_param.mode         = L->mode;
        s_param.pos_x        = L->x;
        s_param.pos_y        = L->y;
        s_param.width        = L->w;
        s_param.height       = L->h;
        s_param.parts        = L->parts;
        s_param.control      = L->control;
        s_param.header_width = L->w;
        s_param.header_x     = L->x;
        s_param.header_y     = L->y;
        LOG("layout %s", L->name);
    }
    LOG("Open url=%s ...", url);
    int rc = sceWebBrowserDialogOpen(&s_param);
    LOG("sceWebBrowserDialogOpen -> 0x%08x", (unsigned)rc);
    if (rc != 0) {
        if (s_cycle && s_layout + 1 < LAYOUT_COUNT) {
            s_layout++;
            s_state = P_WAIT;
            s_frames = 0;
            return;
        }
        server_stop();
        s_state = P_OFF;
        return;
    }
    s_state = P_RUN;
    s_frames = 0;
    s_last_status = -1;
    s_closed_by_evo = 0;
}

static void read_result(void)
{
    evo_wbd_result_t r;
    memset(&r, 0, sizeof r);
    int rc = sceWebBrowserDialogGetResult(&r);
    LOG("GetResult -> 0x%08x result=0x%08x (closed by %s)",
        (unsigned)rc, (unsigned)r.result, s_closed_by_evo ? "EVO" : "the user");
    if (!s_closed_by_evo)
        sceWebBrowserDialogClose();
}

/* The session is over (the user closed the browser). The subsystem stays
 * initialized: initializing it again after the unjail was never tried on
 * hardware, and evo_webui_shutdown() terminates it at exit anyway. */
static void finish(void)
{
    LOG("session closed by the user");
    server_stop();
    s_state = P_OFF;
}

static int dialog_gone(int st)
{
    return st == CDLG_STATUS_FINISHED || st == CDLG_STATUS_NONE ||
           st == CDLG_STATUS_INITIALIZED || st < 0;
}

/* ---- public ---- */

void evo_webui_preload(void)
{
    int rc = sceCommonDialogInitialize();
    evo_bt("web: sceCommonDialogInitialize -> 0x%08x (0x80b80002 = already up, fine)", (unsigned)rc);
    s_preload_rc = sceSysmoduleLoadModule(SCE_SYSMODULE_WEB_BROWSER_DIALOG);
    evo_bt("web: sceSysmoduleLoadModule(0xAB) -> 0x%08x", (unsigned)s_preload_rc);
    if (s_preload_rc < 0) return;
    rc = sceWebBrowserDialogInitialize();
    evo_bt("web: sceWebBrowserDialogInitialize (pre-unjail) -> 0x%08x", (unsigned)rc);
    if (rc == 0) s_init_done = 1;
}

int evo_webui_active(void)
{
    return s_state == P_RUN || s_state == P_CLOSING;
}

void evo_webui_shutdown(void)
{
    server_stop();
    if (s_init_done) {
        int rc = sceWebBrowserDialogTerminate();
        LOG("Terminate -> 0x%08x", (unsigned)rc);
        s_init_done = 0;
    }
    s_state = P_OFF;
}

int evo_webui_session_active(void)
{
    return s_state != P_OFF;
}

int evo_webui_open(const char *upstream, const char *path)
{
    return evo_webui_open_ex(upstream, path, NULL);
}

int evo_webui_open_ex(const char *upstream, const char *path, const char *hook_profile)
{
    if (s_state != P_OFF)
        return 1;
    snprintf(s_hook_profile, sizeof s_hook_profile, "%s", hook_profile ? hook_profile : "");
    if (s_preload_rc < 0) {
        LOG("open refused: the dialog module never loaded (0x%08x)", (unsigned)s_preload_rc);
        return -2;
    }
    memset(&s_up, 0, sizeof s_up);
    if (!upstream || parse_upstream(upstream) != 0) {
        LOG("open refused: '%s' is not http[s]://<host>:<port>", upstream ? upstream : "(null)");
        return -1;
    }
    snprintf(s_open_url, sizeof s_open_url, "http://127.0.0.1:%d%s", s_port,
             (path && path[0] == '/') ? path : "/");
    snprintf(s_reopen_url, sizeof s_reopen_url, "%s", s_open_url);
    s_layout = 0;                        /* beside the rail, no browser chrome */
    s_cycle = 0;
    s_checked = 1;                       /* a provider open wins over the trigger */
    LOG("open: %s via %s hook=%s", s_open_url, s_up.base,
        s_hook_profile[0] ? s_hook_profile : "media-server");
    s_state = P_WAIT;
    s_frames = OPEN_DELAY_FRAMES - 6;    /* a few frames: let the screen draw first */
    return 0;
}

int evo_webui_take_play(char *url, size_t url_cap, char *title, size_t title_cap)
{
    if (!s_play_pending) return 0;
    s_play_pending = 0;
    snprintf(url, url_cap, "%s", s_play_url);
    snprintf(title, title_cap, "%s", s_play_title);
    return 1;
}

void evo_webui_playback_ended(int played)
{
    if (s_state != P_PLAYING) return;
    LOG("playback %s - reopening %s", played ? "ended" : "failed to start", s_reopen_url);
    s_state = P_WAIT;
    s_frames = OPEN_DELAY_FRAMES / 2;    /* ~1 s: the player screen is going away */
}

void evo_webui_pump(void)
{
    if (!s_checked) {
        s_checked = 1;
        if (!read_trigger()) return;
        if (s_cycle && s_layout < 0) s_layout = 0;
        LOG("trigger: open=%s proxy=%s port=%d layout=%d cycle=%d preload=0x%08x init=%d",
            s_open_url, s_up.enabled ? s_up.base : "(none)", s_port, s_layout + 1, s_cycle,
            (unsigned)s_preload_rc, s_init_done);
        if (s_preload_rc < 0) {
            LOG("NO-GO at sceSysmoduleLoadModule(0xAB)");
            return;
        }
        snprintf(s_reopen_url, sizeof s_reopen_url, "%s", s_open_url);
        s_state = P_WAIT;
        s_frames = 0;
    }
    if (s_state == P_OFF) return;

    tlog_drain();

    switch (s_state) {
    case P_OFF:
    case P_PLAYING:
        return;
    case P_WAIT:
        if (++s_frames >= OPEN_DELAY_FRAMES) open_dialog(s_reopen_url);
        return;
    case P_RUN: {
        int st = sceWebBrowserDialogUpdateStatus();
        s_frames++;
        if (st != s_last_status) {
            LOG("status %d -> %d at frame %d", s_last_status, st, s_frames);
            s_last_status = st;
        } else if (s_frames % RUN_LOG_EVERY == 0) {
            LOG("still status %d at frame %d", st, s_frames);
        }

        int handoff = 0, close_req = 0;
        pthread_mutex_lock(&s_mx);
        close_req = s_close_req;
        s_close_req = 0;
        if (s_handoff_ready) {
            handoff = 1;
            s_handoff_ready = 0;
            snprintf(s_play_url, sizeof s_play_url, "%s", s_h_url);
            snprintf(s_play_title, sizeof s_play_title, "%s", s_h_title);
            if (s_h_return[0])
                snprintf(s_reopen_url, sizeof s_reopen_url, "%s", s_h_return);
        }
        pthread_mutex_unlock(&s_mx);

        int cycle_up = s_cycle && s_frames >= s_hold_frames;
        if ((handoff || cycle_up || close_req) && !dialog_gone(st) && !s_closed_by_evo) {
            s_closed_by_evo = 1;
            int rc = sceWebBrowserDialogClose();
            LOG("EVO closes the dialog (%s) -> 0x%08x",
                handoff ? "handoff" : close_req ? "Back to EVO" : "layout hold up", (unsigned)rc);
            s_state = handoff ? P_CLOSING : P_RUN;
            s_frames = 0;
            if (handoff) return;
        }
        if (!dialog_gone(st)) return;
        read_result();
        if (s_cycle && s_closed_by_evo && s_layout + 1 < LAYOUT_COUNT) {
            s_layout++;
            s_state = P_WAIT;
            s_frames = 0;
            return;
        }
        finish();       /* the user closed it: the probe ends */
        return;
    }
    case P_CLOSING: {
        int st = sceWebBrowserDialogUpdateStatus();
        if (!dialog_gone(st) && ++s_frames < 600) return;
        read_result();
        LOG("handing '%s' to the player", s_play_title);
        s_play_pending = 1;
        s_state = P_PLAYING;
        return;
    }
    }
}

#else
typedef int evo_webui_not_built; /* keep the TU non-empty */
#endif /* EVO_APP_MODULE */
