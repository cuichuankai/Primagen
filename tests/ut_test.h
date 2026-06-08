#ifndef UT_TEST_H
#define UT_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static int ut_tests_run = 0;
static int ut_tests_passed = 0;
static int ut_tests_failed = 0;
static int ut_assertions = 0;

#define UT_ASSERT(cond) do { \
    ut_assertions++; \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while(0)

#define UT_ASSERT_STR_EQ(a, b) do { \
    ut_assertions++; \
    const char* _a = (a); \
    const char* _b = (b); \
    if ((_a == NULL && _b != NULL) || (_a != NULL && _b == NULL) || \
        (_a != NULL && _b != NULL && strcmp(_a, _b) != 0)) { \
        fprintf(stderr, "  FAIL: \"%s\" != \"%s\" (line %d)\n", \
                _a ? _a : "(null)", _b ? _b : "(null)", __LINE__); \
        return 1; \
    } \
} while(0)

#define UT_ASSERT_INT_EQ(a, b) do { \
    ut_assertions++; \
    int _a = (int)(a); \
    int _b = (int)(b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL: %d != %d (line %d)\n", _a, _b, __LINE__); \
        return 1; \
    } \
} while(0)

#define UT_ASSERT_NULL(p) do { \
    ut_assertions++; \
    if ((p) != NULL) { \
        fprintf(stderr, "  FAIL: expected NULL, got %p (line %d)\n", (void*)(p), __LINE__); \
        return 1; \
    } \
} while(0)

#define UT_ASSERT_NOT_NULL(p) do { \
    ut_assertions++; \
    if ((p) == NULL) { \
        fprintf(stderr, "  FAIL: expected non-NULL (line %d)\n", __LINE__); \
        return 1; \
    } \
} while(0)

#define UT_RUN_TEST(test_func) do { \
    ut_tests_run++; \
    printf("  Running %s... ", #test_func); \
    int _result = test_func(); \
    if (_result == 0) { \
        ut_tests_passed++; \
        printf("PASS\n"); \
    } else { \
        ut_tests_failed++; \
        printf("FAIL\n"); \
    } \
} while(0)

#define UT_TEST_SUMMARY() do { \
    printf("\n========================================\n"); \
    printf("Test Summary: %d/%d passed, %d failed\n", \
           ut_tests_passed, ut_tests_run, ut_tests_failed); \
    printf("Total assertions: %d\n", ut_assertions); \
    printf("========================================\n"); \
    return ut_tests_failed > 0 ? 1 : 0; \
} while(0)

#endif
