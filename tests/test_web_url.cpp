/*
 * tests/test_web_url.cpp - #237: what the Web section's address field makes
 * of typed text.
 */

#include "evo_web_url.h"

#include <cstdio>
#include <cstring>

extern "C" {
    extern int g_tests_run;
    extern int g_tests_passed;
    extern int g_tests_failed;
}

#define TEST_START(name) do { g_tests_run++; printf("  RUN  %-50s", name); fflush(stdout); } while (0)
#define TEST_PASS() do { g_tests_passed++; printf(" [ PASS ]\n"); } while (0)
#define TEST_FAIL(reason) do { g_tests_failed++; printf(" [ FAIL ] (%s at line %d)\n", reason, __LINE__); } while (0)
#define TEST_ASSERT(cond, reason) do { if (!(cond)) { TEST_FAIL(reason); return; } } while (0)

static int norm(const char *in, const char *want, int kind)
{
    char out[256];
    const int k = evo_web_normalize_input(in, out, sizeof out);
    if (k != kind || std::strcmp(out, want) != 0) {
        printf("\n    '%s' -> '%s' (%d), wanted '%s' (%d)", in ? in : "(null)", out, k, want, kind);
        return 0;
    }
    return 1;
}

static void test_addresses()
{
    TEST_START("web url: addresses get a scheme");
    TEST_ASSERT(norm("example.com", "https://example.com", EVO_WEB_INPUT_URL), "bare host");
    TEST_ASSERT(norm("  en.wikipedia.org/wiki/PS5 ", "https://en.wikipedia.org/wiki/PS5", EVO_WEB_INPUT_URL), "path, blanks");
    TEST_ASSERT(norm("https://x.org/a?b=c", "https://x.org/a?b=c", EVO_WEB_INPUT_URL), "kept");
    TEST_ASSERT(norm("HTTP://X.ORG", "HTTP://X.ORG", EVO_WEB_INPUT_URL), "scheme any case");
    TEST_ASSERT(norm("about:blank", "about:blank", EVO_WEB_INPUT_URL), "about:");
    TEST_PASS();
}

static void test_lan()
{
    TEST_START("web url: LAN servers use http");
    TEST_ASSERT(norm("192.168.0.5:8096", "http://192.168.0.5:8096", EVO_WEB_INPUT_URL), "ip:port");
    TEST_ASSERT(norm("10.0.0.2/web", "http://10.0.0.2/web", EVO_WEB_INPUT_URL), "ip/path");
    TEST_ASSERT(norm("localhost:8080", "http://localhost:8080", EVO_WEB_INPUT_URL), "localhost");
    TEST_ASSERT(norm("1.2.3", "https://1.2.3", EVO_WEB_INPUT_URL), "not a full ip: a dotted host");
    TEST_PASS();
}

static void test_searches()
{
    TEST_START("web url: other text is a search");
    TEST_ASSERT(norm("cats and dogs", "https://duckduckgo.com/?q=cats+and+dogs", EVO_WEB_INPUT_SEARCH), "words");
    TEST_ASSERT(norm("ps5", "https://duckduckgo.com/?q=ps5", EVO_WEB_INPUT_SEARCH), "one word");
    TEST_ASSERT(norm("c++ & go?", "https://duckduckgo.com/?q=c%2B%2B+%26+go%3F", EVO_WEB_INPUT_SEARCH), "encoded");
    TEST_ASSERT(norm("end.", "https://duckduckgo.com/?q=end.", EVO_WEB_INPUT_SEARCH), "trailing dot");
    TEST_ASSERT(norm("example.com is down", "https://duckduckgo.com/?q=example.com+is+down", EVO_WEB_INPUT_SEARCH), "spaces");
    TEST_PASS();
}

static void test_empty_and_cut()
{
    TEST_START("web url: empty input, small buffers");
    TEST_ASSERT(norm("   ", "", EVO_WEB_INPUT_EMPTY), "blanks");
    TEST_ASSERT(norm(nullptr, "", EVO_WEB_INPUT_EMPTY), "null");
    char small[12];
    evo_web_normalize_input("example.com", small, sizeof small);
    TEST_ASSERT(std::strlen(small) == sizeof small - 1, "cut, terminated");
    char tiny[8];
    evo_web_normalize_input("a b c d e f", tiny, sizeof tiny);
    TEST_ASSERT(std::strlen(tiny) < sizeof tiny, "search cut, terminated");
    TEST_PASS();
}

static void test_host()
{
    TEST_START("web url: host label");
    char h[64];
    evo_web_url_host("https://en.m.wikipedia.org/wiki/X", h, sizeof h);
    TEST_ASSERT(std::strcmp(h, "en.m.wikipedia.org") == 0, "host");
    evo_web_url_host("http://user:pw@nas.local:8096/web?x", h, sizeof h);
    TEST_ASSERT(std::strcmp(h, "nas.local:8096") == 0, "credentials dropped");
    evo_web_url_host("", h, sizeof h);
    TEST_ASSERT(h[0] == 0, "empty");
    TEST_PASS();
}

extern "C" void run_web_url_tests(void)
{
    printf("\n[web url]\n");
    test_addresses();
    test_lan();
    test_searches();
    test_empty_and_cut();
    test_host();
}

__attribute__((constructor)) static void auto_run_web_url_tests(void)
{
    run_web_url_tests();
}
