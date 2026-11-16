/*
 * tests/test_hdr10plus.cpp - the HDR10+ scene peak read from a T.35 payload
 * and from an Annex-B access unit. The payloads are built here field by field
 * from SMPTE ST 2094-40, so this pins the parser to the written syntax, not to
 * a captured stream.
 */

#include "evo_hdr10plus.h"

#include <cstdio>
#include <cstdint>
#include <vector>

extern "C" {
    extern int g_tests_run;
    extern int g_tests_passed;
    extern int g_tests_failed;
}

#define TEST_START(name) do { g_tests_run++; printf("  RUN  %-50s", name); fflush(stdout); } while (0)
#define TEST_PASS() do { g_tests_passed++; printf(" [ PASS ]\n"); } while (0)
#define TEST_FAIL(reason) do { g_tests_failed++; printf(" [ FAIL ] (%s at line %d)\n", reason, __LINE__); } while (0)
#define TEST_ASSERT(cond, reason) do { if (!(cond)) { TEST_FAIL(reason); return; } } while (0)

namespace {

struct Bits {
    std::vector<uint8_t> v;
    int n = 0;
    void put(uint32_t val, int bits) {
        for (int i = bits - 1; i >= 0; i--, n++) {
            if ((n & 7) == 0) v.push_back(0);
            v.back() |= ((val >> i) & 1u) << (7 - (n & 7));
        }
    }
};

/* A whole-picture HDR10+ payload. maxscl in 0.00001 units of 10000 nits. */
std::vector<uint8_t> t35(uint32_t r, uint32_t g, uint32_t b, uint32_t p99, int windows = 1, bool peakTable = false)
{
    Bits s;
    s.put(0xB5, 8); s.put(0x003C, 16); s.put(0x0001, 16); s.put(4, 8);
    s.put(1, 8);                      /* application_version */
    s.put(windows, 2);
    for (int w = 1; w < windows; w++) {
        for (int i = 0; i < 6; i++) s.put(0x1234, 16);
        s.put(7, 8);
        for (int i = 0; i < 3; i++) s.put(0x0101, 16);
        s.put(1, 1);
    }
    s.put(4000, 27);                  /* targeted display maximum luminance */
    s.put(peakTable ? 1 : 0, 1);
    if (peakTable) { s.put(2, 5); s.put(3, 5); for (int i = 0; i < 6; i++) s.put(9, 4); }
    s.put(r, 17); s.put(g, 17); s.put(b, 17);
    s.put(500, 17);                   /* average_maxrgb */
    s.put(3, 4);
    s.put(1, 7);  s.put(90000, 17);   /* low percentage: must not count as a peak */
    s.put(50, 7); s.put(800, 17);
    s.put(99, 7); s.put(p99, 17);
    s.put(0, 10);                     /* fraction_bright_pixels */
    s.put(0, 1); s.put(0, 1); s.put(0, 1);
    return s.v;
}

/* Wrap SEI payloads into one prefix SEI NAL, escaped, then a slice NAL. */
std::vector<uint8_t> au(const std::vector<std::vector<uint8_t>>& payloads, const std::vector<int>& types)
{
    std::vector<uint8_t> rbsp;
    for (size_t i = 0; i < payloads.size(); i++) {
        rbsp.push_back((uint8_t)types[i]);
        size_t sz = payloads[i].size();
        while (sz >= 255) { rbsp.push_back(0xFF); sz -= 255; }
        rbsp.push_back((uint8_t)sz);
        rbsp.insert(rbsp.end(), payloads[i].begin(), payloads[i].end());
    }
    rbsp.push_back(0x80);
    std::vector<uint8_t> out = {0, 0, 0, 1, 0x40, 0x01, 0xAA, 0xBB,       /* VPS */
                                0, 0, 1, 39 << 1, 0x01};                    /* prefix SEI */
    int zeros = 0;
    for (uint8_t c : rbsp) {
        if (zeros >= 2 && c <= 3) { out.push_back(3); zeros = 0; }
        out.push_back(c);
        zeros = c == 0 ? zeros + 1 : 0;
    }
    const uint8_t slice[] = {0, 0, 1, 19 << 1, 0x01, 0x11, 0x22, 0x33};
    out.insert(out.end(), slice, slice + sizeof slice);
    return out;
}

bool near(float a, float b) { return a > b - 0.06f && a < b + 0.06f; }

void test_t35_maxscl()
{
    TEST_START("hdr10+: peak is the largest MaxSCL");
    auto p = t35(8000, 12345, 3000, 9000);
    TEST_ASSERT(near(evo_hdr10plus_peak_t35(p.data(), (int)p.size()), 1234.5f), "1234.5 nits");
    TEST_PASS();
}

void test_t35_percentile_fallback()
{
    TEST_START("hdr10+: MaxSCL 0 falls back to the 99% percentile");
    auto p = t35(0, 0, 0, 6500);
    TEST_ASSERT(near(evo_hdr10plus_peak_t35(p.data(), (int)p.size()), 650.0f), "650 nits, not the 1% entry");
    TEST_PASS();
}

void test_t35_windows_and_table()
{
    TEST_START("hdr10+: extra windows and the peak table are skipped");
    auto p = t35(40000, 1, 2, 3, 3, true);
    TEST_ASSERT(near(evo_hdr10plus_peak_t35(p.data(), (int)p.size()), 4000.0f), "4000 nits");
    TEST_PASS();
}

void test_t35_rejects()
{
    TEST_START("hdr10+: other T.35 users, short and empty payloads give 0");
    auto p = t35(8000, 8000, 8000, 8000);
    auto dolby = p; dolby[2] = 0x3B;                 /* another provider */
    auto app = p;   app[5] = 5;
    TEST_ASSERT(evo_hdr10plus_peak_t35(dolby.data(), (int)dolby.size()) == 0.0f, "provider");
    TEST_ASSERT(evo_hdr10plus_peak_t35(app.data(), (int)app.size()) == 0.0f, "application id");
    TEST_ASSERT(evo_hdr10plus_peak_t35(p.data(), 12) == 0.0f, "cut short");
    TEST_ASSERT(evo_hdr10plus_peak_t35(nullptr, 40) == 0.0f, "null");
    auto zero = t35(0, 0, 0, 0);
    TEST_ASSERT(evo_hdr10plus_peak_t35(zero.data(), (int)zero.size()) == 0.0f, "no peak at all");
    TEST_PASS();
}

void test_annexb()
{
    TEST_START("hdr10+: found in an access unit, after another SEI");
    std::vector<uint8_t> other(300, 0x00);           /* long, full of zeros: escaping and 0xFF sizes */
    auto a = au({other, t35(0, 0, 10000, 5)}, {137, 4});
    TEST_ASSERT(near(evo_hdr10plus_peak_annexb(a.data(), (int)a.size()), 1000.0f), "1000 nits");
    TEST_PASS();
}

void test_annexb_none()
{
    TEST_START("hdr10+: an access unit without it gives 0");
    auto a = au({std::vector<uint8_t>(24, 0x55)}, {137});
    TEST_ASSERT(evo_hdr10plus_peak_annexb(a.data(), (int)a.size()) == 0.0f, "none");
    /* the payload sits after the first slice: not a prefix SEI, not read */
    std::vector<uint8_t> late = {0, 0, 1, 19 << 1, 0x01, 0x11};
    auto tail = au({t35(9000, 9000, 9000, 9000)}, {4});
    late.insert(late.end(), tail.begin(), tail.end());
    TEST_ASSERT(evo_hdr10plus_peak_annexb(late.data(), (int)late.size()) == 0.0f, "after the slice");
    TEST_ASSERT(evo_hdr10plus_peak_annexb(a.data(), 3) == 0.0f, "tiny");
    TEST_PASS();
}

} // namespace

extern "C" void run_hdr10plus_tests(void)
{
    printf("\n[hdr10plus]\n");
    test_t35_maxscl();
    test_t35_percentile_fallback();
    test_t35_windows_and_table();
    test_t35_rejects();
    test_annexb();
    test_annexb_none();
}

__attribute__((constructor)) static void auto_run_hdr10plus_tests(void)
{
    run_hdr10plus_tests();
}
