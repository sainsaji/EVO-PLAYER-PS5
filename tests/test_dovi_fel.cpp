/*
 * tests/test_dovi_fel.cpp - the Dolby Vision enhancement-layer maths and the
 * NAL 63 split (evo_dovi_fel.c). The dequantisation is checked against the
 * integer form of ETSI GS CCM 001's linear dead zone, written out here on a
 * 16-bit signal, so the float version a shader will use is pinned to it.
 */

#include "evo_dovi_fel.h"

#include <cmath>
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

const int DENOM = 23;   /* coefficient_log2_denom of real streams */

/* The residual on a 16-bit signal, in integers: sign * min((2|d|-1)*S + 2T, 2R),
 * fixed point with DENOM fraction bits, scaled so 1.0 is 65536. */
int64_t residual16(int el, int offset, int64_t S, int64_t T, int64_t R)
{
    int64_t d = el - offset;
    if (d == 0) return 0;
    const int64_t sign = d < 0 ? -1 : 1;
    int64_t dq = (2 * d - sign) * S + sign * 2 * T;
    if (dq > 2 * R) dq = 2 * R;
    if (dq < -2 * R) dq = -2 * R;
    return dq / (INT64_C(1) << (DENOM - 15));
}

void test_residual_matches_integer_form()
{
    TEST_START("dovi fel: residual matches the integer form");
    const int64_t S = (INT64_C(1) << DENOM) / 300, T = (INT64_C(1) << DENOM) / 5000, R = (INT64_C(1) << DENOM) / 8;
    evo_dovi_nlq n = {};
    for (int c = 0; c < 3; c++)
        evo_dovi_nlq_set(&n, c, 512, (uint64_t)R, (uint64_t)S, (uint64_t)T, DENOM);
    TEST_ASSERT(n.fel == 1, "a slope makes it FEL");
    for (int el = 0; el < 1024; el += 7) {
        const double want = (double)residual16(el, 512, S, T, R);
        const double got = (double)evo_dovi_nlq_residual(&n, 1, (float)el) * 65536.0;
        TEST_ASSERT(std::fabs(got - want) <= 1.5, "within one 16-bit step");
    }
    TEST_ASSERT(evo_dovi_nlq_residual(&n, 0, 512.0f) == 0.0f, "the offset code adds nothing");
    TEST_ASSERT(evo_dovi_nlq_residual(&n, 0, 520.0f) == -evo_dovi_nlq_residual(&n, 0, 504.0f), "odd symmetry");
    TEST_ASSERT(std::fabs(evo_dovi_nlq_residual(&n, 0, 1023.0f) - 0.125f) < 1e-6f, "clamped at vdr_in_max");
    TEST_PASS();
}

void test_mel_is_not_fel()
{
    TEST_START("dovi fel: a minimal EL adds nothing");
    evo_dovi_nlq n = {};
    for (int c = 0; c < 3; c++)   /* what a MEL RPU carries: max 1.0, slope 0, threshold 0 */
        evo_dovi_nlq_set(&n, c, 0, UINT64_C(1) << DENOM, 0, 0, DENOM);
    TEST_ASSERT(n.fel == 0, "MEL");
    for (int el = 0; el < 1024; el += 31)
        TEST_ASSERT(evo_dovi_nlq_residual(&n, 2, (float)el) == 0.0f, "zero for every code");
    evo_dovi_nlq_set(&n, 2, 512, UINT64_C(1) << DENOM, 0, 1, DENOM);
    TEST_ASSERT(n.fel == 1, "one chroma threshold is enough");
    TEST_ASSERT(evo_dovi_nlq_residual(&n, 5, 3.0f) == 0.0f && evo_dovi_nlq_residual(nullptr, 0, 3.0f) == 0.0f, "bad args");
    TEST_PASS();
}

void test_el_extract()
{
    TEST_START("dovi fel: NAL 63 units come out unwrapped");
    const std::vector<uint8_t> au = {
        0, 0, 0, 1, 0x40, 0x01, 0xAA,                    /* VPS, base layer */
        0, 0, 1, 0x7E, 0x01, 0x42, 0x01, 0x11, 0x22,     /* EL: wrapped SPS */
        0, 0, 1, 0x26, 0x01, 0x99, 0x98,                 /* base layer slice */
        0, 0, 0, 1, 0x7E, 0x01, 0x26, 0x01, 0x33, 0x00, 0x00, 0x03, 0x01,   /* EL slice, escaped */
        0, 0, 1, 0x7C, 0x01, 0x19, 0x08,                 /* RPU (NAL 62) */
    };
    const std::vector<uint8_t> want = {
        0, 0, 0, 1, 0x42, 0x01, 0x11, 0x22,
        0, 0, 0, 1, 0x26, 0x01, 0x33, 0x00, 0x00, 0x03, 0x01,
    };
    uint8_t out[64];
    const int n = evo_dovi_el_extract(au.data(), (int)au.size(), out, sizeof out);
    TEST_ASSERT(n == (int)want.size(), "length");
    TEST_ASSERT(std::vector<uint8_t>(out, out + n) == want, "bytes");
    TEST_ASSERT(evo_dovi_el_extract(au.data(), (int)au.size(), out, 10) == -1, "too small a buffer");
    TEST_ASSERT(evo_dovi_el_extract(au.data(), 7, out, sizeof out) == 0, "no EL in the AU");
    TEST_ASSERT(evo_dovi_el_extract(nullptr, 9, out, sizeof out) == 0, "null");
    TEST_PASS();
}

} // namespace

extern "C" void run_dovi_fel_tests(void)
{
    printf("\n[dovi_fel]\n");
    test_residual_matches_integer_form();
    test_mel_is_not_fel();
    test_el_extract();
}

__attribute__((constructor)) static void auto_run_dovi_fel_tests(void)
{
    run_dovi_fel_tests();
}
