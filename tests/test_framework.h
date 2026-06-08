#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static int _tests_run = 0;
static int _tests_passed = 0;
static int _tests_failed = 0;
static const char* _current_test_name = NULL;

#define TEST_RUN(test) do { \
    _current_test_name = #test; \
    printf("  RUN  %s\n", _current_test_name); \
    test(); \
} while(0)

#define RUN_TEST_SUITE(name) do { \
    printf("\n=== %s ===\n", name); \
    int prev_passed = _tests_passed; \
    int prev_failed = _tests_failed; \
    void run_tests(void); run_tests(); \
    int suite_passed = _tests_passed - prev_passed; \
    int suite_failed = _tests_failed - prev_failed; \
    printf("  Suite result: %d passed, %d failed\n", suite_passed, suite_failed); \
} while(0)

#define ASSERT_TRUE(condition, msg) do { \
    _tests_run++; \
    if (!(condition)) { \
        _tests_failed++; \
        printf("  FAIL %s: %s (line %d): %s\n", _current_test_name, __FILE__, __LINE__, msg); \
        return; \
    } else { \
        _tests_passed++; \
    } \
} while(0)

#define ASSERT_FALSE(condition, msg) ASSERT_TRUE(!(condition), msg)

#define ASSERT_NULL(ptr, msg) ASSERT_TRUE((ptr) == NULL, msg)
#define ASSERT_NOT_NULL(ptr, msg) ASSERT_TRUE((ptr) != NULL, msg)

#define ASSERT_EQ_INT(expected, actual, msg) do { \
    _tests_run++; \
    int _expected = (expected); \
    int _actual = (actual); \
    if (_expected != _actual) { \
        _tests_failed++; \
        printf("  FAIL %s: %s (line %d): %s - expected %d, got %d\n", \
               _current_test_name, __FILE__, __LINE__, msg, _expected, _actual); \
        return; \
    } else { \
        _tests_passed++; \
    } \
} while(0)

#define ASSERT_EQ_SIZE(expected, actual, msg) do { \
    _tests_run++; \
    size_t _expected = (expected); \
    size_t _actual = (actual); \
    if (_expected != _actual) { \
        _tests_failed++; \
        printf("  FAIL %s: %s (line %d): %s - expected %zu, got %zu\n", \
               _current_test_name, __FILE__, __LINE__, msg, _expected, _actual); \
        return; \
    } else { \
        _tests_passed++; \
    } \
} while(0)

#define ASSERT_EQ_STR(expected, actual, msg) do { \
    _tests_run++; \
    const char* _expected = (expected); \
    const char* _actual = (actual); \
    if (!_expected && !_actual) { _tests_passed++; break; } \
    if (!_expected || !_actual || strcmp(_expected, _actual) != 0) { \
        _tests_failed++; \
        printf("  FAIL %s: %s (line %d): %s - expected \"%s\", got \"%s\"\n", \
               _current_test_name, __FILE__, __LINE__, msg, \
               _expected ? _expected : "NULL", _actual ? _actual : "NULL"); \
        return; \
    } else { \
        _tests_passed++; \
    } \
} while(0)

#define ASSERT_EQ_DOUBLE(expected, actual, tolerance, msg) do { \
    _tests_run++; \
    double _expected = (expected); \
    double _actual = (actual); \
    double diff = _expected - _actual; \
    if (diff < 0) diff = -diff; \
    if (diff > (tolerance)) { \
        _tests_failed++; \
        printf("  FAIL %s: %s (line %d): %s - expected %.6f, got %.6f\n", \
               _current_test_name, __FILE__, __LINE__, msg, _expected, _actual); \
        return; \
    } else { \
        _tests_passed++; \
    } \
} while(0)

#define ASSERT_NO_ERROR(err, msg) do { \
    _tests_run++; \
    Error _err = (err); \
    if (_err.code != ERR_NONE) { \
        _tests_failed++; \
        printf("  FAIL %s: %s (line %d): %s - error code %d: %s\n", \
               _current_test_name, __FILE__, __LINE__, msg, _err.code, _err.message); \
        return; \
    } else { \
        _tests_passed++; \
    } \
} while(0)

#define ASSERT_ERROR(expected_code, err, msg) do { \
    _tests_run++; \
    Error _err = (err); \
    int _expected = (expected_code); \
    if (_err.code != _expected) { \
        _tests_failed++; \
        printf("  FAIL %s: %s (line %d): %s - expected error code %d, got %d\n", \
               _current_test_name, __FILE__, __LINE__, msg, _expected, _err.code); \
        return; \
    } else { \
        _tests_passed++; \
    } \
} while(0)

void print_test_summary(void) {
    printf("\n========================================\n");
    printf("Test Summary: %d total, %d passed, %d failed\n",
           _tests_run, _tests_passed, _tests_failed);
    printf("========================================\n");
}

int get_test_exit_code(void) {
    return _tests_failed > 0 ? 1 : 0;
}

#endif // TEST_FRAMEWORK_H