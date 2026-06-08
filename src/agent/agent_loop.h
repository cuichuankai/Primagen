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
#include "../providers/llm_provider.h"
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>

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
    LLMProvider* provider;

    pthread_mutex_t inbox_mutex;
    pthread_cond_t inbox_cond;
    InboundTaskNode* inbox_head;
    InboundTaskNode* inbox_tail;
    pthread_t processing_thread;
    bool processing_thread_started;
} AgentLoop;

AgentLoop* agent_loop_new(SessionManager* session_mgr, ContextBuilder* ctx_builder, ToolRegistry* tool_reg, MessageBus* bus, Config* config, PluginManager* plugin_mgr, const char* workspace_path);
void agent_loop_free(AgentLoop* loop);
void agent_loop_set_llm_provider(AgentLoop* loop, LLMProvider* provider);
void agent_loop_run(AgentLoop* loop);
void agent_loop_request_stop(AgentLoop* loop);
void agent_loop_stop(AgentLoop* loop);
void agent_loop_register_builtin_commands(AgentLoop* loop);
void agent_loop_register_builtin_tools(PluginManager* manager, ToolContext* ctx);
void agent_loop_register_builtin_channels(PluginManager* manager, Config* cfg);

/* Active-task tracking: a per-loop registry of in-flight subagent tasks.
 * add_active_task registers a task; remove_active_task unregisters it.
 * Both are thread-safe. */
void add_active_task(AgentLoop* loop, const char* task_id, const char* session_key,
                     pthread_t thread, InboundMessage* msg, size_t session_msg_count_before);

#endif // AGENT_LOOP_H
