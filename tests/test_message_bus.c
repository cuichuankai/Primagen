#include "test_framework.h"
#include "../src/bus/message_bus.h"
#include "../src/include/message.h"
#include <pthread.h>

TEST(bus_create_and_free) {
    MessageBus* bus = message_bus_new();
    ASSERT_NOT_NULL(bus);
    message_bus_free(bus);
}

TEST(bus_send_receive_inbound) {
    MessageBus* bus = message_bus_new();
    InboundMessage* msg = inbound_message_new("test_ch", "chat1", "hello");
    message_bus_send_inbound(bus, msg);
    InboundMessage* recv = message_bus_receive_inbound(bus);
    ASSERT_NOT_NULL(recv);
    ASSERT_EQ_STR("test_ch", recv->channel.data);
    ASSERT_EQ_STR("chat1", recv->chat_id.data);
    ASSERT_EQ_STR("hello", recv->content.data);
    inbound_message_free(recv);
    message_bus_free(bus);
}

TEST(bus_send_receive_outbound) {
    MessageBus* bus = message_bus_new();
    OutboundMessage* msg = outbound_message_new("out_ch", "chat2", "response");
    message_bus_send_outbound(bus, msg);
    OutboundMessage* recv = message_bus_receive_outbound(bus);
    ASSERT_NOT_NULL(recv);
    ASSERT_EQ_STR("out_ch", recv->channel.data);
    ASSERT_EQ_STR("response", recv->content.data);
    outbound_message_free(recv);
    message_bus_free(bus);
}

TEST(bus_send_receive_internal) {
    MessageBus* bus = message_bus_new();
    ToolCall tc = {string_new("id1"), string_new("tool1"), string_new("{\"a\":1}")};
    InternalEvent* ev = internal_event_new_llm_result("sess1", error_new(ERR_NONE, ""), "hello from LLM", &tc, 1);
    message_bus_send_internal(bus, ev);
    InternalEvent* recv = message_bus_receive_internal_timed(bus, 1000);
    ASSERT_NOT_NULL(recv);
    ASSERT_EQ_INT(EVENT_LLM_RESULT, recv->type);
    ASSERT_EQ_STR("sess1", recv->session_key.data);
    ASSERT_EQ_STR("hello from LLM", recv->llm_response.data);
    ASSERT_EQ_SIZE(1, recv->tool_calls_count);
    ASSERT_EQ_STR("tool1", recv->tool_calls[0].name.data);
    internal_event_free(recv);
    message_bus_free(bus);
}

TEST(bus_close_drops_messages) {
    MessageBus* bus = message_bus_new();
    message_bus_close(bus);
    InboundMessage* msg = inbound_message_new("ch", "id", "after close");
    message_bus_send_inbound(bus, msg);
    InboundMessage* recv = message_bus_receive_inbound_timed(bus, 100);
    ASSERT_NULL(recv);
    message_bus_free(bus);
}

TEST(bus_timed_receive_timeout) {
    MessageBus* bus = message_bus_new();
    InboundMessage* recv = message_bus_receive_inbound_timed(bus, 50);
    ASSERT_NULL(recv);
    message_bus_free(bus);
}

TEST(bus_multiple_messages_ordering) {
    MessageBus* bus = message_bus_new();
    for (int i = 0; i < 10; i++) {
        char content[32];
        snprintf(content, sizeof(content), "msg_%d", i);
        InboundMessage* msg = inbound_message_new("ch", "id", content);
        message_bus_send_inbound(bus, msg);
    }
    for (int i = 0; i < 10; i++) {
        InboundMessage* recv = message_bus_receive_inbound(bus);
        ASSERT_NOT_NULL(recv);
        char expected[32];
        snprintf(expected, sizeof(expected), "msg_%d", i);
        ASSERT_EQ_STR(expected, recv->content.data);
        inbound_message_free(recv);
    }
    message_bus_free(bus);
}

TEST_SUITE(message_bus) {
    BEGIN_SUITE(message_bus);
    RUN_TEST(bus_create_and_free);
    RUN_TEST(bus_send_receive_inbound);
    RUN_TEST(bus_send_receive_outbound);
    RUN_TEST(bus_send_receive_internal);
    RUN_TEST(bus_close_drops_messages);
    RUN_TEST(bus_timed_receive_timeout);
    RUN_TEST(bus_multiple_messages_ordering);
    END_SUITE();
}
