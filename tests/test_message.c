#include "ut_test.h"
#include "../src/include/message.h"
#include "../src/include/common.h"
#include "../src/common/common.c"
#include "../src/common/message.c"

int test_message_new_user() {
    Message* msg = message_new(ROLE_USER, "hello");
    UT_ASSERT_NOT_NULL(msg);
    UT_ASSERT_INT_EQ(msg->role, ROLE_USER);
    UT_ASSERT_STR_EQ(msg->content.data, "hello");
    UT_ASSERT_INT_EQ((int)msg->tool_calls_count, 0);
    message_free(msg);
    return 0;
}

int test_message_new_assistant() {
    Message* msg = message_new(ROLE_ASSISTANT, "response text");
    UT_ASSERT_NOT_NULL(msg);
    UT_ASSERT_INT_EQ(msg->role, ROLE_ASSISTANT);
    UT_ASSERT_STR_EQ(msg->content.data, "response text");
    message_free(msg);
    return 0;
}

int test_message_new_null_content() {
    Message* msg = message_new(ROLE_USER, NULL);
    UT_ASSERT_NOT_NULL(msg);
    UT_ASSERT(msg->content.data != NULL);
    message_free(msg);
    return 0;
}

int test_message_free_null() {
    message_free(NULL);
    return 0;
}

int test_message_add_tool_call() {
    Message* msg = message_new(ROLE_ASSISTANT, "using tool");
    UT_ASSERT_INT_EQ((int)msg->tool_calls_count, 0);

    message_add_tool_call(msg, "call_001", "read_file", "{\"path\":\"/tmp/test\"}");
    UT_ASSERT_INT_EQ((int)msg->tool_calls_count, 1);
    UT_ASSERT_STR_EQ(msg->tool_calls[0].id.data, "call_001");
    UT_ASSERT_STR_EQ(msg->tool_calls[0].name.data, "read_file");
    UT_ASSERT_STR_EQ(msg->tool_calls[0].arguments.data, "{\"path\":\"/tmp/test\"}");

    message_add_tool_call(msg, "call_002", "exec", "{\"command\":\"ls\"}");
    UT_ASSERT_INT_EQ((int)msg->tool_calls_count, 2);
    UT_ASSERT_STR_EQ(msg->tool_calls[1].name.data, "exec");

    message_free(msg);
    return 0;
}

int test_inbound_message() {
    InboundMessage* msg = inbound_message_new("cli", "user1", "hello world");
    UT_ASSERT_NOT_NULL(msg);
    UT_ASSERT_STR_EQ(msg->channel.data, "cli");
    UT_ASSERT_STR_EQ(msg->chat_id.data, "user1");
    UT_ASSERT_STR_EQ(msg->content.data, "hello world");
    inbound_message_free(msg);
    return 0;
}

int test_inbound_message_free_null() {
    inbound_message_free(NULL);
    return 0;
}

int test_outbound_message() {
    OutboundMessage* msg = outbound_message_new("cli", "user1", "response");
    UT_ASSERT_NOT_NULL(msg);
    UT_ASSERT_STR_EQ(msg->channel.data, "cli");
    UT_ASSERT_STR_EQ(msg->content.data, "response");
    outbound_message_free(msg);
    return 0;
}

int test_outbound_message_free_null() {
    outbound_message_free(NULL);
    return 0;
}

int test_internal_event_llm_result() {
    Error err = error_new(ERR_NONE, "");
    ToolCall calls[2];
    calls[0].id = string_new("id1");
    calls[0].name = string_new("tool1");
    calls[0].arguments = string_new("{\"a\":1}");
    calls[1].id = string_new("id2");
    calls[1].name = string_new("tool2");
    calls[1].arguments = string_new("{\"b\":2}");

    InternalEvent* ev = internal_event_new_llm_result("session1", err, "response text", calls, 2);
    UT_ASSERT_NOT_NULL(ev);
    UT_ASSERT_INT_EQ(ev->type, EVENT_LLM_RESULT);
    UT_ASSERT_STR_EQ(ev->session_key.data, "session1");
    UT_ASSERT_STR_EQ(ev->llm_response.data, "response text");
    UT_ASSERT_INT_EQ((int)ev->tool_calls_count, 2);
    UT_ASSERT_STR_EQ(ev->tool_calls[0].id.data, "id1");
    UT_ASSERT_STR_EQ(ev->tool_calls[1].name.data, "tool2");

    string_free(&calls[0].id);
    string_free(&calls[0].name);
    string_free(&calls[0].arguments);
    string_free(&calls[1].id);
    string_free(&calls[1].name);
    string_free(&calls[1].arguments);
    internal_event_free(ev);
    return 0;
}

int test_internal_event_tool_result() {
    Error err = error_new(ERR_TOOL, "tool error");
    InternalEvent* ev = internal_event_new_tool_result("session1", "call_001", "read_file", "file content", err);
    UT_ASSERT_NOT_NULL(ev);
    UT_ASSERT_INT_EQ(ev->type, EVENT_TOOL_RESULT);
    UT_ASSERT_STR_EQ(ev->session_key.data, "session1");
    UT_ASSERT_STR_EQ(ev->tool_call_id.data, "call_001");
    UT_ASSERT_STR_EQ(ev->tool_name.data, "read_file");
    UT_ASSERT_STR_EQ(ev->tool_result.data, "file content");
    UT_ASSERT_INT_EQ(ev->tool_error.code, ERR_TOOL);
    internal_event_free(ev);
    return 0;
}

int test_internal_event_free_null() {
    internal_event_free(NULL);
    return 0;
}

int main() {
    printf("=== Message Module Tests ===\n");

    UT_RUN_TEST(test_message_new_user);
    UT_RUN_TEST(test_message_new_assistant);
    UT_RUN_TEST(test_message_new_null_content);
    UT_RUN_TEST(test_message_free_null);
    UT_RUN_TEST(test_message_add_tool_call);
    UT_RUN_TEST(test_inbound_message);
    UT_RUN_TEST(test_inbound_message_free_null);
    UT_RUN_TEST(test_outbound_message);
    UT_RUN_TEST(test_outbound_message_free_null);
    UT_RUN_TEST(test_internal_event_llm_result);
    UT_RUN_TEST(test_internal_event_tool_result);
    UT_RUN_TEST(test_internal_event_free_null);

    UT_TEST_SUMMARY();
}
