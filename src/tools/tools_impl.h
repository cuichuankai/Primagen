#ifndef FILESYSTEM_TOOLS_H
#define FILESYSTEM_TOOLS_H

#include "../include/common.h"

// FileSystem tools
Error tool_read_file(const char* args_json, String* result);
Error tool_write_file(const char* args_json, String* result);
Error tool_edit_file(const char* args_json, String* result);
Error tool_list_dir(const char* args_json, String* result);

// Shell tool
Error tool_exec(const char* args_json, String* result);

// Web tools
Error tool_web_search(const char* args_json, String* result);
Error tool_web_fetch(const char* args_json, String* result);

// Message tool
Error tool_send_message(const char* args_json, String* result);

// Spawn tool (subagent)
Error tool_spawn(const char* args_json, String* result);

// Cron tool
Error tool_cron(const char* args_json, String* result);

#endif // FILESYSTEM_TOOLS_H