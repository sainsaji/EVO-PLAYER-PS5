/*
 * tests/test_runner.c — Comprehensive Unit & Integration Test Suite for EVO Player
 *
 * Runs natively on Linux / macOS / Docker / CI without requiring a PS5 console.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>

#include "evo_direct_mem.h"
#include "evo_draw.h"
#include "evo_focus.h"
#include "evo_theme.h"
#include "evo_screens.h"
#include "evo_addon.h"
#include "addon_emby.h"
#include "evo_provider.h"
#include "evo_provider_bundle.h"
#include "evo_changelog.h"
#include "evo_net.h"
#include "evo_data_path.h"
#include "nuvio_stremio.h"
#include "nuvio_progress.h"
#include "nuvio_json.h"
#include "nuvio_debrid.h"

#include <sys/stat.h>

#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "SDL_ps5tilemap.inc"

/* Test runner assertion tracking */
static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_START(name) do { \
    g_tests_run++; \
    printf("  RUN  %-50s", name); \
    fflush(stdout); \
} while(0)

#define TEST_PASS() do { \
    g_tests_passed++; \
    printf(" [ PASS ]\n"); \
} while(0)

#define TEST_FAIL(reason) do { \
    g_tests_failed++; \
    printf(" [ FAIL ] (%s at line %d)\n", reason, __LINE__); \
} while(0)

#define TEST_ASSERT(cond, reason) do { \
    if (!(cond)) { \
        TEST_FAIL(reason); \
        return; \
    } \
} while(0)

char g_current_media_title[256] = {0};

static bool str_contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    char a[256], b[256];
    int i = 0;
    for (; haystack[i] && i < 255; i++) {
        char c = haystack[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        a[i] = c;
    }
    a[i] = 0;
    i = 0;
    for (; needle[i] && i < 255; i++) {
        char c = needle[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        b[i] = c;
    }
    b[i] = 0;
    return strstr(a, b) != NULL;
}

void clean_media_title(const char *path, char *line1, size_t line1_sz, char *line2, size_t line2_sz) {
    if (!path || !path[0]) {
        if (line1 && line1_sz > 0) line1[0] = '\0';
        if (line2 && line2_sz > 0) line2[0] = '\0';
        return;
    }

    if (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0) {
        if (g_current_media_title[0]) {
            snprintf(line1, line1_sz, "%s", g_current_media_title);
        } else {
            const char *slash = strrchr(path, '/');
            const char *fname = slash ? slash + 1 : path;
            char temp[256];
            snprintf(temp, sizeof(temp), "%s", fname);
            char *q = strchr(temp, '?');
            if (q) *q = '\0';
            if (temp[0] && strcmp(temp, "stream") != 0 && strcmp(temp, "master.m3u8") != 0) {
                snprintf(line1, line1_sz, "%s", temp);
            } else {
                snprintf(line1, line1_sz, "Emby Media Stream");
            }
        }
        if (line2 && line2_sz > 0) {
            snprintf(line2, line2_sz, "EMBY STREAM");
        }
        return;
    }

    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;

    char original[256];
    snprintf(original, sizeof(original), "%s", name);

    char *ext = strrchr(original, '.');
    if (ext) *ext = 0;

    char season[16] = "";
    int season_pos = -1;

    for (int i = 0; original[i]; i++) {
        if ((original[i] == 'S' || original[i] == 's') &&
            original[i+1] >= '0' && original[i+1] <= '9' &&
            original[i+2] >= '0' && original[i+2] <= '9' &&
            (original[i+3] == 'E' || original[i+3] == 'e') &&
            original[i+4] >= '0' && original[i+4] <= '9' &&
            original[i+5] >= '0' && original[i+5] <= '9') {
            snprintf(season, sizeof(season), "S%c%cE%c%c", original[i+1], original[i+2], original[i+4], original[i+5]);
            season_pos = i;
            break;
        }
    }

    if (season_pos > 0) {
        char title[256];
        snprintf(title, sizeof(title), "%s", original);
        title[season_pos] = 0;

        while (strlen(title) > 0 &&
              (title[strlen(title)-1] == '.' || title[strlen(title)-1] == '_' || title[strlen(title)-1] == '-' || title[strlen(title)-1] == ' '))
            title[strlen(title)-1] = 0;

        snprintf(line1, line1_sz, "%s", title);
    } else {
        snprintf(line1, line1_sz, "%s", original);
    }

    const char *quality = str_contains_ci(name, "2160") ? "2160p" :
                          str_contains_ci(name, "1080") ? "1080p" :
                          str_contains_ci(name, "720")  ? "720p"  : "";

    const char *codec = (str_contains_ci(name, "hevc") || str_contains_ci(name, "h265") || str_contains_ci(name, "x265")) ? "HEVC" :
                        (str_contains_ci(name, "h264") || str_contains_ci(name, "x264")) ? "H.264" : "";

    const char *ch = str_contains_ci(name, "6ch") ? "6CH" :
                     str_contains_ci(name, "5.1") ? "5.1" :
                     str_contains_ci(name, "2ch") ? "2CH" : "";

    snprintf(line2, line2_sz, "%s%s%s%s%s%s%s",
        season,
        season[0] && quality[0] ? "  " : "",
        quality,
        (season[0] || quality[0]) && codec[0] ? "  " : "",
        codec,
        (season[0] || quality[0] || codec[0]) && ch[0] ? "  " : "",
        ch);
}

/* ==========================================================================
 * 1. CPU SIMD / Color Converter Tests
 * ========================================================================== */

/* ==========================================================================
 * 2. Direct Memory Slab Allocator Tests
 * ========================================================================== */

static void test_direct_mem_lifecycle(void)
{
    TEST_START("Direct Memory: Allocation, Alignment & Reset");
    
    int rc = evo_direct_mem_init(4 * 1024 * 1024); /* 4 MB test arena */
    TEST_ASSERT(rc == 0, "evo_direct_mem_init failed");
    
    void *p1 = evo_direct_mem_alloc(1024);
    TEST_ASSERT(p1 != NULL, "64-byte aligned alloc failed");
    TEST_ASSERT(((uintptr_t)p1 % 64) == 0, "Pointer not 64-byte aligned");
    
    void *p2 = evo_direct_mem_alloc(2048);
    TEST_ASSERT(p2 != NULL, "128-byte aligned alloc failed");
    TEST_ASSERT(((uintptr_t)p2 % 64) == 0, "Pointer not 64-byte aligned");
    
    evo_direct_mem_stats_t stats;
    evo_direct_mem_get_stats(&stats);
    TEST_ASSERT(stats.allocated_bytes >= (1024 + 2048), "Stats allocated bytes incorrect");
    TEST_ASSERT(stats.num_allocations == 2, "Alloc count mismatch");
    
    evo_direct_mem_free(p1);
    evo_direct_mem_free(p2);
    evo_direct_mem_get_stats(&stats);
    TEST_ASSERT(stats.allocated_bytes == 0, "Free did not clear allocated bytes");

    /*
     * #6: a request larger than the whole pool must still succeed (graceful
     * malloc fallback) and free() must route it back correctly — the 4K video
     * buffers depend on this when the console can't give the full slab.
     */
    void *big = evo_direct_mem_alloc(8 * 1024 * 1024);
    TEST_ASSERT(big != NULL, "oversize alloc did not fall back");
    memset(big, 0xAB, 8 * 1024 * 1024); /* must be writable for its full extent */
    evo_direct_mem_free(big);
    evo_direct_mem_get_stats(&stats);
    TEST_ASSERT(stats.allocated_bytes == 0, "fallback free leaked into pool stats");

    evo_direct_mem_shutdown();
    TEST_PASS();
}

/* ==========================================================================
 * 3. UI Typography, Text Fitting & Title Cleaning Tests
 * ========================================================================== */

static void test_clean_media_title_resolution(void)
{
    TEST_START("UI Media Title: Local & Remote Title Resolution");
    char line1[256];
    char line2[256];
    
    /* 1. Local USB Movie Path */
    g_current_media_title[0] = '\0';
    clean_media_title("/mnt/usb0/Movies/Inception.2010.1080p.BluRay.x264.mkv", line1, sizeof(line1), line2, sizeof(line2));
    TEST_ASSERT(strstr(line1, "Inception") != NULL, "Movie title parsing failed");
    TEST_ASSERT(strstr(line2, "1080p") != NULL || strstr(line2, "H.264") != NULL, "Metadata parsing failed");
    
    /* 2. TV Show Season / Episode */
    clean_media_title("/mnt/usb0/TV/Breaking.Bad.S05E14.720p.mkv", line1, sizeof(line1), line2, sizeof(line2));
    TEST_ASSERT(strstr(line1, "Breaking.Bad") != NULL || strstr(line1, "Breaking Bad") != NULL, "TV title parsing failed");
    TEST_ASSERT(strstr(line2, "S05E14") != NULL, "Season/Episode tag missing");
    
    /* 3. Remote Emby Stream with explicit active title */
    strncpy(g_current_media_title, "Interstellar (2014) [IMAX]", sizeof(g_current_media_title) - 1);
    clean_media_title("http://192.168.0.11:8096/emby/Videos/9988/stream.mkv?static=true", line1, sizeof(line1), line2, sizeof(line2));
    TEST_ASSERT(strcmp(line1, "Interstellar (2014) [IMAX]") == 0, "Emby stream title not resolved");
    TEST_ASSERT(strcmp(line2, "EMBY STREAM") == 0, "Emby stream subtitle badge missing");
    
    g_current_media_title[0] = '\0';
    TEST_PASS();
}

/* ==========================================================================
 * 4. Addon / Emby Client Tests
 * ========================================================================== */

static void test_emby_url_and_config(void)
{
    TEST_START("Addons: Emby Server URL Formatting & Credentials");
    
    emby_config_t *cfg = emby_get_config();
    TEST_ASSERT(cfg != NULL, "emby_get_config returned NULL");
    
    emby_set_server("192.168.0.50", 8096, "testuser", "securepass123");
    TEST_ASSERT(strcmp(cfg->host, "192.168.0.50") == 0, "Host not updated");
    TEST_ASSERT(cfg->port == 8096, "Port not updated");
    TEST_ASSERT(strcmp(cfg->username, "testuser") == 0, "Username not updated");
    TEST_ASSERT(strcmp(cfg->password, "securepass123") == 0, "Password not updated");
    
    char stream_url[512];
    strncpy(cfg->token, "sample_token_xyz", sizeof(cfg->token) - 1);
    int rc = emby_build_stream_url("item_12345", stream_url, sizeof(stream_url));
    TEST_ASSERT(rc == 0, "emby_build_stream_url failed");
    TEST_ASSERT(strstr(stream_url, "http://192.168.0.50:8096/emby/Videos/item_12345/stream") != NULL,
                "Stream URL path malformed");
    TEST_ASSERT(strstr(stream_url, "api_key=sample_token_xyz") != NULL,
                "Stream URL missing API authentication token");
    
    TEST_PASS();
}

/* ==========================================================================
 * 5. Navigation & Focus Engine Tests
 * ========================================================================== */

/*
 * #90: the provider bundle path check.
 *
 * This is a trust boundary, not a tidiness check. A bundle's manifest names
 * where each of its files is written and, later, what LoadDocument opens, and
 * RmlUi's file interface reaches disk with a plain fopen() that validates
 * nothing. An entry of "../../../data/evoplayer/emby.conf" getting past
 * evo_bundle_path_safe() means a downloaded file written over stored
 * credentials.
 *
 * The docs claim every one of these is "enforced, not assumed". This is what
 * makes that sentence checkable.
 */
static void test_provider_bundle_path_safety(void)
{
    TEST_START("Providers: bundle path escape rejection");

    /* Legitimate shapes a real bundle uses. */
    static const char *ok[] = {
        "main.rml", "main.rcss", "fonts/Inter.ttf", "img/logo.png",
        "a/b/c/deep.rcss", "with-dash_and_underscore.rml",
        "dotted.name.rcss",            /* dots are fine inside a component */
        NULL
    };
    for (int i = 0; ok[i]; ++i)
        TEST_ASSERT(evo_bundle_path_safe(ok[i]) == 1, ok[i]);

    /* Every rejection the header promises. */
    static const char *bad[] = {
        NULL,                          /* placeholder, replaced below */
        "",                            /* empty */
        "/etc/passwd",                 /* absolute */
        "C:/windows/win.ini",          /* drive letter */
        "..",                          /* bare parent */
        "../secret",                   /* escape up */
        "a/../../b",                   /* escape mid-path */
        "a/..",                        /* escape at the end */
        "...",                         /* a longer run of dots is still dots */
        "a/.../b",
        "a//b",                        /* empty component */
        "a/./b",                       /* single-dot component */
        "dir\\file.rml",              /* backslash separator */
        "file://main.rml",             /* URI scheme */
        "evo:mem/art0-1",              /* would collide with the texture registry */
        "http://host/x.rml",
        "tab\there.rml",               /* control character */
        NULL
    };
    /* index 0 is the NULL pointer case, which the loop below cannot express */
    TEST_ASSERT(evo_bundle_path_safe(NULL) == 0, "NULL accepted");
    for (int i = 1; bad[i]; ++i)
        TEST_ASSERT(evo_bundle_path_safe(bad[i]) == 0, bad[i]);

    /* Over the length cap. */
    char too_long[EVO_BUNDLE_MAX_PATH + 16];
    memset(too_long, 'a', sizeof too_long - 1);
    too_long[sizeof too_long - 1] = 0;
    TEST_ASSERT(evo_bundle_path_safe(too_long) == 0, "over-length path accepted");

    /* A provider id is a directory name, so it is checked the same way. */
    TEST_ASSERT(evo_provider_id_valid("iptv") == 1, "valid id rejected");
    TEST_ASSERT(evo_provider_id_valid("real-debrid") == 1, "valid id rejected");
    TEST_ASSERT(evo_provider_id_valid("") == 0, "empty id accepted");
    TEST_ASSERT(evo_provider_id_valid("../x") == 0, "escaping id accepted");
    TEST_ASSERT(evo_provider_id_valid("IPTV") == 0, "uppercase id accepted");
    TEST_ASSERT(evo_provider_id_valid("a b") == 0, "id with space accepted");
    TEST_ASSERT(evo_provider_id_valid("-lead") == 0, "leading dash accepted");
    TEST_ASSERT(evo_provider_id_valid("trail_") == 0, "trailing underscore accepted");

    /* evo_bundle_path() must refuse what path_safe refuses, and must not
     * produce a path outside the provider's own directory. */
    char out[512];
    TEST_ASSERT(evo_bundle_path("iptv", "../escape.rml", out, sizeof out) != 0,
                "evo_bundle_path allowed an escape");
    TEST_ASSERT(evo_bundle_path("iptv", "main.rml", out, sizeof out) == 0,
                "evo_bundle_path rejected a legitimate name");
    TEST_ASSERT(strstr(out, "/providers/iptv/main.rml") != NULL,
                "evo_bundle_path built the wrong path");

    TEST_PASS();
}

/*
 * #90: query-value escaping. Providers build query strings by hand, so a raw
 * '&' or '=' in a search term changes the request rather than the term.
 */
static void test_provider_url_escape(void)
{
    TEST_START("Providers: URL query-value escaping");

    char out[256];
    TEST_ASSERT(evo_provider_url_escape("plain", out, sizeof out) == 5, "len");
    TEST_ASSERT(strcmp(out, "plain") == 0, "unreserved text was altered");

    TEST_ASSERT(evo_provider_url_escape("a b&c=d", out, sizeof out) > 0, "escape failed");
    TEST_ASSERT(strcmp(out, "a%20b%26c%3Dd") == 0, "separators not escaped");

    /* RFC 3986 unreserved set passes through untouched. */
    TEST_ASSERT(evo_provider_url_escape("-_.~", out, sizeof out) == 4, "unreserved len");
    TEST_ASSERT(strcmp(out, "-_.~") == 0, "unreserved set escaped");

    /* Overflow is an error, never a truncated - and therefore different -
     * query value. */
    char tiny[4];
    TEST_ASSERT(evo_provider_url_escape("abcdef", tiny, sizeof tiny) < 0,
                "overflow silently truncated");

    TEST_PASS();
}

static void test_navigation_grid_and_focus(void)
{
    TEST_START("Navigation Engine: Grid & Focus Bounds Clamping");
    
    evo_focus f;
    evo_focus_init(&f, 10, 6, 0); /* 10 items, 6 visible, wrap=0 */
    TEST_ASSERT(f.index == 0 && f.scroll == 0, "Focus init failed");
    
    /* Move Down */
    evo_focus_move(&f, +1);
    TEST_ASSERT(f.index == 1, "Focus move +1 failed");
    
    /* Move past visible window */
    evo_focus_move(&f, +6);
    TEST_ASSERT(f.index == 7, "Focus move +6 failed");
    TEST_ASSERT(f.scroll > 0, "Focus did not scroll window");
    
    /* Clamp at upper bound */
    evo_focus_move(&f, +50);
    TEST_ASSERT(f.index == 9, "Focus did not clamp at max index");
    
    /* Clamp at lower bound */
    evo_focus_move(&f, -50);
    TEST_ASSERT(f.index == 0 && f.scroll == 0, "Focus did not clamp at zero");
    
    TEST_PASS();
}

/* ==========================================================================
 * 6. Changelog Model Consistency Tests
 * ========================================================================== */

static void test_changelog_model_integrity(void)
{
    TEST_START("Changelog Model: Release & Badges Integrity");
    
    TEST_ASSERT(EVO_CHANGELOG_RELEASE_COUNT > 0, "No changelog releases defined");
    
    for (int i = 0; i < EVO_CHANGELOG_RELEASE_COUNT; i++) {
        const evo_changelog_release *r = &EVO_CHANGELOG_RELEASES[i];
        TEST_ASSERT(r->version != NULL && strlen(r->version) > 0, "Release version is empty");
        TEST_ASSERT(r->date != NULL && strlen(r->date) > 0, "Release date is empty");
        TEST_ASSERT(r->tagline != NULL && strlen(r->tagline) > 0, "Release tagline is empty");
        TEST_ASSERT(r->item_count > 0, "Release has no changelog items");
        TEST_ASSERT(r->items != NULL, "Release items pointer is NULL");
        
        for (int j = 0; j < r->item_count; j++) {
            TEST_ASSERT(r->items[j].text != NULL, "Changelog item text is NULL");
            TEST_ASSERT(r->items[j].kind >= 0 && r->items[j].kind <= 3, "Invalid changelog item category");
        }
    }
    
    TEST_PASS();
}

/* ==========================================================================
 * 7. Common UI Widgets & Surround Studio Rendering Tests
 * ========================================================================== */

/* ==========================================================================
 * Mock Drawing Vtable for Host UI Tests
 * ========================================================================== */

;

/* ==========================================================================
 * Main Test Runner Entrypoint
 * ========================================================================== */

/* ------------------------------------------------------------------------- */
/* evo_net worker pool                                                       */
/* ------------------------------------------------------------------------- */

static int g_slow_secs = 1;
static void *slow_http_conn(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[1024];
    (void)recv(fd, buf, sizeof buf, 0);
    sleep((unsigned)g_slow_secs);
    static const char resp[] =
        "HTTP/1.0 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
    (void)send(fd, resp, sizeof resp - 1, 0);
    close(fd);
    return NULL;
}

static int g_slow_accepts = 4;
static void *slow_http_server(void *arg)
{
    int ls = (int)(intptr_t)arg;
    for (int i = 0; i < g_slow_accepts; ++i) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) break;
        pthread_t t;
        pthread_create(&t, NULL, slow_http_conn, (void *)(intptr_t)fd);
        pthread_detach(t);
    }
    return NULL;
}

static int g_net_done = 0;
static int g_net_ok = 0;
static void net_count_cb(int success, int status, const char *body, size_t len, void *ud)
{
    (void)status; (void)ud;
    g_net_done++;
    if (success && len == 2 && body && !memcmp(body, "ok", 2)) g_net_ok++;
}

static void test_net_worker_pool_parallel(void)
{
    TEST_START("evo_net: 4 slow requests run in parallel");

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(ls >= 0, "socket");
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    TEST_ASSERT(bind(ls, (struct sockaddr *)&a, sizeof a) == 0, "bind");
    TEST_ASSERT(listen(ls, 8) == 0, "listen");
    socklen_t al = sizeof a;
    getsockname(ls, (struct sockaddr *)&a, &al);

    pthread_t srv;
    pthread_create(&srv, NULL, slow_http_server, (void *)(intptr_t)ls);

    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/x", ntohs(a.sin_port));

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    for (int i = 0; i < 4; ++i)
        TEST_ASSERT(evo_net_request_async("GET", url, NULL, NULL, 0, net_count_cb, NULL) == 0,
                    "request not queued");
    for (int spin = 0; spin < 1000 && g_net_done < 4; ++spin) {
        evo_net_poll();
        usleep(10000);
    }
    gettimeofday(&t1, NULL);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;

    pthread_join(srv, NULL);
    close(ls);
    evo_net_shutdown();

    TEST_ASSERT(g_net_done == 4, "not every callback fired");
    TEST_ASSERT(g_net_ok == 4, "a response body was wrong");
    TEST_ASSERT(secs < 2.5, "requests ran serially");
    TEST_PASS();
}

static void test_net_request_timeout(void)
{
    TEST_START("evo_net: per-request timeout is applied");
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT(bind(ls, (struct sockaddr *)&a, sizeof a) == 0 && listen(ls, 4) == 0, "listen");
    socklen_t al = sizeof a;
    getsockname(ls, (struct sockaddr *)&a, &al);
    g_slow_secs = 3;
    g_slow_accepts = 1;
    pthread_t srv;
    pthread_create(&srv, NULL, slow_http_server, (void *)(intptr_t)ls);

    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/x", ntohs(a.sin_port));
    g_net_done = g_net_ok = 0;
    TEST_ASSERT(evo_net_request_async_timeout("GET", url, NULL, NULL, 0, 1, net_count_cb, NULL) == 0, "queued");
    for (int spin = 0; spin < 600 && !g_net_done; ++spin) { evo_net_poll(); usleep(10000); }
    pthread_join(srv, NULL);
    close(ls);
    g_slow_secs = 1;
    g_slow_accepts = 4;
    TEST_ASSERT(g_net_done == 1, "callback fired");
    TEST_ASSERT(g_net_ok == 0, "a 1 s timeout against a 3 s server must fail");
    TEST_PASS();
}

/* ------------------------------------------------------------------------- */
/* Nuvio: the Stremio addon protocol                                         */
/* ------------------------------------------------------------------------- */

static void test_nuvio_urls(void)
{
    TEST_START("nuvio: canonical base + resource URLs");
    char b[NUVIO_MAX_BASE_URL], u[2048];

    TEST_ASSERT(nuvio_canonical_base("https://v3-cinemeta.strem.io/manifest.json", b, sizeof b) == 0, "cinemeta");
    TEST_ASSERT(!strcmp(b, "https://v3-cinemeta.strem.io"), b);

    TEST_ASSERT(nuvio_canonical_base("stremio://torrentio.strem.fun/sort=qualitysize/manifest.json\r\n", b, sizeof b) == 0, "stremio://");
    TEST_ASSERT(!strcmp(b, "https://torrentio.strem.fun/sort=qualitysize"), b);

    TEST_ASSERT(nuvio_canonical_base("https://x.io/abc/manifest.json?token=1/", b, sizeof b) == 0, "query");
    TEST_ASSERT(!strcmp(b, "https://x.io/abc?token=1/"), b);
    TEST_ASSERT(nuvio_canonical_base("ftp://x.io/manifest.json", b, sizeof b) == -1, "ftp accepted");
    TEST_ASSERT(nuvio_canonical_base("https:///manifest.json", b, sizeof b) == -1, "no host accepted");

    TEST_ASSERT(nuvio_manifest_url("https://x.io/abc?token=1", u, sizeof u) == 0, "manifest url");
    TEST_ASSERT(!strcmp(u, "https://x.io/abc/manifest.json?token=1"), u);

    nuvio_resource_url("https://x.io/abc?token=1", "stream", "series", "tt0903747:2:5", u, sizeof u);
    TEST_ASSERT(!strcmp(u, "https://x.io/abc/stream/series/tt0903747%3A2%3A5.json?token=1"), u);

    /* CatalogRepositoryImpl.buildCatalogUrl, all three shapes. */
    nuvio_catalog_url("https://c.io", "movie", "top", NULL, 0, u, sizeof u);
    TEST_ASSERT(!strcmp(u, "https://c.io/catalog/movie/top.json"), u);
    nuvio_catalog_url("https://c.io", "movie", "top", NULL, 100, u, sizeof u);
    TEST_ASSERT(!strcmp(u, "https://c.io/catalog/movie/top/skip=100.json"), u);
    nuvio_catalog_url("https://c.io?k=v", "series", "top", "the office", 0, u, sizeof u);
    TEST_ASSERT(!strcmp(u, "https://c.io/catalog/series/top/search=the%20office.json?k=v"), u);
    TEST_PASS();
}

static const char NUVIO_MANIFEST[] =
    "{\"id\":\"com.stremio.torrentio.addon\",\"version\":\"0.0.14\",\"name\":\"Torrentio\","
    "\"resources\":[\"stream\",{\"name\":\"meta\",\"types\":[\"anime\"],\"idPrefixes\":[\"kitsu\"]}],"
    "\"types\":[\"movie\",\"series\",\"anime\"],\"idPrefixes\":[\"tt\",\"kitsu\"],"
    "\"catalogs\":[{\"type\":\"movie\",\"id\":\"top\",\"name\":\"Popular\","
    "\"extra\":[{\"name\":\"search\"},{\"name\":\"skip\"},{\"name\":\"genre\",\"options\":[\"Action\"]}]},"
    "{\"type\":\"movie\",\"id\":\"search\",\"name\":\"Search\",\"extra\":[{\"name\":\"search\",\"isRequired\":true}]},"
    "{\"type\":\"movie\",\"id\":\"bygenre\",\"name\":\"By genre\",\"extraRequired\":[\"genre\"]}]}";

static void test_nuvio_manifest(void)
{
    TEST_START("nuvio: manifest + resource matching");
    static nuvio_addon_t a;
    TEST_ASSERT(nuvio_parse_manifest(NUVIO_MANIFEST, sizeof NUVIO_MANIFEST - 1, "https://t.io", &a) == 0, "parse");
    TEST_ASSERT(!strcmp(a.name, "Torrentio"), "name");
    TEST_ASSERT(a.catalog_count == 3, "catalog count");
    TEST_ASSERT(a.catalogs[0].supports_search && a.catalogs[0].supports_skip, "top extras");
    TEST_ASSERT(!a.catalogs[0].needs_other_extra && !a.catalogs[0].search_only, "optional genre is not required");
    TEST_ASSERT(a.catalogs[1].search_only, "required search");
    TEST_ASSERT(a.catalogs[2].needs_other_extra, "legacy extraRequired genre");

    /* "stream" is a bare string: manifest types + prefixes. */
    TEST_ASSERT(nuvio_addon_serves(&a, "stream", "movie", "tt0111161"), "movie tt");
    TEST_ASSERT(nuvio_addon_serves(&a, "stream", "series", "tt0903747:1:1"), "series tt");
    TEST_ASSERT(!nuvio_addon_serves(&a, "stream", "tv", "tt1"), "type not in manifest");
    TEST_ASSERT(!nuvio_addon_serves(&a, "stream", "movie", "tmdb:1"), "prefix not in manifest");
    /* "meta" object: its own types and prefixes. */
    TEST_ASSERT(nuvio_addon_serves(&a, "meta", "anime", "kitsu:1"), "meta anime kitsu");
    TEST_ASSERT(!nuvio_addon_serves(&a, "meta", "movie", "tt1"), "meta limited to anime");
    TEST_ASSERT(!nuvio_addon_serves(&a, "subtitles", "movie", "tt1"), "absent resource");
    TEST_ASSERT(nuvio_parse_manifest("{\"name\":\"x\"}", 12, "b", &a) == -1, "manifest with no id");
    TEST_PASS();
}

static void test_nuvio_catalog(void)
{
    TEST_START("nuvio: catalog metas -> folder items");
    static const char J[] =
        "{\"metas\":[{\"id\":\"tt0111161\",\"type\":\"movie\",\"name\":\"The Shawshank Redemption\","
        "\"poster\":\"https://p/1.jpg\",\"releaseInfo\":\"1994\",\"imdbRating\":\"9.3\"},"
        "{\"id\":\"tt0111161\",\"type\":\"movie\",\"name\":\"dup\"},"
        "{\"id\":\"kitsu:1\",\"name\":\"No type\",\"releaseInfo\":2001},"
        "{\"name\":\"no id\"}]}";
    evo_provider_item_t items[8];
    int raw = 0;
    int n = nuvio_parse_catalog(J, sizeof J - 1, "anime", items, 8, &raw);
    TEST_ASSERT(raw == 4, "raw count");
    TEST_ASSERT(n == 2, "dedupe + drop id-less");
    TEST_ASSERT(!strcmp(items[0].id, "m:movie:tt0111161"), items[0].id);
    TEST_ASSERT(items[0].is_folder, "folder");
    TEST_ASSERT(!strcmp(items[0].subtitle, "1994  -  IMDb 9.3"), items[0].subtitle);
    TEST_ASSERT(!strcmp(items[0].art_url, "https://p/1.jpg"), "poster");
    TEST_ASSERT(!strcmp(items[1].id, "m:anime:kitsu:1"), items[1].id);
    TEST_ASSERT(!strcmp(items[1].subtitle, "2001"), "numeric releaseInfo");

    /* An id too long for the item id is dropped, not truncated. */
    char big[600];
    char longid[300];
    memset(longid, 'a', sizeof longid - 1);
    longid[sizeof longid - 1] = 0;
    snprintf(big, sizeof big, "{\"metas\":[{\"id\":\"%s\",\"type\":\"movie\",\"name\":\"x\"}]}", longid);
    n = nuvio_parse_catalog(big, strlen(big), "movie", items, 8, &raw);
    TEST_ASSERT(n == 0 && raw == 1, "long id not dropped");
    TEST_ASSERT(nuvio_parse_catalog("nope", 4, "movie", items, 8, &raw) == -1, "bad json");
    TEST_PASS();
}

static void test_nuvio_meta(void)
{
    TEST_START("nuvio: series meta -> seasons + episodes");
    static const char J[] =
        "{\"meta\":{\"id\":\"tt0903747\",\"type\":\"series\",\"name\":\"Breaking Bad\",\"runtime\":\"49 min\","
        "\"videos\":[{\"id\":\"tt0903747:2:1\",\"title\":\"Seven Thirty-Seven\",\"season\":2,\"episode\":1},"
        "{\"id\":\"tt0903747:1:1\",\"name\":\"Pilot\",\"season\":1,\"number\":1},"
        "{\"id\":\"tt0903747:0:1\",\"title\":\"Special\",\"season\":0,\"episode\":1},"
        "{\"id\":\"tt0903747:1:2\",\"title\":\"Cat's in the Bag\",\"season\":\"1\",\"episode\":\"2\"}]}}";
    nuvio_meta_t m;
    TEST_ASSERT(nuvio_parse_meta(J, sizeof J - 1, &m) == 0, "parse");
    TEST_ASSERT(m.video_count == 4, "videos");
    TEST_ASSERT(m.runtime_sec == 49 * 60, "runtime");
    TEST_ASSERT(m.videos[1].episode == 1 && !strcmp(m.videos[1].title, "Pilot"), "number/name fallbacks");
    TEST_ASSERT(m.videos[3].season == 1 && m.videos[3].episode == 2, "string season/episode");
    int s[8];
    int ns = nuvio_meta_seasons(&m, s, 8);
    TEST_ASSERT(ns == 3 && s[0] == 1 && s[1] == 2 && s[2] == 0, "season order, specials last");
    nuvio_meta_free(&m);
    TEST_ASSERT(m.videos == NULL, "freed");
    TEST_PASS();
}

static void test_nuvio_streams(void)
{
    TEST_START("nuvio: streams, magnets, ordering");
    static const char J[] =
        "{\"streams\":["
        "{\"name\":\"Torrentio\\n720p\",\"title\":\"Show.S01E02.720p\\n\xf0\x9f\x91\xa4 50\",\"infoHash\":\"abcdef0123\","
        " \"fileIdx\":3,\"sources\":[\"tracker:udp://tr.example:1337/announce\",\"dht:abcdef0123\"],"
        " \"behaviorHints\":{\"filename\":\"Show S01E02 720p.mkv\",\"bingeGroup\":\"x\"}},"
        "{\"name\":\"Direct 4K\",\"url\":\"https://cdn.example/v.mkv\",\"behaviorHints\":{\"videoSize\":123456}},"
        "{\"name\":\"Needs headers 1080p\",\"url\":\"https://h.example/v.m3u8\","
        " \"behaviorHints\":{\"proxyHeaders\":{\"request\":{\"Referer\":\"https://h.example\"}}}},"
        "{\"name\":\"YouTube\",\"ytId\":\"dQw4w9WgXcQ\"},"
        "{\"name\":\"External\",\"externalUrl\":\"https://web.example\"}]}";
    static nuvio_stream_t st[8];
    int count = 0;
    int n = nuvio_parse_streams(J, sizeof J - 1, "Torrentio", 1, 2, st, 8, &count);
    TEST_ASSERT(n == 3 && count == 3, "yt/external skipped");
    TEST_ASSERT(st[0].needs_resolver && !strncmp(st[0].url, "magnet:?xt=urn:btih:abcdef0123&tr=udp%3A%2F%2F", 45), st[0].url);
    TEST_ASSERT(st[0].quality == 720, "quality from title");
    TEST_ASSERT(st[1].quality == 2160 && st[1].size_bytes == 123456, "4K + size");
    TEST_ASSERT(st[2].needs_headers, "proxy headers flagged");

    nuvio_magnet_hints_t h;
    TEST_ASSERT(nuvio_magnet_parse(st[0].url, &h) == 0, "magnet parse");
    TEST_ASSERT(!strcmp(h.info_hash, "abcdef0123"), "hash");
    TEST_ASSERT(h.file_idx == 3 && h.season == 1 && h.episode == 2, "hints");
    TEST_ASSERT(!strcmp(h.filename, "Show S01E02 720p.mkv"), h.filename);
    char clean[2048];
    TEST_ASSERT(nuvio_magnet_strip_hints(st[0].url, clean, sizeof clean) == 0, "strip");
    TEST_ASSERT(!strstr(clean, "evo_") && strstr(clean, "&tr=udp"), clean);
    TEST_ASSERT(nuvio_magnet_parse("https://x", &h) == -1, "not a magnet");

    nuvio_sort_streams(st, count);
    TEST_ASSERT(st[0].quality == 2160, "4K first");
    TEST_ASSERT(st[1].quality == 720 && !st[1].needs_headers, "then 720p");
    TEST_ASSERT(st[2].needs_headers, "header streams last");
    TEST_PASS();
}

/* ------------------------------------------------------------------------- */
/* End to end: the real providers against a fake addon / debrid / backend     */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *method;
    const char *path;        /* matched without the query unless it has one */
    int         status;
    const char *body;
    const char *body2;       /* served from the second call on, if set */
    int         calls;
} fake_route_t;

#define FAKE_MAX_ROUTES 32
#define FAKE_LOG_MAX    96
static fake_route_t   g_routes[FAKE_MAX_ROUTES];
static int            g_route_n;
static char           g_fake_log[FAKE_LOG_MAX][6144];
static int            g_fake_log_n;
static pthread_mutex_t g_fake_mx = PTHREAD_MUTEX_INITIALIZER;
static int            g_fake_ls = -1;
static int            g_fake_port;
static volatile int   g_fake_stop;
static pthread_t      g_fake_thr;

static void fake_route(const char *m, const char *p, int st, const char *b, const char *b2)
{
    fake_route_t *r = &g_routes[g_route_n++];
    r->method = m; r->path = p; r->status = st; r->body = b; r->body2 = b2; r->calls = 0;
}

static void *fake_conn(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char req[6144];
    size_t got = 0;
    char *hend = NULL;
    while (got < sizeof req - 1) {
        ssize_t n = recv(fd, req + got, sizeof req - 1 - got, 0);
        if (n <= 0) break;
        got += (size_t)n;
        req[got] = 0;
        hend = strstr(req, "\r\n\r\n");
        if (hend) {
            const char *cl = strstr(req, "Content-Length: ");
            size_t want = cl && cl < hend ? (size_t)atoi(cl + 16) : 0;
            if (got >= (size_t)(hend + 4 - req) + want) break;
        }
    }
    req[got] = 0;

    char method[16] = "", path[2048] = "";
    sscanf(req, "%15s %2047s", method, path);

    pthread_mutex_lock(&g_fake_mx);
    if (g_fake_log_n < FAKE_LOG_MAX)
        snprintf(g_fake_log[g_fake_log_n++], sizeof g_fake_log[0], "%s", req);
    const fake_route_t *hit = NULL;
    const char *body = "{}";
    int status = 404;
    for (int i = 0; i < g_route_n; ++i) {
        fake_route_t *r = &g_routes[i];
        if (strcmp(r->method, method) != 0) continue;
        size_t pl = strchr(r->path, '?') ? strlen(path) : strcspn(path, "?");
        if (strlen(r->path) != pl || strncmp(r->path, path, pl) != 0) continue;
        hit = r;
        body = (r->calls > 0 && r->body2) ? r->body2 : r->body;
        status = r->status;
        r->calls++;
        break;
    }
    pthread_mutex_unlock(&g_fake_mx);
    if (!hit) body = "{\"error\":\"no route\"}";

    char head[256];
    size_t bl = strlen(body);
    int hl = snprintf(head, sizeof head,
                      "HTTP/1.1 %d X\r\nContent-Type: application/json\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n", status, bl);
    (void)send(fd, head, (size_t)hl, 0);
    if (bl) (void)send(fd, body, bl, 0);
    close(fd);
    return NULL;
}

static void *fake_accept(void *arg)
{
    (void)arg;
    while (!g_fake_stop) {
        int fd = accept(g_fake_ls, NULL, NULL);
        if (fd < 0) break;
        pthread_t t;
        pthread_create(&t, NULL, fake_conn, (void *)(intptr_t)fd);
        pthread_detach(t);
    }
    return NULL;
}

static int fake_start(void)
{
    g_fake_ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(g_fake_ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_fake_ls, (struct sockaddr *)&a, sizeof a) != 0) return -1;
    if (listen(g_fake_ls, 32) != 0) return -1;
    socklen_t al = sizeof a;
    getsockname(g_fake_ls, (struct sockaddr *)&a, &al);
    g_fake_port = ntohs(a.sin_port);
    g_fake_stop = 0;
    return pthread_create(&g_fake_thr, NULL, fake_accept, NULL);
}

static void fake_stop(void)
{
    g_fake_stop = 1;
    shutdown(g_fake_ls, SHUT_RDWR);
    close(g_fake_ls);
    pthread_join(g_fake_thr, NULL);
}

/* The first logged request whose request line contains `needle`. */
static const char *fake_find(const char *needle)
{
    const char *hit = NULL;
    pthread_mutex_lock(&g_fake_mx);
    for (int i = 0; i < g_fake_log_n && !hit; ++i) {
        const char *eol = strstr(g_fake_log[i], "\r\n");
        size_t n = eol ? (size_t)(eol - g_fake_log[i]) : strlen(g_fake_log[i]);
        char line[2100];
        snprintf(line, sizeof line, "%.*s", (int)(n < 2099 ? n : 2099), g_fake_log[i]);
        if (strstr(line, needle)) hit = g_fake_log[i];
    }
    pthread_mutex_unlock(&g_fake_mx);
    return hit;
}

static int g_it_done, g_it_ok, g_it_n, g_it_more;
static evo_provider_item_t g_it[64];
static void items_cb(int ok, const evo_provider_item_t *it, int n, int more, void *ud)
{
    (void)ud;
    g_it_done = 1; g_it_ok = ok; g_it_n = n; g_it_more = more;
    for (int i = 0; i < n && i < 64; ++i) g_it[i] = it[i];
}

static int g_rs_done, g_rs_ok;
static evo_stream_choice_t g_rs;
static void resolve_cb(int ok, const evo_stream_choice_t *c, int n, void *ud)
{
    (void)ud;
    g_rs_done = 1; g_rs_ok = ok && n > 0;
    if (g_rs_ok) g_rs = c[0];
}

static void pump(volatile int *flag)
{
    for (int i = 0; i < 800 && !*flag; ++i) { evo_net_poll(); usleep(5000); }
}

static int list(const evo_provider_t *p, const char *id, int page)
{
    g_it_done = g_it_ok = g_it_n = g_it_more = 0;
    if (p->list_catalog(id, page, items_cb, NULL) != 0) return -1;
    pump(&g_it_done);
    return g_it_done && g_it_ok ? g_it_n : -1;
}

static char g_tmp[256];
static void write_text(const char *rel, const char *text)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", g_tmp, rel);
    FILE *f = fopen(p, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static int file_contains(const char *rel, const char *needle)
{
    char p[512], buf[16384];
    snprintf(p, sizeof p, "%s/%s", g_tmp, rel);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, needle) != NULL;
}

static char B_MANIFEST[1024], B_ACCOUNT_ADDONS[512];

static void test_nuvio_e2e_setup(void)
{
    TEST_START("nuvio e2e: fake servers + registry");
    TEST_ASSERT(fake_start() == 0, "fake server");

    snprintf(B_MANIFEST, sizeof B_MANIFEST,
        "{\"id\":\"test.addon\",\"version\":\"1\",\"name\":\"TestAddon\","
        "\"resources\":[\"catalog\",\"meta\",\"stream\"],\"types\":[\"movie\",\"series\"],"
        "\"idPrefixes\":[\"tt\"],\"catalogs\":[{\"type\":\"movie\",\"id\":\"top\",\"name\":\"Popular\","
        "\"extra\":[{\"name\":\"search\"},{\"name\":\"skip\"}]},"
        "{\"type\":\"movie\",\"id\":\"g\",\"name\":\"Genre\",\"extraRequired\":[\"genre\"]}]}");
    fake_route("GET", "/addon/manifest.json", 200, B_MANIFEST, NULL);
    fake_route("GET", "/addon/catalog/movie/top.json", 200,
        "{\"metas\":[{\"id\":\"tt1\",\"type\":\"movie\",\"name\":\"Film One\",\"poster\":\"http://p/1.jpg\"},"
        "{\"id\":\"tt2\",\"type\":\"series\",\"name\":\"Show Two\",\"description\":\"line1\\nline2\"}]}", NULL);
    fake_route("GET", "/addon/catalog/movie/top/skip=2.json", 200, "{\"metas\":[]}", NULL);
    fake_route("GET", "/addon/catalog/movie/top/search=film.json", 200,
        "{\"metas\":[{\"id\":\"tt1\",\"type\":\"movie\",\"name\":\"Film One\"}]}", NULL);
    fake_route("GET", "/addon/meta/movie/tt1.json", 200,
        "{\"meta\":{\"id\":\"tt1\",\"type\":\"movie\",\"name\":\"Film One\",\"poster\":\"http://p/1.jpg\"}}", NULL);
    fake_route("GET", "/addon/meta/series/tt2.json", 200,
        "{\"meta\":{\"id\":\"tt2\",\"type\":\"series\",\"name\":\"Show \\u00c9lan\","
        "\"videos\":[{\"id\":\"tt2:1:2\",\"title\":\"Second\",\"season\":1,\"episode\":2},"
        "{\"id\":\"tt2:1:1\",\"title\":\"First\",\"season\":1,\"episode\":1}]}}", NULL);
    fake_route("GET", "/addon/stream/movie/tt1.json", 200,
        "{\"streams\":[{\"name\":\"Test\\n720p\",\"url\":\"http://127.0.0.1:9/film720.mkv\"},"
        "{\"name\":\"Torrent\\n4k \xf0\x9f\x94\xa5\",\"infoHash\":\"deadbeef\",\"fileIdx\":0,"
        "\"behaviorHints\":{\"filename\":\"Film.One.2160p.mkv\"}}]}", NULL);
    fake_route("GET", "/addon/stream/series/tt2%3A1%3A2.json", 200,
        "{\"streams\":[{\"name\":\"Test\\n1080p\",\"url\":\"http://127.0.0.1:9/s1e2.mkv\"}]}", NULL);

    snprintf(g_tmp, sizeof g_tmp, "/tmp/evo_nuvio_test_%d", (int)getpid());
    mkdir(g_tmp, 0777);
    char d[400];
    snprintf(d, sizeof d, "%s/providers", g_tmp);            mkdir(d, 0777);
    snprintf(d, sizeof d, "%s/providers/nuvio-native", g_tmp); mkdir(d, 0777);
    char addons[128];
    snprintf(addons, sizeof addons, "http://127.0.0.1:%d/addon/manifest.json\n", g_fake_port);
    write_text("providers/nuvio-native/addons.txt", addons);
    write_text("torbox.conf", "key=TBKEY\n");

    setenv("EVO_DATA_DIR_OVERRIDE", g_tmp, 1);
    char api[96];
    snprintf(api, sizeof api, "http://127.0.0.1:%d/tb", g_fake_port);
    setenv("EVO_TORBOX_API", api, 1);
    snprintf(api, sizeof api, "http://127.0.0.1:%d/rd", g_fake_port);
    setenv("EVO_REALDEBRID_API", api, 1);
    evo_data_path_rebind();

    TEST_ASSERT(evo_provider_mgr_init() > 0, "registry");
    const evo_provider_t *nv = evo_provider_find("nuvio-native");
    TEST_ASSERT(nv && evo_provider_is_enabled("nuvio-native"), "nuvio-native registered + enabled");
    TEST_ASSERT(evo_provider_is_enabled("torbox"), "torbox enabled by its key");
    TEST_ASSERT(!evo_provider_is_enabled("realdebrid"), "realdebrid off without a key");
    TEST_PASS();
}

static void test_nuvio_e2e_browse(void)
{
    TEST_START("nuvio e2e: root -> catalog -> movie streams");
    const evo_provider_t *nv = evo_provider_find("nuvio-native");
    TEST_ASSERT(nv, "provider");

    int n = list(nv, "", 0);
    TEST_ASSERT(n == 1, "root rows (genre-required catalog hidden)");
    TEST_ASSERT(!strncmp(g_it[0].id, "c:0:movie:top", 13) && strstr(g_it[0].title, "Popular - Movies"), g_it[0].title);

    n = list(nv, "c:0:movie:top", 0);
    TEST_ASSERT(n == 2 && g_it_more == 1, "catalog page + has_more");
    TEST_ASSERT(!strcmp(g_it[1].overview, "line1 line2"), g_it[1].overview);
    n = list(nv, "c:0:movie:top", 1);
    TEST_ASSERT(n == 0 && fake_find("/catalog/movie/top/skip=2.json"), "page 2 by skip");

    n = list(nv, "m:movie:tt1", 0);
    TEST_ASSERT(n == 2, "movie goes straight to its streams");
    TEST_ASSERT(!strncmp(g_it[0].id, "x:", 2) && !g_it[0].is_folder, "stream rows are playable");
    TEST_ASSERT(strstr(g_it[0].title, "Torrent 4k") && !strstr(g_it[0].title, "\xf0"), "4K first, emoji stripped");
    TEST_ASSERT(strstr(g_it[0].subtitle, "via debrid"), g_it[0].subtitle);
    TEST_ASSERT(strstr(g_it[1].subtitle, "720p"), g_it[1].subtitle);

    const char *t = nv->play_title(g_it[1].id);
    TEST_ASSERT(t && !strcmp(t, "Film One"), t ? t : "null");

    /* Direct stream: resolves to itself. */
    g_rs_done = 0;
    TEST_ASSERT(evo_provider_resolve_chain("nuvio-native", g_it[1].id, resolve_cb, NULL) == 0, "chain");
    pump(&g_rs_done);
    TEST_ASSERT(g_rs_ok && !strcmp(g_rs.url, "http://127.0.0.1:9/film720.mkv"), g_rs.url);
    TEST_ASSERT(nv->resolve("x:999:0", resolve_cb, NULL) == -1, "stale stream id refused");
    TEST_PASS();
}

static char B_TB_FILES[512];

static void test_nuvio_e2e_torbox(void)
{
    TEST_START("nuvio e2e: magnet -> resolver chain -> Torbox");
    const evo_provider_t *nv = evo_provider_find("nuvio-native");
    snprintf(B_TB_FILES, sizeof B_TB_FILES,
        "{\"success\":true,\"data\":{\"id\":42,\"files\":["
        "{\"id\":7,\"name\":\"Film/sample.mkv\",\"size\":10,\"mimetype\":\"video/x-matroska\"},"
        "{\"id\":8,\"name\":\"Film/Film.One.2160p.mkv\",\"size\":9000,\"mimetype\":\"video/x-matroska\"},"
        "{\"id\":9,\"name\":\"Film/readme.txt\",\"size\":1}]}}");
    fake_route("POST", "/tb/torrents/createtorrent", 200, "{\"success\":true,\"data\":{\"torrent_id\":42}}", NULL);
    fake_route("GET",  "/tb/torrents/mylist", 200, B_TB_FILES, NULL);
    fake_route("GET",  "/tb/torrents/requestdl", 200, "{\"success\":true,\"data\":\"https://dl.tb.example/f.mkv\"}", NULL);

    /* A connected Emby sits ahead of Torbox in the table, and its resolve()
     * accepts any string. The chain must not offer it someone else's magnet. */
    emby_config_t *ec = emby_get_config();
    int emby_was = ec->is_connected;
    ec->is_connected = 1;
    evo_provider_set_enabled("emby", 1);

    TEST_ASSERT(list(nv, "m:movie:tt1", 0) == 2, "streams");
    g_rs_done = 0;
    TEST_ASSERT(evo_provider_resolve_chain("nuvio-native", g_it[0].id, resolve_cb, NULL) == 0, "chain");
    pump(&g_rs_done);
    ec->is_connected = emby_was;
    TEST_ASSERT(g_rs_ok && !strcmp(g_rs.url, "https://dl.tb.example/f.mkv"), g_rs.url);
    TEST_ASSERT(!strcmp(g_rs.label, "Torbox"), g_rs.label);

    const char *create = fake_find("/tb/torrents/createtorrent");
    TEST_ASSERT(create && strstr(create, "Authorization: Bearer TBKEY"), "bearer");
    TEST_ASSERT(strstr(create, "multipart/form-data; boundary=") &&
                strstr(create, "magnet:?xt=urn:btih:deadbeef") &&
                strstr(create, "name=\"add_only_if_cached\"\r\n\r\ntrue"), "multipart body");
    TEST_ASSERT(!strstr(create, "evo_"), "EVO's hints stripped before sending");
    TEST_ASSERT(fake_find("file_id=8&"), "filename hint picked the 2160p file, not the sample");
    TEST_PASS();
}

static void test_nuvio_e2e_realdebrid(void)
{
    TEST_START("nuvio e2e: magnet -> Real-Debrid (Torbox off)");
    const evo_provider_t *nv = evo_provider_find("nuvio-native");
    const evo_provider_t *rd = evo_provider_find("realdebrid");
    TEST_ASSERT(rd && rd->set_source("bad key!") == -1, "key with a space refused");
    TEST_ASSERT(rd->set_source("RDKEY") == 0 && rd->is_configured(), "key saved");
    TEST_ASSERT(file_contains("realdebrid.conf", "key=RDKEY"), "persisted");
    evo_provider_set_enabled("realdebrid", 1);
    evo_provider_set_enabled("torbox", 0);

    fake_route("POST", "/rd/torrents/addMagnet", 201, "{\"id\":\"ABC\",\"uri\":\"x\"}", NULL);
    fake_route("GET",  "/rd/torrents/info/ABC", 200,
        "{\"status\":\"waiting_files_selection\",\"files\":[{\"id\":1,\"path\":\"/Film.One.2160p.mkv\",\"bytes\":9000},"
        "{\"id\":2,\"path\":\"/x.nfo\",\"bytes\":1}]}",
        "{\"status\":\"downloaded\",\"links\":[\"https://real-debrid.com/d/XYZ\"]}");
    fake_route("POST", "/rd/torrents/selectFiles/ABC", 204, "", NULL);
    fake_route("POST", "/rd/unrestrict/link", 200,
        "{\"download\":\"https://rd.example/f.mkv\",\"filesize\":9000}", NULL);

    TEST_ASSERT(list(nv, "m:movie:tt1", 0) == 2, "streams");
    g_rs_done = 0;
    TEST_ASSERT(evo_provider_resolve_chain("nuvio-native", g_it[0].id, resolve_cb, NULL) == 0, "chain");
    pump(&g_rs_done);
    TEST_ASSERT(g_rs_ok && !strcmp(g_rs.url, "https://rd.example/f.mkv"), g_rs.url);

    const char *add = fake_find("/rd/torrents/addMagnet");
    TEST_ASSERT(add && strstr(add, "application/x-www-form-urlencoded") &&
                !strstr(add, "application/json") && strstr(add, "magnet=magnet%3A%3Fxt"), "form body, one content type");
    const char *sel = fake_find("/rd/torrents/selectFiles/ABC");
    TEST_ASSERT(sel && strstr(sel, "files=1"), "picked file 1");
    TEST_ASSERT(fake_find("/rd/unrestrict/link"), "unrestricted");
    evo_provider_set_enabled("torbox", 1);
    TEST_PASS();
}

static void test_nuvio_e2e_series_progress(void)
{
    TEST_START("nuvio e2e: series, progress, resume, search");
    const evo_provider_t *nv = evo_provider_find("nuvio-native");

    TEST_ASSERT(list(nv, "m:series:tt2", 0) == 1 && !strcmp(g_it[0].id, "s:1:series:tt2"), "one season");
    TEST_ASSERT(strstr(g_it[0].subtitle, "Show \xc3\x89lan") && strstr(g_it[0].subtitle, "2 episodes"),
                "\\u00c9 decoded to UTF-8");
    TEST_ASSERT(list(nv, "s:1:series:tt2", 0) == 2, "episodes");
    TEST_ASSERT(!strcmp(g_it[0].title, "1. First") && !strcmp(g_it[1].id, "v:series:tt2|tt2:1:2"),
                "sorted by episode");
    TEST_ASSERT(list(nv, "v:series:tt2|tt2:1:2", 0) == 1, "episode streams");
    const char *t = nv->play_title(g_it[0].id);
    TEST_ASSERT(t && !strcmp(t, "Show \xc3\x89lan - S01E02 - Second"), t ? t : "null");
    TEST_ASSERT(nv->resume_sec(g_it[0].id) == 0, "nothing to resume yet");

    g_rs_done = 0;
    TEST_ASSERT(nv->resolve(g_it[0].id, resolve_cb, NULL) == 0, "resolve");
    nv->report_progress(g_it[0].id, 0, 0, EVO_PROVIDER_PLAY_START);
    nv->report_progress(g_it[0].id, 600, 2400, EVO_PROVIDER_PLAY_UPDATE);
    nv->report_progress(g_it[0].id, 900, 2400, EVO_PROVIDER_PLAY_STOP);
    TEST_ASSERT(file_contains("providers/nuvio-native/progress.json", "\"progress_key\":\"tt2_s1e2\""), "NuvioTV key");
    TEST_ASSERT(file_contains("providers/nuvio-native/progress.json", "\"position\":900000"), "ms");
    TEST_ASSERT(nv->resume_sec(g_it[0].id) == 900, "resume from progress");

    TEST_ASSERT(list(nv, "", 0) == 2 && !strcmp(g_it[0].id, "cw"), "Continue Watching row");
    TEST_ASSERT(list(nv, "cw", 0) == 1 && !strcmp(g_it[0].id, "v:series:tt2|tt2:1:2"), "cw entry");
    TEST_ASSERT(strstr(g_it[0].subtitle, "S01E02") && strstr(g_it[0].subtitle, "37%"), g_it[0].subtitle);

    g_it_done = 0;
    TEST_ASSERT(nv->search("film", 0, items_cb, NULL) == 0, "search");
    pump(&g_it_done);
    TEST_ASSERT(g_it_ok && g_it_n == 1 && !strcmp(g_it[0].id, "m:movie:tt1"), "search hit");

    TEST_ASSERT(nv->set_source("stremio://other.example/manifest.json") == 0, "add addon");
    TEST_ASSERT(file_contains("providers/nuvio-native/addons.txt", "https://other.example/manifest.json"), "saved");
    TEST_ASSERT(nv->set_source("not a url") == -1, "garbage refused");
    TEST_ASSERT(nv->set_source("") == 0 &&
                file_contains("providers/nuvio-native/addons.txt", "other.example"),
                "empty (the setup page opening) keeps the list");
    TEST_PASS();
}

static char B_ACCT_CONF[512], B_PULL[512];

static void test_nuvio_e2e_account(void)
{
    TEST_START("nuvio e2e: account sign-in, addon + progress sync");
    const evo_provider_t *nv = evo_provider_find("nuvio-native");

    snprintf(B_ACCOUNT_ADDONS, sizeof B_ACCOUNT_ADDONS,
             "[{\"url\":\"http://127.0.0.1:%d/addon/manifest.json\",\"enabled\":true,\"sort_order\":0},"
             "{\"url\":\"https://disabled.example/manifest.json\",\"enabled\":false,\"sort_order\":1}]",
             g_fake_port);
    snprintf(B_PULL, sizeof B_PULL,
             "[{\"progress_key\":\"tt2_s1e1\",\"content_id\":\"tt2\",\"content_type\":\"series\","
             "\"video_id\":\"tt2:1:1\",\"season\":1,\"episode\":1,\"position\":60000,"
             "\"duration\":2400000,\"last_watched\":1700000000000}]");
    fake_route("POST", "/sb/auth/v1/token?grant_type=password", 200,
        "{\"access_token\":\"AT1\",\"refresh_token\":\"RT1\",\"expires_in\":3600,\"user\":{\"id\":\"u1\"}}", NULL);
    fake_route("POST", "/sb/rest/v1/rpc/get_sync_owner", 200, "\"owner-9\"", NULL);
    fake_route("GET",  "/sb/rest/v1/addons", 200, B_ACCOUNT_ADDONS, NULL);
    fake_route("POST", "/sb/rest/v1/rpc/sync_pull_watch_progress", 200, B_PULL, NULL);
    fake_route("POST", "/sb/rest/v1/rpc/sync_push_watch_progress", 204, "", NULL);

    snprintf(B_ACCT_CONF, sizeof B_ACCT_CONF,
             "backend_url=http://127.0.0.1:%d/sb/\npublishable_key=PK\nemail=a@b.c\npassword=hunter2\nprofile=2\n",
             g_fake_port);
    write_text("providers/nuvio-native/account.conf", B_ACCT_CONF);
    TEST_ASSERT(nv->init() == 0, "re-init (rebind)");

    int n = list(nv, "", 0);
    TEST_ASSERT(n >= 1, "root after account sync");
    for (int i = 0; i < 100; ++i) { evo_net_poll(); usleep(5000); }   /* progress pull/push */

    const char *tok = fake_find("grant_type=password");
    TEST_ASSERT(tok && strstr(tok, "apikey: PK") && strstr(tok, "\"password\":\"hunter2\""), "password grant");
    const char *ad = fake_find("/sb/rest/v1/addons?");
    TEST_ASSERT(ad && strstr(ad, "user_id=eq.owner-9") && strstr(ad, "profile_id=eq.2") &&
                strstr(ad, "Authorization: Bearer AT1"), "owner's addons, bearer");
    TEST_ASSERT(!file_contains("providers/nuvio-native/account.conf", "hunter2"), "password removed from disk");
    TEST_ASSERT(file_contains("providers/nuvio-native/account.conf", "refresh_token=RT1"), "refresh token kept");
    TEST_ASSERT(!file_contains("providers/nuvio-native/addons.txt", "disabled.example"), "disabled addon left out");
    TEST_ASSERT(file_contains("providers/nuvio-native/progress.json", "tt2_s1e1"), "pulled progress merged");

    const char *push = fake_find("sync_push_watch_progress");
    TEST_ASSERT(push && strstr(push, "\"p_entries\":[") && strstr(push, "\"progress_key\":\"tt2_s1e2\"") &&
                strstr(push, "\"p_profile_id\":2") && strstr(push, "\"p_origin_client_id\":\"evo-ps5-"),
                "dirty entry pushed in NuvioTV's shape");
    TEST_ASSERT(!strstr(push, "tt2_s1e1"), "pulled entry not pushed back");

    fake_stop();
    TEST_PASS();
}

static void test_nuvio_units_misc(void)
{
    TEST_START("nuvio: json writer, progress store, file pick");
    nuvio_jw_t w;
    nuvio_jw_init(&w);
    nuvio_jw_obj(&w, NULL);
    nuvio_jw_str(&w, "q", "a\"b\\c\n");
    nuvio_jw_i64(&w, "t", 1700000000123LL);
    nuvio_jw_arr(&w, "a");
    nuvio_jw_bool(&w, NULL, 1);
    nuvio_jw_null(&w, NULL);
    nuvio_jw_end_arr(&w);
    nuvio_jw_end_obj(&w);
    char *s = nuvio_jw_finish(&w);
    TEST_ASSERT(s && !strcmp(s, "{\"q\":\"a\\\"b\\\\c\\n\",\"t\":1700000000123,\"a\":[true,null]}"), s ? s : "null");
    free(s);
    nuvio_jw_init(&w);
    nuvio_jw_obj(&w, NULL);
    TEST_ASSERT(nuvio_jw_finish(&w) == NULL, "unbalanced document refused");

    static nuvio_progress_t p;
    TEST_ASSERT(nuvio_progress_init(&p) == 0, "init");
    nuvio_progress_entry_t e;
    memset(&e, 0, sizeof e);
    nuvio_progress_key("tt9", 0, 0, e.progress_key, sizeof e.progress_key);
    TEST_ASSERT(!strcmp(e.progress_key, "tt9"), "movie key");
    snprintf(e.content_id, sizeof e.content_id, "tt9");
    e.position_ms = 500; e.duration_ms = 1000; e.last_watched_ms = 10;
    nuvio_progress_upsert(&p, &e, 1);
    e.position_ms = 100; e.last_watched_ms = 5;                 /* older: ignored */
    nuvio_progress_upsert(&p, &e, 0);
    TEST_ASSERT(p.count == 1 && p.entries[0].position_ms == 500 && p.entries[0].dirty, "last write wins");
    e.position_ms = 950; e.last_watched_ms = 20;
    nuvio_progress_upsert(&p, &e, 1);
    int idx[4];
    TEST_ASSERT(nuvio_progress_continue(&p, idx, 4) == 0, "95% is completed, not continue");
    nuvio_progress_free(&p);

    nuvio_debrid_file_t f[3];
    memset(f, 0, sizeof f);
    snprintf(f[0].path, sizeof f[0].path, "Show/Show.S01E01.mkv"); f[0].size = 5; f[0].is_video = 1;
    snprintf(f[1].path, sizeof f[1].path, "Show/Show.S01E10.mkv"); f[1].size = 9; f[1].is_video = 1;
    snprintf(f[2].path, sizeof f[2].path, "Show/Show.1x02.mkv");   f[2].size = 7; f[2].is_video = 1;
    nuvio_magnet_hints_t h;
    memset(&h, 0, sizeof h);
    h.file_idx = -1; h.season = 1; h.episode = 1;
    TEST_ASSERT(nuvio_debrid_select(f, 3, &h) == 0, "S01E01 is not S01E10");
    h.episode = 2;
    TEST_ASSERT(nuvio_debrid_select(f, 3, &h) == 2, "1x02 form");
    h.episode = 7;
    TEST_ASSERT(nuvio_debrid_select(f, 3, &h) == -1, "a missing episode is a failure, not the biggest file");
    h.season = h.episode = 0;
    TEST_ASSERT(nuvio_debrid_select(f, 3, &h) == 1, "no hints: largest");
    TEST_PASS();
}

int main(void)
{
    printf("\n======================================================================\n");
    printf("  EVO Player — Automated Test Suite & Coverage Verification\n");
    printf("======================================================================\n\n");
    
    test_direct_mem_lifecycle();
    test_clean_media_title_resolution();
    test_emby_url_and_config();
    test_provider_bundle_path_safety();
    test_provider_url_escape();
    test_navigation_grid_and_focus();
    test_changelog_model_integrity();
    test_net_worker_pool_parallel();
    test_net_request_timeout();
    test_nuvio_urls();
    test_nuvio_manifest();
    test_nuvio_catalog();
    test_nuvio_meta();
    test_nuvio_streams();
    test_nuvio_units_misc();
    test_nuvio_e2e_setup();
    test_nuvio_e2e_browse();
    test_nuvio_e2e_torbox();
    test_nuvio_e2e_realdebrid();
    test_nuvio_e2e_series_progress();
    test_nuvio_e2e_account();

    printf("\n----------------------------------------------------------------------\n");
    printf("  Results: %d/%d passed (%d failed)\n",
           g_tests_passed, g_tests_run, g_tests_failed);
    printf("======================================================================\n\n");
    
    return (g_tests_failed == 0) ? 0 : 1;
}
