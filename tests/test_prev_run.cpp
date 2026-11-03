/*
 * tests/test_prev_run.cpp - #229: the previous run's log is kept on its own,
 * and how it ended is read correctly.
 */

#include "evo_prev_run.h"

#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
    extern int g_tests_run;
    extern int g_tests_passed;
    extern int g_tests_failed;
}

#define TEST_START(name) do { g_tests_run++; printf("  RUN  %-50s", name); fflush(stdout); } while (0)
#define TEST_PASS() do { g_tests_passed++; printf(" [ PASS ]\n"); } while (0)
#define TEST_FAIL(reason) do { g_tests_failed++; printf(" [ FAIL ] (%s at line %d)\n", reason, __LINE__); } while (0)
#define TEST_ASSERT(cond, reason) do { if (!(cond)) { TEST_FAIL(reason); return; } } while (0)

static void write_file(const char *path, const std::string &text)
{
    FILE *f = std::fopen(path, "wb");
    if (!f) return;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

static std::string read_file(const char *path)
{
    std::string s;
    FILE *f = std::fopen(path, "rb");
    if (!f) return s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

static const char *kLog = "/tmp/test_evo_prev.log";
static const char *kPrev = "/tmp/test_evo_prev.prev.log";

static std::string session(const char *name, const char *ending)
{
    std::string s = "[1.000] INFO  log session start: wall clock 2026-11-03T10:00:00 at this line\n";
    s += std::string("[2.000] INFO  ") + name + ": playing something\n";
    s += std::string("[3.000] ") + ending + "\n";
    return s;
}

static void test_prev_run_states(void)
{
    TEST_START("Last run: crashed / closed / ended / none");

    write_file(kLog, session("first", "INFO  soft close: parked - GPU idle") +
                     session("second", "ERROR CRASH signal=11 addr=0x48 anchor=0x0"));
    TEST_ASSERT(evo_prev_run_save(kLog, kPrev, 1 << 20) == EVO_PREV_RUN_CRASHED, "crash not seen");
    std::string prev = read_file(kPrev);
    TEST_ASSERT(prev.find("second: playing") != std::string::npos, "last session missing");
    TEST_ASSERT(prev.find("first: playing") == std::string::npos, "older session copied too");
    TEST_ASSERT(std::strcmp(evo_prev_run_path(), kPrev) == 0, "path not remembered");

    write_file(kLog, session("a", "INFO  CRASH signal=11 earlier") + session("b", "INFO  soft close: parked"));
    TEST_ASSERT(evo_prev_run_save(kLog, kPrev, 1 << 20) == EVO_PREV_RUN_CLOSED, "clean close not seen");

    write_file(kLog, session("c", "INFO  pace: 5.0s presents=300"));
    TEST_ASSERT(evo_prev_run_save(kLog, kPrev, 1 << 20) == EVO_PREV_RUN_ENDED, "PS-button end not seen");

    /* An old log with no session headers: the tail is the run. */
    write_file(kLog, "[1.000] INFO  old build line\n[2.000] INFO  CRASH signal=6\n");
    TEST_ASSERT(evo_prev_run_save(kLog, kPrev, 1 << 20) == EVO_PREV_RUN_CRASHED, "headerless log");

    write_file(kLog, "");
    TEST_ASSERT(evo_prev_run_save(kLog, kPrev, 1 << 20) == EVO_PREV_RUN_NONE, "empty log");
    std::remove(kLog);
    TEST_ASSERT(evo_prev_run_save(kLog, kPrev, 1 << 20) == EVO_PREV_RUN_NONE, "missing log");
    TEST_ASSERT(evo_prev_run_path()[0] == 0, "path kept for NONE");
    TEST_PASS();
}

static void test_prev_run_cap(void)
{
    TEST_START("Last run: a long run is capped at a line start");
    std::string s = "[0.000] INFO  log session start: wall clock x at this line\n";
    for (int i = 0; i < 20000; ++i) {
        char line[96];
        std::snprintf(line, sizeof line, "[%d.000] INFO  line number %d of a long session\n", i, i);
        s += line;
    }
    s += "[9.000] INFO  CRASH signal=11\n";
    write_file(kLog, s);
    TEST_ASSERT(evo_prev_run_save(kLog, kPrev, 64 * 1024) == EVO_PREV_RUN_CRASHED, "crash at the end missed");
    const std::string prev = read_file(kPrev);
    TEST_ASSERT(prev.size() <= 64 * 1024 && prev.size() > 60 * 1024, "cap not applied");
    TEST_ASSERT(prev[0] == '[', "copy does not start at a line start");
    TEST_ASSERT(prev.find("line number 19999") != std::string::npos, "the end of the run is missing");
    std::remove(kLog);
    std::remove(kPrev);
    TEST_PASS();
}

extern "C" void run_prev_run_tests(void)
{
    test_prev_run_states();
    test_prev_run_cap();
}

__attribute__((constructor)) static void auto_run_prev_run_tests(void)
{
    run_prev_run_tests();
}
