#ifndef SUBAGENT_H
#define SUBAGENT_H

#include "common.h"
#include "config.h"
#include "../bus/message_bus.h"
#include "../tools/tool.h"
#include "../context/context_builder.h"
#include "../providers/llm_provider.h"

typedef struct SubagentManager SubagentManager;

typedef struct {
    char* task;
    char* label;
    const char* origin_channel;
    const char* origin_chat_id;
} SubagentSpawnRequest;

typedef struct {
    MessageBus* bus;
    Config* config;
    LLMProvider* provider;
    char* workspace;
    ToolRegistry* tool_registry;
    ContextBuilder* ctx_builder;
} SubagentSharedContext;

SubagentSharedContext* subagent_shared_context_create(
    LLMProvider* provider,
    const char* workspace,
    void* bus,
    Config* config
);

void subagent_shared_context_destroy(SubagentSharedContext* shared);

SubagentManager* subagent_manager_create(
    SubagentSharedContext* shared
);

void subagent_manager_destroy(SubagentManager* manager);

char* subagent_manager_spawn(
    SubagentManager* manager,
    const SubagentSpawnRequest* request
);

int subagent_manager_cancel_by_session(SubagentManager* manager, const char* session_key);

#endif // SUBAGENT_H
