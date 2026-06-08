#include "test_framework.h"
#include "../src/tools/tool.h"
#include "../src/include/common.h"

static Error test_tool_fn(void* user_data, const char* args_json, String* result) {
    (void)user_data; (void)args_json;
    *result = string_new("test_result");
    return error_new(ERR_NONE, "");
}

TEST(tool_registry_new_free) {
    ToolRegistry* reg = tool_registry_new();
    ASSERT_NOT_NULL(reg);
    ASSERT_NOT_NULL(reg->tools);
    ASSERT_EQ_SIZE(0, reg->count);
    ASSERT_EQ_SIZE(8, reg->capacity);
    tool_registry_free(reg);
}

TEST(tool_registry_register_and_get) {
    ToolRegistry* reg = tool_registry_new();
    Error err = tool_registry_register(reg, "test_tool", "A test tool", "{}", test_tool_fn, NULL);
    ASSERT_EQ_INT(ERR_NONE, err.code);
    ASSERT_EQ_SIZE(1, reg->count);
    
    Tool* t = tool_registry_get(reg, "test_tool");
    ASSERT_NOT_NULL(t);
    ASSERT_EQ_STR("test_tool", t->def.name.data);
    ASSERT_EQ_STR("A test tool", t->def.description.data);
    
    tool_registry_free(reg);
}

TEST(tool_registry_get_nonexistent) {
    ToolRegistry* reg = tool_registry_new();
    Tool* t = tool_registry_get(reg, "no_such_tool");
    ASSERT_NULL(t);
    tool_registry_free(reg);
}

TEST(tool_registry_register_multiple) {
    ToolRegistry* reg = tool_registry_new();
    tool_registry_register(reg, "tool_a", "Tool A", "{}", test_tool_fn, NULL);
    tool_registry_register(reg, "tool_b", "Tool B", "{}", test_tool_fn, NULL);
    tool_registry_register(reg, "tool_c", "Tool C", "{}", test_tool_fn, NULL);
    ASSERT_EQ_SIZE(3, reg->count);
    
    ASSERT_NOT_NULL(tool_registry_get(reg, "tool_a"));
    ASSERT_NOT_NULL(tool_registry_get(reg, "tool_b"));
    ASSERT_NOT_NULL(tool_registry_get(reg, "tool_c"));
    
    tool_registry_free(reg);
}

TEST(tool_registry_execute) {
    ToolRegistry* reg = tool_registry_new();
    tool_registry_register(reg, "exec_test", "Exec test", "{}", test_tool_fn, NULL);
    
    String result = string_new("");
    Error err = tool_registry_execute(reg, "exec_test", "{}", &result);
    ASSERT_EQ_INT(ERR_NONE, err.code);
    ASSERT_EQ_STR("test_result", result.data);
    
    string_free(&result);
    tool_registry_free(reg);
}

TEST(tool_registry_execute_nonexistent) {
    ToolRegistry* reg = tool_registry_new();
    String result = string_new("");
    Error err = tool_registry_execute(reg, "no_tool", "{}", &result);
    ASSERT_TRUE(err.code != ERR_NONE);
    string_free(&result);
    tool_registry_free(reg);
}

TEST(tool_registry_capacity_growth) {
    ToolRegistry* reg = tool_registry_new();
    for (int i = 0; i < 20; i++) {
        char name[32];
        snprintf(name, sizeof(name), "tool_%d", i);
        tool_registry_register(reg, name, "desc", "{}", test_tool_fn, NULL);
    }
    ASSERT_EQ_SIZE(20, reg->count);
    ASSERT_TRUE(reg->capacity >= 20);
    tool_registry_free(reg);
}

TEST_SUITE(tool_registry) {
    BEGIN_SUITE(tool_registry);
    RUN_TEST(tool_registry_new_free);
    RUN_TEST(tool_registry_register_and_get);
    RUN_TEST(tool_registry_get_nonexistent);
    RUN_TEST(tool_registry_register_multiple);
    RUN_TEST(tool_registry_execute);
    RUN_TEST(tool_registry_execute_nonexistent);
    RUN_TEST(tool_registry_capacity_growth);
    END_SUITE();
}
