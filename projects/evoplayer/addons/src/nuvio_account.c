/*
 * nuvio_account.c — see nuvio_account.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#include "nuvio_account.h"
#include "nuvio_json.h"
#include "evo_net.h"
#include "evo_provider_log.h"
#include "cJSON.h"

#define MAX_WAITERS 8

static struct {
    char    path[512];
    char    server[256];
    char    backend[512];
    char    key[640];
    char    email[160];
    char    password[160];      /* in memory only until the first sign-in */
    char    refresh[512];
    char    access[2400];
    int64_t access_exp_ms;
    char    user_id[64];
    char    client_id[48];
    int     profile;
    char    status[160];

    int     ensuring;
    struct { nuvio_account_cb cb; void *ud; } waiters[MAX_WAITERS];
    int     waiter_count;
} A;

static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void set_status(const char *s)
{
    snprintf(A.status, sizeof A.status, "%s", s);
    PROV_LOG("nuvio account: %s", s);
}

static void trim_slash(char *s)
{
    size_t n = strlen(s);
    while (n && s[n - 1] == '/') s[--n] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

static void make_client_id(void)
{
    static const char ALPHA[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t x = ((uint64_t)tv.tv_sec << 20) ^ (uint64_t)tv.tv_usec ^
                 ((uint64_t)getpid() << 40) ^ 0x9E3779B97F4A7C15ull;
    int o = snprintf(A.client_id, sizeof A.client_id, "evo-ps5-");
    for (int i = 0; i < 32 && o + 1 < (int)sizeof A.client_id; ++i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;          /* xorshift64 */
        A.client_id[o++] = ALPHA[x % (sizeof ALPHA - 1)];
    }
    A.client_id[o] = '\0';
}

static void save_conf(void)
{
    if (!A.path[0]) return;
    FILE *f = fopen(A.path, "w");
    if (!f) return;
    fprintf(f, "# Nuvio account for EVO. The password line is removed after the\n"
               "# first successful sign-in; the refresh token below replaces it.\n");
    if (A.server[0])    fprintf(f, "server=%s\n", A.server);
    if (A.backend[0])   fprintf(f, "backend_url=%s\n", A.backend);
    if (A.key[0])       fprintf(f, "publishable_key=%s\n", A.key);
    if (A.email[0])     fprintf(f, "email=%s\n", A.email);
    if (A.password[0] && !A.refresh[0])
                        fprintf(f, "password=%s\n", A.password);
    fprintf(f, "profile=%d\n", A.profile);
    if (A.refresh[0])   fprintf(f, "refresh_token=%s\n", A.refresh);
    if (A.user_id[0])   fprintf(f, "user_id=%s\n", A.user_id);
    fprintf(f, "client_id=%s\n", A.client_id);
    fclose(f);
}

void nuvio_account_init(const char *path)
{
    int ensuring = A.ensuring;          /* a rebind must not strand waiters */
    memset(&A.server, 0, (size_t)((char *)&A.ensuring - (char *)&A.server));
    A.ensuring = ensuring;
    snprintf(A.path, sizeof A.path, "%s", path ? path : "");
    A.profile = 1;

    FILE *f = A.path[0] ? fopen(A.path, "r") : NULL;
    if (f) {
        char line[3072];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *eq = strchr(line, '=');
            if (!eq || line[0] == '#') continue;
            *eq = '\0';
            const char *k = line, *v = eq + 1;
            if      (!strcmp(k, "server"))          snprintf(A.server,   sizeof A.server,   "%s", v);
            else if (!strcmp(k, "backend_url"))     snprintf(A.backend,  sizeof A.backend,  "%s", v);
            else if (!strcmp(k, "publishable_key")) snprintf(A.key,      sizeof A.key,      "%s", v);
            else if (!strcmp(k, "email"))           snprintf(A.email,    sizeof A.email,    "%s", v);
            else if (!strcmp(k, "password"))        snprintf(A.password, sizeof A.password, "%s", v);
            else if (!strcmp(k, "refresh_token"))   snprintf(A.refresh,  sizeof A.refresh,  "%s", v);
            else if (!strcmp(k, "user_id"))         snprintf(A.user_id,  sizeof A.user_id,  "%s", v);
            else if (!strcmp(k, "client_id"))       snprintf(A.client_id, sizeof A.client_id, "%s", v);
            else if (!strcmp(k, "profile"))         A.profile = atoi(v) > 0 ? atoi(v) : 1;
        }
        fclose(f);
    }
    trim_slash(A.server);
    trim_slash(A.backend);
    if (!A.client_id[0]) make_client_id();
    set_status(nuvio_account_is_configured() ? "configured" : "not set up");
}

int nuvio_account_is_configured(void)
{
    int where = A.server[0] || (A.backend[0] && A.key[0]);
    int who   = A.refresh[0] || (A.email[0] && A.password[0]);
    return where && who;
}

int nuvio_account_has_session(void) { return A.refresh[0] != 0; }
const char *nuvio_account_status(void) { return A.status; }
int nuvio_account_profile(void) { return A.profile; }
const char *nuvio_account_client_id(void) { return A.client_id; }

/* ------------------------------------------------------------------------- */
/* Sign-in                                                                   */
/* ------------------------------------------------------------------------- */

static void ensure_done(int ok)
{
    A.ensuring = 0;
    int n = A.waiter_count;
    A.waiter_count = 0;
    /* Copy first: a callback may start another ensure. */
    struct { nuvio_account_cb cb; void *ud; } w[MAX_WAITERS];
    memcpy(w, A.waiters, sizeof w);
    for (int i = 0; i < n; ++i)
        if (w[i].cb) w[i].cb(ok, NULL, 0, w[i].ud);
}

static void apikey_hdr(char *out, size_t cap) { snprintf(out, cap, "apikey: %s", A.key); }

static void on_token(int ok, int status, const char *body, size_t len, void *ud);

static void request_token(int use_refresh)
{
    char url[640];
    snprintf(url, sizeof url, "%s/auth/v1/token?grant_type=%s", A.backend,
             use_refresh ? "refresh_token" : "password");

    nuvio_jw_t w;
    nuvio_jw_init(&w);
    nuvio_jw_obj(&w, NULL);
    if (use_refresh) {
        nuvio_jw_str(&w, "refresh_token", A.refresh);
    } else {
        nuvio_jw_str(&w, "email", A.email);
        nuvio_jw_str(&w, "password", A.password);
    }
    nuvio_jw_end_obj(&w);
    char *body = nuvio_jw_finish(&w);

    char ak[700];
    apikey_hdr(ak, sizeof ak);
    const char *h[1] = { ak };
    int rc = body ? evo_net_request_async("POST", url, body, h, 1, on_token,
                                          (void *)(intptr_t)use_refresh)
                  : -1;
    free(body);
    if (rc != 0) { set_status("could not reach the server"); ensure_done(0); }
}

static void on_token(int ok, int status, const char *body, size_t len, void *ud)
{
    int was_refresh = (int)(intptr_t)ud;
    cJSON *r = (ok && body) ? cJSON_ParseWithLength(body, len) : NULL;
    const cJSON *at = cJSON_GetObjectItemCaseSensitive(r, "access_token");
    const cJSON *rt = cJSON_GetObjectItemCaseSensitive(r, "refresh_token");
    const cJSON *ex = cJSON_GetObjectItemCaseSensitive(r, "expires_in");
    const cJSON *user = cJSON_GetObjectItemCaseSensitive(r, "user");
    const cJSON *uid = cJSON_GetObjectItemCaseSensitive(user, "id");

    if (!cJSON_IsString(at) || !at->valuestring || !cJSON_IsString(rt) || !rt->valuestring ||
        strlen(at->valuestring) >= sizeof A.access || strlen(rt->valuestring) >= sizeof A.refresh) {
        cJSON_Delete(r);
        if (was_refresh && A.email[0] && A.password[0]) {
            /* A refresh token can be revoked; the password, if we still have
             * it this session, is the way back in. */
            A.refresh[0] = '\0';
            request_token(0);
            return;
        }
        char s[96];
        snprintf(s, sizeof s, "sign-in failed (HTTP %d)", status);
        set_status(s);
        if (was_refresh) A.refresh[0] = '\0';
        save_conf();
        ensure_done(0);
        return;
    }

    snprintf(A.access, sizeof A.access, "%s", at->valuestring);
    snprintf(A.refresh, sizeof A.refresh, "%s", rt->valuestring);
    int64_t secs = cJSON_IsNumber(ex) ? (int64_t)ex->valuedouble : 3600;
    A.access_exp_ms = now_ms() + secs * 1000;
    if (cJSON_IsString(uid) && uid->valuestring)
        snprintf(A.user_id, sizeof A.user_id, "%s", uid->valuestring);
    cJSON_Delete(r);

    /* The password has done its job; it does not stay on disk. */
    A.password[0] = '\0';
    save_conf();
    set_status("signed in");
    ensure_done(1);
}

static void sign_in(void)
{
    if (A.refresh[0])                       request_token(1);
    else if (A.email[0] && A.password[0])   request_token(0);
    else { set_status("no credentials - add email= and password= to account.conf"); ensure_done(0); }
}

static void on_discovery(int ok, int status, const char *body, size_t len, void *ud)
{
    (void)ud;
    cJSON *r = (ok && body) ? cJSON_ParseWithLength(body, len) : NULL;
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(r, "version");
    const cJSON *svc = cJSON_GetObjectItemCaseSensitive(r, "service");
    const cJSON *bu  = cJSON_GetObjectItemCaseSensitive(r, "backend_url");
    const cJSON *pk  = cJSON_GetObjectItemCaseSensitive(r, "publishable_key");
    int good = cJSON_IsNumber(ver) && (int)ver->valuedouble == 1 &&
               cJSON_IsString(svc) && svc->valuestring &&
               (!strcmp(svc->valuestring, "nuvio") || !strcmp(svc->valuestring, "Nuvio")) &&
               cJSON_IsString(bu) && bu->valuestring &&
               (!strncmp(bu->valuestring, "https://", 8) || !strncmp(bu->valuestring, "http://", 7)) &&
               cJSON_IsString(pk) && pk->valuestring && pk->valuestring[0];
    if (good) {
        snprintf(A.backend, sizeof A.backend, "%s", bu->valuestring);
        snprintf(A.key, sizeof A.key, "%s", pk->valuestring);
        trim_slash(A.backend);
    }
    cJSON_Delete(r);
    if (!good) {
        char s[96];
        snprintf(s, sizeof s, "server discovery failed (HTTP %d)", status);
        set_status(s);
        ensure_done(0);
        return;
    }
    save_conf();
    sign_in();
}

int nuvio_account_ensure(nuvio_account_cb cb, void *ud)
{
    if (!nuvio_account_is_configured()) return -1;
    if (A.access[0] && now_ms() < A.access_exp_ms - 60 * 1000) {
        if (cb) cb(1, NULL, 0, ud);
        return 0;
    }
    if (A.waiter_count >= MAX_WAITERS) return -2;
    A.waiters[A.waiter_count].cb = cb;
    A.waiters[A.waiter_count].ud = ud;
    A.waiter_count++;
    if (A.ensuring) return 0;
    A.ensuring = 1;

    if (!A.backend[0] || !A.key[0]) {
        char url[320];
        snprintf(url, sizeof url, "%s/.well-known/nuvio", A.server);
        if (strncmp(url, "http", 4) != 0) {
            char tmp[320];
            snprintf(tmp, sizeof tmp, "https://%s", url);
            snprintf(url, sizeof url, "%s", tmp);
        }
        if (evo_net_request_async("GET", url, NULL, NULL, 0, on_discovery, NULL) != 0) {
            set_status("could not reach the server");
            A.waiter_count--;
            A.ensuring = 0;
            return -3;
        }
        return 0;
    }
    sign_in();
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Authenticated requests                                                    */
/* ------------------------------------------------------------------------- */

typedef struct req {
    char  method[8];
    char  path[1024];     /* after /rest/v1/ */
    char *body;
    int   retried;
    nuvio_account_cb cb;
    void *ud;
} req_t;

static void req_finish(req_t *q, int ok, const char *body, size_t len)
{
    nuvio_account_cb cb = q->cb;
    void *ud = q->ud;
    free(q->body);
    free(q);
    if (cb) cb(ok, body, len, ud);
}

static void on_ready(int ok, const char *b, size_t l, void *ud);

static void on_req(int ok, int status, const char *body, size_t len, void *ud)
{
    req_t *q = (req_t *)ud;
    if (!ok && status == 401 && !q->retried) {
        /* The JWT expired under us: drop it, refresh once, and go again. */
        q->retried = 1;
        A.access[0] = '\0';
        A.access_exp_ms = 0;
        if (nuvio_account_ensure(on_ready, q) != 0) req_finish(q, 0, NULL, 0);
        return;
    }
    if (!ok) PROV_LOG("nuvio account: %s %s -> HTTP %d", q->method, q->path, status);
    req_finish(q, ok, body, len);
}

static void req_send(req_t *q)
{
    char url[1700];
    snprintf(url, sizeof url, "%s/rest/v1/%s", A.backend, q->path);
    char ak[700], auth[2500];
    apikey_hdr(ak, sizeof ak);
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", A.access);
    const char *h[3] = { ak, auth, "Accept: application/json" };
    if (evo_net_request_async(q->method, url, q->body, h, 3, on_req, q) != 0)
        req_finish(q, 0, NULL, 0);
}

static void on_ready(int ok, const char *b, size_t l, void *ud)
{
    (void)b; (void)l;
    req_t *q = (req_t *)ud;
    if (!ok) { req_finish(q, 0, NULL, 0); return; }
    req_send(q);
}

static int start_req(const char *method, const char *path, const char *body,
                     nuvio_account_cb cb, void *ud)
{
    if (!nuvio_account_is_configured() || strlen(path) >= sizeof(((req_t *)0)->path))
        return -1;
    req_t *q = (req_t *)calloc(1, sizeof *q);
    if (!q) return -2;
    snprintf(q->method, sizeof q->method, "%s", method);
    snprintf(q->path, sizeof q->path, "%s", path);
    if (body) {
        size_t n = strlen(body) + 1;
        q->body = (char *)malloc(n);
        if (!q->body) { free(q); return -2; }
        memcpy(q->body, body, n);
    }
    q->cb = cb;
    q->ud = ud;
    if (nuvio_account_ensure(on_ready, q) != 0) {
        free(q->body);
        free(q);
        return -3;
    }
    return 0;
}

int nuvio_account_rpc(const char *name, const char *json_body,
                      nuvio_account_cb cb, void *ud)
{
    char path[160];
    snprintf(path, sizeof path, "rpc/%s", name);
    return start_req("POST", path, json_body ? json_body : "{}", cb, ud);
}

int nuvio_account_select(const char *path_and_query, nuvio_account_cb cb, void *ud)
{
    return start_req("GET", path_and_query, NULL, cb, ud);
}
