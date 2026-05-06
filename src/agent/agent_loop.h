#ifndef AGENT_LOOP_H
#define AGENT_LOOP_H

#include "../include/common.h"
#include "../include/message.h"
#include "../session/session.h"
#include "../context/context_builder.h"
#include "../tools/tool.h"
#include "../tools/tools_impl.h"
#include "../bus/message_bus.h"
#include "../include/config.h"
#include "../plugin/plugin_manager.h"
#include "../tools/tool_executor.h"
#include "command_dispatcher.h"
#include "tool_router.h"
#include "session_orchestrator.h"
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>

typedef Error (*LLMProvider)(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, String* response, ToolCall** tool_calls, size_t* tool_calls_count);
typedef void (*LLMProviderAsync)(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, void (*callback)(Error, const char*, ToolCall*, size_t, void*), void* user_data);

typedef struct InboundTaskNode {
    InboundMessage* inbound;
    struct InboundTaskNode* next;
} InboundTaskNode;

typedef struct AgentLoop {
    SessionManager* session_mgr;
    ContextBuilder* ctx_builder;
    ToolRegistry* tool_registry;
    ToolExecutor* tool_executor;
    MessageBus* bus;
    Config* config;
    PluginManager* plugin_mgr;
    char workspace_path[512];
    CommandDispatcher* command_dispatcher;
    ToolRouter* tool_router;
    SessionOrchestrator* session_orchestrator;
    atomic_bool running;
    LLMProvider llm_call;
    LLMProviderAsync llm_call_async;

    pthread_mutex_t inbox_mutex;
    pthread_cond_t inbox_cond;
    InboundTaskNode* inbox_head;
    InboundTaskNode* inbox_tail;
    pthread_t processing_thread;
    bool processing_thread_started;
} AgentLoop;

AgentLoop* agent_loop_new(SessionManager* session_mgr, ContextBuilder* ctx_builder, ToolRegistry* tool_reg, MessageBus* bus, Config* config, PluginManager* plugin_mgr, const char* workspace_path);
void agent_loop_free(AgentLoop* loop);
void agent_loop_set_llm_provider_async(AgentLoop* loop, LLMProviderAsync provider);
void agent_loop_run(AgentLoop* loop);
void agent_loop_stop(AgentLoop* loop);
void agent_loop_register_builtin_commands(AgentLoop* loop);
void agent_loop_register_builtin_tools(PluginManager* manager, ToolContext* ctx);
void agent_loop_register_builtin_channels(PluginManager* manager, Config* cfg);

#endif // AGENT_LOOP_H
