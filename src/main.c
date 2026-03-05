#include "include/common.h"
#include "include/message.h"
#include "bus/message_bus.h"
#include "session/session.h"
#include "context/context_builder.h"
#include "tools/tool.h"
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
    // Initialize components
    MessageBus* bus = message_bus_new();
    SessionManager* session_mgr = session_manager_new("./workspace");
    ContextBuilder* ctx_builder = context_builder_new();
    ToolRegistry* tool_reg = tool_registry_new();
    
    // Set up context builder
    context_builder_set_identity(ctx_builder, "You are a helpful AI assistant.");
    
    // Register tools (example: a simple echo tool)
    tool_registry_register(tool_reg, "echo", "Echo the input", "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}}", echo_tool);
    
    // Create agent loop
    AgentLoop* loop = agent_loop_new(session_mgr, ctx_builder, tool_reg, bus);
    agent_loop_set_llm_provider(loop, stub_llm_call);
    
    // Start agent thread
    pthread_t thread;
    pthread_create(&thread, NULL, agent_thread, loop);
    
    // Simulate inbound message
    InboundMessage* msg = inbound_message_new("test", "123", "Hello, agent!");
    message_bus_send_inbound(bus, msg);
    
    // Wait for outbound
    OutboundMessage* out = message_bus_receive_outbound(bus);
    printf("Response: %s\n", out->content.data);
    outbound_message_free(out);
    
    // Cleanup
    pthread_cancel(thread);
    pthread_join(thread, NULL);
    agent_loop_free(loop);
    tool_registry_free(tool_reg);
    context_builder_free(ctx_builder);
    session_manager_free(session_mgr);
    message_bus_free(bus);
    
    return 0;
}