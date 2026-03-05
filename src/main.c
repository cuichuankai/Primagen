#include "include/common.h"
#include "include/message.h"
#include "bus/message_bus.h"
#include "session/session.h"
#include "context/context_builder.h"
#include "tools/tool.h"
#include "tools/tools_impl.h"
#include "agent/agent_loop.h"
#include "providers/llm_provider.h"
#include <pthread.h>

void* agent_thread(void* arg) {
    AgentLoop* loop = (AgentLoop*)arg;
    agent_loop_run(loop);
    return NULL;
}

Error echo_tool(const char* args_json, String* result) {
    // Simple implementation
    *result = string_new("Echo: ");
    // Append args_json for simplicity
    result->data = realloc(result->data, result->len + strlen(args_json) + 1);
    strcat(result->data, args_json);
    result->len = strlen(result->data);
    return error_new(ERR_NONE, "");
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    // Simulate a simple agent message processing
    printf("Primagen - AI Agent Framework\n");
    printf("================================\n\n");

    // Initialize basic components
    String identity = string_new("You are Primagen, a helpful AI assistant built in C.");
    printf("Identity: %s\n\n", identity.data);

    // Simulate creating an inbound message
    InboundMessage* msg = inbound_message_new("cli", "123", "Hello, what can you do?");

    printf("Inbound Message:\n");
    printf("  Channel: %s\n", msg->channel.data);
    printf("  Chat ID: %s\n", msg->chat_id.data);
    printf("  Content: %s\n\n", msg->content.data);

    // Simulate agent processing and creating response
    OutboundMessage* response = outbound_message_new("cli", "123", 
        "I can process messages, run tools, and manage conversations.");

    printf("Response: %s\n\n", response->content.data);

    // Display implemented components
    printf("Implemented Components:\n");
    printf("  ✓ Message Bus (async queues)\n");
    printf("  ✓ Agent Loop (ReAct paradigm)\n");
    printf("  ✓ Context Builder (prompt assembly with skills)\n");
    printf("  ✓ Tool Registry (dynamic tool execution)\n");
    printf("  ✓ Session Manager (state persistence)\n");
    printf("  ✓ Memory System (long-term memory)\n");
    printf("  ✓ Subagent Manager (background task execution)\n");
    printf("  ✓ Cron Service (scheduled task management)\n");
    printf("  ✓ Heartbeat Service (periodic operations)\n");
    printf("  ✓ Skills Loader (extensible capabilities)\n");
    printf("  ✓ LLM Provider Interface (model integration)\n\n");

    // Cleanup
    string_free(&identity);
    inbound_message_free(msg);
    outbound_message_free(response);

    printf("Primagen agent framework initialized successfully!\n");
    return 0;
}