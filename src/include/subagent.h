#ifndef SUBAGENT_H
#define SUBAGENT_H

#include <pthread.h>
#include <stdbool.h>
#include "common.h"
#include "message.h"
#include "../tools/tool.h"

typedef struct SubagentManager SubagentManager;

typedef struct {
    char* task;
    char* label;
    char* origin_channel;
    char* origin_chat_id;
    char* session_key;
} SubagentSpawnRequest;

SubagentManager* subagent_manager_create(
    void* provider,  // LLMProvider stub
    const char* workspace,
    void* bus,       // MessageBus stub
    const char* model,
    double temperature,
    int max_tokens,
    const char* reasoning_effort,
    const char* brave_api_key,
    const char* web_proxy,
    bool restrict_to_workspace
);

void subagent_manager_destroy(SubagentManager* manager);

char* subagent_manager_spawn(
    SubagentManager* manager,
    const SubagentSpawnRequest* request
);

int subagent_manager_cancel_by_session(SubagentManager* manager, const char* session_key);

#endif // SUBAGENT_H