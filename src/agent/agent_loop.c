#include "agent_loop.h"
#include "../include/common.h"
#include "../include/logger.h"

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
        
        // Skip empty messages
        if (inbound->content.len == 0) {
            inbound_message_free(inbound);
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
        
        // Multi-turn loop for tool calls
        int max_turns = 10;
        int turn = 0;
        bool conversation_turn_done = false;

        while (!conversation_turn_done && turn < max_turns) {
            turn++;
            
            // Build context (System Prompt)
            // Note: Context might update if tools modify memory/files, though session history is the main change
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
                string_free(&response);
                conversation_turn_done = true; // Stop processing this message
                break;
            }

            // Log tool calls
            for (size_t i = 0; i < tool_calls_count; i++) {
                log_info("Tool Call: %s (args: %s)", tool_calls[i].name.data, tool_calls[i].arguments.data);
            }

            // Add assistant message (content + tool_calls)
            Message* assistant_msg = message_new(ROLE_ASSISTANT, response.data);
            for (size_t i = 0; i < tool_calls_count; i++) {
                message_add_tool_call(assistant_msg, tool_calls[i].id.data, tool_calls[i].name.data, tool_calls[i].arguments.data);
            }
            session_add_message(session, assistant_msg);

            // If no tool calls, this is the final answer
            if (tool_calls_count == 0) {
                OutboundMessage* outbound = outbound_message_new(inbound->channel.data, inbound->chat_id.data, response.data);
                message_bus_send_outbound(loop->bus, outbound);
                string_free(&response);
                conversation_turn_done = true;
            } else {
                // Execute tools
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
                        log_error("Tool Execution Failed: %s", err.message);
                        string_free(&result);
                        result = string_new("Tool execution failed");
                    } else {
                        // Don't log full content for skill loading to keep logs clean
                        if (strcmp(tool_calls[i].name.data, "skill") == 0) {
                             log_info("Tool Result: [Skill content loaded, length: %zu bytes]", result.len);
                        } else {
                             log_info("Tool Result: %s", result.data);
                        }
                    }
                    
                    Message* tool_msg = message_new(ROLE_TOOL, result.data);
                    tool_msg->tool_call_id = string_copy(&tool_calls[i].id);
                    tool_msg->name = string_copy(&tool_calls[i].name);
                    session_add_message(session, tool_msg);
                    string_free(&result);
                }
                string_free(&response); // Free response string, message copy owns data
                
                // Cleanup tool calls array (data copied to message)
                for (size_t i = 0; i < tool_calls_count; i++) {
                    string_free(&tool_calls[i].id);
                    string_free(&tool_calls[i].name);
                    string_free(&tool_calls[i].arguments);
                }
                free(tool_calls);
                
                // Loop continues to next turn to let LLM see tool results
            }
            
            // Save session after each turn (optional, but safer)
            session_manager_save(loop->session_mgr, session);
        }
        
        inbound_message_free(inbound); // Free inbound message after full conversation loop

    }
}
