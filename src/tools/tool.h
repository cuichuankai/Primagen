#ifndef TOOL_H
#define TOOL_H

#include "../include/common.h"

typedef struct {
    String name;
    String description;
    String parameters; // JSON schema
} ToolDefinition;

typedef Error (*ToolExecuteFunc)(void* user_data, const char* args_json, String* result);
typedef void (*ToolUserDataDestroyFunc)(void* user_data);

typedef struct {
    ToolDefinition def;
    ToolExecuteFunc execute;
    void* user_data;
    void* plugin_ref;
    ToolUserDataDestroyFunc user_data_destroy;
} Tool;

typedef struct {
    Tool* tools;
    size_t count;
    size_t capacity;
} ToolRegistry;

// Functions
ToolRegistry* tool_registry_new();
void tool_registry_free(ToolRegistry* reg);
Error tool_registry_register(ToolRegistry* reg, const char* name, const char* desc, const char* params_schema, ToolExecuteFunc exec, void* user_data);
Error tool_registry_register_full(ToolRegistry* reg, const char* name, const char* desc,
                                   const char* params_schema, ToolExecuteFunc exec,
                                   void* user_data, void* plugin_ref,
                                   ToolUserDataDestroyFunc user_data_destroy);
Error tool_registry_register_plugin_tool(ToolRegistry* reg, const char* name, const char* desc,
                                          const char* params_schema, ToolExecuteFunc exec,
                                          void* user_data, void* plugin_ref);
Tool* tool_registry_get(ToolRegistry* reg, const char* name);
Error tool_registry_execute(ToolRegistry* reg, const char* name, const char* args_json, String* result);
Error tool_registry_execute_with_user_data(ToolRegistry* reg, const char* name, const char* args_json, void* user_data, String* result);

#endif // TOOL_H
