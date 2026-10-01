/*
 * tools/iptv_epg_host.c — Comprehensive host test for IPTV XMLTV EPG parsing.
 *
 * Compiles provider_iptv.c in host environment with mocked net/ui hooks,
 * feeds it real M3U and XMLTV EPG data, and asserts on parsed EPG fields.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "evo_net.h"
#include "evo_provider.h"

void evo_provider_item_clear(evo_provider_item_t *it)
{
    if (!it) return;
    memset(it, 0, sizeof *it);
}

void evo_provider_stream_choice_clear(evo_stream_choice_t *c)
{
    if (!c) return;
    memset(c, 0, sizeof *c);
}

/* Mock symbols needed by provider_iptv.c */
static int g_reload_called = 0;
void evo_rmlui_provider_reload(void)
{
    g_reload_called++;
}

int evo_net_request_async(const char *method, const char *url,
                          const char *post_data, const char **headers,
                          int header_count, evo_net_cb callback, void *user_data)
{
    (void)method; (void)url; (void)post_data; (void)headers;
    (void)header_count; (void)callback; (void)user_data;
    return 0;
}

const char *evo_data_path(const char *filename)
{
    return filename;
}

int favorites_is_favorite(const char *key)
{
    (void)key;
    return 0;
}

/* Include provider_iptv.c directly to access internals and static functions */
#include "../projects/evoplayer/addons/src/provider_iptv.c"

static char *read_file(const char *path, size_t *out_sz)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    if (out_sz) *out_sz = got;
    return buf;
}

int main(int argc, char **argv)
{
    const char *m3u_path = (argc > 1) ? argv[1] : "tests/fixtures/Canais_live.m3u";
    const char *xmltv_path = (argc > 2) ? argv[2] : "tests/fixtures/kiwi_sample.xml";

    printf("=================================================================\n");
    printf("  IPTV EPG C Parser Host Verification\n");
    printf("=================================================================\n");
    printf("  M3U file   : %s\n", m3u_path);
    printf("  XMLTV file : %s\n", xmltv_path);

    size_t m3u_len = 0;
    char *m3u_data = read_file(m3u_path, &m3u_len);
    if (!m3u_data) {
        fprintf(stderr, "FATAL: Failed to read %s\n", m3u_path);
        return 1;
    }

    size_t xmltv_len = 0;
    char *xmltv_data = read_file(xmltv_path, &xmltv_len);
    if (!xmltv_data) {
        fprintf(stderr, "FATAL: Failed to read %s\n", xmltv_path);
        free(m3u_data);
        return 1;
    }

    printf("  Loaded M3U   : %zu bytes\n", m3u_len);
    printf("  Loaded XMLTV : %zu bytes\n\n", xmltv_len);

    /* Step 1: Parse M3U */
    int ch_count = parse_m3u(m3u_data, m3u_len);
    printf("[1] parse_m3u: returned %d channels (G.ch_count=%d, G.gr_count=%d)\n",
           ch_count, G.ch_count, G.gr_count);
    assert(ch_count > 0);
    assert(G.ch_count > 0);

    /* Step 2: Parse XMLTV */
    printf("[2] Running parse_xmltv on %zu bytes...\n", xmltv_len);
    clock_t t0 = clock();
    parse_xmltv(xmltv_data, xmltv_len);
    clock_t t1 = clock();
    double parse_sec = (double)(t1 - t0) / CLOCKS_PER_SEC;
    printf("    parse_xmltv completed in %.3f seconds\n", parse_sec);

    /* Step 3: Evaluate EPG mapping results */
    int mapped_tvg = 0;
    int mapped_now = 0;
    int mapped_next = 0;
    int mapped_logo = 0;

    for (int i = 0; i < G.ch_count; i++) {
        if (G.ch[i].tvg_id && G.ch[i].tvg_id[0]) mapped_tvg++;
        if (G.ch[i].now && G.ch[i].now[0])       mapped_now++;
        if (G.ch[i].next && G.ch[i].next[0])     mapped_next++;
        if (G.ch[i].logo && G.ch[i].logo[0])     mapped_logo++;
    }

    printf("\n[3] EPG Mapping Statistics:\n");
    printf("    Total channels      : %d\n", G.ch_count);
    printf("    Channels with tvg_id: %d (%.1f%%)\n", mapped_tvg, (mapped_tvg * 100.0) / G.ch_count);
    printf("    Channels with logo  : %d (%.1f%%)\n", mapped_logo, (mapped_logo * 100.0) / G.ch_count);
    printf("    Channels with NOW   : %d (%.1f%%)\n", mapped_now, (mapped_now * 100.0) / G.ch_count);
    printf("    Channels with NEXT  : %d (%.1f%%)\n", mapped_next, (mapped_next * 100.0) / G.ch_count);

    /* Step 4: Sample first 5 channels as seen on Screen Page 1 */
    printf("\n[4] Page 1 (first 5 channels):\n");
    for (int i = 0; i < 5 && i < G.ch_count; i++) {
        evo_provider_item_t item;
        fill_channel_item(&item, i, "root");
        printf("    CH %-4d: '%s'\n", i, item.title);
        printf("      NOW     : %s\n", item.now_title[0] ? item.now_title : "(none)");
        printf("      NEXT    : %s\n", item.next_title[0] ? item.next_title : "(none)");
        printf("      SUBTITLE: %s\n", item.subtitle);
    }

    /* Step 5: Sample first 10 channels with EPG items */
    printf("\n[5] First 10 channels with matched EPG items:\n");
    int shown = 0;
    for (int i = 0; i < G.ch_count && shown < 10; i++) {
        if (G.ch[i].now || G.ch[i].next) {
            evo_provider_item_t item;
            fill_channel_item(&item, i, "root");
            printf("    CH %-4d: '%s'\n", i, item.title);
            printf("      NOW     : %s\n", item.now_title[0] ? item.now_title : "(none)");
            printf("      NEXT    : %s\n", item.next_title[0] ? item.next_title : "(none)");
            printf("      SUBTITLE: %s\n", item.subtitle);
            shown++;
        }
    }

    /* Assertions for test pass */
    assert(mapped_tvg > 0);
    assert(mapped_now > 0 || mapped_next > 0);

    printf("\n=================================================================\n");
    printf("  EPG TEST PASSED: Successfully mapped channels & guides!\n");
    printf("=================================================================\n");

    free(m3u_data);
    free(xmltv_data);
    free_channels();

    return 0;
}
