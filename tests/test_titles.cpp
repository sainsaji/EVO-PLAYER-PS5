/*
 * tests/test_titles.cpp - readable titles from release file names
 * (evo::tidyReleaseName) and server addresses from media URLs
 * (NetworkStorageService::parseServerUrl, behind Home's SERVER OFFLINE).
 */

#include "evo/services/ReleaseName.hpp"
#include "evo/services/NetworkStorageService.hpp"

#include <cstdio>
#include <string>

extern "C" {
    extern int g_tests_run;
    extern int g_tests_passed;
    extern int g_tests_failed;
}

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

static bool tidy_is(const char* in, const char* want) {
    const std::string got = evo::tidyReleaseName(in);
    if (got == want) return true;
    printf("\n       tidy('%s') = '%s', want '%s'", in, got.c_str(), want);
    return false;
}

static void test_tidy_release_names(void) {
    TEST_START("Titles: release tags, year, site prefix");
    TEST_ASSERT(tidy_is("Clarksons Farm S01E08 720p AMZN WEBRip x264-GalaxyTV", "Clarksons Farm S01E08"), "episode");
    TEST_ASSERT(tidy_is("Blade Runner 2049 2017 1080p BluRay x264", "Blade Runner 2049 (2017)"), "title number vs year");
    TEST_ASSERT(tidy_is("Mad Max Fury Road 2015 2160p UHD BluRay REMUX HDR HEVC", "Mad Max Fury Road (2015)"),
                "Max is a title word");
    TEST_ASSERT(tidy_is("Charlottes Web 2006 1080p", "Charlottes Web (2006)"), "Web is a title word");
    TEST_ASSERT(tidy_is("[SubsPlease] Frieren - 01 (1080p) [ABCD1234]", "Frieren - 01"), "group prefix");
    TEST_ASSERT(tidy_is("Spider-Man No Way Home 2021 WEB-DL DDP5 1 H 264-GROUP", "Spider-Man No Way Home (2021)"),
                "hyphenated title");
    TEST_ASSERT(tidy_is("Movie (2019) [1080p]", "Movie (2019)"), "bracketed year");
    TEST_ASSERT(tidy_is("Some Show S02E03 Episode Name 1080p WEB-DL", "Some Show S02E03 Episode Name"), "episode name kept");
    TEST_ASSERT(tidy_is("www 5MovieRulz company - Bethlehem Kudumba Unit (2026) 1080p Malayalam HQ HDRip - x264",
                        "Bethlehem Kudumba Unit (2026)"),
                "site prefix");
    TEST_PASS();
}

static void test_tidy_plain_and_captures(void) {
    TEST_START("Titles: plain names and capture timestamps");
    TEST_ASSERT(tidy_is("ESub", "ESub"), "one word");
    TEST_ASSERT(tidy_is("native", "native"), "lower case word");
    TEST_ASSERT(tidy_is("GTAVI An Extended Look", "GTAVI An Extended Look"), "Extended is a title word");
    TEST_ASSERT(tidy_is("20261028 225403 00907", "28 Oct 2026, 22:54"), "capture timestamp");
    TEST_ASSERT(tidy_is("20261399 225403", "20261399 225403"), "not a real date");
    TEST_ASSERT(tidy_is("", ""), "empty");
    TEST_PASS();
}

static void test_parse_server_url(void) {
    TEST_START("Server URL: host, port, protocol");
    std::string host;
    int port = 0;
    bool ftp = false;
    TEST_ASSERT(evo::NetworkStorageService::parseServerUrl("smb://192.168.0.9/Share/a.mkv", host, port, ftp),
                "smb parses");
    TEST_ASSERT(host == "192.168.0.9" && port == 445 && !ftp, "smb default port");
    TEST_ASSERT(evo::NetworkStorageService::parseServerUrl("ftp://user:p%40ss@nas.local:2121/m/a.mkv", host, port, ftp),
                "ftp parses");
    TEST_ASSERT(host == "nas.local" && port == 2121 && ftp, "ftp with credentials and port");
    TEST_ASSERT(evo::NetworkStorageService::parseServerUrl("ftp://10.0.0.2", host, port, ftp), "no path");
    TEST_ASSERT(host == "10.0.0.2" && port == 21, "ftp default port");
    TEST_ASSERT(!evo::NetworkStorageService::parseServerUrl("/mnt/usb0/a.mkv", host, port, ftp), "local path");
    TEST_ASSERT(!evo::NetworkStorageService::parseServerUrl("evo://jellyfin/123", host, port, ftp), "provider");
    TEST_ASSERT(!evo::NetworkStorageService::parseServerUrl("smb:///Share", host, port, ftp), "no host");
    TEST_PASS();
}

extern "C" void run_title_tests(void) {
    test_tidy_release_names();
    test_tidy_plain_and_captures();
    test_parse_server_url();
}

__attribute__((constructor)) static void auto_run_title_tests(void) {
    run_title_tests();
}
