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
#include <stdbool.h>
#include <pthread.h>

typedef struct AgentLoop AgentLoop;

// Provider interface
typedef Error (*LLMProvider)(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, String* response, ToolCall** tool_calls, size_t* tool_calls_count);
typedef void (*LLMProviderAsync)(const char* system_prompt, Session* session, ToolRegistry* tools, Config* config, void (*callback)(Error, const char*, ToolCall*, size_t, void*), void* user_data);

// Active task tracking node
typedef enum {
    SESSION_STATE_IDLE,
    SESSION_STATE_WAITING_LLM,
    SESSION_STATE_WAITING_TOOL
} SessionState;

typedef struct {
    SessionState state;
    int turn;
    char channel[64];
    char chat_id[512];
    char latest_user_content[1024];
} SessionContext;

typedef struct ActiveTaskNode {
    char task_id[32];
    char session_key[256];
    SessionContext ctx;
    pthread_t thread; // Kept for legacy/fallback tasks if needed
    bool cancelling;
    struct ActiveTaskNode* next;
} ActiveTaskNode;

typedef struct InboundTaskNode {
    InboundMessage* inbound;
    struct InboundTaskNode* next;
} InboundTaskNode;

struct AgentLoop {
    SessionManager* session_mgr;
    ContextBuilder* ctx_builder;
    ToolRegistry* tool_reg;
    ToolExecutor* tool_executor;  // Async tool executor
    MessageBus* bus;
    Config* config;
    PluginManager* plugin_mgr;  // Plugin manager for reload-plugins command
    char workspace_path[512];   // Workspace path for plugin commands
    bool running;
    LLMProvider llm_call;
    LLMProviderAsync llm_call_async;

    // Task tracking
    pthread_mutex_t task_mutex;
    pthread_mutex_t state_mutex;
    ActiveTaskNode* active_tasks;
    char current_session_key[256];  // For /stop command
    pthread_mutex_t inbox_mutex;
    pthread_cond_t inbox_cond;
    InboundTaskNode* inbox_head;
    InboundTaskNode* inbox_tail;
    pthread_t processing_thread;
    bool processing_thread_started;
};

// Functions
AgentLoop* agent_loop_new(SessionManager* session_mgr, ContextBuilder* ctx_builder, ToolRegistry* tool_reg, MessageBus* bus, Config* config, PluginManager* plugin_mgr, const char* workspace_path);
void agent_loop_free(AgentLoop* loop);
void agent_loop_set_llm_provider_async(AgentLoop* loop, LLMProviderAsync provider);
void agent_loop_run(AgentLoop* loop);
void agent_loop_stop(AgentLoop* loop);
void agent_loop_register_builtin_commands(AgentLoop* loop);  // Register built-in commands with PluginManager
void agent_loop_register_builtin_tools(PluginManager* manager, ToolContext* ctx);  // Register built-in tools with PluginManager
void agent_loop_register_builtin_channels(PluginManager* manager, Config* cfg);  // Register built-in channels with PluginManager

#endif // AGENT_LOOP_H
