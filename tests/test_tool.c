#include "ut_test.h"
#include "../src/include/common.h"
#include "../src/common/common.c"
#include "../src/tools/tool.h"
#include "../src/tools/tool.c"

static Error mock_tool_exec(void* user_data, const char* args_json, String* result) {
    (void)user_data;
    (void)args_json;
    string_append(result, "mock_result");
    return error_new(ERR_NONE, "");
}

static Error mock_tool_error(void* user_data, const char* args_json, String* result) {
    (void)user_data;
    (void)args_json;
    (void)result;
    return error_new(ERR_TOOL, "mock error");
}

int test_tool_registry_new() {
    ToolRegistry* reg = tool_registry_new();
    UT_ASSERT_NOT_NULL(reg);
    UT_ASSERT_INT_EQ((int)reg->count, 0);
    UT_ASSERT_INT_EQ((int)reg->capacity, 8);
    tool_registry_free(reg);
    return 0;
}

int test_tool_registry_register() {
    ToolRegistry* reg = tool_registry_new();
    Error err = tool_registry_register(reg, "test_tool", "A test tool", "{}", mock_tool_exec, NULL);
    UT_ASSERT_INT_EQ(err.code, ERR_NONE);
    UT_ASSERT_INT_EQ((int)reg->count, 1);
    UT_ASSERT_STR_EQ(reg->tools[0].def.name.data, "test_tool");
    UT_ASSERT_STR_EQ(reg->tools[0].def.description.data, "A test tool");
    tool_registry_free(reg);
    return 0;
}

int test_tool_registry_get() {
    ToolRegistry* reg = tool_registry_new();
    tool_registry_register(reg, "tool_a", "Tool A", "{}", mock_tool_exec, NULL);
    tool_registry_register(reg, "tool_b", "Tool B", "{}", mock_tool_exec, NULL);

    Tool* found = tool_registry_get(reg, "tool_a");
    UT_ASSERT_NOT_NULL(found);
    UT_ASSERT_STR_EQ(found->def.name.data, "tool_a");

    Tool* found2 = tool_registry_get(reg, "tool_b");
    UT_ASSERT_NOT_NULL(found2);
    UT_ASSERT_STR_EQ(found2->def.name.data, "tool_b");

    Tool* not_found = tool_registry_get(reg, "nonexistent");
    UT_ASSERT_NULL(not_found);

    tool_registry_free(reg);
    return 0;
}

int test_tool_registry_get_null_name() {
    ToolRegistry* reg = tool_registry_new();
    Tool* found = tool_registry_get(reg, NULL);
    UT_ASSERT_NULL(found);
    tool_registry_free(reg);
    return 0;
}

int test_tool_registry_get_null_reg() {
    Tool* found = tool_registry_get(NULL, "test");
    UT_ASSERT_NULL(found);
    return 0;
}

int test_tool_registry_execute() {
    ToolRegistry* reg = tool_registry_new();
    tool_registry_register(reg, "mock", "Mock tool", "{}", mock_tool_exec, NULL);

    String result = string_new("");
    Error err = tool_registry_execute(reg, "mock", "{}", &result);
    UT_ASSERT_INT_EQ(err.code, ERR_NONE);
    UT_ASSERT_STR_EQ(result.data, "mock_result");

    string_free(&result);
    tool_registry_free(reg);
    return 0;
}

int test_tool_registry_execute_not_found() {
    ToolRegistry* reg = tool_registry_new();
    String result = string_new("");
    Error err = tool_registry_execute(reg, "nonexistent", "{}", &result);
    UT_ASSERT_INT_EQ(err.code, ERR_TOOL);
    string_free(&result);
    tool_registry_free(reg);
    return 0;
}

int test_tool_registry_execute_error_tool() {
    ToolRegistry* reg = tool_registry_new();
    tool_registry_register(reg, "error_tool", "Error tool", "{}", mock_tool_error, NULL);

    String result = string_new("");
    Error err = tool_registry_execute(reg, "error_tool", "{}", &result);
    UT_ASSERT_INT_EQ(err.code, ERR_TOOL);
    string_free(&result);
    tool_registry_free(reg);
    return 0;
}

int test_tool_registry_growth() {
    ToolRegistry* reg = tool_registry_new();
    for (int i = 0; i < 20; i++) {
        char name[32];
        snprintf(name, sizeof(name), "tool_%d", i);
        tool_registry_register(reg, name, "desc", "{}", mock_tool_exec, NULL);
    }
    UT_ASSERT_INT_EQ((int)reg->count, 20);
    UT_ASSERT(reg->capacity >= 20);

    Tool* found = tool_registry_get(reg, "tool_15");
    UT_ASSERT_NOT_NULL(found);
    UT_ASSERT_STR_EQ(found->def.name.data, "tool_15");

    tool_registry_free(reg);
    return 0;
}

int test_tool_registry_free_null() {
    tool_registry_free(NULL);
    return 0;
}

int test_tool_registry_alias_lookup() {
    ToolRegistry* reg = tool_registry_new();
    tool_registry_register(reg, "read_file", "Read file", "{}", mock_tool_exec, NULL);

    Tool* found = tool_registry_get(reg, "plugin.read_file");
    UT_ASSERT_NOT_NULL(found);
    UT_ASSERT_STR_EQ(found->def.name.data, "read_file");

    tool_registry_free(reg);
    return 0;
}

int main() {
    printf("=== Tool Module Tests ===\n");

    UT_RUN_TEST(test_tool_registry_new);
    UT_RUN_TEST(test_tool_registry_register);
    UT_RUN_TEST(test_tool_registry_get);
    UT_RUN_TEST(test_tool_registry_get_null_name);
    UT_RUN_TEST(test_tool_registry_get_null_reg);
    UT_RUN_TEST(test_tool_registry_execute);
    UT_RUN_TEST(test_tool_registry_execute_not_found);
    UT_RUN_TEST(test_tool_registry_execute_error_tool);
    UT_RUN_TEST(test_tool_registry_growth);
    UT_RUN_TEST(test_tool_registry_free_null);
    UT_RUN_TEST(test_tool_registry_alias_lookup);

    UT_TEST_SUMMARY();
}
