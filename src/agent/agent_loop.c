#include "agent_loop.h"
#include "../include/common.h"

AgentLoop* agent_loop_new(SessionManager* session_mgr, ContextBuilder* ctx_builder, ToolRegistry* tool_reg, MessageBus* bus) {
    AgentLoop* loop = malloc(sizeof(AgentLoop));
    if (!loop) return NULL;
    loop->session_mgr = session_mgr;
    loop->ctx_builder = ctx_builder;
    loop->tool_reg = tool_reg;
    loop->bus = bus;
    loop->llm_call = NULL;
    return loop;
}

void agent_loop_free(AgentLoop* loop) {
    free(loop);
}

void agent_loop_set_llm_provider(AgentLoop* loop, Error (*provider)(const char*, String*, ToolCall**, size_t*)) {
    loop->llm_call = provider;
}

void agent_loop_run(AgentLoop* loop) {
    while (1) {
        // Receive inbound message
        InboundMessage* inbound = message_bus_receive_inbound(loop->bus);
        if (!inbound) continue;
        
        // Get or create session
        char key[256];
        snprintf(key, sizeof(key), "%s:%s", inbound->channel.data, inbound->chat_id.data);
        Session* session = session_manager_get(loop->session_mgr, key);
        if (!session) {
            session_manager_load(loop->session_mgr, key, &session);
        }
        
        // Add user message
        Message* user_msg = message_new(ROLE_USER, inbound->content.data);
        session_add_message(session, user_msg);
        
        // Build context
        String prompt = context_builder_build(loop->ctx_builder, session, loop->tool_reg);
        
        // LLM call
        String response = string_new("");
        ToolCall* tool_calls = NULL;
        size_t tool_calls_count = 0;
        Error err = loop->llm_call(prompt.data, &response, &tool_calls, &tool_calls_count);
        string_free(&prompt);
        
        if (err.code != ERR_NONE) {
            // Error handling
            OutboundMessage* outbound = outbound_message_new(inbound->channel.data, inbound->chat_id.data, "Error occurred");
            message_bus_send_outbound(loop->bus, outbound);
            inbound_message_free(inbound);
            continue;
        }
        
        // Process tool calls
        for (size_t i = 0; i < tool_calls_count; i++) {
            String result = string_new("");
            err = tool_registry_execute(loop->tool_reg, tool_calls[i].name.data, tool_calls[i].arguments.data, &result);
            if (err.code != ERR_NONE) {
                string_free(&result);
                result = string_new("Tool execution failed");
            }
            Message* tool_msg = message_new(ROLE_TOOL, result.data);
            tool_msg->tool_call_id = string_copy(&tool_calls[i].id);
            tool_msg->name = string_copy(&tool_calls[i].name);
            session_add_message(session, tool_msg);
            string_free(&result);
        }
        
        // Add assistant message
        Message* assistant_msg = message_new(ROLE_ASSISTANT, response.data);
        for (size_t i = 0; i < tool_calls_count; i++) {
            message_add_tool_call(assistant_msg, tool_calls[i].id.data, tool_calls[i].name.data, tool_calls[i].arguments.data);
        }
        session_add_message(session, assistant_msg);
        
        // Save session
        session_manager_save(loop->session_mgr, session);
        
        // Send outbound
        OutboundMessage* outbound = outbound_message_new(inbound->channel.data, inbound->chat_id.data, response.data);
        message_bus_send_outbound(loop->bus, outbound);
        
        // Free
        string_free(&response);
        for (size_t i = 0; i < tool_calls_count; i++) {
            string_free(&tool_calls[i].id);
            string_free(&tool_calls[i].name);
            string_free(&tool_calls[i].arguments);
        }
        free(tool_calls);
        inbound_message_free(inbound);
    }
}