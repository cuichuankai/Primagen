#include "ut_test.h"
#include "../src/include/common.h"
#include "../src/common/common.c"

int test_string_new_null() {
    String s = string_new(NULL);
    UT_ASSERT(s.data != NULL);
    UT_ASSERT_INT_EQ(s.len, 0);
    UT_ASSERT_INT_EQ(s.capacity, 16);
    UT_ASSERT_STR_EQ(s.data, "");
    string_free(&s);
    return 0;
}

int test_string_new_empty() {
    String s = string_new("");
    UT_ASSERT(s.data != NULL);
    UT_ASSERT_INT_EQ(s.len, 0);
    UT_ASSERT_STR_EQ(s.data, "");
    string_free(&s);
    return 0;
}

int test_string_new_content() {
    String s = string_new("hello world");
    UT_ASSERT(s.data != NULL);
    UT_ASSERT_INT_EQ((int)s.len, 11);
    UT_ASSERT_STR_EQ(s.data, "hello world");
    string_free(&s);
    return 0;
}

int test_string_free_null_ptr() {
    String* s = NULL;
    string_free(s);
    return 0;
}

int test_string_free_twice_safe() {
    String s = string_new("test");
    string_free(&s);
    UT_ASSERT_NULL(s.data);
    UT_ASSERT_INT_EQ((int)s.len, 0);
    string_free(&s);
    return 0;
}

int test_string_append() {
    String s = string_new("hello");
    string_append(&s, " ");
    string_append(&s, "world");
    UT_ASSERT_STR_EQ(s.data, "hello world");
    UT_ASSERT_INT_EQ((int)s.len, 11);
    string_free(&s);
    return 0;
}

int test_string_append_null() {
    String s = string_new("hello");
    string_append(&s, NULL);
    UT_ASSERT_STR_EQ(s.data, "hello");
    string_free(&s);
    return 0;
}

int test_string_append_growth() {
    String s = string_new("a");
    for (int i = 0; i < 100; i++) {
        string_append(&s, "bcdefghijklmnop");
    }
    UT_ASSERT(s.len > 16);
    UT_ASSERT(s.capacity > s.len);
    string_free(&s);
    return 0;
}

int test_string_copy() {
    String s = string_new("original");
    String c = string_copy(&s);
    UT_ASSERT_STR_EQ(c.data, "original");
    UT_ASSERT(c.data != s.data);
    string_free(&s);
    string_free(&c);
    return 0;
}

int test_string_copy_null() {
    String c = string_copy(NULL);
    UT_ASSERT(c.data != NULL);
    string_free(&c);
    return 0;
}

int test_string_equals() {
    String a = string_new("hello");
    String b = string_new("hello");
    String c = string_new("world");
    UT_ASSERT_INT_EQ(string_equals(&a, &b), 1);
    UT_ASSERT_INT_EQ(string_equals(&a, &c), 0);
    UT_ASSERT_INT_EQ(string_equals(NULL, &a), 0);
    UT_ASSERT_INT_EQ(string_equals(NULL, NULL), 0);
    string_free(&a);
    string_free(&b);
    string_free(&c);
    return 0;
}

int test_dynamic_array_basic() {
    DynamicArray arr = dynamic_array_new(sizeof(int));
    UT_ASSERT(arr.items != NULL);
    UT_ASSERT_INT_EQ((int)arr.count, 0);
    UT_ASSERT_INT_EQ((int)arr.capacity, 8);

    for (int i = 0; i < 5; i++) {
        dynamic_array_add(&arr, &i);
    }
    UT_ASSERT_INT_EQ((int)arr.count, 5);

    for (int i = 0; i < 5; i++) {
        int* val = (int*)dynamic_array_get(&arr, i);
        UT_ASSERT(val != NULL);
        UT_ASSERT_INT_EQ(*val, i);
    }

    dynamic_array_free(&arr);
    UT_ASSERT_NULL(arr.items);
    return 0;
}

int test_dynamic_array_growth() {
    DynamicArray arr = dynamic_array_new(sizeof(int));
    for (int i = 0; i < 20; i++) {
        dynamic_array_add(&arr, &i);
    }
    UT_ASSERT_INT_EQ((int)arr.count, 20);
    UT_ASSERT(arr.capacity >= 20);
    dynamic_array_free(&arr);
    return 0;
}

int test_dynamic_array_get_out_of_bounds() {
    DynamicArray arr = dynamic_array_new(sizeof(int));
    int val = 42;
    dynamic_array_add(&arr, &val);
    void* result = dynamic_array_get(&arr, 5);
    UT_ASSERT_NULL(result);
    dynamic_array_free(&arr);
    return 0;
}

int test_dynamic_array_free_null() {
    dynamic_array_free(NULL);
    return 0;
}

int test_error_new() {
    Error err = error_new(ERR_NONE, "no error");
    UT_ASSERT_INT_EQ(err.code, ERR_NONE);
    UT_ASSERT_STR_EQ(err.message, "no error");

    Error err2 = error_new(ERR_MEMORY, NULL);
    UT_ASSERT_INT_EQ(err2.code, ERR_MEMORY);
    UT_ASSERT_STR_EQ(err2.message, "");

    Error err3 = error_new(ERR_FILE, "file not found");
    UT_ASSERT_INT_EQ(err3.code, ERR_FILE);
    UT_ASSERT_STR_EQ(err3.message, "file not found");
    return 0;
}

int test_error_print_null() {
    error_print(NULL);
    return 0;
}

int test_string_array() {
    StringArray arr = string_array_new();
    UT_ASSERT(arr.items != NULL);
    UT_ASSERT_INT_EQ((int)arr.count, 0);

    string_array_add(&arr, "hello");
    string_array_add(&arr, "world");
    UT_ASSERT_INT_EQ((int)arr.count, 2);
    UT_ASSERT_STR_EQ(arr.items[0].data, "hello");
    UT_ASSERT_STR_EQ(arr.items[1].data, "world");

    string_array_free(&arr);
    UT_ASSERT_NULL(arr.items);
    return 0;
}

int main() {
    printf("=== Common Module Tests ===\n");

    UT_RUN_TEST(test_string_new_null);
    UT_RUN_TEST(test_string_new_empty);
    UT_RUN_TEST(test_string_new_content);
    UT_RUN_TEST(test_string_free_null_ptr);
    UT_RUN_TEST(test_string_free_twice_safe);
    UT_RUN_TEST(test_string_append);
    UT_RUN_TEST(test_string_append_null);
    UT_RUN_TEST(test_string_append_growth);
    UT_RUN_TEST(test_string_copy);
    UT_RUN_TEST(test_string_copy_null);
    UT_RUN_TEST(test_string_equals);
    UT_RUN_TEST(test_dynamic_array_basic);
    UT_RUN_TEST(test_dynamic_array_growth);
    UT_RUN_TEST(test_dynamic_array_get_out_of_bounds);
    UT_RUN_TEST(test_dynamic_array_free_null);
    UT_RUN_TEST(test_error_new);
    UT_RUN_TEST(test_error_print_null);
    UT_RUN_TEST(test_string_array);

    UT_TEST_SUMMARY();
}
