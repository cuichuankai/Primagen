#ifndef AGENT_LOOP_H
#define AGENT_LOOP_H

#include "../include/common.h"
#include "../include/message.h"
#include "../session/session.h"
#include "../context/context_builder.h"
#include "../tools/tool.h"
#include "../bus/message_bus.h"

typedef struct {
    SessionManager* session_mgr;
    ContextBuilder* ctx_builder;
    ToolRegistry* tool_reg;
    MessageBus* bus;
    // Provider interface (simplified)
    Error (*llm_call)(const char* prompt, String* response, ToolCall** tool_calls, size_t* tool_calls_count);
} AgentLoop;

// Functions
AgentLoop* agent_loop_new(SessionManager* session_mgr, ContextBuilder* ctx_builder, ToolRegistry* tool_reg, MessageBus* bus);
void agent_loop_free(AgentLoop* loop);
void agent_loop_set_llm_provider(AgentLoop* loop, Error (*provider)(const char*, String*, ToolCall**, size_t*));
void agent_loop_run(AgentLoop* loop);

#endif // AGENT_LOOP_H