#include "test_framework.h"
#include "../src/include/utils.h"
#include <math.h>

TEST(estimate_tokens_ascii) {
    size_t t = estimate_tokens("Hello World! This is a test.");
    ASSERT_TRUE(t > 0);
    ASSERT_TRUE(t <= 30);
}

TEST(estimate_tokens_empty) {
    ASSERT_EQ_SIZE(0, estimate_tokens(""));
    ASSERT_EQ_SIZE(0, estimate_tokens(NULL));
}

TEST(estimate_tokens_chinese) {
    size_t t = estimate_tokens("你好世界");
    ASSERT_TRUE(t >= 4);
}

TEST(estimate_tokens_mixed) {
    size_t ascii_only = estimate_tokens("Hello World");
    size_t mixed = estimate_tokens("Hello 你好 World");
    ASSERT_TRUE(mixed > ascii_only);
}

TEST(estimate_tokens_long_chinese) {
    char buf[3001];
    for (int i = 0; i < 1000; i++) {
        buf[i*3] = (char)0xE4;
        buf[i*3+1] = (char)0xBD;
        buf[i*3+2] = (char)0xA0;
    }
    buf[3000] = '\0';
    size_t t = estimate_tokens(buf);
    ASSERT_TRUE(t >= 1000);
}

TEST(estimate_tokens_pure_ascii) {
    char buf[401];
    memset(buf, 'a', 400);
    buf[400] = '\0';
    size_t t = estimate_tokens(buf);
    ASSERT_EQ_SIZE(100, t);
}

TEST(strip_think_tags_basic) {
    char* result = strip_think_tags("Hello<think\nthinking\n</think\nWorld");
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(strstr(result, "think") == NULL || strstr(result, "Hello") != NULL);
    free(result);
}

TEST(strip_think_tags_no_tags) {
    char* result = strip_think_tags("No think tags here");
    ASSERT_NOT_NULL(result);
    ASSERT_EQ_STR("No think tags here", result);
    free(result);
}

TEST(escape_xml_basic) {
    char* result = escape_xml("a<b>c&d\"e'f");
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(strstr(result, "&lt;") != NULL);
    ASSERT_TRUE(strstr(result, "&gt;") != NULL);
    ASSERT_TRUE(strstr(result, "&amp;") != NULL);
    free(result);
}

TEST(escape_xml_empty) {
    char* result = escape_xml("");
    ASSERT_NOT_NULL(result);
    free(result);
}

TEST(generate_id_unique) {
    char* id1 = generate_id("test");
    char* id2 = generate_id("test");
    ASSERT_NOT_NULL(id1);
    ASSERT_NOT_NULL(id2);
    ASSERT_TRUE(strcmp(id1, id2) != 0);
    ASSERT_TRUE(strncmp(id1, "test_", 5) == 0);
    free(id1);
    free(id2);
}

TEST(str_trim_left_basic) {
    ASSERT_EQ_STR("hello", str_trim_left("hello"));
    ASSERT_EQ_STR("hello", str_trim_left("  hello"));
    ASSERT_EQ_STR("hello", str_trim_left("\t\nhello"));
    ASSERT_EQ_STR("hello world", str_trim_left("  hello world"));
    ASSERT_EQ_STR("", str_trim_left("   "));
    ASSERT_EQ_STR("", str_trim_left(""));
}

TEST(str_trim_left_null) {
    ASSERT_TRUE(str_trim_left(NULL) == NULL);
}

TEST(str_trim_left_no_spaces) {
    ASSERT_EQ_STR("/new", str_trim_left("/new"));
}

TEST(str_trim_left_command_with_spaces) {
    ASSERT_EQ_STR("/new", str_trim_left("  /new"));
    ASSERT_EQ_STR("/help", str_trim_left("\t/help"));
    ASSERT_EQ_STR("/new session", str_trim_left("  /new session"));
}

TEST(str_trim_copy_basic) {
    char* r1 = str_trim_copy("  hello  ");
    ASSERT_NOT_NULL(r1);
    ASSERT_EQ_STR("hello", r1);
    free(r1);
    
    char* r2 = str_trim_copy("hello");
    ASSERT_NOT_NULL(r2);
    ASSERT_EQ_STR("hello", r2);
    free(r2);
    
    char* r3 = str_trim_copy("  /new  ");
    ASSERT_NOT_NULL(r3);
    ASSERT_EQ_STR("/new", r3);
    free(r3);
}

TEST(str_trim_copy_null) {
    char* r = str_trim_copy(NULL);
    ASSERT_NULL(r);
}

TEST(str_trim_copy_empty) {
    char* r = str_trim_copy("");
    ASSERT_NOT_NULL(r);
    ASSERT_EQ_STR("", r);
    free(r);
    
    char* r2 = str_trim_copy("   ");
    ASSERT_NOT_NULL(r2);
    ASSERT_EQ_STR("", r2);
    free(r2);
}

TEST_SUITE(utils) {
    BEGIN_SUITE(utils);
    RUN_TEST(estimate_tokens_ascii);
    RUN_TEST(estimate_tokens_empty);
    RUN_TEST(estimate_tokens_chinese);
    RUN_TEST(estimate_tokens_mixed);
    RUN_TEST(estimate_tokens_long_chinese);
    RUN_TEST(estimate_tokens_pure_ascii);
    RUN_TEST(strip_think_tags_basic);
    RUN_TEST(strip_think_tags_no_tags);
    RUN_TEST(escape_xml_basic);
    RUN_TEST(escape_xml_empty);
    RUN_TEST(generate_id_unique);
    RUN_TEST(str_trim_left_basic);
    RUN_TEST(str_trim_left_null);
    RUN_TEST(str_trim_left_no_spaces);
    RUN_TEST(str_trim_left_command_with_spaces);
    RUN_TEST(str_trim_copy_basic);
    RUN_TEST(str_trim_copy_null);
    RUN_TEST(str_trim_copy_empty);
    END_SUITE();
}
