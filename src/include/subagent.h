#ifndef SUBAGENT_H
#define SUBAGENT_H

#include "common.h"
#include "config.h"

typedef struct SubagentManager SubagentManager;

typedef struct {
    const char* task;
    const char* label;
    const char* origin_channel;
    const char* origin_chat_id;
} SubagentSpawnRequest;

// Functions
SubagentManager* subagent_manager_create(
    void* provider,
    const char* workspace,
    void* bus,
    Config* config
);

void subagent_manager_destroy(SubagentManager* manager);

char* subagent_manager_spawn(
    SubagentManager* manager,
    const SubagentSpawnRequest* request
);

int subagent_manager_cancel_by_session(SubagentManager* manager, const char* session_key);

#endif // SUBAGENT_H
