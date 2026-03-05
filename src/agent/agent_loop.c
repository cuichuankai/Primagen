#include "agent_loop.h"
#include "../include/common.h"

AgentLoop* agent_loop_new(SessionManager* session_mgr, ContextBuilder* ctx_builder, ToolRegistry* tool_reg, MessageBus* bus, Config* config) {
    AgentLoop* loop = malloc(sizeof(AgentLoop));
    if (!loop) return NULL;
    loop->session_mgr = session_mgr;
    loop->ctx_builder = ctx_builder;
    loop->tool_reg = tool_reg;
    loop->bus = bus;
    loop->config = config;
    loop->llm_call = NULL;
    loop->running = false;
    return loop;
}

void agent_loop_free(AgentLoop* loop) {
    free(loop);
}

void agent_loop_set_llm_provider(AgentLoop* loop, LLMProvider provider) {
    loop->llm_call = provider;
}

void agent_loop_stop(AgentLoop* loop) {
    if (loop) {
        loop->running = false;
    }
}

void agent_loop_run(AgentLoop* loop) {
    loop->running = true;
    while (loop->running) {
        // Receive inbound message
        InboundMessage* inbound = message_bus_receive_inbound(loop->bus);
        if (!inbound) {
            // No message, sleep briefly to avoid busy loop if receive is non-blocking (it should be blocking though)
            // But if receive returns NULL, it might mean queue is empty or error.
            // Assuming receive is blocking or we sleep.
            usleep(100000); // 100ms
            continue;
        }
        
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
        
        // Build context (System Prompt)
        String system_prompt = context_builder_build(loop->ctx_builder, session, loop->tool_reg);
        
        // LLM call
        String response = string_new("");
        ToolCall* tool_calls = NULL;
        size_t tool_calls_count = 0;
        
        Error err;
        if (loop->llm_call) {
            err = loop->llm_call(system_prompt.data, session, loop->tool_reg, loop->config, &response, &tool_calls, &tool_calls_count);
        } else {
            err = error_new(ERR_INVALID_PARAM, "No LLM provider set");
        }
        
        string_free(&system_prompt);
        
        if (err.code != ERR_NONE) {
            // Error handling
            char err_msg[256];
            snprintf(err_msg, sizeof(err_msg), "Error occurred in LLM call: %s", err.message);
            OutboundMessage* outbound = outbound_message_new(inbound->channel.data, inbound->chat_id.data, err_msg);
            message_bus_send_outbound(loop->bus, outbound);
            inbound_message_free(inbound);
            string_free(&response);
            continue;
        }
        
        // Process tool calls
        for (size_t i = 0; i < tool_calls_count; i++) {
            // Add assistant message with tool calls BEFORE adding tool results
            // But wait, we usually add assistant message first?
            // OpenAI expects: User -> Assistant (with tool_calls) -> Tool (with tool_call_id)
            // So we must add the assistant message first.
        }

        // Add assistant message (which might have content AND tool calls)
        Message* assistant_msg = message_new(ROLE_ASSISTANT, response.data);
        for (size_t i = 0; i < tool_calls_count; i++) {
            message_add_tool_call(assistant_msg, tool_calls[i].id.data, tool_calls[i].name.data, tool_calls[i].arguments.data);
        }
        session_add_message(session, assistant_msg);
        
        // Execute tools and add results
        for (size_t i = 0; i < tool_calls_count; i++) {
            String result = string_new("");
            
            // Notify user about tool usage if configured
            if (loop->config && loop->config->channels.send_tool_hints) {
                char hint[256];
                snprintf(hint, sizeof(hint), "🛠️ Using tool: %s", tool_calls[i].name.data);
                OutboundMessage* outbound = outbound_message_new(inbound->channel.data, inbound->chat_id.data, hint);
                message_bus_send_outbound(loop->bus, outbound);
            }

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
        
        // If there were tool calls, we might need to loop back to LLM?
        // The original code didn't loop back immediately. It just processed one turn.
        // But typically ReAct/Function calling requires a loop until no tool calls.
        // For now, let's keep it simple (one turn) or maybe check if we should recurse.
        // But the `agent_loop` implies a loop.
        // If we want to support multi-turn in one user request, we need a nested loop.
        // However, the `inbound` message is consumed.
        
        // Let's implement a simple recursion if tool calls were made.
        // But wait, `inbound` is freed. We can't reuse it.
        // And we need to send the tool outputs back to LLM to get the final answer.
        
        if (tool_calls_count > 0) {
            // We need to call LLM again with the new history (including tool outputs).
            // But we don't have a new user message.
            // We just call LLM again.
            
            // Re-build system prompt (context might have changed?)
            String sys_prompt_2 = context_builder_build(loop->ctx_builder, session, loop->tool_reg);
            
            String response_2 = string_new("");
            ToolCall* tool_calls_2 = NULL; // We don't support recursive tools in this simple version yet?
            size_t tool_calls_count_2 = 0;
            
            err = loop->llm_call(sys_prompt_2.data, session, loop->tool_reg, loop->config, &response_2, &tool_calls_2, &tool_calls_count_2);
            string_free(&sys_prompt_2);
            
            if (err.code == ERR_NONE) {
                // Add the final answer
                Message* final_msg = message_new(ROLE_ASSISTANT, response_2.data);
                session_add_message(session, final_msg);
                
                // Send outbound
                OutboundMessage* outbound = outbound_message_new(inbound->channel.data, inbound->chat_id.data, response_2.data);
                message_bus_send_outbound(loop->bus, outbound);
                
                // Clean up 2nd call
                if (tool_calls_2) {
                     for(size_t k=0; k<tool_calls_count_2; k++) {
                         string_free(&tool_calls_2[k].id);
                         string_free(&tool_calls_2[k].name);
                         string_free(&tool_calls_2[k].arguments);
                     }
                     free(tool_calls_2);
                }
            }
            string_free(&response_2);
        } else {
            // No tool calls, just send the response
            OutboundMessage* outbound = outbound_message_new(inbound->channel.data, inbound->chat_id.data, response.data);
            message_bus_send_outbound(loop->bus, outbound);
        }
        
        // Save session
        session_manager_save(loop->session_mgr, session);
        
        // Free first call resources
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
