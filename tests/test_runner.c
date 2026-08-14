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

#include "pp_compute_pipeline.h"
#include "evo_direct_mem.h"
#include "evo_draw.h"
#include "evo_focus.h"
#include "evo_theme.h"
#include "evo_widgets.h"
#include "evo_screens.h"
#include "evo_addon.h"
#include "addon_emby.h"
#include "evo_changelog.h"
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

static void test_compute_pipeline_init(void)
{
    TEST_START("Compute Pipeline: Initialization & Backend Info");
    pp_compute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = PP_COMPUTE_BACKEND_CPU_SIMD;
    cfg.num_workers = 4;
    int rc = pp_compute_pipeline_init(&cfg);
    TEST_ASSERT(rc == 0, "pp_compute_pipeline_init failed");
    
    const char *bname = pp_compute_pipeline_get_backend_name();
    TEST_ASSERT(bname != NULL, "Backend name was NULL");
    TEST_ASSERT(strstr(bname, "CPU SIMD") != NULL || strstr(bname, "AVX2") != NULL || strstr(bname, "Workgroups") != NULL,
                "Backend name does not match expected SIMD pipeline string");
    
    TEST_PASS();
}

static void test_compute_pipeline_conversion_accuracy(void)
{
    TEST_START("Compute Pipeline: YUV420P to RGB Conversion Accuracy");
    const int w = 64;
    const int h = 64;
    
    uint8_t *y_plane = (uint8_t *)malloc(w * h);
    uint8_t *u_plane = (uint8_t *)malloc((w / 2) * (h / 2));
    uint8_t *v_plane = (uint8_t *)malloc((w / 2) * (h / 2));
    uint32_t *dst_rgb = (uint32_t *)malloc(w * h * sizeof(uint32_t));
    
    TEST_ASSERT(y_plane && u_plane && v_plane && dst_rgb, "Buffer allocation failed");
    
    /* Standard HD Neutral Gray: Y=128, U=128, V=128 */
    memset(y_plane, 128, w * h);
    memset(u_plane, 128, (w / 2) * (h / 2));
    memset(v_plane, 128, (w / 2) * (h / 2));
    memset(dst_rgb, 0, w * h * sizeof(uint32_t));
    
    pp_frame src;
    memset(&src, 0, sizeof(src));
    src.width = w;
    src.height = h;
    src.format = PP_FRAME_YUV420P;
    src.planes[0] = y_plane;
    src.planes[1] = u_plane;
    src.planes[2] = v_plane;
    src.strides[0] = w;
    src.strides[1] = w / 2;
    src.strides[2] = w / 2;
    
    int rc = pp_compute_pipeline_convert(&src, dst_rgb, w, h, 1);
    TEST_ASSERT(rc == 0, "Conversion returned error");
    
    /* Check middle pixel RGB values (BT.709/601 neutral gray ~128 +/- 5) */
    uint32_t sample = dst_rgb[(h / 2) * w + (w / 2)];
    uint8_t r = (sample >> 16) & 0xFF;
    uint8_t g = (sample >> 8) & 0xFF;
    uint8_t b = sample & 0xFF;
    
    TEST_ASSERT(abs((int)r - 128) <= 6, "Red channel deviation out of bounds");
    TEST_ASSERT(abs((int)g - 128) <= 6, "Green channel deviation out of bounds");
    TEST_ASSERT(abs((int)b - 128) <= 6, "Blue channel deviation out of bounds");
    
    free(y_plane);
    free(u_plane);
    free(v_plane);
    free(dst_rgb);
    
    TEST_PASS();
}

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
    
    evo_direct_mem_shutdown();
    TEST_PASS();
}

/* ==========================================================================
 * 3. UI Typography, Text Fitting & Title Cleaning Tests
 * ========================================================================== */

static void test_ui_text_measurement_and_fitting(void)
{
    TEST_START("UI Typography: Text Width & Ellipsis Fitting");
    
    const char *test_str = "The Quick Brown Fox Jumps Over The Lazy Dog";
    int w_small = evo_text_w(test_str, EVO_FACE_SMALL);
    int w_sub   = evo_text_w(test_str, EVO_FACE_SUB);
    int w_menu  = evo_text_w(test_str, EVO_FACE_MENU);
    int w_title = evo_text_w(test_str, EVO_FACE_TITLE);
    
    TEST_ASSERT(w_small > 0 && w_sub > w_small && w_menu > w_sub && w_title > w_menu,
                "Font face widths do not scale hierarchically");
    
    /* Test evo_text_fit */
    uint32_t *fake_fb = (uint32_t *)calloc(1920 * 100, sizeof(uint32_t));
    TEST_ASSERT(fake_fb != NULL, "Allocation failed");
    
    int fitted_w = evo_text_fit(fake_fb, 0, 0, 200, test_str, 0xFFFFFFFF, EVO_FACE_SUB);
    TEST_ASSERT(fitted_w <= 200, "Fitted text width exceeds max_w");
    
    /* Short string should fit completely without ellipsis */
    int short_w = evo_text_fit(fake_fb, 0, 0, 200, "EVO", 0xFFFFFFFF, EVO_FACE_SUB);
    TEST_ASSERT(short_w > 0 && short_w <= 200, "Short string width is invalid");
    
    free(fake_fb);
    TEST_PASS();
}

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

static void test_common_ui_widgets_and_surround_screen(void)
{
    TEST_START("Common UI: Badges, Stat Cards & Surround Studio Screen");

    uint32_t *mock_fb = (uint32_t *)calloc(1920 * 1080, sizeof(uint32_t));
    TEST_ASSERT(mock_fb != NULL, "Failed to allocate mock framebuffer");

    /* 1. Test Categorical Badges */
    evo_widget_category_badge(mock_fb, 100, 100, 90, 26, EVO_BADGE_ACCENT, "NEW");
    evo_widget_category_badge(mock_fb, 200, 100, 90, 26, EVO_BADGE_SUCCESS, "FIXED");
    evo_widget_category_badge(mock_fb, 300, 100, 90, 26, EVO_BADGE_WARNING, "IMPROVED");
    evo_widget_category_badge(mock_fb, 400, 100, 90, 26, EVO_BADGE_DANGER, "REMOVED");

    /* 2. Test Stat / Monitor Card */
    evo_stat_card card;
    memset(&card, 0, sizeof(card));
    card.header_label = "SPEAKER CALIBRATION MONITOR";
    card.title = "FRONT LEFT (FL)";
    card.line1 = "TONE FREQ: 330.0 HZ";
    card.line2 = "PS5 AUDIO OUT: S16_8CH (CH 0)";
    card.status_text = "STATUS: [ ACTIVE NOW ]";
    card.is_active = 1;
    evo_widget_stat_card(mock_fb, 130, 160, 460, 220, &card);

    /* 3. Test Speaker Stage Node */
    evo_speaker_node node;
    memset(&node, 0, sizeof(node));
    node.label = "FL";
    node.sub = "330 Hz [ON]";
    node.is_active = 1;
    node.is_selected = 1;
    evo_widget_speaker_node(mock_fb, 600, 300, 150, 82, &node);

    /* 4. Test Full Surround Sound Studio Screen Rendering (5.1 & 7.1) */
    static const evo_surround_speaker_info test_spk[8] = {
        { "CENTER",       "FC",   554.0,  -75, -260, 150, 82, 2, 6 },
        { "SUBWOOFER",   "LFE",   55.0,   85, -260, 150, 82, 3, 8 },
        { "FRONT LEFT",   "FL",  330.0, -460, -180, 150, 82, 0, 5 },
        { "FRONT RIGHT",  "FR",  440.0,  310, -180, 150, 82, 1, 7 },
        { "SIDE LEFT",    "SL", 1109.0, -510,   10, 150, 82, 6, 9 },
        { "SIDE RIGHT",   "SR", 1319.0,  360,   10, 150, 82, 7, 10 },
        { "BACK LEFT",    "BL",  659.0, -380,  200, 150, 82, 4, 11 },
        { "BACK RIGHT",   "BR",  880.0,  230,  200, 150, 82, 5, 12 }
    };

    evo_surround_test_model m;
    memset(&m, 0, sizeof(m));
    m.is_51_layout   = 1;
    m.selected_item  = 0;
    m.active_channel = 0;
    m.surround_mode  = 1;
    m.speakers       = test_spk;
    m.speaker_count  = 8;

    static const evo_hint hints[] = {
        { EVO_GLYPH_CROSS, "TEST" },
        { EVO_GLYPH_CIRCLE, "BACK" }
    };

    evo_screen_surround_test(mock_fb, &m, 0, 0, hints, 2);

    /* Switch to 7.1 layout and render */
    m.is_51_layout = 0;
    m.active_channel = 6;
    evo_screen_surround_test(mock_fb, &m, 0, 0, hints, 2);

    free(mock_fb);
    TEST_PASS();
}

/* ==========================================================================
 * Mock Drawing Vtable for Host UI Tests
 * ========================================================================== */

static int test_text_w(const char *s, int face)
{
    if (!s) return 0;
    int char_w = (face == 0) ? 8 : (face == 1) ? 11 : (face == 2) ? 14 : 18;
    return (int)strlen(s) * char_w;
}

static void test_text_draw(uint32_t *fb, int x, int y, const char *s, uint32_t c, int face)
{
    (void)fb; (void)x; (void)y; (void)s; (void)c; (void)face;
}

static const evo_draw_vtable g_test_vtable = {
    .text = test_text_draw,
    .text_w = test_text_w,
    .icon = NULL,
    .icon_tinted = NULL,
    .glyph = NULL,
    .glyph_tinted = NULL
};

/* ==========================================================================
 * Main Test Runner Entrypoint
 * ========================================================================== */

int main(void)
{
    evo_draw_bind(&g_test_vtable);

    printf("\n======================================================================\n");
    printf("  EVO Player — Automated Test Suite & Coverage Verification\n");
    printf("======================================================================\n\n");
    
    test_compute_pipeline_init();
    test_compute_pipeline_conversion_accuracy();
    test_direct_mem_lifecycle();
    test_ui_text_measurement_and_fitting();
    test_clean_media_title_resolution();
    test_emby_url_and_config();
    test_navigation_grid_and_focus();
    test_changelog_model_integrity();
    test_common_ui_widgets_and_surround_screen();
    
    printf("\n----------------------------------------------------------------------\n");
    printf("  Results: %d/%d passed (%d failed)\n",
           g_tests_passed, g_tests_run, g_tests_failed);
    printf("======================================================================\n\n");
    
    return (g_tests_failed == 0) ? 0 : 1;
}
