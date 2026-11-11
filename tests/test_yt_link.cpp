/*
 * tests/test_yt_link.cpp - which text counts as a YouTube video. The text
 * comes from the phone page, the cast port or the keyboard, so anything that
 * is not an 11-character id must be refused before it reaches a request.
 */

#include "evo_yt_link.h"

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

static int id_of(const char *link, const char *want)
{
    char id[16] = "unset";
    const int rc = evo_yt_link_video_id(link, id, sizeof id);
    if (want == nullptr)
    {
        if (rc == 0)
            printf("\n    '%s' -> '%s', wanted no id", link ? link : "(null)", id);
        return rc != 0;
    }
    if (rc != 0 || std::strcmp(id, want) != 0)
    {
        printf("\n    '%s' -> rc %d '%s', wanted '%s'", link ? link : "(null)", rc, id, want);
        return 0;
    }
    return 1;
}

static void test_link_forms()
{
    TEST_START("yt link: every link form gives the id");
    TEST_ASSERT(id_of("https://www.youtube.com/watch?v=aqz-KE-bpKQ", "aqz-KE-bpKQ"), "watch");
    TEST_ASSERT(id_of("https://www.youtube.com/watch?feature=share&v=aqz-KE-bpKQ&t=30", "aqz-KE-bpKQ"), "&v=");
    TEST_ASSERT(id_of("https://youtu.be/aqz-KE-bpKQ?si=abc", "aqz-KE-bpKQ"), "youtu.be");
    TEST_ASSERT(id_of("https://youtube.com/shorts/aqz-KE-bpKQ", "aqz-KE-bpKQ"), "shorts");
    TEST_ASSERT(id_of("https://www.youtube.com/embed/aqz-KE-bpKQ", "aqz-KE-bpKQ"), "embed");
    TEST_ASSERT(id_of("https://www.youtube.com/live/aqz-KE-bpKQ", "aqz-KE-bpKQ"), "live");
    TEST_ASSERT(id_of("https://music.youtube.com/watch?v=aqz-KE-bpKQ", "aqz-KE-bpKQ"), "music");
    TEST_ASSERT(id_of("  aqz-KE-bpKQ\r\n", "aqz-KE-bpKQ"), "bare id, blanks");
    TEST_PASS();
}

static void test_refused()
{
    TEST_START("yt link: other text gets no id");
    TEST_ASSERT(id_of("https://example.com/watch?v=aqz-KE-bpKQ", nullptr), "not youtube");
    TEST_ASSERT(id_of("https://youtu.be/short", nullptr), "too short");
    TEST_ASSERT(id_of("https://youtu.be/aqz-KE-bpKQX", nullptr), "too long");
    TEST_ASSERT(id_of("https://youtu.be/aqz\"KE-bpKQ", nullptr), "quote in id");
    TEST_ASSERT(id_of("", nullptr), "empty");
    TEST_ASSERT(id_of(nullptr, nullptr), "null");
    char small[8];
    TEST_ASSERT(evo_yt_link_video_id("aqz-KE-bpKQ", small, sizeof small) != 0, "buffer too small");
    TEST_PASS();
}

extern "C" void run_yt_link_tests(void)
{
    printf("\n[yt link]\n");
    test_link_forms();
    test_refused();
}

__attribute__((constructor)) static void auto_run_yt_link_tests(void)
{
    run_yt_link_tests();
}
