/*
 * tests/test_cast_dial.cpp - Unit tests for EVO Player DIAL cast receiver
 * (SSDP + DIAL protocol matching CastDesk).
 */

#include "evo_cast_receiver.h"
#include <cstdio>
#include <cstring>
#include <cassert>

extern "C" {
    extern int g_tests_run;
    extern int g_tests_passed;
    extern int g_tests_failed;
}

#define TEST_START(name) do { g_tests_run++; printf("  RUN  %-50s", name); fflush(stdout); } while (0)
#define TEST_PASS() do { g_tests_passed++; printf(" [ PASS ]\n"); } while (0)
#define TEST_FAIL(reason) do { g_tests_failed++; printf(" [ FAIL ] (%s at line %d)\n", reason, __LINE__); } while (0)
#define TEST_ASSERT(cond, reason) do { if (!(cond)) { TEST_FAIL(reason); return; } } while (0)

static void test_param_parsing()
{
    TEST_START("cast: parameter parsing from DIAL launch payload");
    const char *payload = "pairingCode=6b8e3a2f-1234&v=dQw4w9WgXcQ&t=120";

    char code[64] = {0};
    int r1 = evo_cast_parse_param(payload, "pairingCode", code, sizeof(code));
    TEST_ASSERT(r1 == 1, "pairingCode found");
    TEST_ASSERT(strcmp(code, "6b8e3a2f-1234") == 0, "pairingCode value match");

    char vid[64] = {0};
    int r2 = evo_cast_parse_param(payload, "v", vid, sizeof(vid));
    TEST_ASSERT(r2 == 1, "v found");
    TEST_ASSERT(strcmp(vid, "dQw4w9WgXcQ") == 0, "v value match");

    char t[32] = {0};
    int r3 = evo_cast_parse_param(payload, "t", t, sizeof(t));
    TEST_ASSERT(r3 == 1, "t found");
    TEST_ASSERT(strcmp(t, "120") == 0, "t value match");

    char missing[32] = {0};
    int r4 = evo_cast_parse_param(payload, "nonexistent", missing, sizeof(missing));
    TEST_ASSERT(r4 == 0, "nonexistent key returns 0");
    TEST_ASSERT(missing[0] == '\0', "missing buffer empty");

    /* URL decoding check */
    const char *enc_payload = "title=Hello+World%21&url=http%3A%2F%2Ftest.com%2Fvideo.mp4";
    char title[64] = {0};
    evo_cast_parse_param(enc_payload, "title", title, sizeof(title));
    TEST_ASSERT(strcmp(title, "Hello World!") == 0, "title URL decoded");

    char url[128] = {0};
    evo_cast_parse_param(enc_payload, "url", url, sizeof(url));
    TEST_ASSERT(strcmp(url, "http://test.com/video.mp4") == 0, "url URL decoded");

    TEST_PASS();
}

static void test_friendly_name_and_status()
{
    TEST_START("cast: friendly name and app state toggling");
    evo_cast_receiver_set_friendly_name("Living Room EVO");
    TEST_ASSERT(strcmp(evo_cast_receiver_friendly_name(), "Living Room EVO") == 0, "friendly name updated");

    TEST_ASSERT(evo_cast_receiver_app_is_running() == 0, "initial app state stopped");
    evo_cast_receiver_set_app_running(1);
    TEST_ASSERT(evo_cast_receiver_app_is_running() == 1, "app state running");
    evo_cast_receiver_set_app_running(0);
    TEST_ASSERT(evo_cast_receiver_app_is_running() == 0, "app state stopped again");

    TEST_PASS();
}

static void test_additional_data()
{
    TEST_START("cast: additionalData store and clear");
    evo_cast_receiver_clear_additional_data();
    evo_cast_receiver_set_additional_data("screenId", "12345678-abcd");
    evo_cast_receiver_set_additional_data("loungeToken", "tok_xyz987");

    /* Clearing should reset */
    evo_cast_receiver_clear_additional_data();
    TEST_PASS();
}

static void test_stream_mpd_and_title()
{
    TEST_START("cast: stream MPD and title storage");
    const char *sample_mpd = "<?xml version=\"1.0\"?><MPD></MPD>";
    evo_cast_receiver_set_stream_mpd(sample_mpd, "Sample Video");
    TEST_ASSERT(strcmp(evo_cast_receiver_stream_title(), "Sample Video") == 0, "title match");

    evo_cast_receiver_set_direct_stream("http://example.com/stream.mp4", "Direct Video");
    TEST_ASSERT(strcmp(evo_cast_receiver_stream_title(), "Direct Video") == 0, "direct title match");

    TEST_PASS();
}

static void test_lounge_parsing()
{
    TEST_START("cast: lounge command JSON videoId parsing");
    const char *set_playlist = "[[1,[\"setPlaylist\",{\"videoIds\":\"dQw4w9WgXcQ\",\"currentTime\":\"45.2\"}]]]";
    char vid[64] = {0};
    char toff[32] = {0};
    int r1 = evo_cast_parse_lounge_video(set_playlist, vid, sizeof(vid), toff, sizeof(toff));
    TEST_ASSERT(r1 == 1, "setPlaylist video parsed");
    TEST_ASSERT(strcmp(vid, "dQw4w9WgXcQ") == 0, "videoId matches");
    TEST_ASSERT(strcmp(toff, "45.2") == 0, "currentTime matches");

    const char *now_playing = "[[2,[\"nowPlaying\",{\"videoId\":\"9bZkp7q19f0\"}]]]";
    char vid2[64] = {0};
    char toff2[32] = {0};
    int r2 = evo_cast_parse_lounge_video(now_playing, vid2, sizeof(vid2), toff2, sizeof(toff2));
    TEST_ASSERT(r2 == 1, "nowPlaying video parsed");
    TEST_ASSERT(strcmp(vid2, "9bZkp7q19f0") == 0, "videoId matches");

    const char *other_event = "[[3,[\"loungeStatus\",{\"queueId\":\"RQ123\"}]]]";
    char vid3[64] = {0};
    int r3 = evo_cast_parse_lounge_video(other_event, vid3, sizeof(vid3), NULL, 0);
    TEST_ASSERT(r3 == 0, "unrelated event ignored");

    TEST_PASS();
}

extern "C" void run_cast_dial_tests(void)
{
    printf("\n[cast dial]\n");
    test_param_parsing();
    test_friendly_name_and_status();
    test_additional_data();
    test_stream_mpd_and_title();
    test_lounge_parsing();
}

__attribute__((constructor)) static void auto_run_cast_dial_tests(void)
{
    run_cast_dial_tests();
}
