#ifndef AGENT_LOOP_H
#define AGENT_LOOP_H

#include "../include/common.h"
#include "../include/message.h"
#include "../session/session.h"
#include "../context/context_builder.h"
#include "../tools/tool.h"
#include "../bus/message_bus.h"
#include "../include/config.h"
#include <stdbool.h>
#include <pthread.h>

typedef struct AgentLoop AgentLoop;

// Provider interface
typedef Error (*LLMProvider)(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, String* response, ToolCall** tool_calls, size_t* tool_calls_count);

// Active task tracking node
typedef struct ActiveTaskNode {
    char task_id[32];
    char session_key[256];
    pthread_t thread;
    bool cancelling;
    struct ActiveTaskNode* next;
} ActiveTaskNode;

struct AgentLoop {
    SessionManager* session_mgr;
    ContextBuilder* ctx_builder;
    ToolRegistry* tool_reg;
    MessageBus* bus;
    Config* config;
    bool running;
    LLMProvider llm_call;

    // Task tracking
    pthread_mutex_t task_mutex;
    ActiveTaskNode* active_tasks;
    char current_session_key[256];  // For /stop command
};

// Functions
AgentLoop* agent_loop_new(SessionManager* session_mgr, ContextBuilder* ctx_builder, ToolRegistry* tool_reg, MessageBus* bus, Config* config);
void agent_loop_free(AgentLoop* loop);
void agent_loop_set_llm_provider(AgentLoop* loop, LLMProvider provider);
void agent_loop_run(AgentLoop* loop);
void agent_loop_stop(AgentLoop* loop);

#endif // AGENT_LOOP_H
