/* Full-length YouTube playback from a link (see evo_yt_link.h).
 *
 * Why this route: EVO's ANDROID / ANDROID_VR resolves (evo_cast_receiver.c)
 * get stream URLs that YouTube cuts at about 60 s without a PO token. The
 * web_embedded client's HLS manifest plays in full, but every segment answers
 * 403 until the `n` value in the manifest path is transformed by YouTube's
 * player JavaScript. yt-dlp's solver (yt-dlp-ejs) does that transform; EVO runs
 * it in QuickJS-NG, the same way `yt-dlp --js-runtimes quickjs` does.
 *
 * Fragility: YouTube changes the player script every few weeks. A newer
 * yt-dlp-ejs release (third_party/yt-dlp-ejs + tools/gen_ytjs_blob.py) is the
 * fix when the solver stops finding the function. If YouTube starts asking a
 * PO token for web_embedded HLS too, this route closes; the caller then falls
 * back to the 60 s resolve. */
#include "evo_yt_link.h"
#include "evo_boot_log.h"
#include "evo_data_path.h"
#include "evo_net.h"
#include "cJSON.h"
#include "quickjs.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern const unsigned char evo_ytjs_lib[];
extern const unsigned long evo_ytjs_lib_len;
extern const unsigned char evo_ytjs_core[];
extern const unsigned long evo_ytjs_core_len;

/* Same client as yt-dlp 2026.10.26's `web_embedded`. */
#define YT_EMB_CLIENT_VERSION "2.20260708.00.00"
#define YT_EMB_UA "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/15.5 Safari/605.1.15,gzip(gfe)"
#define YT_EMB_EMBED_URL "https://www.reddit.com/"
#define YT_CACHE_LEAF "ytjs_player.cache"

#define YT_JS_THREAD_STACK (4u << 20)
#define YT_JS_MAX_STACK    (3u << 20)
#define YT_JS_MEM_LIMIT    (256u << 20)
#define YT_JS_DEADLINE_MS  60000

/* ---- link parsing ---------------------------------------------------- */

static int is_id_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static int take_id(const char *p, char *out, size_t cap)
{
    if (!p || cap < 12) return -1;
    for (int i = 0; i < 11; i++)
        if (!is_id_char(p[i])) return -1;
    if (is_id_char(p[11])) return -1;
    memcpy(out, p, 11);
    out[11] = '\0';
    return 0;
}

int evo_yt_link_video_id(const char *link, char *out_id, size_t cap)
{
    if (!link || !out_id || cap < 12) return -1;
    while (*link == ' ' || *link == '\t' || *link == '\r' || *link == '\n') link++;
    if (take_id(link, out_id, cap) == 0) return 0;               /* bare id */

    const char *host = strstr(link, "youtu.be/");
    if (host && take_id(host + 9, out_id, cap) == 0) return 0;
    if (!strstr(link, "youtube.com/")) return -1;

    static const char *const keys[] = { "?v=", "&v=", "/shorts/", "/embed/", "/live/", "/v/" };
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        const char *p = strstr(link, keys[k]);
        if (p && take_id(p + strlen(keys[k]), out_id, cap) == 0) return 0;
    }
    return -1;
}

/* ---- small helpers --------------------------------------------------- */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* iframe_api names the current player as "player\/<8 hex>\/" (or unescaped). */
static int find_player_id(const char *js, char out[9])
{
    for (const char *p = strstr(js, "player"); p; p = strstr(p + 1, "player")) {
        const char *q = p + 6;
        if (*q == '\\') q++;
        if (*q != '/') continue;
        q++;
        int i = 0;
        while (i < 8 && is_hex(q[i])) i++;
        if (i != 8) continue;
        if (q[8] != '/' && q[8] != '\\') continue;
        memcpy(out, q, 8);
        out[8] = '\0';
        return 0;
    }
    return -1;
}

/* Length of the JSON object starting at p ('{'), strings and escapes
 * respected; 0 if it never closes. */
static size_t json_object_len(const char *p)
{
    int depth = 0, in_str = 0;
    for (const char *q = p; *q; q++) {
        if (in_str) {
            if (*q == '\\' && q[1]) q++;
            else if (*q == '"') in_str = 0;
            continue;
        }
        if (*q == '"') in_str = 1;
        else if (*q == '{') depth++;
        else if (*q == '}' && --depth == 0) return (size_t)(q - p) + 1;
    }
    return 0;
}

/* yt-dlp: (?:signatureTimestamp|sts)\s*:\s*(?P<sts>[0-9]{5}) */
static long find_sts(const char *js)
{
    static const char *const keys[] = { "signatureTimestamp", "sts" };
    for (size_t k = 0; k < 2; k++) {
        size_t kl = strlen(keys[k]);
        for (const char *p = strstr(js, keys[k]); p; p = strstr(p + 1, keys[k])) {
            const char *q = p + kl;
            while (*q == ' ' || *q == '\t' || *q == '\n') q++;
            if (*q != ':') continue;
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\n') q++;
            int d = 0;
            while (d < 6 && q[d] >= '0' && q[d] <= '9') d++;
            if (d != 5) continue;
            return strtol(q, NULL, 10);
        }
    }
    return 0;
}

static int http_get(const char *url, const char **hdrs, int nh, char **body, size_t *len)
{
    int status = 0;
    *body = NULL;
    *len = 0;
    int rc = evo_net_http_get_sync(url, hdrs, nh, body, len, &status);
    if (rc != EVO_NET_OK || status != 200 || !*body) {
        evo_log("yt-link: GET %.80s failed (rc=%d status=%d)", url, rc, status);
        free(*body);
        *body = NULL;
        return -1;
    }
    return 0;
}

/* ---- preprocessed player cache --------------------------------------- */
/* File: "id=<8 hex> sts=<n>\n" then the preprocessed player script. */

static char *cache_load(const char *player_id, long *out_sts)
{
    FILE *f = fopen(evo_data_path(YT_CACHE_LEAF), "rb");
    if (!f) return NULL;
    char line[64];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return NULL; }
    char id[16] = {0};
    long sts = 0;
    if (sscanf(line, "id=%15s sts=%ld", id, &sts) != 2 || strcmp(id, player_id) != 0 || sts <= 0) {
        fclose(f);
        return NULL;
    }
    long start = ftell(f);
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    fseek(f, start, SEEK_SET);
    if (end <= start) { fclose(f); return NULL; }
    size_t n = (size_t)(end - start);
    char *buf = (char *)malloc(n + 1);
    if (buf && fread(buf, 1, n, f) != n) { free(buf); buf = NULL; }
    fclose(f);
    if (!buf) return NULL;
    buf[n] = '\0';
    *out_sts = sts;
    return buf;
}

static void cache_store(const char *player_id, long sts, const char *pre, size_t len)
{
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s", evo_data_path(YT_CACHE_LEAF ".tmp"));
    FILE *f = fopen(tmp_path, "wb");
    if (!f) return;
    fprintf(f, "id=%s sts=%ld\n", player_id, sts);
    size_t w = fwrite(pre, 1, len, f);
    int bad = fclose(f) != 0 || w != len;
    if (bad) { remove(tmp_path); return; }
    char final_path[512];
    snprintf(final_path, sizeof(final_path), "%s", evo_data_path(YT_CACHE_LEAF));
    remove(final_path);
    if (rename(tmp_path, final_path) != 0) remove(tmp_path);
}

/* ---- the solver, on its own 4 MiB thread ----------------------------- */

struct solve_job {
    const char *player;        /* raw base.js or the preprocessed script */
    size_t player_len;
    int preprocessed;
    const char *challenge;
    char result[256];          /* out: transformed n */
    char *preprocessed_out;    /* out (malloc): when !preprocessed */
    size_t preprocessed_out_len;
    int rc;
    int64_t deadline;
    char err[200];
};

static int js_interrupt(JSRuntime *rt, void *opaque)
{
    (void)rt;
    const struct solve_job *job = (const struct solve_job *)opaque;
    return now_ms() > job->deadline;
}

static void js_error(JSContext *ctx, struct solve_job *job, const char *where)
{
    JSValue e = JS_GetException(ctx);
    const char *m = JS_ToCString(ctx, e);
    snprintf(job->err, sizeof(job->err), "%s: %s", where, m ? m : "exception");
    if (m) JS_FreeCString(ctx, m);
    JS_FreeValue(ctx, e);
}

static void *solve_thread(void *arg)
{
    struct solve_job *job = (struct solve_job *)arg;
    job->rc = -1;
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) { snprintf(job->err, sizeof(job->err), "no runtime"); return NULL; }
    JS_SetMemoryLimit(rt, YT_JS_MEM_LIMIT);
    JS_SetMaxStackSize(rt, YT_JS_MAX_STACK);
    JS_SetInterruptHandler(rt, js_interrupt, job);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { JS_FreeRuntime(rt); snprintf(job->err, sizeof(job->err), "no context"); return NULL; }

    JSValue v = JS_Eval(ctx, (const char *)evo_ytjs_lib, evo_ytjs_lib_len, "lib.min.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) { js_error(ctx, job, "lib"); goto out; }
    JS_FreeValue(ctx, v);
    static const char glue[] = "Object.assign(globalThis, lib);";
    v = JS_Eval(ctx, glue, sizeof(glue) - 1, "glue.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) { js_error(ctx, job, "glue"); goto out; }
    JS_FreeValue(ctx, v);
    v = JS_Eval(ctx, (const char *)evo_ytjs_core, evo_ytjs_core_len, "core.min.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) { js_error(ctx, job, "core"); goto out; }
    JS_FreeValue(ctx, v);

    /* The request object; the challenge is an [A-Za-z0-9_-] path segment, and
     * the player text goes in as a property, not through JSON. */
    char req[512];
    if (job->preprocessed)
        snprintf(req, sizeof(req),
                 "{\"type\":\"preprocessed\",\"requests\":[{\"type\":\"n\",\"challenges\":[\"%s\"]}]}",
                 job->challenge);
    else
        snprintf(req, sizeof(req),
                 "{\"type\":\"player\",\"requests\":[{\"type\":\"n\",\"challenges\":[\"%s\"]}],"
                 "\"output_preprocessed\":true}", job->challenge);
    JSValue input = JS_ParseJSON(ctx, req, strlen(req), "req.json");
    if (JS_IsException(input)) { js_error(ctx, job, "request"); goto out; }
    JS_SetPropertyStr(ctx, input, job->preprocessed ? "preprocessed_player" : "player",
                      JS_NewStringLen(ctx, job->player, job->player_len));

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "jsc");
    JSValue res = JS_Call(ctx, fn, global, 1, (JSValueConst *)&input);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, input);
    if (JS_IsException(res)) { JS_FreeValue(ctx, global); js_error(ctx, job, "jsc"); goto out; }
    JSValue json = JS_JSONStringify(ctx, res, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(ctx, res);
    JS_FreeValue(ctx, global);
    if (JS_IsException(json)) { js_error(ctx, job, "stringify"); goto out; }
    size_t jlen = 0;
    const char *js = JS_ToCStringLen(ctx, &jlen, json);
    JS_FreeValue(ctx, json);
    if (!js) { js_error(ctx, job, "result"); goto out; }

    cJSON *root = cJSON_ParseWithLength(js, jlen);
    JS_FreeCString(ctx, js);
    if (!root) { snprintf(job->err, sizeof(job->err), "result is not JSON"); goto out; }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "result") != 0) {
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "error");
        snprintf(job->err, sizeof(job->err), "solver: %s", cJSON_IsString(e) ? e->valuestring : "error");
        cJSON_Delete(root);
        goto out;
    }
    const cJSON *resp = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "responses"), 0);
    const cJSON *rtype = cJSON_GetObjectItemCaseSensitive(resp, "type");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(resp, "data");
    const cJSON *val = cJSON_GetObjectItemCaseSensitive(data, job->challenge);
    if (!cJSON_IsString(rtype) || strcmp(rtype->valuestring, "result") != 0 ||
        !cJSON_IsString(val) || !val->valuestring[0] ||
        strlen(val->valuestring) >= sizeof(job->result)) {
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(resp, "error");
        snprintf(job->err, sizeof(job->err), "n: %s", cJSON_IsString(e) ? e->valuestring : "no result");
        cJSON_Delete(root);
        goto out;
    }
    snprintf(job->result, sizeof(job->result), "%s", val->valuestring);
    const cJSON *pre = cJSON_GetObjectItemCaseSensitive(root, "preprocessed_player");
    if (!job->preprocessed && cJSON_IsString(pre)) {
        job->preprocessed_out_len = strlen(pre->valuestring);
        job->preprocessed_out = strdup(pre->valuestring);
    }
    cJSON_Delete(root);
    job->rc = 0;

out:
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return NULL;
}

static int run_solver(struct solve_job *job)
{
    pthread_attr_t attr;
    pthread_t th;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, YT_JS_THREAD_STACK);
    job->deadline = now_ms() + YT_JS_DEADLINE_MS;
    int64_t t0 = now_ms();
    int rc = pthread_create(&th, &attr, solve_thread, job);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        evo_log("yt-link: could not start the solver thread (%d)", rc);
        return -1;
    }
    pthread_join(th, NULL);
    evo_log("yt-link: solver %s in %lld ms (%s player, %zu B)%s%s",
            job->rc == 0 ? "ok" : "FAILED", (long long)(now_ms() - t0),
            job->preprocessed ? "cached" : "full", job->player_len,
            job->rc == 0 ? "" : ": ", job->rc == 0 ? "" : job->err);
    return job->rc;
}

/* ---- resolve ---------------------------------------------------------- */

static int solve_n(const char *player_id, const char *challenge, long *io_sts,
                   int sts_only, char *out_n, size_t out_cap)
{
    long sts = 0;
    char *pre = cache_load(player_id, &sts);
    if (pre && sts_only) {
        free(pre);
        *io_sts = sts;
        return 0;
    }
    if (pre) {
        struct solve_job job;
        memset(&job, 0, sizeof(job));
        job.player = pre;
        job.player_len = strlen(pre);
        job.preprocessed = 1;
        job.challenge = challenge;
        int rc = run_solver(&job);
        free(pre);
        free(job.preprocessed_out);
        if (rc == 0) {
            snprintf(out_n, out_cap, "%s", job.result);
            *io_sts = sts;
            return 0;
        }
        /* A stale or broken cache: fall through to the full player. */
    }

    char url[160];
    snprintf(url, sizeof(url), "https://www.youtube.com/s/player/%s/player_ias.vflset/en_US/base.js", player_id);
    const char *hdrs[] = { "User-Agent: " YT_EMB_UA };
    char *js = NULL;
    size_t js_len = 0;
    if (http_get(url, hdrs, 1, &js, &js_len) != 0) return -1;
    sts = find_sts(js);
    if (sts <= 0) {
        evo_log("yt-link: no signatureTimestamp in player %s", player_id);
        free(js);
        return -1;
    }
    *io_sts = sts;
    if (sts_only) {
        /* The player request needs sts before there is an n to solve; keep
         * the script for the solve step by solving a dummy challenge now,
         * which also fills the cache. */
        challenge = "AAAAAAAAAAAAAAAA";
    }
    struct solve_job job;
    memset(&job, 0, sizeof(job));
    job.player = js;
    job.player_len = js_len;
    job.preprocessed = 0;
    job.challenge = challenge;
    int rc = run_solver(&job);
    free(js);
    if (job.preprocessed_out) {
        cache_store(player_id, sts, job.preprocessed_out, job.preprocessed_out_len);
        free(job.preprocessed_out);
    }
    if (rc != 0) return -1;
    if (!sts_only) snprintf(out_n, out_cap, "%s", job.result);
    return 0;
}

int evo_yt_resolve_full(const char *video_id, char *out_url, size_t url_cap,
                        char *out_title, size_t title_cap)
{
    char id[16];
    if (!out_url || url_cap == 0 || take_id(video_id, id, sizeof(id)) != 0) return -1;
    out_url[0] = '\0';
    if (out_title && title_cap) out_title[0] = '\0';
    int64_t t0 = now_ms();

    /* 1. The current player version. */
    const char *ua_hdr[] = { "User-Agent: " YT_EMB_UA };
    char *api = NULL;
    size_t api_len = 0;
    if (http_get("https://www.youtube.com/iframe_api", ua_hdr, 1, &api, &api_len) != 0) return -1;
    char player_id[9];
    int have_player = find_player_id(api, player_id) == 0;
    free(api);
    if (!have_player) {
        evo_log("yt-link: no player version in iframe_api");
        return -1;
    }

    /* 2. signatureTimestamp (from the cache, or the player script). */
    long sts = 0;
    char dummy[8];
    if (solve_n(player_id, NULL, &sts, 1, dummy, sizeof(dummy)) != 0) return -1;

    /* 3. The embed page's ytcfg, as yt-dlp does: its INNERTUBE_CONTEXT carries
     * thirdParty.embeddedPlayerContext (an encrypted token), and
     * WEB_PLAYER_CONTEXT_CONFIGS carries encryptedHostFlags. A hand-built
     * context without them gets "This video is unavailable". */
    char embed_url[96];
    snprintf(embed_url, sizeof(embed_url), "https://www.youtube.com/embed/%s?html5=1", id);
    const char *ehdrs[] = { "User-Agent: " YT_EMB_UA, "Referer: " YT_EMB_EMBED_URL };
    char *page = NULL;
    size_t page_len = 0;
    if (http_get(embed_url, ehdrs, 2, &page, &page_len) != 0) return -1;
    cJSON *cfg = NULL;
    for (const char *p = strstr(page, "ytcfg.set"); p && !cfg; p = strstr(p + 1, "ytcfg.set")) {
        const char *q = p + 9;
        while (*q == ' ') q++;
        if (*q != '(') continue;
        q++;
        while (*q == ' ') q++;
        if (*q != '{') continue;
        size_t olen = json_object_len(q);
        if (!olen) continue;
        cJSON *c = cJSON_ParseWithLength(q, olen);
        if (c && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(c, "INNERTUBE_CONTEXT"))) cfg = c;
        else cJSON_Delete(c);
    }
    free(page);
    if (!cfg) {
        evo_log("yt-link: no ytcfg with INNERTUBE_CONTEXT in the embed page");
        return -1;
    }
    cJSON *ctx = cJSON_GetObjectItemCaseSensitive(cfg, "INNERTUBE_CONTEXT");
    cJSON *third = cJSON_GetObjectItemCaseSensitive(ctx, "thirdParty");
    if (!cJSON_IsObject(third)) {
        cJSON_DeleteItemFromObject(ctx, "thirdParty");
        third = cJSON_CreateObject();
        cJSON_AddItemToObject(ctx, "thirdParty", third);
    }
    cJSON_DeleteItemFromObject(third, "embedUrl");
    cJSON_AddStringToObject(third, "embedUrl", YT_EMB_EMBED_URL);

    char ver[96] = YT_EMB_CLIENT_VERSION;
    const cJSON *cv = cJSON_GetObjectItemCaseSensitive(cfg, "INNERTUBE_CONTEXT_CLIENT_VERSION");
    if (cJSON_IsString(cv) && cv->valuestring[0]) snprintf(ver, sizeof(ver), "%s", cv->valuestring);
    char visitor_hdr[1024] = "";
    const cJSON *vdat = cJSON_GetObjectItemCaseSensitive(cfg, "VISITOR_DATA");
    if (cJSON_IsString(vdat) && strlen(vdat->valuestring) < sizeof(visitor_hdr) - 20)
        snprintf(visitor_hdr, sizeof(visitor_hdr), "X-Goog-Visitor-Id: %s", vdat->valuestring);
    const cJSON *flags = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(cfg, "WEB_PLAYER_CONTEXT_CONFIGS"),
            "WEB_PLAYER_CONTEXT_CONFIG_ID_EMBEDDED_PLAYER"),
        "encryptedHostFlags");

    evo_log("yt-link: embed config ver=%s visitor=%s flags=%s encctx=%s", ver,
            visitor_hdr[0] ? "yes" : "no", cJSON_IsString(flags) ? "yes" : "no",
            cJSON_GetObjectItemCaseSensitive(third, "embeddedPlayerContext") ? "yes" : "no");
    /* flags is base64url-ish ([A-Za-z0-9_=-]); check before it goes into the
     * body as text. */
    char flag_str[512] = "";
    if (cJSON_IsString(flags) && strlen(flags->valuestring) < sizeof(flag_str))
        snprintf(flag_str, sizeof(flag_str), "%s", flags->valuestring);
    for (const char *f = flag_str; *f; f++)
        if (!is_id_char(*f) && *f != '=') { flag_str[0] = '\0'; break; }
    char *ctx_json = cJSON_PrintUnformatted(ctx);
    cJSON_Delete(cfg);
    if (!ctx_json) return -1;
    size_t body_cap = strlen(ctx_json) + strlen(flag_str) + 512;
    char *body = (char *)malloc(body_cap);
    if (!body) { free(ctx_json); return -1; }
    snprintf(body, body_cap,
             "{\"context\":%s,\"videoId\":\"%s\",\"playbackContext\":{\"contentPlaybackContext\":"
             "{\"html5Preference\":\"HTML5_PREF_WANTS\",\"signatureTimestamp\":%ld%s%s%s}},"
             "\"contentCheckOk\":true,\"racyCheckOk\":true}",
             ctx_json, id, sts,
             flag_str[0] ? ",\"encryptedHostFlags\":\"" : "", flag_str, flag_str[0] ? "\"" : "");
    free(ctx_json);

    char ver_hdr[128];
    snprintf(ver_hdr, sizeof(ver_hdr), "X-YouTube-Client-Version: %s", ver);
    const char *phdrs[6] = {
        "Content-Type: application/json",
        "User-Agent: " YT_EMB_UA,
        "X-YouTube-Client-Name: 56",
        ver_hdr,
        "Origin: https://www.youtube.com",
        visitor_hdr,
    };
    char *resp = NULL;
    size_t resp_len = 0;
    int status = 0;
    int rc = evo_net_http_post_sync("https://www.youtube.com/youtubei/v1/player?prettyPrint=false",
                                    body, phdrs, visitor_hdr[0] ? 6 : 5, &resp, &resp_len, &status);
    free(body);
    if (rc != EVO_NET_OK || status != 200 || !resp) {
        evo_log("yt-link: player API failed (rc=%d status=%d)", rc, status);
        free(resp);
        return -1;
    }
    cJSON *root = cJSON_ParseWithLength(resp, resp_len);
    free(resp);
    if (!root) return -1;

    const cJSON *ps = cJSON_GetObjectItemCaseSensitive(root, "playabilityStatus");
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(ps, "status");
    if (!cJSON_IsString(st) || strcmp(st->valuestring, "OK") != 0) {
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(ps, "reason");
        evo_log("yt-link: %s not playable: %s (%s)", id,
                cJSON_IsString(st) ? st->valuestring : "?",
                cJSON_IsString(reason) ? reason->valuestring : "");
        cJSON_Delete(root);
        return -1;
    }
    const cJSON *vd = cJSON_GetObjectItemCaseSensitive(root, "videoDetails");
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(vd, "title");
    if (out_title && title_cap && cJSON_IsString(title))
        snprintf(out_title, title_cap, "%s", title->valuestring);
    const cJSON *sd = cJSON_GetObjectItemCaseSensitive(root, "streamingData");
    const cJSON *hls = cJSON_GetObjectItemCaseSensitive(sd, "hlsManifestUrl");
    if (!cJSON_IsString(hls) || !hls->valuestring[0]) {
        evo_log("yt-link: no hlsManifestUrl for %s", id);
        cJSON_Delete(root);
        return -1;
    }
    char manifest[EVO_NET_MAX_URL];
    if (strlen(hls->valuestring) >= sizeof(manifest)) {
        evo_log("yt-link: manifest URL too long (%zu)", strlen(hls->valuestring));
        cJSON_Delete(root);
        return -1;
    }
    snprintf(manifest, sizeof(manifest), "%s", hls->valuestring);
    cJSON_Delete(root);

    /* 4. Solve the /n/<challenge>/ path segment and swap it in. */
    char *np = strstr(manifest, "/n/");
    if (np) {
        char challenge[128];
        const char *cs = np + 3;
        size_t cl = strcspn(cs, "/?");
        if (cl == 0 || cl >= sizeof(challenge)) return -1;
        memcpy(challenge, cs, cl);
        challenge[cl] = '\0';
        for (size_t i = 0; i < cl; i++)
            if (!is_id_char(challenge[i])) return -1;
        char solved[256];
        if (solve_n(player_id, challenge, &sts, 0, solved, sizeof(solved)) != 0) return -1;
        size_t need = (size_t)(cs - manifest) + strlen(solved) + strlen(cs + cl) + 1;
        if (need > url_cap) return -1;
        snprintf(out_url, url_cap, "%.*s%s%s", (int)(cs - manifest), manifest, solved, cs + cl);
        evo_log("yt-link: %s n %s -> %s", id, challenge, solved);
    } else {
        snprintf(out_url, url_cap, "%s", manifest);
    }
    evo_log("yt-link: resolved %s in %lld ms (player %s, sts %ld)", id,
            (long long)(now_ms() - t0), player_id, sts);
    return 0;
}
