#ifndef FILESYSTEM_TOOLS_H
#define FILESYSTEM_TOOLS_H

#include "../include/common.h"
#include "../bus/message_bus.h"
#include "../include/subagent.h"
#include "../include/cron.h"
#include "../include/skills.h"
#include "../memory/memory.h"
#include "tool.h"

typedef struct {
    MessageBus* bus;
    SubagentManager* subagent_mgr;
    CronService* cron_service;
    SkillsLoader* skills_loader;
    Memory* memory;
    const char* workspace;
} ToolContext;

// Helper to register all standard tools
void register_all_tools(ToolRegistry* reg, ToolContext* ctx);

// FileSystem tools
Error tool_read_file(void* user_data, const char* args_json, String* result);
Error tool_write_file(void* user_data, const char* args_json, String* result);
Error tool_edit_file(void* user_data, const char* args_json, String* result);
Error tool_list_dir(void* user_data, const char* args_json, String* result);

// Shell tool
Error tool_exec(void* user_data, const char* args_json, String* result);

// Web tools
Error tool_web_search(void* user_data, const char* args_json, String* result);
Error tool_web_fetch(void* user_data, const char* args_json, String* result);

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
