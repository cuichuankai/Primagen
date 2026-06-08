#include "ut_test.h"
#include "../src/include/common.h"
#include "../src/include/message.h"
#include "../src/common/common.c"
#include "../src/common/message.c"
#include "../src/tools/tool.h"
#include "../src/tools/tool.c"
#include "../src/tools/tool_executor.h"
#include "../src/tools/tool_executor.c"

static Error echo_tool_exec(void* user_data, const char* args_json, String* result) {
    (void)user_data;
    string_append(result, args_json ? args_json : "");
    return error_new(ERR_NONE, "");
}

static void async_callback_counter(Error err, const char* result, void* user_data) {
    int* counter = (int*)user_data;
    (void)err;
    (void)result;
    (*counter)++;
}

int test_tool_executor_new() {
    ToolRegistry* reg = tool_registry_new();
    ToolExecutor* executor = tool_executor_new(reg, 2);
    UT_ASSERT_NOT_NULL(executor);
    UT_ASSERT_INT_EQ((int)executor->num_workers, 2);
    tool_executor_destroy(executor);
    tool_registry_free(reg);
    return 0;
}

int test_tool_executor_new_auto_workers() {
    ToolRegistry* reg = tool_registry_new();
    ToolExecutor* executor = tool_executor_new(reg, 0);
    UT_ASSERT_NOT_NULL(executor);
    UT_ASSERT(executor->num_workers >= 1);
    tool_executor_destroy(executor);
    tool_registry_free(reg);
    return 0;
}

int test_tool_executor_destroy_null() {
    tool_executor_destroy(NULL);
    return 0;
}

int test_tool_executor_submit() {
    ToolRegistry* reg = tool_registry_new();
    tool_registry_register(reg, "echo", "Echo tool", "{}", echo_tool_exec, NULL);
    ToolExecutor* executor = tool_executor_new(reg, 2);

    int counter = 0;
    int result = tool_executor_submit(executor, "echo", "{\"msg\":\"hello\"}", &counter, NULL);
    UT_ASSERT_INT_EQ(result, 0);

    usleep(100000);

    tool_executor_destroy(executor);
    tool_registry_free(reg);
    return 0;
}

int test_tool_executor_submit_async() {
    ToolRegistry* reg = tool_registry_new();
    tool_registry_register(reg, "echo", "Echo tool", "{}", echo_tool_exec, NULL);
    ToolExecutor* executor = tool_executor_new(reg, 2);

    int counter = 0;
    tool_executor_submit_async(executor, "echo", "{\"msg\":\"test\"}", async_callback_counter, &counter);

    usleep(200000);
    UT_ASSERT_INT_EQ(counter, 1);

    tool_executor_destroy(executor);
    tool_registry_free(reg);
    return 0;
}

int test_tool_executor_submit_null() {
    ToolRegistry* reg = tool_registry_new();
    ToolExecutor* executor = tool_executor_new(reg, 2);

    int result = tool_executor_submit(NULL, "echo", "{}", NULL, NULL);
    UT_ASSERT_INT_EQ(result, -1);

    int result2 = tool_executor_submit(executor, NULL, "{}", NULL, NULL);
    UT_ASSERT_INT_EQ(result2, -1);

    tool_executor_destroy(executor);
    tool_registry_free(reg);
    return 0;
}

int test_tool_executor_execute_sync() {
    ToolRegistry* reg = tool_registry_new();
    tool_registry_register(reg, "echo", "Echo tool", "{}", echo_tool_exec, NULL);
    ToolExecutor* executor = tool_executor_new(reg, 2);

    String result = string_new("");
    Error err = tool_executor_execute_sync(executor, "echo", "{\"test\":1}", &result, 5000);
    UT_ASSERT_INT_EQ(err.code, ERR_NONE);
    UT_ASSERT(result.len > 0);

    string_free(&result);
    tool_executor_destroy(executor);
    tool_registry_free(reg);
    return 0;
}

int test_tool_executor_execute_sync_timeout() {
    ToolRegistry* reg = tool_registry_new();
    ToolExecutor* executor = tool_executor_new(reg, 2);

    String result = string_new("");
    Error err = tool_executor_execute_sync(executor, "nonexistent_tool", "{}", &result, 100);
    UT_ASSERT(err.code != ERR_NONE);

    string_free(&result);
    tool_executor_destroy(executor);
    tool_registry_free(reg);
    return 0;
}

int main() {
    printf("=== ToolExecutor Module Tests ===\n");

    UT_RUN_TEST(test_tool_executor_new);
    UT_RUN_TEST(test_tool_executor_new_auto_workers);
    UT_RUN_TEST(test_tool_executor_destroy_null);
    UT_RUN_TEST(test_tool_executor_submit);
    UT_RUN_TEST(test_tool_executor_submit_async);
    UT_RUN_TEST(test_tool_executor_submit_null);
    UT_RUN_TEST(test_tool_executor_execute_sync);
    UT_RUN_TEST(test_tool_executor_execute_sync_timeout);

    UT_TEST_SUMMARY();
}
