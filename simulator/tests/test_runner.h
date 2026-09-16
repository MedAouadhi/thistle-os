/*
 * Minimal C test framework for simulator tests.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

/* One set of counters for the whole binary, not one per translation unit.
 *
 * These were `static`, which gave every test file its own private copies: an
 * assertion incremented the count in the file that failed, while RUN_TEST and
 * the exit code in test_sim_main.c read a set that nothing ever touched. Every
 * failing test printed FAIL and was then counted as a pass, and the process
 * still exited 0 — so the suite could not go red. Defined in test_sim_main.c. */
extern int _tests_run;
extern int _tests_passed;
extern int _tests_failed;

#define TEST(name) void test_##name(void)

/* A failing assertion prints its own FAIL and returns from the test body, so
 * whether the test passed is only knowable by watching the failure count. The
 * unconditional PASS this replaced printed "PASS" directly under a "FAIL" and
 * counted it in the summary, which made a red suite read as green. */
#define RUN_TEST(name) do { \
    int _failed_before = _tests_failed; \
    _tests_run++; \
    printf("  TEST %-50s ", #name); \
    test_##name(); \
    if (_tests_failed == _failed_before) { \
        printf("PASS\n"); \
        _tests_passed++; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL\n    %s:%d: %s != %s (%d != %d)\n", __FILE__, __LINE__, #a, #b, (int)(a), (int)(b)); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("FAIL\n    %s:%d: %s is false\n", __FILE__, __LINE__, #x); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_NEAR(a, b, eps) do { \
    if (fabs((double)(a) - (double)(b)) > (eps)) { \
        printf("FAIL\n    %s:%d: %s != %s (%.4f != %.4f)\n", __FILE__, __LINE__, #a, #b, (double)(a), (double)(b)); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("FAIL\n    %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_STR_CONTAINS(haystack, needle) do { \
    if (strstr((haystack), (needle)) == NULL) { \
        printf("FAIL\n    %s:%d: \"%s\" not found in output\n", __FILE__, __LINE__, (needle)); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_SUMMARY() do { \
    printf("\n  %d/%d tests passed", _tests_passed, _tests_run); \
    if (_tests_failed > 0) printf(" (%d FAILED)", _tests_failed); \
    printf("\n"); \
} while(0)
