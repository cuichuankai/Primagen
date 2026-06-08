#ifndef FILESYSTEM_TOOLS_H
#define FILESYSTEM_TOOLS_H

#include "../include/common.h"
#include "../include/utils.h"
#include "../bus/message_bus.h"
#include "../include/subagent.h"
#include "../include/cron.h"
#include "../include/skills.h"
#include "../memory/memory.h"
#include "../include/config.h"
#include "../plugin/plugin_manager.h"
#include "tool.h"
#include <pthread.h>

#define TOOL_CONTEXT_MAGIC 0x50474E31

typedef struct {
    unsigned int magic;
    MessageBus* bus;
    SubagentManager* subagent_mgr;
    CronService* cron_service;
    SkillsLoader* skills_loader;
    Memory* memory;
    Config* config;
    PluginManager* plugin_mgr;
    const char* workspace;
    pthread_mutex_t route_mutex;
    char current_channel[128];
    char current_chat_id[512];
} ToolContext;

void tool_context_set_route(ToolContext* ctx, const char* channel, const char* chat_id);
void tool_context_destroy(void* user_data);
ToolContext* tool_context_clone_with_route(ToolContext* ctx, const char* channel, const char* chat_id);

/* Allocates a new ToolContext, initializes all fields, sets the magic
 * stamp, and prepares the route mutex. Returns NULL on OOM. */
ToolContext* tool_context_new(MessageBus* bus,
                              SubagentManager* subagent_mgr,
                              CronService* cron_service,
                              SkillsLoader* skills_loader,
                              Memory* memory,
                              Config* config,
                              PluginManager* plugin_mgr,
                              const char* workspace);

// FileSystem tools
Error tool_read_file(void* user_data, const char* args_json, String* result);
Error tool_write_file(void* user_data, const char* args_json, String* result);
Error tool_edit_file(void* user_data, const char* args_json, String* result);
Error tool_list_dir(void* user_data, const char* args_json, String* result);

// Shell tool
Error tool_exec(void* user_data, const char* args_json, String* result);

// Message tool
Error tool_send_message(void* user_data, const char* args_json, String* result);

// Spawn tool (subagent)
Error tool_spawn(void* user_data, const char* args_json, String* result);

// Cron tool
Error tool_cron(void* user_data, const char* args_json, String* result);

// Skill tool
Error tool_skill(void* user_data, const char* args_json, String* result);

// Memory tool
Error tool_memory(void* user_data, const char* args_json, String* result);

#endif // FILESYSTEM_TOOLS_H
